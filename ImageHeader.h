#pragma once

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
