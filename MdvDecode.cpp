// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include "MdvDecode.h"
#include "Track.h"

__int64 Timestamp(int time, int traceFreq)
{
    return (__int64)time * 1000000 / traceFreq;
}

// Read file into vector
inline bool Load(const string& path, vector<BYTE>& result)
{
    result.clear();
    char buffer[512];

    int fileIndex = 1;
    while (true)
    {
        sprintf(buffer, "%s\\logic-1-%d", path.c_str(), fileIndex);
        FILE* f = fopen(buffer, "rb");
        if (f == NULL)
            break;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        size_t base = result.size();
        if (base == 0)
            result.reserve((size_t)len * 100);
        result.resize(base + len);
        fread(&result[base], 1, len, f);
        fclose(f);
        fileIndex++;
    }

    return result.size() != 0;
}

// Per-chunk instrumentation: how often does ByteSync see the two tracks drift
// far enough apart that it silently declines to resync (delta >= 40% of the
// bit period)? Set BYTE_SYNC_TOLERANCE, reset once per DecodeBlock, printed
// in verbose mode after each block.
static const float BYTE_SYNC_TOLERANCE = 0.4f;

struct ByteSyncStats
{
    int missCount = 0;
    float maxRatio = 0.0f;    // max observed |delta|/period among misses
    int firstMissByteIdx = -1;
    int lastMissByteIdx = -1;
};
static ByteSyncStats g_bsStats;

// Byte-window flux picture params (mid-block region-of-interest dump). Set
// once from main() before the decode loop. When both are non-negative and
// this chunk's debugTag is active, DecodeBlock saves a flux+alignment JPG
// per track around block-data byte offset g_fluxByteOffset.
static int g_fluxByteOffset = -1;
static int g_fluxByteWindow = 16;

// If chunkIdx is the -flux-jpg target, format a debug tag into `buf` and
// return it; otherwise return nullptr. Keeps the per-chunk decode loop
// visually uncluttered by the debug wiring.
static const char* MakeFluxJpgTag(int chunkIdx, int fluxJpgChunk, int time,
    int traceFreq, char* buf, size_t bufLen)
{
    if (chunkIdx != fluxJpgChunk) return nullptr;
    snprintf(buf, bufLen, "chunk%d", chunkIdx);
    printf("Dumping flux picture for chunk #%d (timestamp %lld uSec)\n",
        chunkIdx, (long long)Timestamp(time, traceFreq));
    return buf;
}

// Slice the per-track alignmentBuffer around `byteOffset` (block-level, 0-based)
// with `halfWidth` bytes of context on each side, and hand it to DrawErrorNamed
// together with the track's full flux. DrawErrorImpl auto-clips the picture to
// the flux region matching alignment[0], so windowing the alignment is enough.
static void SaveByteFluxPictures(Track& track1, Track& track2, const char* debugTag,
    int byteOffset, int halfWidth)
{
    // block.data is interleaved (t1, t2, t1, t2, ...); byte P → track P%2, index P/2
    int perTrackByte = byteOffset / 2;
    int firstByte = perTrackByte - halfWidth;
    if (firstByte < 0) firstByte = 0;
    int lastByte = perTrackByte + halfWidth + 1;
    int firstBit = firstByte * 8;
    int lastBit = lastByte * 8;

    auto dumpOne = [&](Track& t, int trackNum) {
        const vector<int>& a = t.GetAlignmentBuffer();
        const vector<int>& v = t.GetBitValueBuffer();
        if (a.empty()) return;
        int lo = min(firstBit, (int)a.size());
        int hi = min(lastBit,  (int)a.size());
        if (hi - lo < 8) return;   // fewer than one byte of ticks → not useful
        vector<int> subA(a.begin() + lo, a.begin() + hi);
        vector<int> subV;
        if ((int)v.size() >= hi)
            subV.assign(v.begin() + lo, v.begin() + hi);
        char path[256];
        snprintf(path, sizeof(path), "flux_%s_byte%d_track%d.jpg",
            debugTag, byteOffset, trackNum);
        DrawErrorNamedBits(t.GetFlux(), subA, subV, path);
        printf("Saved %s (alignment %d bits, ticks %d..%d)\n", path, hi - lo, lo, hi);

        // Text dump for cross-checking: labeled bits + tick times + raw flux
        // intervals overlapping the window's time range. Lets us compare what
        // the decoder concluded vs what the flux numerically contains.
        printf("  track%d labels (bit lo..hi, LSB-first): ", trackNum);
        for (size_t i = 0; i < subV.size(); i++)
            printf("%d%s", subV[i], ((i + 1) % 8 == 0) ? " " : "");
        printf("\n");
        printf("  track%d ticks (sample times):", trackNum);
        for (size_t i = 0; i < subA.size(); i++)
        {
            if (i < 8 || i >= subA.size() - 8)
                printf(" %d", subA[i]);
            else if (i == 8)
                printf(" ...");
        }
        printf("\n");
        int tStart = subA.front();
        int tEnd   = subA.back();
        const vector<int>& flux = t.GetFlux();
        printf("  track%d flux intervals in [%d..%d]:", trackNum, tStart, tEnd);
        int cum = 0;
        int printed = 0;
        for (size_t i = 0; i < flux.size() && cum <= tEnd; i++)
        {
            cum += flux[i];
            if (cum >= tStart && cum <= tEnd)
            {
                printf(" %d", flux[i]);
                if (++printed >= 80) { printf(" ..."); break; }
            }
        }
        printf("\n");
    };
    dumpOne(track1, 1);
    dumpOne(track2, 2);
}

// Returns true if ByteSync applied the correction; false if delta was too big
// (miss, tracked in g_bsStats) or if we had no midTime yet.
bool ByteSync(Track& track1, Track& track2)
{
    float period = (track1.GetPeriod() + track2.GetPeriod()) * 0.5;
    float midTime = track2.GetMidTime();
    if (midTime == 0)
        return false;

    double delta = track1.GetExpectedTime() - midTime;
    double absDelta = fabs(delta);
    if (absDelta < period * BYTE_SYNC_TOLERANCE)
    {
        track1.SetPeriod(period);
        track2.SetPeriod(period);
        delta *= 0.5;
        track1.AdjustExpectedTime(-(float)delta);
        track2.AdjustExpectedTime(+(float)delta);
        return true;
    }

    g_bsStats.missCount++;
    float ratio = (float)(absDelta / period);
    if (ratio > g_bsStats.maxRatio)
        g_bsStats.maxRatio = ratio;
    return false;
}

void ReadBlockData(vector<BYTE>& data, Track& track1, Track& track2, int maxNum)
{
    data.clear();
    BYTE b1, b2;
    while ((maxNum -= 2) >= 0)
    {
        if (!track1.ReadByte(b1))
            break;
        data.push_back(b1);     // Blocks with an odd number of bytes are possible, e.g. GST 68K/OS
        if (!track2.ReadByte(b2))
            break;
        data.push_back(b2);

        int missBefore = g_bsStats.missCount;
        ByteSync(track1, track2);
        if (g_bsStats.missCount > missBefore)
        {
            int idx = (int)data.size();
            if (g_bsStats.firstMissByteIdx < 0)
                g_bsStats.firstMissByteIdx = idx;
            g_bsStats.lastMissByteIdx = idx;
        }
    }
}

float GetBytePeriod(Track& track1, Track& track2)
{
    return (track1.GetPeriod() + track2.GetPeriod()) * 4;
}

bool SyncTracks(Track& track1, Track& track2)
{
    float trackOffset = GetBytePeriod(track1, track2) * 0.5f;
    if (track1.GetTime() + trackOffset > track2.GetTime())
    {
        track2.AdvanceTo(track1.GetTime() + trackOffset);
        track2.ResetExpectedTime();
    }
    else
    {
        track1.AdvanceTo(track2.GetTime() - trackOffset);
        track1.ResetExpectedTime();
    }
    return true;
}

enum FailReason
{
    FR_NONE = 0,
    FR_PHASE_LOCK_BOTH,
    FR_PHASE_LOCK_TRACK1_RESCUED,   // track1 failed, track2 rescued (soft success)
    FR_PHASE_LOCK_TRACK2_RESCUED,   // track2 failed, track1 rescued (soft success)
    FR_PREAMBLE_BOTH,
    FR_PREAMBLE_TRACK2_ONLY,        // recovered with track1's preamble (soft success)
    FR_NO_DATA,
    FR_CHUNK_TOO_SHORT,
    FR__COUNT
};

static const char* FailReasonName(FailReason r)
{
    switch (r)
    {
    case FR_NONE:                        return "ok";
    case FR_PHASE_LOCK_BOTH:             return "phase-lock fail (both tracks)";
    case FR_PHASE_LOCK_TRACK1_RESCUED:   return "phase-lock track1 rescued via track2";
    case FR_PHASE_LOCK_TRACK2_RESCUED:   return "phase-lock track2 rescued via track1";
    case FR_PREAMBLE_BOTH:               return "preamble not found (both tracks)";
    case FR_PREAMBLE_TRACK2_ONLY:        return "preamble track2 only (recovered)";
    case FR_NO_DATA:                     return "phase lock ok but no data extracted";
    case FR_CHUNK_TOO_SHORT:             return "chunk shorter than minBlockLen";
    default:                             return "unknown";
    }
}

bool DecodeBlock(Block& block, Track& track1, Track& track2, float maxDeviation, int minNumBytes, int maxBlockSize, const Params& params, FailReason& reason, const char* debugTag = nullptr)
{
    block.data.clear();
    block.preamble.clear();
    reason = FR_NONE;
    g_bsStats = ByteSyncStats();

#ifdef _DEBUG
    __int64 timestamp = Timestamp(block.startTime, params.traceFreq);
#endif

    // Try both tracks. If either locks, use its period estimate for the
    // other and let SyncTracks position it. This rescues chunks where one
    // track has a magnetisation dropout or PLL-warm-up glitch but the other
    // stays clean (the two tracks share the same physical tape speed, so
    // the bit period is authoritative from whichever one locked).
    bool track1Locked = track1.StartPhaseLock(maxDeviation, minNumBytes);
    bool track2Locked = track2.StartPhaseLock(maxDeviation, minNumBytes);
    if (!track1Locked && !track2Locked)
    {
        reason = FR_PHASE_LOCK_BOTH;
        return false;
    }
    if (!track1Locked)
    {
        track1.Restart(track2.GetPeriod());   // adopt track2's period; SyncTracks below repositions us
        reason = FR_PHASE_LOCK_TRACK1_RESCUED;
    }
    else if (!track2Locked)
    {
        track2.Restart(track1.GetPeriod());
        reason = FR_PHASE_LOCK_TRACK2_RESCUED;
    }

    // Valid data should have signal on both tracks, so ignore anything before
    SyncTracks(track1, track2);
    block.endTime = -track1.GetTime();

    // QL specific logic:
    // The ZX8302 returns whole bytes, so we need to make some assumptions about how it identifies the first valid bit of the stream.
    // Zeroes would be needed to synchronize the PLL (since for 1s it's not clear which of the transitions is the one in the middle).
    // We know that when formatting a cartridge, QDOS writes at least three zeroes followed by one FF to each track.
    // Alternate bytes are stored on the two microdrive tracks and track 2 has a 4 bit offset compared to track 1.
    // We will assume that the ZX8302 looks for zeros followed by one FF. Perhaps only on track1 (it comes first), or we could also look on both.
    // The two tracks need to roughly keep their alignment, since the ZX8302 alternates reading from them and exposes only a single
    // 'byte-available' hardware flag to the OS. Each track has its own byte-long output buffer.
    // Looking at the output of QDOS sector reads, sometimes bad sectors end with all-zeros as QDOS gives up if it cannot receive a byte on one of
    // the tracks for some time. To increase the amount of data recovered, we will continue to read bytes and temporarily fill with zeroes the
    // bad track instead, with the hope that we will be able to sync up again later.

    vector<BYTE> preamble1, preamble2;
    int preambleStartTime1, preambleStartTime2;
    bool preamble1Ok = track1.FindPreamble(2, 1, 30, preamble1, &preambleStartTime1);
    bool preamble2Ok = track2.FindPreamble(2, 1, 30, preamble2, &preambleStartTime2);

    if (preamble1Ok && !preamble2Ok)
    {
        if (params.verbose)
            printf("Warning: track2 preamble not found, using track1\n");
        track2.Restart(track1.GetPeriod());
        SyncTracks(track1, track2);
        preamble2 = preamble1;
        reason = FR_PREAMBLE_TRACK2_ONLY;
    }
    else if (!preamble1Ok && preamble2Ok)
    {
        // Symmetric fallback: track1 had a magnetisation glitch or PLL
        // warm-up issue; use track2's preamble timing to bootstrap track1.
        if (params.verbose)
            printf("Warning: track1 preamble not found, using track2\n");
        track1.Restart(track2.GetPeriod());
        SyncTracks(track1, track2);
        preamble1 = preamble2;
        preamble1Ok = true;
        reason = FR_PREAMBLE_TRACK2_ONLY;
    }
    else if (!preamble1Ok && !preamble2Ok)
    {
        reason = FR_PREAMBLE_BOTH;
    }

    float bytePeriod = GetBytePeriod(track1, track2);
    size_t preambleSize = min(preamble1.size(), preamble2.size());
    if (preambleSize < preamble1.size())
        preambleStartTime1 += (int)(bytePeriod * (preamble1.size() - preambleSize) + 0.5);
    block.gapLen += preambleStartTime1;
    block.startTime += preambleStartTime1;
    block.endTime += block.startTime;
    if (preamble1Ok)
    {
        // Align to end
        for (size_t i = 0; i < preambleSize; i++)
        {
            block.preamble.push_back(preamble1[preamble1.size() - preambleSize + i]);
            block.preamble.push_back(preamble2[preamble2.size() - preambleSize + i]);
        }
    }
    else
    {
        // Align to start
        for (size_t i = 0; i < preambleSize; i++)
        {
            block.preamble.push_back(preamble1[i]);
            block.preamble.push_back(preamble2[i]);
        }
    }

    // Check that track alignment is still consistent
    if (params.verbose && preamble1Ok && preamble2Ok)
    {
        float distance = abs(track1.GetTime() + bytePeriod * 0.5 - track2.GetTime());
        if (distance > bytePeriod * 0.25)
        {
            printf("Warning: track1 and track2 preambles are not aligned at timestamp %zu uSec\n", Timestamp(block.startTime, params.traceFreq));
        }
    }

    if (preamble1Ok)
    {
        // Only the "data" pass (unbounded read) is worth a mid-block flux
        // picture — the header pass is only a handful of bytes.
        bool wantByteDump = debugTag && g_fluxByteOffset >= 0 && maxBlockSize == 0;
        if (wantByteDump)
        {
            track1.EnableAlignmentCollection(true);
            track2.EnableAlignmentCollection(true);
        }
        ReadBlockData(block.data, track1, track2, maxBlockSize ? maxBlockSize : INT_MAX);
        if (wantByteDump)
        {
            SaveByteFluxPictures(track1, track2, debugTag, g_fluxByteOffset, g_fluxByteWindow);
            track1.EnableAlignmentCollection(false);
            track2.EnableAlignmentCollection(false);
        }
        if (params.verbose)
        {
            printf("Read block with %zu bytes\n", block.data.size());
            if (g_bsStats.missCount > 0)
                printf("  ByteSync misses (|delta| >= %.0f%% period): count=%d maxRatio=%.2f byte=[%d..%d] (ts=%lldus)\n",
                    BYTE_SYNC_TOLERANCE * 100.0f,
                    g_bsStats.missCount, g_bsStats.maxRatio,
                    g_bsStats.firstMissByteIdx, g_bsStats.lastMissByteIdx,
                    (long long)Timestamp(block.startTime, params.traceFreq));
        }
    }
    else
    {
        block.data = block.preamble;
        block.preamble.clear();
    }

    block.endTime += track1.GetTime();

 /*   if (block.data.size() == 16 && block.data[1] == 0)
    {
        return true;
    } */

    return preamble1Ok;
}

bool ShouldSwapTracks(const vector<Chunk>& chunkList, float avgPeriod, float maxDeviation, int minNumBytes)
{
    // Same logic as the one used later when starting to decode blocks
    int voteSwap = 0;
    int voteDontSwap = 0;
    for (const Chunk& chunk : chunkList)
    {
        Track track1(chunk.track1, avgPeriod);
        Track track2(chunk.track2, avgPeriod);
        if (track1.StartPhaseLock(maxDeviation, minNumBytes) &&
            track2.StartPhaseLock(maxDeviation, minNumBytes))
        {
            SyncTracks(track1, track2);

            vector<BYTE> preamble1, preamble2;
            int preambleStartTime1_, preambleStartTime2_;
            bool preamble1Ok = track1.FindPreamble(2, 1, 30, preamble1, &preambleStartTime1_);
            bool preamble2Ok = track2.FindPreamble(2, 1, 30, preamble2, &preambleStartTime2_);
            if (preamble1Ok && preamble2Ok)
            {
                float bytePeriod = GetBytePeriod(track1, track2);
                float distance = abs(track1.GetTime() + bytePeriod * 0.5 - track2.GetTime());
                if (distance < bytePeriod * 0.5)
                    voteDontSwap++;
                else
                    voteSwap++;
                if (voteSwap == 100 || voteDontSwap == 100)
                    break;
            }
        }
    }

    return voteSwap > voteDontSwap;
}

void SwapTracks(vector<Chunk>& chunkList)
{
    for (Chunk& chunk : chunkList)
        swap(chunk.track1, chunk.track2);
}

bool DecodeBlocks(const Chunk& chunk, vector<Block>& blockList, int time, float* pAvgPeriod, float maxDeviation, int minNumBytes, const Params& params, int *pExtraGap, int minBlockLen, FailReason& reason, const char* debugTag = nullptr)
{
    reason = FR_NONE;
    int maxBlockHeaderSize = params.blockHeaderLen;
    Block block;
    block.gapLen = chunk.gapLen + *pExtraGap;
    block.startTime = time;
    *pExtraGap = 0;
    Track track1(chunk.track1, *pAvgPeriod);
    Track track2(chunk.track2, *pAvgPeriod);
    if (debugTag)
    {
        char t1[64], t2[64];
        snprintf(t1, sizeof(t1), "%s_track1", debugTag);
        snprintf(t2, sizeof(t2), "%s_track2", debugTag);
        track1.SetDebug(true, t1);
        track2.SetDebug(true, t2);
    }

    if (chunk.track1.size() >= (minBlockLen + 1) * 8)
    {
        // Update tape speed
        vector<int> betterFlux;
        HistResult hr1 = DoHistogramAndImproveFlux(chunk.track1, betterFlux);
        if (!betterFlux.empty())
            track1.ReplaceFlux(betterFlux); // TODO: check that speed is consistent with other track, or replacing the flux will cause issues
        HistResult hr2 = DoHistogramAndImproveFlux(chunk.track2, betterFlux);
        if (!betterFlux.empty())
            track2.ReplaceFlux(betterFlux);
        HistResult hr = MergeHistResult(hr1, hr2);
        if (hr.quality != INT_MAX)
        {
            float damp = (hr.quality == 0) ? 0.95 : 0.99;
            *pAvgPeriod = *pAvgPeriod * damp + hr.period * (1 - damp);
        }
    }
#ifdef _DEBUG
    block.speed = *pAvgPeriod;
#endif
    if (params.verbose)
        printf("Speed %.1f\n", *pAvgPeriod);

    track1.SetPeriod(*pAvgPeriod);
    track2.SetPeriod(*pAvgPeriod);

    // Per-track "has a long gap" diagnostic: at the current rough bit-period,
    // scan the chunk's raw intervals for anything > 2× period. That's a
    // dropout candidate (a stretch of tape with no captured transitions
    // that PLL can't have kept lock through). Stamped on every block pushed
    // from this chunk; DrawPhase2Layout renders it as a red "1" or "2".
    {
        float gapThreshold = *pAvgPeriod * 2.0f;
        block.track1HasGap = false;
        for (int iv : chunk.track1)
            if ((float)iv > gapThreshold) { block.track1HasGap = true; break; }
        block.track2HasGap = false;
        for (int iv : chunk.track2)
            if ((float)iv > gapThreshold) { block.track2HasGap = true; break; }
    }

    if (maxBlockHeaderSize && chunk.dataLen <= (int)(MIN_SECTOR_SIZE * 8 * *pAvgPeriod))    // Only large chunks can contain block headers + sector data
        maxBlockHeaderSize = 0;

    bool bOk = DecodeBlock(block, track1, track2, maxDeviation, minNumBytes, maxBlockHeaderSize, params, reason, debugTag);

    if (block.data.size())
        blockList.push_back(block);
    else
    {
        *pExtraGap = block.gapLen + chunk.dataLen;
        bOk = false;
        if (reason == FR_NONE)
            reason = FR_NO_DATA;
    }

    if (bOk && maxBlockHeaderSize)
    {
        track1.SetPeriod(*pAvgPeriod);
        track2.SetPeriod(*pAvgPeriod);
        block.startTime += track1.GetTime();
        block.gapLen = 0;

        FailReason innerReason = FR_NONE;
        if (!DecodeBlock(block, track1, track2, maxDeviation, minNumBytes, 0, params, innerReason, debugTag))
            return false;
        if (block.data.size())
            blockList.push_back(block);
        else
        {
            int x = 0;
        }
    }

    return bOk;
}

int ChooseFirstSector(vector<Block>& blockList, int loopLen)
{
    int largestDistance = blockList[0].startTime + loopLen - blockList.back().startTime;
    int firstSector = 0;
    for (int i = 1; i < blockList.size(); i++)
    {
        int delta = blockList[i].startTime - blockList[i - 1].startTime;
        if (delta > largestDistance)
        {
            largestDistance = delta;
            firstSector = i;
        }
    }
    /*
sort(gapList.begin(), gapList.end());
int medianGap = gapList[gapList.size() / 2];

int firstSector = 0;
int largestDistance = INT_MIN;
for (int i = 0; i < blockList.size(); i++)
{
    int distance = abs(blockList[i].gapLen - medianGap);
    if (distance > largestDistance)
    {
        largestDistance = distance;
        firstSector = i;
    }
}
*/
    return firstSector;
}

static void ReportGapLen(const vector<Block>& blockList, size_t len, const char *name, float conversionScale)
{
    vector<int> gapLen;
    for (const Block& b : blockList)
        if (b.data.size() == len)
            gapLen.push_back(b.gapLen);
    if (!gapLen.empty())
    {
        std::sort(gapLen.begin(), gapLen.end());
        int medianLen = gapLen[gapLen.size() / 2];
        printf("Median %s gap length: %.1f bytes\n", name, medianLen * conversionScale);
    }
}

int DetectAndReport(const vector<Block>& blockList, const Params& params)
{
    int detectedOS = OS_UNKNOWN;
    vector<int> spuriousLen;
    vector<int> bigLen;
    vector<int> smallLen;

    float k = params.mdvFreq / (4.0f * params.traceFreq);

    int numReliable = 0;
    int numUnreliable = 0;
    for (const Block& b : blockList)
    {
        int size = (int)b.data.size();
        if (b.preamble.size() < 8)
            spuriousLen.push_back(size);
        else if (size >= MIN_SECTOR_SIZE)
            bigLen.push_back(size);
        else
            smallLen.push_back(size);
        if (b.numCopies >= 2)
            numReliable++;
        else
            numUnreliable++;
    }

    printf("Found %zu sectors and %zu chunks of spurious data\n", bigLen.size(), spuriousLen.size());
    printf("%d good blocks out of %zu\n", numReliable, blockList.size());

    if (bigLen.size() && smallLen.size())
    {
        std::sort(bigLen.begin(), bigLen.end());
        std::sort(smallLen.begin(), smallLen.end());
        unsigned int numSectors = bigLen.size();
        int sectorLen = bigLen[numSectors / 2];
        if (smallLen.size() > numSectors * 3 / 2)
        {
            // 2 or more headers per sector
            int headerLen1 = smallLen[smallLen.size() / 4];
            int headerLen2 = smallLen[smallLen.size() * 3 / 4];
            printf("Header and sector sizes (bytes): %d, %d, %d\n", headerLen1, headerLen2, sectorLen);
            ReportGapLen(blockList, headerLen1, "header1", k);
            ReportGapLen(blockList, headerLen2, "header2", k);

            if (sectorLen == 514)
                detectedOS = OS_OPD;
        }
        else
        {
            // 1 header per sector
            int headerLen = smallLen[smallLen.size() / 2];
            printf("Header and sector sizes (bytes): %d, %d\n", headerLen, sectorLen);
            ReportGapLen(blockList, headerLen, "header", k);

            if (sectorLen == 1029 && headerLen == 17)
            {
                detectedOS = OS_GST;
                if (numSectors < 100)
                    printf("%d sectors missing\n", 100 - numSectors);
            }
            else if (sectorLen == 526 && headerLen == 16 && numSectors >= 150)
                detectedOS = OS_QDOS;   // Note: block header and sector data are back to back
            else if ((sectorLen == 528 || sectorLen == 529) && (headerLen == 15 || headerLen == 16))
                detectedOS = OS_SPECTRUM;

        }
        ReportGapLen(blockList, sectorLen, "sector", k);
    }

    static const char* osName[] = { "Unknown", "QDOS", "Spectrum", "OPD", "GST" };
    printf("Detected operating system: %s\n", osName[detectedOS]);

    return detectedOS;
}

bool FindBestChannels(vector<BYTE>& data, Params& params)
{
    int numTransitions[8] = { 0 };
    for (size_t i = 1; i < data.size(); i++)
    {
        int diff = data[i - 1] ^ data[i];
        for (int ch = 0; ch < 8; ch++)
        {
            if (diff & 1)
                numTransitions[ch]++;
            diff >>= 1;
        }
    }

    int maxIndex = max_element(numTransitions, numTransitions + 8) - numTransitions;
    if (numTransitions[maxIndex] < 200000)
        return false;
    params.track2Mask = 1 << maxIndex;
    numTransitions[maxIndex] = 0;

    maxIndex = max_element(numTransitions, numTransitions + 8) - numTransitions;
    if (numTransitions[maxIndex] < 200000)
        return false;
    params.track1Mask = 1 << maxIndex;
    return true;
}

void ExportTrack(const vector<BYTE>& data, BYTE mask, int start, int count)
{
    count /= 8;
    FILE* f = fopen("export.bin", "wb");
    int n = 0;
    BYTE acc = 0;
    for (; start < data.size(); start++)
    {
        acc <<= 1;
        if (data[start] & mask)
            acc |= 1;
        n = (n + 1) & 7;
        if (n == 0)
        {
            fwrite(&acc, 1, 1, f);
            if (--count == 0)
                break;
        }
    }
    fclose(f);
}

#ifdef _DEBUG
void ExportSpeed(const vector<Block>& blockList, const Params& params)
{
    float nominalSpeed = 76;    // 76 cm/sec
    float k = nominalSpeed * params.traceFreq / params.mdvFreq;
    FILE* f = fopen("speed.csv", "wb");
    for (const Block& b : blockList)
        fprintf(f, "%f,%f\r\n", (float)b.endTime / params.traceFreq, k / b.speed);
    fclose(f);
}
#endif

int main(int argc, char* argv[])
{
    printf("MdvDecode 1.1 by Daniele Terdina\n");
    Params params;
    params.verbose = false;
    params.blockHeaderLen = 0;
    //params.blockHeaderLen = 4;  // OPD
    params.traceFreq = 24000000;
    params.mdvFreq = 100000;
    params.track1Mask = 1;
    params.track2Mask = 2;
    bool canChooseChannels = true;
    bool saveJpg = false;
    int fluxJpgChunk = -1;   // -flux-jpg <index>: dump flux picture for that chunk
    int fluxByteOffset = -1; // -flux-byte <k>: also dump a mid-block flux picture around byte k of the block
    int fluxByteWindow = 16; // -flux-window <w>: half-window in bytes (per track) around the byte of interest
    int dumpBlock = -1;      // -dump-block <index>: dump raw copies of the given merged block
    const float maxFreqError = 0.2;
    const float minGapDuration = 0.002;

#ifndef MDVDECODE_DEBUG_HARDCODED_INPUT
    //==========================================================================
    // Parse command line parameters
    //==========================================================================
    int argIndex = 1;
    bool argError = false;
    while (argIndex < argc && !argError && (argv[argIndex][0] == '-' || argv[argIndex][0] == '/'))
    {
        char* p = argv[argIndex];
        while (*p == '-' || *p == '/')
            p++;
        if (!_stricmp(p, "opd"))
            params.blockHeaderLen = 4;
        else if (!_stricmp(p, "zx"))
            params.mdvFreq = 80000;
        else if (!_stricmp(p, "v") || !_stricmp(p, "verbose"))
            params.verbose = true;
        else if (!_stricmp(p, "flux-jpg") && argIndex + 1 < argc)
        {
            fluxJpgChunk = atoi(argv[argIndex + 1]);
            argIndex++;
        }
        else if (!_stricmp(p, "dump-block") && argIndex + 1 < argc)
        {
            dumpBlock = atoi(argv[argIndex + 1]);
            argIndex++;
        }
        else if (!_stricmp(p, "flux-byte") && argIndex + 1 < argc)
        {
            fluxByteOffset = atoi(argv[argIndex + 1]);
            argIndex++;
        }
        else if (!_stricmp(p, "flux-window") && argIndex + 1 < argc)
        {
            fluxByteWindow = atoi(argv[argIndex + 1]);
            argIndex++;
        }
        else if (!_stricmp(p, "channels") && argIndex + 2 < argc)
        {
            canChooseChannels = false;
            int ch1 = atoi(argv[argIndex + 1]);
            int ch2 = atoi(argv[argIndex + 2]);
            argIndex += 2;
            argError = ch1 == ch2 || ch1 < 0 || ch1 > 7 || ch2 < 0 || ch2 > 7;
            params.track1Mask = 1 << ch1;
            params.track2Mask = 1 << ch2;
        }
        else if (!_stricmp(p, "freq") && argIndex + 1 < argc)
        {
            int freq = atoi(argv[argIndex + 1]);
            argIndex++;
            argError = freq <= 0;
            params.traceFreq = freq;
        }
        else if (!_stricmp(p, "jpg"))
            saveJpg = true;
        else
            argError = true;
        argIndex++;
    }
    if (argError || argIndex >= argc || argc > argIndex + 2)
    {
        printf("Usage: MdvDecode [<options>] <input_directory> [<output_file>]\n");
        printf("  Possible options (can be omitted to use default values):\n"
               "  -verbose      print additional messages for troubleshooting\n"
               "  -opd          needed to succesfully parse ICL OPD cartridges\n"
               "  -zx           hint that this is a ZX Spectrum cartridge\n"
               "  -channels <track1ch> <track2ch>   specify which logic trace channels contain each track\n"
               "  -freq <frequency>  trace sampling frequency (default: 24 MHz)\n"
               "  -jpg          save a diagnostic .jpg visualization of the decoded blocks\n"
               "  -flux-jpg <chunk_index>       save flux+alignment picture for phase-lock/preamble of that chunk\n"
               "  -flux-byte <byte_offset>      also save mid-block flux picture around that byte of the data block\n"
               "  -flux-window <bytes>          half-window (default 16) of bytes around -flux-byte to show\n"
               "  -dump-block <merged_idx>      dump raw copies of the given merged block to stdout\n");
        return 1;
    }

    string inputDir(argv[argIndex]);
    string outputFile;
    if (argIndex + 1 < argc)
        outputFile = argv[argIndex + 1];
    else
        outputFile = inputDir + ".MDVRAW";
#else
    // Debugging
    //#define FILENAME "E:\\dev\\MdvDecode\\tapezx"
    //#define FILENAME "E:\\dev\\MdvDecode\\ASM1"
    //#define FILENAME "E:\\dev\\MdvDecode\\GstUtil"
    #define FILENAME "E:\\dev\\MdvDecode\\zkul"
    //#define FILENAME "E:\\dev\\MdvDecode\\dragonhold_backup"
    //#define FILENAME "E:\\dev\\MdvDecode\\icl_Basic"
    //#define FILENAME "E:\\dev\\MdvDecode\\icl_demo"
    string inputDir(FILENAME);
    string outputFile(FILENAME ".MDVRAW");
    params.verbose = true;
#endif

    //==========================================================================
    // Load logic trace
    //==========================================================================
    vector<BYTE> data;
    if (!Load(inputDir.c_str(), data))
    {
        printf("ERROR: Cannot open input files at %s\n", inputDir.c_str());
        return 1;
    }
    // TODO: read trace frequency from metadata
    printf("Read %zu bytes (%.2f seconds)\n", data.size(), (float)data.size() / params.traceFreq);

    //==========================================================================
    // Find the logic analyzer channels that contain signals
    //==========================================================================
    if (canChooseChannels)
    {
        if (!FindBestChannels(data, params))
        {
            printf("The logic trace doesn't contain enough channels with valid data\n");
            return 1;
        }
    }

    //ExportTrack(data, params.track1Mask, params.traceFreq, 100000);

    //=======================================================================================================
    // Remove short spikes, measure time between transitions, identify continuous chunks containing no gaps
    //=======================================================================================================
    const int maxSpuriousSize = params.traceFreq / params.mdvFreq / 20;
    RemoveSpurious(data, maxSpuriousSize, params.track1Mask);
    RemoveSpurious(data, maxSpuriousSize, params.track2Mask);
    vector<Chunk> fluxList = FluxChunks(data, (int)(minGapDuration * params.traceFreq), params.track1Mask, params.track2Mask);
    printf("Found %d data chunks\n", max((int)fluxList.size() - 2, 0));

    const int minBlockLen = 6; // 4 preamble + 1 data + 1 checksum for each of the two tracks
    float avgPeriod = (float)params.traceFreq / params.mdvFreq;
    int numFailures = 0;
    int time = fluxList[0].gapLen + fluxList[0].dataLen;
    size_t start = 1;
    if (fluxList[0].gapLen > avgPeriod * 20)
    {
        // Don't discard first chunk if the trace starts with a gap
        start = 0;
        time = 0;
    }

    //==========================================================================
    // Find which of the two tracks is track1 (leads track2 by 4 bits)
    //==========================================================================
    if (canChooseChannels)
    {
        if (ShouldSwapTracks(fluxList, avgPeriod, maxFreqError, minBlockLen))
            SwapTracks(fluxList);
    }

    //============================================================================================================
    // Decode each chunk to a block of bytes, trying to keep the correct alignment with the magnetic transitions
    //============================================================================================================
    vector<Block> allBlocks;
    int extraGap = 0;
    int failByReason[FR__COUNT] = { 0 };
    int nRecoveredTrack2 = 0;
    // Propagate -flux-byte / -flux-window into DecodeBlock via file-scope statics.
    g_fluxByteOffset = fluxByteOffset;
    g_fluxByteWindow = fluxByteWindow > 0 ? fluxByteWindow : 16;
    for (size_t i = start; i < fluxList.size() - 1; i++)    // Ignore first and last chunk as they will usually be truncated
    {
        time += fluxList[i].gapLen;
        //if (fluxList[i].track1.size() > (minBlockLen + 1) * 8 &&
        //    fluxList[i].track2.size() > (minBlockLen + 1) * 8)
        {
            FailReason reason = FR_NONE;
            char dbgTag[32];
            const char* debugTagArg = MakeFluxJpgTag((int)i, fluxJpgChunk, time, params.traceFreq, dbgTag, sizeof(dbgTag));
            bool decoded = DecodeBlocks(fluxList[i], allBlocks, time, &avgPeriod, maxFreqError, minBlockLen, params, &extraGap, minBlockLen, reason, debugTagArg);
            if (!decoded)
            {
                bool longEnough = fluxList[i].track1.size() > (minBlockLen + 1) * 8 &&
                                  fluxList[i].track2.size() > (minBlockLen + 1) * 8;
                if (longEnough)
                {
                    if (params.verbose)
                        printf("Error decoding data chunk #%zu, timestamp %zu uSec (%s)\n",
                            i, Timestamp(time, params.traceFreq), FailReasonName(reason));
                    numFailures++;
                    if (reason >= 0 && reason < FR__COUNT)
                        failByReason[reason]++;
                }
                else
                {
                    failByReason[FR_CHUNK_TOO_SHORT]++;
                }
            }
            else if (reason == FR_PREAMBLE_TRACK2_ONLY)
            {
                nRecoveredTrack2++;
            }
        }

        time += fluxList[i].dataLen;
    }
    if (params.verbose)
    {
        printf("Failure breakdown:\n");
        for (int r = 1; r < FR__COUNT; r++)
            if (failByReason[r])
                printf("  %-40s %d\n", FailReasonName((FailReason)r), failByReason[r]);
        if (nRecoveredTrack2)
            printf("  (recovered %d chunks via track1 preamble fallback)\n", nRecoveredTrack2);
    }

    // Optionally drop blocks without a proper preamble before merging.
    // DetectAndReport uses the same criterion (preamble.size() < 8) to label
    // chunks as "spurious". Dropping them cleans up the merge on tapes with
    // lots of overwritten sectors (e.g. ZX Spectrum captures), but the
    // resulting .MDVRAW is no longer a faithful reproduction of the tape.
    // Kept off by default; enable when debugging the ZX Spectrum path.
#if 0
    {
        size_t before = allBlocks.size();
        int carriedTime = 0;
        size_t out = 0;
        for (size_t in = 0; in < allBlocks.size(); in++)
        {
            Block& b = allBlocks[in];
            if (b.preamble.size() < 8)
            {
                carriedTime += b.gapLen + (b.endTime - b.startTime);
            }
            else
            {
                b.gapLen += carriedTime;
                carriedTime = 0;
                if (out != in)
                    allBlocks[out] = std::move(b);
                out++;
            }
        }
        allBlocks.resize(out);
        if (params.verbose)
            printf("Dropped %zu blocks with short preamble (%zu remain)\n",
                before - allBlocks.size(), allBlocks.size());
    }
#endif

    for (size_t i = 0; i < allBlocks.size(); i++)
        allBlocks[i].dbgId = (int)i;
#ifdef _DEBUG
    //ExportSpeed(allBlocks, params);
#endif

    //==================================================================================================
    // Try to match copies of the same blocks across multiple tape revolutions and merge them together
    //==================================================================================================
    // Phase 2 JPG (+ CSV) is a deep-dive diagnostic — only emit when both
    // -jpg and -verbose are set, so casual -jpg users just get the main
    // block-layout image (from DrawAllBlocks) without extra artifacts.
    string phase2JpgPath;
    bool wantPhase2Jpg = saveJpg && params.verbose;
    if (wantPhase2Jpg)
    {
        phase2JpgPath = outputFile;
        size_t dot = phase2JpgPath.find_last_of('.');
        size_t slash = phase2JpgPath.find_last_of("/\\");
        if (dot != string::npos && (slash == string::npos || dot > slash))
            phase2JpgPath.resize(dot);
        phase2JpgPath += "_phase2.jpg";
    }
    vector<Block> masterBlocks = MergeAllBlocks(allBlocks, time, params, wantPhase2Jpg ? phase2JpgPath.c_str() : nullptr);

    //==========================================================================
    // Guess the OS that formatted the cartridge and print some stats
    //==========================================================================
    int detectedOS = DetectAndReport(masterBlocks, params);

    //==========================================================================
    // OS specific knowledge (optional)
    //==========================================================================
    int firstBlock;
    auto pFileSys = CheckFileSystem(detectedOS, masterBlocks, params, &firstBlock);

    if (firstBlock < 0)
        firstBlock = ChooseFirstSector(masterBlocks, time);

    for (Block& b : allBlocks)
    {
        //b.masterId = (b.masterId + masterBlocks.size() - firstBlock) % masterBlocks.size();
        Block& m = masterBlocks[b.masterId];
        b.isGood = m.isGood;
        b.sectorMapType = m.sectorMapType;
        b.order = (b.masterId + masterBlocks.size() - firstBlock) % masterBlocks.size();
    }

    //==========================================================================
    // Debug output
    //==========================================================================
    if (dumpBlock >= 0 && dumpBlock < (int)masterBlocks.size())
    {
        const Block& mb = masterBlocks[dumpBlock];
        printf("=== dump-block %d: merged size=%zu isGood=%d numCopies=%d ===\n",
            dumpBlock, mb.data.size(), mb.isGood ? 1 : 0, mb.numCopies);
        // Show merged bytes for reference
        printf("merged: ");
        for (size_t k = 0; k < mb.data.size(); k++)
            printf("%02X%s", mb.data[k], ((k + 1) % 32 == 0) ? "\n        " : " ");
        printf("\n");

        // Find raw copies (each has masterId == dumpBlock)
        int revIdx = 0;
        for (const Block& rb : allBlocks)
        {
            if (rb.masterId != dumpBlock) continue;
            printf("copy %d (rawTs=%lldus, size=%zu): ",
                revIdx++, (long long)Timestamp(rb.startTime, params.traceFreq), rb.data.size());
            for (size_t k = 0; k < rb.data.size(); k++)
                printf("%02X%s", rb.data[k], ((k + 1) % 32 == 0) ? "\n        " : " ");
            printf("\n");
        }
    }

    if (saveJpg)
    {
        string jpgPath = outputFile;
        size_t dot = jpgPath.find_last_of('.');
        size_t slash = jpgPath.find_last_of("/\\");
        if (dot != string::npos && (slash == string::npos || dot > slash))
            jpgPath.resize(dot);
        jpgPath += ".jpg";
        DrawAllBlocks(allBlocks, pFileSys.get(), firstBlock, jpgPath.c_str(), params.verbose);
    }

    //==========================================================================
    // Create the resulting MDVRAW file
    //==========================================================================
    Tape tape;
    tape.CreateFromBlocks(masterBlocks, firstBlock, avgPeriod);
    tape.SaveToFile(outputFile.c_str(), params.mdvFreq, detectedOS);

    if (numFailures)
        printf("%d failures\n", numFailures);
    else
        printf("All good!\n");

    return 0;
}