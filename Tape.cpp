// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include "MdvDecode.h"

void Tape::OutputGap(int numBits, BYTE value)
{
	if (numBits)
	{
		if (numGapBits)
		{
			int numShift = min(numBits, 8 - numGapBits);
			numGapBits += numShift;
			numBits -= numShift;
			gapByte <<= numShift;
			gapByte |= (value >> (8 - numShift));
			if (numGapBits == 8)
			{
				gapFlag.push_back(gapByte);
				gapByte = 0;
				numGapBits = 0;
			}
		}
	}
	int numBytes = numBits >> 3;
	if (numBytes)
	{
		gapFlag.insert(gapFlag.end(), numBytes, value);
		numBits &= 7;
	}
	if (numBits)
	{
		gapByte = value >> (8 - numBits);
		numGapBits = numBits;
	}
}

void Tape::Align(size_t size)
{
	static BYTE zero = 0;

	if (size & 1)
	{
		// Add a zero to properly align to track 1
		data.insert(data.end(), &zero, &zero + 1);
		OutputGap(1, 0xFF);
	}
}

void Tape::CreateFromBlocks(const vector<Block>& blockList, int firstBlock, float bitDuration)
{
	gapByte = 0;
	numGapBits = 0;

	float scale = 1.0f / (4.0f * bitDuration);
	for (size_t i = 0; i < blockList.size(); i++)
	{
		const Block& block = blockList[(i + firstBlock) % blockList.size()];

		// GAP
		int gapBytes = (int)(block.gapLen * scale + 0.5);
		gapBytes = max(gapBytes, 0);
		gapBytes = (gapBytes + 1) & -2;	// bytes come in pairs (one per track)
		if (gapBytes)
		{
			data.resize(data.size() + gapBytes);
			OutputGap(gapBytes, 0);
		}

		// PREAMBLE
		Align(block.preamble.size());
		data.insert(data.end(), block.preamble.begin(), block.preamble.end());
		OutputGap(block.preamble.size(), 0xFF);

		// DATA
		data.insert(data.end(), block.data.begin(), block.data.end());
		OutputGap(block.data.size(), 0xFF);
		Align(block.data.size());
	}

	if (numGapBits)
		gapFlag.push_back(gapByte << (8 - numGapBits));
}

#define ULA_QL	1
#define ULA_INTERFACE_1	2
#define CREATOR_MDVDECODE 1

struct Header
{
    char		id[8];					// "QLMDVRAW"
    uint16_t    headerSize;
    uint16_t	fileFormatMajorVersion;
    uint16_t	fileFormatMinorVersion;
    uint16_t	creatorId;				// 1 = MdvDecode, 2 = Q-emuLator
    uint16_t	creatorMajorVersion;
    uint16_t	creatorMinorVersion;
    uint16_t	creatorRevision;
    uint8_t		ulaFamily;				// 0 = Unknown, 1 = ZX8302, 2 = Interface 1
    uint8_t     recognizedFileSystem;	// 0 = Unknown, 1 = QDOS, 2 = Spectrum, 3 = OPD, 4 = GST/OK
    uint32_t    frequency;				// Signal frequency (100 kHz for QL and 80 kHz for Spectrum)
	uint32_t	flags;					// Flags/reserved. Set unused bits to 0. Bit 0 = write protect
	uint32_t    dataOffset;
    uint32_t    dataLength;
    uint32_t    gapBitmapOffset;
    uint32_t    gapBitmapLength;
    uint32_t    junctionStartOffset;	// Optional unreliable section of the tape
    uint32_t    junctionLength;			// Length of unreliable section, or 0 if not used
    uint32_t    extensionOffset;		// Flexible format extension
    uint32_t	recommendedInitialTapePosition;	// Position of sector 0 header, or 0xFFFFFFFF if not known
    uint32_t    currentTapePosition;	// Optional, if we want to restart the tape from the last used position
};

void Tape::SaveToFile(const char* fileName, int mdvFreq, int detectedOS)
{
	printf("Tape is %zu bytes long including gaps (%.1f seconds)\n", data.size(), (float)data.size() * 4 / mdvFreq);
	int dataPad = (-(int)data.size() & 3);
	int gapPad = (-(int)gapFlag.size() & 3);

	Header header = { 0 };
	strncpy(header.id, "QLMDVRAW", sizeof(header.id));
	header.fileFormatMajorVersion = 1;
	header.fileFormatMinorVersion = 0;
	header.headerSize = sizeof(Header);
	header.creatorId = CREATOR_MDVDECODE;
	header.creatorMajorVersion = 1;
	header.creatorMinorVersion = 0;
	header.creatorRevision = 0;
	header.ulaFamily = (detectedOS == OS_SPECTRUM) ? ULA_INTERFACE_1 : ULA_QL;
	header.recognizedFileSystem = detectedOS;
	//header.reserved[0] = header.reserved[1] = header.reserved[2] = 0;
	header.frequency = mdvFreq;
	header.dataOffset = sizeof(Header);
	header.dataLength = (uint32_t)data.size();
	header.gapBitmapOffset = header.dataOffset + header.dataLength + dataPad;
	header.gapBitmapLength = (uint32_t)gapFlag.size();
	//header.junctionStartOffset = header.junctionLength = 0;
	// Sector 0 header block is emitted first by CreateFromBlocks, so its
	// position in the data section is 0.
	header.recommendedInitialTapePosition = 0;

	if (dataPad)
		data.insert(data.end(), dataPad, 0);
	if (gapPad)
		gapFlag.insert(gapFlag.end(), gapPad, 0);

	FILE* f = fopen(fileName, "wb");
	if (f)
	{
		fwrite(&header, sizeof(header), 1, f);
		fwrite(&data[0], 1, data.size(), f);
		fwrite(&gapFlag[0], 1, gapFlag.size(), f);
		fclose(f);
	}
}