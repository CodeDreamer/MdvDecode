// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include "MdvDecode.h"


class FileSystem_Spectrum : public FileSystem
{
public:
	FileSystem_Spectrum()
	{
		headerSize = 15;
		blockHeaderSize = 4;
		sectorSize = 528;
		sectorNumberOffset = 1;
		hasMap = false;
	}

	bool IsGoodBlock(const BYTE* p, int size)
	{
		if (size <= 15 && (p[0] & 1) != 1)	// Sector header
			return false;

		// Verify checksum
		size -= 1;
		int checksum = 0;
		for (int i = 0; i < size; i++)
		{
			checksum += *p++;
			if (checksum >= 255)
				checksum -= 255;
		}
		return p[0] == (BYTE)checksum;
	}

	void ListSectorTypes(const BYTE* pMap, vector<SECTOR_MAP_TYPE>& type)
	{
	}

	int ListFiles(const vector<int>& sectorMap, const vector<Sector>& sectorList, const vector<bool>& isGoodSector, vector<File>& fileList)
	{

		return 0;
	}
};

FileSystem* CreateFileSystem_Spectrum()
{
	return new FileSystem_Spectrum();
}