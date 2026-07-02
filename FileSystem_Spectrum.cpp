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

	// Interface 1 ROM checksum: running byte sum reduced modulo 255.
	static BYTE ChecksumBytes(const BYTE* p, int n)
	{
		int checksum = 0;
		for (int i = 0; i < n; i++)
		{
			checksum += p[i];
			if (checksum >= 255)
				checksum -= 255;
		}
		return (BYTE)checksum;
	}

	bool IsGoodBlock(const BYTE* p, int size)
	{
		if (size <= 15)
		{
			// Sector header: HDFLAG bit 0 must be set, HDCHEK is at byte 14
			// covering bytes 0-13.
			if ((p[0] & 1) != 1)
				return false;
			return p[size - 1] == ChecksumBytes(p, size - 1);
		}

		// Data record: 15-byte RECHDR (with DESCHEK at byte 14 covering
		// bytes 0-13), then 512 bytes of DATA, then DCHEK at byte 527
		// covering the DATA only.
		if (p[14] != ChecksumBytes(p, 14))
			return false;
		return p[size - 1] == ChecksumBytes(p + 15, size - 16);
	}

	void ListSectorTypes(const BYTE* pMap, vector<SECTOR_MAP_TYPE>& type)
	{
	}

	int ListFiles(const vector<int>& sectorMap, const vector<Sector>& sectorList, const vector<bool>& isGoodSector, vector<File>& fileList)
	{
		// ZX Spectrum microdrives have no central directory; instead each
		// sector's data starts with a 15-byte record header:
		//   [0]    recflag (bit 0 = EOF marker for the last record of a file)
		//   [1]    recnum  (block number within the file, 0..)
		//   [2..3] reclen  (bytes used in this record, LE, 0..512)
		//   [4..13] recname (10-char filename, trailing space padded)
		//   [14]   header checksum
		//   [15..526] data
		//   [527]  data checksum
		// Files are reconstructed by grouping records that share a filename.
		// Bad-checksum sectors are skipped (we can't reliably read their name).
		struct RecInfo { int recnum; int sectorIndex; int reclen; };
		std::map<string, vector<RecInfo>> byName;
		for (int i = 0; i < (int)sectorMap.size(); i++)
		{
			if (sectorMap[i] < 0 || !isGoodSector[i]) continue;
			const Sector& s = sectorList[sectorMap[i]];
			if (s.data.size < 15) continue;
			// Trim trailing spaces from the 10-char filename.
			int nameLen = 10;
			while (nameLen > 0 && s.data.pData[4 + nameLen - 1] == ' ')
				nameLen--;
			if (nameLen == 0) continue;
			string name((const char*)&s.data.pData[4], nameLen);
			int recnum = s.data.pData[1];
			int reclen = s.data.pData[2] | (s.data.pData[3] << 8);
			byName[name].push_back({ recnum, i, reclen });
		}
		for (auto& kv : byName)
		{
			auto& recs = kv.second;
			std::sort(recs.begin(), recs.end(),
				[](const RecInfo& a, const RecInfo& b) { return a.recnum < b.recnum; });
			File f;
			f.name = kv.first;
			f.length = 0;
			for (const RecInfo& r : recs)
			{
				f.length += r.reclen;
				f.sectors.push_back(r.sectorIndex);
			}
			fileList.push_back(std::move(f));
		}
		return 0;
	}
};

FileSystem* CreateFileSystem_Spectrum()
{
	return new FileSystem_Spectrum();
}