// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include "MdvDecode.h"

uint16_t GetWord(const BYTE* p)
{
	uint16_t r = (uint16_t)p[0] << 8;
	return (uint16_t)(r + p[1]);
}

uint32_t GetLong(const BYTE* p)
{
	uint32_t r = (uint32_t)p[0] << 8;
	r += p[1];
	r <<= 8;
	r += p[2];
	return (uint32_t)((r << 8) + p[3]);
}



class Histogram
{
public:
	Histogram(int n)
	{
		count.resize(n);
	}

	void Add(int x)
	{
		x = min(max(x, 0), (int)count.size() - 1);
		count[x]++;
	}

	int Mode()
	{
		return max_element(count.begin(), count.end()) - count.begin();
	}

	int size()
	{
		return count.size();
	}

private:
	vector<int> count;
};

int GetSectorDistance(vector<Block>& blockList)
{
	Histogram blockDistanceHist(4);
	int last = -1;
	for (int i = 0; i < blockList.size(); i++)
	{
		if (blockList[i].data.size() >= MIN_SECTOR_SIZE)
		{
			if (last >= 0)
				blockDistanceHist.Add(i - last);
			last = i;
		}
	}
	return blockDistanceHist.Mode();
}

struct BlockTypeInfo
{
	int size;
	int start;
	int headerSize;
	int distance;
	int preambleLen;
};

// scanInternalPreamble: only true for formats that pack the block header and
// sector data back-to-back with no gap (QDOS). In that case, the sector data
// starts with a zeros+FFs preamble the ZX8302 uses to resync. Other formats
// (Spectrum, GST, OPD) have gaps before sector data.
BlockTypeInfo StudyBlockType(const vector<Block>& blockList, int distance, bool scanInternalPreamble)
{
	Histogram sizeHist(1030);
	Histogram realPreambleLen(33);
	Histogram preambleOffset(33);
	Histogram preambleLen(33);
	vector<int> traceDistance;

	int count = 100000;
	int numPreambles = 0;
	int numFound = 0;
	int sectorStart = 0;
	for (auto it = blockList.rbegin(); it != blockList.rend(); ++it)
	{
		const Block& b = *it;
		if (b.preamble.size() < 8)
			continue;
		if (b.data.size() >= MIN_SECTOR_SIZE)
		{
			count = 0;
			sectorStart = b.startTime;
		}
		else
			count++;
		if (count == distance)
		{
			numFound++;
			sizeHist.Add(b.data.size());
			traceDistance.push_back(sectorStart - b.startTime);
			realPreambleLen.Add(b.preamble.size());
			int numZeros = 0;
			int numOnes = 0;
			for (int i = 0; i < preambleOffset.size() + preambleLen.size() && i < b.data.size(); i++)
			{
				if (b.data[i] == 0)
				{
					numZeros++;
					numOnes = 0;
				}
				else if (numZeros >= 6 && b.data[i] == 0xFF)
				{
					if (++numOnes == 2)
					{
						numPreambles++;
						preambleLen.Add(numZeros + numOnes);
						preambleOffset.Add(i + 1 - numZeros - numOnes);
						break;
					}
				}
				else
				{
					numZeros = 0;
					numOnes = 0;
				}
			}
		}
	}

	int start = 0;
	int headerSize = 0;
	if (scanInternalPreamble && numPreambles > numFound / 2)
	{
		// Internal preambles
		headerSize = preambleOffset.Mode();
		start = headerSize + preambleLen.Mode();
	}

	return { sizeHist.Mode(),  start, headerSize, traceDistance[traceDistance.size() / 2], realPreambleLen.Mode() };
}

void FindSectors(FileSystem* pFileSys, vector<Block>& blockList, const vector<BlockTypeInfo>& bti, vector<Sector>& sectorList, vector<int>& sectorMap, const Params& params)
{
	const int MIN_HEADER_GAP = 40 * params.traceFreq / params.mdvFreq;
	const int WRAPAROUND = 10;
	const bool hasEmbeddedBlockHeader = pFileSys->blockHeaderSize && pFileSys->blockHeaderSize == bti.back().headerSize;
	const bool hasSeparateBlockHeader = pFileSys->blockHeaderSize && !hasEmbeddedBlockHeader;

	int time = 0;
	vector<int> startTime;
	for (int i = 0; i < blockList.size() + WRAPAROUND; i++)
	{
		const Block& b = blockList[i % blockList.size()];
		time += b.gapLen;
		startTime.push_back(time);
		time += b.endTime - b.startTime;
	}

	int lastSector = -1;
	int lastSectorStart = INT_MAX;
	int bestHeaderDistance = 0;
	int hasGoodHeader = false;
	int headerBlockIndex = INT_MAX;
	int bestBlockHeaderDistance = 0;
	int hasGoodBlockHeader = false;
	Sector s;
	for (int i = blockList.size() + WRAPAROUND - 1; i >= 0; i--)
	{
		int blockIndex = i % blockList.size();
		Block& b = blockList[blockIndex];
		if (b.preamble.size() >= 8)
		{
			if (b.data.size() >= MIN_SECTOR_SIZE)
			{
				if (lastSector >= 0 && headerBlockIndex < blockList.size())
				{
					if (hasGoodHeader)
					{
						int sectorNum = s.header.pData[pFileSys->sectorNumberOffset];
						// sectorMap maps from physical sector number to index in sectorList
						if (sectorMap[sectorNum] >= 0 && params.verbose)
							printf("Error: Found multiple copies of sector %d\n", sectorNum);
						sectorMap[sectorNum] = sectorList.size();
					}
					if (!s.header.pData)
					{
						// TODO: rebuild header if necessary
					}
					sectorList.push_back(s);
				}

				lastSectorStart = startTime[i];
				lastSector = blockIndex;
				s.data.pData = &b.data[bti.back().start];
				s.data.size = max((int)b.data.size() - bti.back().start, 0);;
				if (hasEmbeddedBlockHeader)
				{
					s.block.pData = &b.data[0];
					s.block.size = min((int)b.data.size(), bti.back().headerSize);
				}
				else
				{
					s.block.pData = nullptr;
					s.block.size = 0;
				}
				s.header.pData = nullptr;
				s.header.size = 0;
				s.blockId = blockIndex;
				s.headerId = -1;

				bestHeaderDistance = INT_MAX;
				hasGoodHeader = false;
				bestBlockHeaderDistance = INT_MAX;
				hasGoodBlockHeader = false;
			}
			else if (lastSector >= 0)
			{
				if (hasSeparateBlockHeader)
				{
					int distance = abs(lastSectorStart - startTime[i] - bti[1].distance);
					bool isGood = b.data.size() >= pFileSys->blockHeaderSize && pFileSys->IsGoodBlock(&b.data[0], pFileSys->blockHeaderSize);
					if (isGood && !hasGoodBlockHeader || distance < bestBlockHeaderDistance)
					{
						hasGoodBlockHeader = isGood;
						bestBlockHeaderDistance = distance;
						s.block.pData = &b.data[0];
						s.block.size = b.data.size();
						s.headerId = blockIndex;
					}
				}

				{
					int distance = abs(lastSectorStart - startTime[i] - bti[0].distance);
					bool isGood = b.data.size() >= pFileSys->headerSize && b.gapLen >= MIN_HEADER_GAP && pFileSys->IsGoodBlock(&b.data[0], pFileSys->headerSize);
					if (isGood && !hasGoodHeader || distance < bestHeaderDistance)
					{
						hasGoodHeader = isGood;
						bestHeaderDistance = distance;
						s.header.pData = &b.data[0];
						s.header.size = b.data.size();
						headerBlockIndex = i;
						s.headerId = blockIndex;
					}
				}
			}
		}
	}
	if (bestHeaderDistance != INT_MAX)
	{
		if (hasGoodHeader)
		{
			int sectorNum = s.header.pData[pFileSys->sectorNumberOffset];
			if (sectorMap[sectorNum] >= 0 && params.verbose)
				printf("Error: Found multiple copies of sector %d\n", sectorNum);
			sectorMap[sectorNum] = sectorList.size();
		}
		sectorList.push_back(s);
	}
}

std::unique_ptr<FileSystem> CheckFileSystem(int detectedOS, vector<Block>& masterBlocks, const Params& params, int *pFirstBlock)
{
	*pFirstBlock = -1;
	std::unique_ptr<FileSystem> pFileSys;
	switch (detectedOS)
	{
	case OS_QDOS:
		pFileSys.reset(CreateFileSystem_QDOS());
		break;
	case OS_GST:
		pFileSys.reset(CreateFileSystem_GST());
		break;
	case OS_OPD:
		pFileSys.reset(CreateFileSystem_OPD());
		break;
	case OS_SPECTRUM:
		pFileSys.reset(CreateFileSystem_Spectrum());
		break;
	}
	if (!pFileSys)
		return nullptr;

	int sectorDistance = GetSectorDistance(masterBlocks);
	if (sectorDistance != 2 && sectorDistance != 3)
		return nullptr;

	vector<BlockTypeInfo> blockInfo;
	const bool scanInternalPreamble = (detectedOS == OS_QDOS);
	for (int dist = sectorDistance - 1; dist >= 0; dist--)
		blockInfo.emplace_back(StudyBlockType(masterBlocks, dist, scanInternalPreamble));

	vector<Sector> sectorList;
	vector<int> sectorMap(256, -1);
	FindSectors(pFileSys.get(), masterBlocks, blockInfo, sectorList, sectorMap, params);

	int sectorSize = pFileSys->sectorSize;
	vector<SECTOR_MAP_TYPE> sectorType(256, SMT_NOT_FOUND);
	BYTE* pMap = nullptr;
	if (pFileSys->hasMap)
	{
		int mapSector = pFileSys->GetMapSectorNum(sectorMap, sectorList);
		if (mapSector < 0 || sectorMap[mapSector] < 0)
		{
			printf("Error: MAP sector not found\n");
			return nullptr;
		}
		else
		{
			Sector& s = sectorList[sectorMap[mapSector]];
			if (s.data.size < sectorSize || !pFileSys->IsGoodBlock(s.data.pData, sectorSize))
			{
				printf("Error: MAP sector is damaged\n");
				return nullptr;
			}

			pMap = s.data.pData;
			pFileSys->ListSectorTypes(pMap, sectorType);
			*pFirstBlock = s.headerId;
		}
	}
	else
	{
		int i = 255;
		while (i >= 0 && sectorMap[i] < 0)
			--i;
		while (i >= 0)
			sectorType[i--] = SMT_FILE;
	}

	int numBadSectors = 0;
	vector<bool> isGoodSector(256, false);
	for (int i = 0; i < 256; i++)
	{
		if (sectorType[i] != SMT_NOT_FOUND && sectorType[i] != SMT_MARKED_BAD)
		{
			if (sectorMap[i] < 0)
			{
				numBadSectors++;
				sectorType[i] = SMT_NOT_FOUND;
				if (params.verbose)
					printf("  bad sector #%d: no sector in map\n", i);
			}
			else
			{
				Sector& s = sectorList[sectorMap[i]];
				if (s.data.size < sectorSize || !pFileSys->IsGoodBlock(s.data.pData, sectorSize))
				{
					numBadSectors++;
					if (params.verbose)
						printf("  bad sector #%d: blockId=%d headerId=%d dataSize=%d %s\n",
							i, s.blockId, s.headerId, s.data.size,
							s.data.size < sectorSize ? "(short data)" : "(checksum fail)");
				}
				else
					isGoodSector[i] = true;
			}
		}
	}
	printf("%d bad sectors\n", numBadSectors);

	vector<File> fileList;
	int numMissingDir = pFileSys->ListFiles(sectorMap, sectorList, isGoodSector, fileList);
	if (numMissingDir)
		printf("%d directory sectors are missing\n", numMissingDir);
	if (!fileList.empty())
	{
		printf("Files:\n");
		for (const File& f : fileList)
		{
			int numBad = 0;
			for (int s : f.sectors)
				if (s < 0 || !isGoodSector[s])
					numBad++;
			printf("  %d %s", f.length, f.name.c_str());
			if (numBad)
				printf(" (%d bad sectors)\n", numBad);
			else
				printf("\n");
		}
	}

	if (pMap)
	{
		for (int i = 0; i < sectorMap.size(); i++)
		{
			int sectorIndex = sectorMap[i];
			if (sectorIndex >= 0)
			{
				Block& b = masterBlocks[sectorList[sectorIndex].blockId];
				b.isGood = isGoodSector[i];
				b.sectorMapType = sectorType[i];
			}
		}
	}

	return pFileSys;
}