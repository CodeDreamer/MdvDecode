// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include "MdvDecode.h"


class FileSystem_OPD : public FileSystem
{
public:
	FileSystem_OPD()
	{
		headerSize = 14;
		blockHeaderSize = 4;
		sectorSize = 514;
		sectorNumberOffset = 1;
		hasMap = true;
	}

	bool IsGoodBlock(const BYTE* p, int size)
	{
		if (size == 14 && p[0] != 0xFF)	// Sector header must start with FF
			return false;

		// Verify checksum
		size -= 2;
		int checksum = 0x0F0F;
		for (int i = 0; i < size; i++)
			checksum += *p++;
		return p[0] == (BYTE)checksum && p[1] == (BYTE)(checksum >> 8);
	}

	int GetMapSectorNum(const vector<int>& sectorMap, const vector<Sector>& sectorList)
	{
		// Search for the block header of the start of file number 1 or 2

		for (int sectorId = 0; sectorId < sectorMap.size(); sectorId++)
		{
			if (sectorMap[sectorId] >= 0)
			{
				const Sector& s = sectorList[sectorMap[sectorId]];
				if (s.block.size == 4 && IsGoodBlock(s.block.pData, s.block.size) && s.block.pData[1] == 1)
				{
					int fileNum = s.block.pData[0];
					if (fileNum == 1 || fileNum == 2 && s.data.size >= 512)
					{
						if (memcmp(s.data.pData + 24, "ICL ", 4) == 0)
							return sectorId;
					}
				}
			}
		}
		return -1;
	}

	void ListSectorTypes(const BYTE* pMap, vector<SECTOR_MAP_TYPE>& type)
	{
		pMap += 0x28;
		for (int s = 0; s < 236; s++)
		{
			switch (pMap[s << 1])
			{
			case 0:
				type[s] = SMT_EMPTY;
				break;

			case 1:
			case 2:
				type[s] = SMT_MAP;
				break;

			case 0xFF:
				type[s] = SMT_MARKED_BAD;
				break;

			default:
				type[s] = SMT_FILE;
				break;
			}
		}
		for (int s = 236; s < 256; s++)
			type[s] = SMT_NOT_FOUND;
	}

	int ListFiles(const vector<int>& sectorMap, const vector<Sector>& sectorList, const vector<bool>& isGoodSector, vector<File>& fileList)
	{
		int mapSectorNum = GetMapSectorNum(sectorMap, sectorList);
		const BYTE* pMap = sectorList[sectorMap[mapSectorNum]].data.pData + 0x28;
		int numErrors = 0;
		int dirLen = 0;
		bool hasDirLen = false;
		bool end = false;
		for (int dirBlockNum = 2; dirBlockNum < 32 && !end; dirBlockNum++)
		{
			int s = FindFileBlock(pMap, 1, dirBlockNum);
			if (s < 0 || !isGoodSector[s])
				s = FindFileBlock(pMap, 2, dirBlockNum);	// There are two copies of the catalog
			if (s < 0 || !isGoodSector[s])
			{
				numErrors++;
			}
			else
			{
				BYTE* p = sectorList[sectorMap[s]].data.pData;
				for (int i = 0; i < 11; i++)
				{
					int len = GetLong(p + 0x16);
					int nameLen = 20;
					while (nameLen > 0 && p[nameLen - 1] == ' ')
						nameLen--;

					int fileId = p[0x1A];
					if (i == 0 && dirBlockNum == 2)
					{
						dirLen = len - 512;
						hasDirLen = true;
					}
					else if (len && nameLen && fileId)
					{
						File f;
						f.length = len;
						f.name = string((char*)p, nameLen);
						len = (len + 511) / 512;
						for (int k = 1; k <= len; k++)
							f.sectors.push_back(FindFileBlock(pMap, fileId, k));
						fileList.emplace_back(f);
					}
					p += 44;
				}
				dirLen -= 512;
				if (hasDirLen && dirLen <= 0)
				{
					end = true;
					break;
				}

			}
		}
		return numErrors;
	}

private:
	int FindFileBlock(const BYTE* pMap, BYTE fileNum, BYTE blockNum)
	{
		for (int s = 0; s < 236; s++)
		{
			if (pMap[0] == fileNum && pMap[1] == blockNum)
				return s;
			pMap += 2;
		}
		return -1;
	}
};

FileSystem* CreateFileSystem_OPD()
{
	return new FileSystem_OPD();
}