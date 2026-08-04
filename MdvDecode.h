// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#pragma once

#define _CRT_SECURE_NO_WARNINGS

#include <vector>
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <map>
#include <memory>
#include "FitDistance.h"

using namespace std;

typedef unsigned char BYTE;

struct Params
{
    int traceFreq;
    int mdvFreq;
    bool verbose;
    int track1Mask;
    int track2Mask;

    // Set to zero for no block header, non-zero if we expect a block header followed
    // by a preamble but with no gap in between.
    int blockHeaderLen;
};

struct HistResult
{
    float period;
    int quality;    // quality > 0 means there is no good separation between short and long pulses
    float shortPeriod;
    int mid;
};

HistResult MergeHistResult(HistResult hr1, HistResult hr2);
HistResult DoHistogramAndImproveFlux(const vector<int>& flux, vector<int>& result);
vector<int> RegularizeFlux(const vector<int>& flux, const HistResult& hrEven, const HistResult& hrOdd);

__int64 Timestamp(int time, int traceFreq);


struct Chunk
{
    int gapLen;
    int dataLen;
    vector<int> track1;
    vector<int> track2;
};

void RemoveSpurious(vector<BYTE>& data, int maxSpuriousLen, BYTE trackMask);
vector<Chunk> FluxChunks(const vector<BYTE>& data, int minGapLen, BYTE track1Mask, BYTE track2Mask);

enum SECTOR_MAP_TYPE
{
    SMT_UNKNOWN,
    SMT_MAP,
    SMT_EMPTY,
    SMT_FILE,
    SMT_MARKED_BAD,
    SMT_NOT_FOUND
};

enum NEXT_TYPE
{
    NT_STRONG,
    NT_OTHER
};

enum MERGE_QUALITY
{
    MQ_BAD,
    MQ_OK,
    MQ_PERFECT
};

struct Block
{
    // Index into the chunk list this block was decoded from, or -1 for a
    // merged master block. Lets the debug options go from a merged block back
    // to the raw chunk of each revolution without matching on timestamps.
    int chunkIndex = -1;
    int gapLen;
    int startTime;
    int nextLoopIndex;
    int nextType;
    int previousLoopIndex;
    bool hasNext;
    bool hasSimilarNearby;
    vector<BYTE> preamble;
    vector<BYTE> data;
    int endTime;
    int numCopies;
    int masterId;
    int order;
    bool isGood;
    int mergeQuality;
    int sectorMapType;
    int dbgId;
    // Set at decode time when the chunk's raw flux contains at least one
    // interval longer than ~2 bit-periods on that track — likely a dropout
    // that will manifest as missing / shifted bits. Rendered on the phase2
    // JPG as a red "1" / "2" inside the block rectangle.
    bool track1HasGap;
    bool track2HasGap;
#ifdef _DEBUG
    float speed;
#endif
};

vector<Block> MergeAllBlocks(vector<Block>& blockList, int totalTime, const Params& params, const char* phase2JpgPath = nullptr);

struct Tape
{
    vector<BYTE> data;
    vector<BYTE> gapFlag;

    void CreateFromBlocks(const vector<Block>& blockList, int firstBlock, float bitDuration);
    void SaveToFile(const char *fileName, int mdvFreq, int detectedOS);

private:
    BYTE gapByte;
    int numGapBits;
    void OutputGap(int numBits, BYTE value);
    void Align(size_t size);
};

struct BlockData
{
    BYTE* pData;
    int size;
};

struct Sector
{
    BlockData	header;
    BlockData	block;
    BlockData	data;
    int blockId;
    int headerId;
};

struct File
{
    string name;
    unsigned int length;
    vector<int> sectors;
};

class FileSystem
{
public:
    virtual bool IsGoodBlock(const BYTE* p, int size) = 0;
    virtual void ListSectorTypes(const BYTE *pMap, vector<SECTOR_MAP_TYPE>& type) = 0;
    virtual int ListFiles(const vector<int>& sectorMap, const vector<Sector>& sectorList, const vector<bool>& isGoodSector, vector<File>& fileList) = 0;
    virtual int GetMapSectorNum(const vector<int>& sectorMap, const vector<Sector>& sectorList)
    {
        return 0;
    }

    int headerSize;
    int blockHeaderSize;
    int sectorSize;
    int sectorNumberOffset;
    bool hasMap;
};

FileSystem * CreateFileSystem_QDOS();
FileSystem* CreateFileSystem_GST();
FileSystem* CreateFileSystem_OPD();
FileSystem* CreateFileSystem_Spectrum();
uint16_t GetWord(const BYTE* p);
uint32_t GetLong(const BYTE* p);

// pRawBlocks, when supplied, is the unmerged block list. It is only used to
// report how the individual revolutions of a bad sector agreed with each other,
// which distinguishes damage in the capture from damage on the tape.
std::unique_ptr<FileSystem> CheckFileSystem(int detectedOS, vector<Block>& blockList, const Params& params, int *pFirstBlock,
    const vector<Block>* pRawBlocks = nullptr);

void DrawError(const vector<int>& fluxData, const vector<int>& alignment);
// Same as DrawError but writes to a specific filename (release-safe;
// used by Track::StartPhaseLock / FindPreamble when the debug flag is set).
void DrawErrorNamed(const vector<int>& fluxData, const vector<int>& alignment, const char* path);
// Variant that also overlays the decoded bit value (0 or 1) inside each cell.
void DrawErrorNamedBits(const vector<int>& fluxData, const vector<int>& alignment,
    const vector<int>& bitValues, const char* path);
void DrawAllBlocks(const vector<Block>& blocks, FileSystem* pFileSys, int firstBlock, const char* jpgPath, bool verbose);

// One revolution's flux around a byte of interest, captured by -flux-block so
// every revolution can be drawn in a single stacked picture for comparison.
struct FluxWindow
{
    vector<int> flux;         // whole track, clipped at draw time
    vector<int> alignment;    // bit-cell boundaries inside the window
    vector<int> bitValues;    // decoded bit per cell
    int chunkIndex;
    int byteValue;            // what this revolution read at the target byte
};
// Draw one lane per revolution, stacked top to bottom and normalized to the
// same width so the bit cells line up across lanes.
void DrawStackedFlux(const vector<FluxWindow>& lanes, const char* path);

// Diagnostic: visualize block layout and connection state after phase 2 of
// MergeAllBlocks (hash-match + MakeConnections), before any Overlaps-based
// placement. Kept separate from DrawAllBlocks; may be removed later.
void DrawPhase2Layout(const vector<Block>& blockList, int offset, int traceFreq, const char* jpgPath, bool verbose);

#define OS_UNKNOWN 0
#define OS_QDOS 1
#define OS_SPECTRUM 2
#define OS_OPD 3
#define OS_GST 4

#define MIN_SECTOR_SIZE 256

