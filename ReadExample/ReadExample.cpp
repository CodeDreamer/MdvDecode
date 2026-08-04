// Example of locating sectors from a MDVRAW for a QDOS formatted microdrive
//

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <algorithm>
#include <string>

#include "../ImageHeader.h"

// Reads a whole file into a buffer allocated with new[].
// On success returns the buffer (owned by the caller) and stores its length
// in *pSize. On failure prints a message and returns NULL.
static uint8_t* LoadFile(const char* path, size_t* pSize)
{
	*pSize = 0;

	FILE* pFile = fopen(path, "rb");
	if (pFile == nullptr)
	{
		printf("Can't open %s\n", path);
		return NULL;
	}
	if (fseek(pFile, 0, SEEK_END) != 0)
	{
		printf("Can't seek in %s\n", path);
		fclose(pFile);
		return nullptr;
	}
	long size = ftell(pFile);
	rewind(pFile);
	if (size <= 0)
	{
		printf("%s is empty\n", path);
		fclose(pFile);
		return nullptr;
	}

	uint8_t* pData = new uint8_t[size];
	size_t read = fread(pData, 1, (size_t)size, pFile);
	fclose(pFile);

	if (read != (size_t)size)
	{
		printf("Can't read %s (got %zu bytes out of %ld)\n", path, read, size);
		delete[] pData;
		return nullptr;
	}

	*pSize = (size_t)size;
	return pData;
}

static int CountLeadingZeros(uint8_t byte)
{
	if (byte == 0)
		return 8;

	int count = 0;
	while ((byte & 0x80) == 0)
	{
		count++;
		byte <<= 1;
	}
	return count;
}

struct Chunk
{
	size_t offset;
	size_t length;
};

struct Sector
{
	uint8_t sectorNum;
	size_t headerOffset;
	size_t blockOffset;
	size_t sectorOffset;

	bool operator<(const Sector& other) const
	{
		// 0 followed by other sectors in decreasing order
		if (sectorNum == 0 || other.sectorNum == 0)
			return sectorNum < other.sectorNum;
		return sectorNum > other.sectorNum;
	}
};

class MdvRawImage
{
protected:
	uint8_t* m_pImage = nullptr;
	size_t m_imageSize = 0;
	Header* m_pHeader = nullptr;
	uint8_t* m_pGapBitmap = nullptr;
	uint8_t* m_pData = nullptr;

public:
	MdvRawImage(uint8_t* pImage, size_t imageSize)
		: m_pImage(pImage), m_imageSize(imageSize)
	{
		m_pHeader = reinterpret_cast<Header*>(pImage);
		m_pGapBitmap = pImage + m_pHeader->gapBitmapOffset;
		m_pData = pImage + m_pHeader->dataOffset;
	}

	virtual ~MdvRawImage()
	{
		delete[] m_pImage;
	}

	bool SanityCheck() const
	{
		if (m_imageSize <= sizeof(Header))
		{
			printf("Image is too small to contain a valid header\n");
			return false;
		}

		const Header& h = *m_pHeader;
		if (h.dataOffset >= sizeof(Header) &&
			h.dataOffset < m_imageSize &&
			h.dataLength > 1024 &&
			h.dataLength < m_imageSize &&
			h.dataOffset + h.dataLength <= m_imageSize &&
			h.gapBitmapOffset >= sizeof(Header) &&
			h.gapBitmapOffset < m_imageSize &&
			h.gapBitmapLength >= ((h.dataLength + 7) >> 3) &&
			h.gapBitmapLength < m_imageSize &&
			h.gapBitmapOffset + h.gapBitmapLength <= m_imageSize
			)
		{
			return true;
		}

		printf("Inconsistent header values\n");
		return false;
	}

	bool IsGap(size_t gapPos) const
	{
		// Gap bitmap is a bit array, with 0 = gap and 1 = data
		return (m_pGapBitmap[(gapPos >> 3)] & (0x80 >> ((int)gapPos & 7))) == 0;
	}

	uint8_t ReadByte(size_t pos) const
	{
		// Tape is a loop, so wrap around by using modulo
		return m_pData[pos % m_pHeader->dataLength];
	}

	void FindSectors(std::vector<Sector>& result) const
	{
		const Header& h = *m_pHeader;
		const uint8_t* pData = m_pImage + h.dataOffset;
		const uint8_t* pGapBitmap = m_pImage + h.gapBitmapOffset;

		//
		// First look for the end of large gaps followed by preambles
		//

		const int MIN_GAP_SIZE = 48;
		if (h.gapBitmapLength <= MIN_GAP_SIZE / 8)
			return;

		std::vector<Chunk> chunkList;
		int gapSize = 0;
		while (gapSize < MIN_GAP_SIZE && pGapBitmap[h.gapBitmapLength - 1 - (gapSize / 8)] == 0)
			gapSize += 8;

		for (size_t i = 0; i < h.gapBitmapLength; i++)
		{
			uint8_t gapMask = pGapBitmap[i];
			if (gapMask == 0)
				gapSize += 8;
			else
			{
				if (gapSize >= MIN_GAP_SIZE)
				{
					size_t offset = i * 8 + CountLeadingZeros(gapMask);
					if (FindPreamble(offset))
						chunkList.push_back({offset, ChunkLen(offset)});
				}
				gapSize = 0;
			}
		}

		//
		// Now match headers to sectors (hardcoded for QDOS format)
		//

		const int HEADER_SIZE = 16;		// 14 bytes sector header, 2 checksum
		const int BLOCK_SIZE = 526;		// 2 bytes block header, 2 checksum, 8 preamble, 512 sector data, 2 checksum
		const int SECTOR_OFFSET = 12;	// block header length (4) plus preamble length (8)
		for (size_t i = 0; i < chunkList.size(); i++)
		{
			if (chunkList[i].length == HEADER_SIZE)
			{
				size_t offset = chunkList[i].offset;
				if (ReadByte(offset) == 0xFF)
				{
					for (size_t j = 1; j <= 4; j++)	// Max distance, assuming some spurious data in between
					{
						const Chunk& c = chunkList[(i + j) % chunkList.size()];
						if (c.length == HEADER_SIZE)
							break;	// Found another header and no sector data in between
						if (c.length >= BLOCK_SIZE)
						{
							uint8_t sectorNum = ReadByte(offset + 1);
							result.push_back({sectorNum, offset, c.offset, (c.offset + SECTOR_OFFSET) % m_pHeader->dataLength });
							break;
						}
					}
				}
			}
		}
	}

protected:
	bool FindPreamble(size_t& offset) const
	{
		const int MAX_BYTES = 20;
		int numZeros = 0;
		int numOnes = 0;
		for (int i = 0; i < MAX_BYTES; i++)
		{
			offset = (offset % m_pHeader->dataLength);
			if (IsGap(offset))
				return false;
			uint8_t byte = m_pData[offset];
			if (byte == 0)
			{
				numZeros++;
				numOnes = 0;
			}
			else
			{
				if (byte == 0xff)
				{
					if (numZeros >= 6 || numOnes > 0)
					{
						numOnes++;
						if (numOnes == 2)
						{
							offset = (offset + 1) % m_pHeader->dataLength;
							return true;
						}
					}
				}
				numZeros = 0;
			}
			offset++;
		}
		return false;
	}

	size_t ChunkLen(size_t offset) const
	{
		const int MAX_BYTES = 1200;
		int len = 0;
		while (len < MAX_BYTES && !IsGap(offset))
		{
			len++;
			offset = (offset + 1) % m_pHeader->dataLength;
		}
		return len;
	}
};

void CopyChunk(uint8_t* pDest, const MdvRawImage& image, size_t offset, size_t length)
{
	for (size_t i = 0; i < length; i++)
		pDest[i] = image.ReadByte(offset + i);

	// Add preamble marker
	pDest[-2] = pDest[-1] = 0xFF;
}

// Replaces the input file's extension with .mdv
static std::string MakeOutputPath(const char* inputPath)
{
	std::string path(inputPath);
	size_t dot = path.find_last_of('.');
	size_t separator = path.find_last_of("/\\");
	if (dot != std::string::npos && (separator == std::string::npos || dot > separator))
		path.erase(dot);
	return path + ".mdv";
}

int main(int argc, char* argv[])
{
	if (argc != 2)
	{
		printf("Usage: ReadExample <image.mdvraw>\n");
		return 1;
	}

	size_t imageSize = 0;
	uint8_t* pImage = LoadFile(argv[1], &imageSize);
	if (pImage == nullptr)
		return 1;

	printf("Read %zu bytes from %s\n", imageSize, argv[1]);
	MdvRawImage image(pImage, imageSize);	// (Note: Transfers ownership of pImage)
	if (!image.SanityCheck())
		return 1;

	// Locate all QDOS sectors in the image
	std::vector<Sector> sectorList;
	image.FindSectors(sectorList);
	printf("Found %zu QDOS sectors with both headers and data\n", sectorList.size());


	if (!sectorList.empty())
	{
		std::sort(sectorList.begin(), sectorList.end());	// Sector 0 followed by others in decreasing order

		const Sector& sector0 = sectorList[0];
		if (sector0.sectorNum != 0)
			printf("Sector 0 is missing!\n");
		else
		{
			// Example 1: validate the QDOS checksum for sector 0
			size_t offset = sector0.sectorOffset;
			uint16_t checksum = 0x0F0F;
			for (size_t i = 0; i < 512; i++)
				checksum += image.ReadByte(offset + i);

			offset += 512;
			uint16_t savedChecksum = ((uint16_t)image.ReadByte(offset + 1) << 8) | image.ReadByte(offset);
			printf("Sector %d checksum is %s\n", sector0.sectorNum, checksum == savedChecksum ? "valid" : "invalid");


			// Example 2: Convert the MDVRAW to a MDV image
			std::string outputPath = MakeOutputPath(argv[1]);
			FILE* fOutput = fopen(outputPath.c_str(), "wbx");	// Avoid overwriting an existing file - requires C11 or later
			if (fOutput == nullptr)
				printf("Can't create %s (does it already exist?)\n", outputPath.c_str());
			else
			{
				int numUsedSectors = sectorList.size() > 1 ? ((int)sectorList[1].sectorNum + 1) : 1;

				uint8_t buffer[686] = { 0 };
				size_t nextAvailable = 0;
				for (int i = 0; i < 255; i++)
				{
					int sectorId = (i == 0) ? 0 : (numUsedSectors - i);
					if (nextAvailable < sectorList.size() && sectorList[nextAvailable].sectorNum == sectorId)
					{
						const Sector& s = sectorList[nextAvailable++];
						CopyChunk(buffer + 12, image, s.headerOffset, 16);	// Sector header
						CopyChunk(buffer + 40, image, s.blockOffset, 4);	// Block header
						CopyChunk(buffer + 52, image, s.sectorOffset, 514);	// Sector data
					}
					else
					{
						// Fill missing sectors zeros
						memset(buffer, 0, sizeof(buffer));
					}

					fwrite(buffer, sizeof(buffer), 1, fOutput);
				}
				printf("Created MDV image %s\n", outputPath.c_str());
				fclose(fOutput);
			}
		}
	}



	return 0;
}
