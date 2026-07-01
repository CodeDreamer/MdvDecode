// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include "MdvDecode.h"


class FileSystem_QDOS : public FileSystem
{
public:
	FileSystem_QDOS()
	{
		headerSize = 16;
		blockHeaderSize = 4;
		sectorSize = 514;
		sectorNumberOffset = 1;
		hasMap = true;
	}

	bool IsGoodBlock(const BYTE* p, int size)
	{
		if (size == 16 && p[0] != 0xFF)	// Sector header must start with FF
			return false;

		// Verify checksum
		size -= 2;
		int checksum = 0x0F0F;
		for (int i = 0; i < size; i++)
			checksum += *p++;
		return p[0] == (BYTE)checksum && p[1] == (BYTE)(checksum >> 8);
	}

	void ListSectorTypes(const BYTE* pMap, vector<SECTOR_MAP_TYPE>& type)
	{
		type[0] = SMT_MAP;
		for (int s = 1; s <= 254; s++)
		{
			switch (pMap[s << 1])
			{
			case 0xFD:
				type[s] = SMT_EMPTY;
				break;

			case 0xFF:
				type[s] = SMT_MARKED_BAD;
				break;

			default:
				type[s] = SMT_FILE;
				break;
			}
		}
		type[254] = SMT_NOT_FOUND;
	}

	int ListFiles(const vector<int>& sectorMap, const vector<Sector>& sectorList, const vector<bool>& isGoodSector, vector<File>& fileList)
	{
		const BYTE* pMap =  sectorList[sectorMap[0]].data.pData;
		int numErrors = 0;
		int dirLen = 0;
		bool hasDirLen = false;
		bool end = false;
		for (int dirBlockNum = 0; dirBlockNum < 32 && !end; dirBlockNum++)
		{
			int s = FindFileBlock(pMap, 0, dirBlockNum);
			if (s < 0 || !isGoodSector[s])
			{
				numErrors++;
			}
			else
			{
				BYTE* p = sectorList[sectorMap[s]].data.pData;
				for (int i = 0; i < 8; i++)
				{
					int len = GetLong(p);
					int nameLen = GetWord(p + 14);

					if (i == 0 && dirBlockNum == 0)
					{
						dirLen = len;
						hasDirLen = true;
					}
					else if (len && nameLen)
					{
						File f;
						f.length = len - 64;
						f.name = string((char*)(p + 16), min(36, nameLen));
						int fileId = (dirBlockNum << 3) + i;
						len = (len + 511) / 512;
						for (int k = 0; k < len; k++)
							f.sectors.push_back(FindFileBlock(pMap, fileId, k));
						fileList.emplace_back(f);
					}
					p += 64;
					dirLen -= 64;
					if (hasDirLen && dirLen <= 0)
					{
						end = true;
						break;
					}
				}

			}
		}
		return numErrors;
	}

private:
	int FindFileBlock(const BYTE* pMap, BYTE fileNum, BYTE blockNum)
	{
		for (int s = 1; s < 254; s++)
		{
			pMap += 2;
			if (pMap[0] == fileNum && pMap[1] == blockNum)
				return s;
		}
		return -1;
	}
};

FileSystem* CreateFileSystem_QDOS()
{
	return new FileSystem_QDOS();
}