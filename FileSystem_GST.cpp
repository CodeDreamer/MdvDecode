// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include "MdvDecode.h"

class FileSystem_GST : public FileSystem
{
public:
	FileSystem_GST()
	{
		headerSize = 15;
		blockHeaderSize = 0;
		sectorSize = 1027;
		sectorNumberOffset = 1;
		hasMap = true;
	}

	bool IsGoodBlock(const BYTE* p, int size)
	{
		// Verify checksum
		// See code at $1BF2 in the GST 68K/OS ROM, called by $1880 to read a sector, and by $1844 to read a header.
		uint16_t checksum = *p++;	// Extra byte before sector
		size -= 3;
		for (int i = 0; i < size; i++)
			checksum += *p++;
		checksum = ~checksum;
		return p[0] == (BYTE)checksum && p[1] == (BYTE)(checksum >> 8);
	}

	void ListSectorTypes(const BYTE* pMap, vector<SECTOR_MAP_TYPE>& type)
	{
		pMap += 29;
		for (int i = 0; i < 100; i++)
			type[i] = (pMap[i] != 0xFD) ? SMT_FILE : SMT_MARKED_BAD;
	}

	int ListFiles(const vector<int>& sectorMap, const vector<Sector>& sectorList, const vector<bool>& isGoodSector, vector<File>& fileList)
	{
		const BYTE* pMap = sectorList[sectorMap[0]].data.pData + 29;
		int numErrors = 0;
		int dirLen = GetWord(pMap - 2);
		int offset = 0x80;
		int s = 0;
		while (s != 0xFE && offset < dirLen)
		{
			if (!isGoodSector[s])
			{
				numErrors++;
			}
			else
			{
				BYTE* p = sectorList[sectorMap[s]].data.pData + 1;
				while (offset < 1024 && offset < dirLen)
				{
					if (p[offset])	// Entry used or deleted?
					{
						int len = GetLong(p + offset + 4);
						int nameLen = p[offset + 17];
						File f;
						f.length = len;
						f.name = string((char*)(p + offset + 18), min(64-18, nameLen));	// TODO: what is the actual max name length?
						int sectorNum = p[offset + 3];
						int maxCount = 100;
						while (sectorNum != 0xFE && --maxCount >= 0)
						{
							f.sectors.push_back(sectorNum);
							sectorNum = pMap[sectorNum];
						}
						fileList.emplace_back(f);
					}
					offset += 64;
				}

			}
			offset = 0;
			dirLen -= 1024;
			s = pMap[s];
		}
		return numErrors;
	}

private:
	bool Checksum()
	{

	}
};

FileSystem* CreateFileSystem_GST()
{
	return new FileSystem_GST();
}