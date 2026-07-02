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

std::unique_ptr<FileSystem> CheckFileSystem(int detectedOS, vector<Block>& blockList, const Params& params, int *pFirstBlock);

void DrawError(const vector<int>& fluxData, const vector<int>& alignment);
void DrawAllBlocks(const vector<Block>& blocks, FileSystem* pFileSys, int firstBlock, const char* jpgPath, bool verbose);

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

