// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include "MdvDecode.h"


int EditDistance(const vector<BYTE>& s1, const vector<BYTE>& s2)
{
    size_t m = s1.size();
    size_t n = s2.size();

    // Create a table to store results of subproblems
    vector<vector<int>> dp(m + 1, vector<int>(n + 1));

    // Fill the known entries in dp[][]
    // If one string is empty, then answer 
    // is length of the other string
    for (size_t i = 0; i <= m; i++)
        dp[i][0] = i;
    for (size_t j = 0; j <= n; j++)
        dp[0][j] = j;

    // Fill the rest of dp[][]
    for (size_t i = 1; i <= m; i++)
    {
        for (size_t j = 1; j <= n; j++)
        {
            if (s1[i - 1] == s2[j - 1])
                dp[i][j] = dp[i - 1][j - 1];
            else
                dp[i][j] = 1 + min({ dp[i][j - 1],
                                 dp[i - 1][j],
                                 dp[i - 1][j - 1] });
        }
    }

    return dp[m][n];
}

size_t block_hash(std::vector<BYTE> const& vec)
{
    size_t seed = vec.size();
    for (auto v : vec)
    {
        unsigned int x = v;
        x = ((x >> 16) ^ x) * 0x45d9f3b;
        x = ((x >> 16) ^ x) * 0x45d9f3b;
        x = (x >> 16) ^ x;
        seed ^= x + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
}

int DistanceDiff(const vector<Block>& blockList, int i, int j, int offset)
{
    return abs(blockList[j].startTime - blockList[i].startTime - offset);
}

void CheckAligned(vector<Block>& blockList, size_t i, size_t j, int& offset)
{
    if (blockList[i].nextLoopIndex >= 0 || blockList[j].previousLoopIndex != -1)
        return;

    int rangeMin = (__int64)offset * 96 / 100;
    int rangeMax = (__int64)offset * 104 / 100;
    int newOffset = blockList[j].startTime - blockList[i].startTime;
    if (newOffset >= rangeMin && newOffset <= rangeMax)
    {
        blockList[i].nextLoopIndex = j;
        blockList[j].previousLoopIndex = i;
        offset = (offset * 3 + newOffset + 2) / 4;
    }
}

void MatchForward(vector<Block>& blockList, size_t i, size_t j, int offset)
{
    while (++j < blockList.size() - 1)
    {
        if (blockList[i].nextLoopIndex >= 0)
            i++;

        bool updated = true;
        while (updated)
        {
            updated = false;
            if (i + 1 < blockList.size() && DistanceDiff(blockList, i + 1, j, offset) < DistanceDiff(blockList, i, j, offset))
            {
                i++;
                updated = true;
            }
            if (j + 1 < blockList.size() && DistanceDiff(blockList, i, j + 1, offset) < DistanceDiff(blockList, i, j, offset))
            {
                j++;
                updated = true;
            }
        }

        CheckAligned(blockList, i, j, offset);
    }
}

struct Connection
{
    int distance;
    int src;
    int dest;

    bool operator<(const Connection& c)
    {
        return distance < c.distance;
    }
};

int DistanceDiff(const vector<Block>& blockList, int i, int j, int iTimeRef, int jTimeRef)
{
    int iDelta = blockList[i].startTime - iTimeRef;
    int jDelta = blockList[j].startTime - jTimeRef;
    return abs(iDelta - jDelta);
}

void MatchForward(const vector<Block>& blockList, int iMin, int iMax, int jMin, int jMax, int iTimeRef, int jTimeRef, vector<Connection>& connectionList)
{
    int i = iMin;
    int j = jMin;
    while (i <= iMax && j <= jMax)
    {
        bool updated = true;
        while (updated)
        {
            updated = false;
            if (i + 1 <= iMax && DistanceDiff(blockList, i + 1, j, iTimeRef, jTimeRef) < DistanceDiff(blockList, i, j, iTimeRef, jTimeRef))
            {
                i++;
                updated = true;
            }
            if (j + 1 < jMax && DistanceDiff(blockList, i, j + 1, iTimeRef, jTimeRef) < DistanceDiff(blockList, i, j, iTimeRef, jTimeRef))
            {
                j++;
                updated = true;
            }
        }
        connectionList.push_back({ DistanceDiff(blockList, i, j, iTimeRef, jTimeRef), i, j });
        i++;
        j++;
    }
}


void MatchBackward(vector<Block>& blockList, size_t i, size_t j, int offset)
{
    while (i-- > 1)
    {
        if (j > 0 && blockList[j].previousLoopIndex >= 0)
            j--;

        bool updated = true;
        while (updated)
        {
            updated = false;
            if (i >= 1 && DistanceDiff(blockList, i - 1, j, offset) < DistanceDiff(blockList, i, j, offset))
            {
                i--;
                updated = true;
            }
            if (j >= 1 && DistanceDiff(blockList, i, j - 1, offset) < DistanceDiff(blockList, i, j, offset))
            {
                j--;
                updated = true;
            }
        }

        CheckAligned(blockList, i, j, offset);
    }
}

Block MergeSameBlock(vector<Block>& blockList, int blockNum, int masterId, const Params& params)
{
    Block result;
    result.gapLen = blockList[blockNum].gapLen;
    result.startTime = blockList[blockNum].startTime;
    result.endTime = blockList[blockNum].endTime;
    vector<Block*> copyList;
    vector<int> gapList;
    int len = 0;
    int i = blockNum;
    do
    {
        len = max(len, (int)blockList[i].data.size());
        copyList.push_back(&blockList[i]);
        gapList.push_back(blockList[i].gapLen);
        if (!blockList[i].hasNext)
            break;
        i = blockList[i].nextLoopIndex;
    } while (i >= 0);

    sort(gapList.begin(), gapList.end());
    result.gapLen = gapList[gapList.size() / 2];    // Possible issue: short and long gaps when block is present only in some loop reads of the tape

    unordered_map<BYTE, int> byteCount;
    result.data.reserve(len);
    int minBestCount = INT_MAX;
    int firstError = -1;
    for (int k = 0; k < len; k++)
    {
        byteCount.clear();
        int bestCount = 0;
        BYTE bestByte = 0;
        for (Block* pBlock : copyList)
        {
            if (k < pBlock->data.size())
            {
                BYTE b = pBlock->data[k];
                int count = ++byteCount[b];
                if (count > bestCount)
                {
                    bestCount = count;
                    bestByte = b;
                }
            }
        }

        result.data.push_back(bestByte);
        if (bestCount == 1 && minBestCount > 1)
            firstError = k;
        minBestCount = min(minBestCount, bestCount);
    }

    if (minBestCount <= 1 && params.verbose)
    {
        int timestamp = Timestamp(blockList[blockNum].startTime, params.traceFreq);
        printf("timestamp %zu uSec + %d bytes\n", timestamp, firstError);
        printf("Possibly bad:");
        i = blockNum;
        do
        {
            printf(" %d (size=%d)", blockList[i].dbgId, blockList[i].data.size());
            i = blockList[i].nextLoopIndex;
        } while (i >= 0);
        printf("\n");
    }

    result.numCopies = minBestCount;
    result.preamble = copyList[0]->preamble;

    int mergeQuality = (minBestCount == copyList.size()) ? MQ_PERFECT : MQ_BAD; // Do all copies match?
    for (Block* pBlock : copyList)
    {
        pBlock->masterId = masterId;
        pBlock->mergeQuality = mergeQuality;
    }
    if (mergeQuality == MQ_BAD && minBestCount >= 2)
    {
        for (Block* pBlock : copyList)
            if (pBlock->data == result.data)
                pBlock->mergeQuality = MQ_OK;
    }

    return result;
}

int MakeConnections(vector<Block>& blockList, vector<Connection>& connectionList, int connectionType, bool checkCrossings = true)
{
    sort(connectionList.begin(), connectionList.end());
    int numConnections = 0;
    for (const Connection& c : connectionList)
    {
        if (!blockList[c.src].hasNext && blockList[c.dest].previousLoopIndex == -1)
        {
            // Don't 'cross' nearby connections
            bool crossing = false;
            if (checkCrossings)
            {
                for (int j = c.src - 1; j >= c.src - 10 && !crossing && j >= 0; j--)
                    if (blockList[j].hasNext && blockList[j].nextLoopIndex > c.dest)
                        crossing = true;
                for (int j = c.src + 1; j <= c.src + 10 && !crossing && j < blockList.size(); j++)
                    if (blockList[j].hasNext && blockList[j].nextLoopIndex < c.dest)
                        crossing = true;
            }
            if (!crossing)
            {
                blockList[c.src].nextLoopIndex = c.dest;
                blockList[c.src].hasNext = true;
                blockList[c.src].nextType = connectionType;
                blockList[c.dest].previousLoopIndex = c.src;
                numConnections++;
            }
        }
    }
    return numConnections;
}

#if 1
// Debugging
void PrintConnections(const vector<Block>& blockList)
{
    for (size_t i = 0; i < blockList.size(); i++)
    {
        if (!blockList[i].hasNext && blockList[i].nextLoopIndex > 0)
            printf("%zu -> -1 (%d)   (size %zu)\n", i, blockList[i].nextLoopIndex, blockList[i].data.size());
        else
            printf("%zu -> %d          (size %zu)\n", i, blockList[i].nextLoopIndex, blockList[i].data.size());
        if (blockList[i].nextLoopIndex == -1 && i + 1 < blockList.size() && blockList[i + 1].nextLoopIndex > 0)
        {
            const vector<BYTE>& data1 = blockList[i].data;
            const vector<BYTE>& data2 = blockList[blockList[i + 1].nextLoopIndex - 1].data;
            printf("  Hash: %zu -> %zu\n", block_hash(data1), block_hash(data2));
        }
    }
}
#endif

size_t size_diff(size_t a, size_t b)
{
    if (a >= b)
        return a - b;
    return b - a;
}

struct UniqueBlock
{
    int startTime;
    int endTime;
    int firstBlockIndex;
    Block* pFirstBlock;
    Block* pLastBlock;

    UniqueBlock(int i, Block *pBlock)
    {
        startTime = pBlock->startTime;
        endTime = pBlock->endTime;
        pFirstBlock = pBlock;
        pLastBlock = pBlock;
        firstBlockIndex = i;
    }

    void AddBlock(int i, Block* pBlock, bool verbose)
    {
        if (verbose)
            printf("%d (%d) => %d (%d)\n", pLastBlock->dbgId, pLastBlock->data.size(), i, pBlock->data.size());
        pLastBlock->nextLoopIndex = i;
        pLastBlock->hasNext = true;
        pLastBlock = pBlock;
    }
    
    bool operator<(const UniqueBlock& b)
    {
        return startTime < b.startTime;
    }

    bool Overlaps(const UniqueBlock& b)
    {
        int overlap = min(endTime, b.endTime) - max(startTime, b.startTime);
        if (overlap <= 0)
            return false;
        int un = max(endTime, b.endTime) - min(startTime, b.startTime);
        return overlap * 100 / un >= 20;
    }
};

vector<Block> MergeAllBlocks(vector<Block>& blockList, int totalTime, const Params& params)
{
    // Assume a tape needs to be between 5 and 9 seconds long
    // TODO: account for different motor speed of QL vs Interface 1
    int minDuration = params.traceFreq * 5;
    int maxDuration = params.traceFreq * 9;

    unordered_map<size_t, int> prevIndexMap;
    vector<int> offsetList;
    unordered_map<size_t, int> blockCount;
    vector<size_t> savedHash;
    for (size_t i = 0; i < blockList.size(); i++)
    {
        Block& block = blockList[i];
        block.nextLoopIndex = -1;
        size_t h = block_hash(block.data);
        auto it = prevIndexMap.find(h);
        if (it != prevIndexMap.end())
        {
#ifdef _DEBUG
            if (block.data != blockList[it->second].data)
                printf("Error: Hash failure!\n");
            //printf("Size %zu dist %d\n", block.data.size(), block.startTime - blockList[it->second].startTime);
#endif
            blockList[it->second].nextLoopIndex = i;
            int delta = block.startTime - blockList[it->second].startTime;
            if (delta >= minDuration && delta <= maxDuration)
            {
                offsetList.push_back(delta);
            }
        }
        prevIndexMap[h] = i;
        blockCount[h]++;
        savedHash.push_back(h);
    }

    vector<Block> result;
    if (offsetList.empty())
    {
        printf("There are no repeating blocks!!\n");
        return result;
    }
    sort(offsetList.begin(), offsetList.end());
    int offset = offsetList[offsetList.size() / 2];
    int numLoops = totalTime / offset + 1;
    printf("%.1f copies of the tape found, %.1f seconds each\n", (float)totalTime / offset, (float)offset / params.traceFreq);


    for (Block& block : blockList)
    {
        block.previousLoopIndex = -1;
        block.hasNext = false;
        block.hasSimilarNearby = false;
        block.masterId = -1;
        block.sectorMapType = SMT_UNKNOWN;
        block.isGood = true;
        block.mergeQuality = MQ_BAD;
        block.nextType = NT_OTHER;
    }

    const float MAX_OFFSET_VARIATION = 0.2f;
    int maxOffsetMargin = (int)(offset * MAX_OFFSET_VARIATION);
    // Mark blocks that could be confused with nearby blocks
    for (size_t i = 0; i < blockList.size(); i++)
    {
        Block& block = blockList[i];
        if (block.nextLoopIndex > 0)
        {
            Block& bNext = blockList[block.nextLoopIndex];
            int distance = bNext.startTime - block.startTime;
            if (distance <= maxOffsetMargin)
            {
                block.hasSimilarNearby = true;
                bNext.hasSimilarNearby = true;
            }
        }
    }

    // Exact matches with no nearby alternatives
    vector<Connection> connectionList;
    for (size_t i = 0; i < blockList.size(); i++)
    {
        Block& block = blockList[i];
        if (block.nextLoopIndex > 0 && !block.hasSimilarNearby)
        {
            Block& bNext = blockList[block.nextLoopIndex];
            int distance = abs(bNext.startTime - block.startTime - offset);
            if (distance <= maxOffsetMargin && !bNext.hasSimilarNearby)
                connectionList.push_back({ distance, (int)i, block.nextLoopIndex });
        }
    }
    int numNewConnections = MakeConnections(blockList, connectionList, NT_STRONG);
    if (params.verbose)
        printf("Added %d unambiguous good connections\n", numNewConnections);
    //PrintConnections(blockList);

    // Other exact matches at the correct distance, and across an increasing number of loops
#if 1
    for (int loop = 1; loop <= 1; loop++)
    {
        int multiLoopOffset = offset * loop;
        connectionList.clear();
        for (size_t i = 0; i < blockList.size(); i++)
        {
            Block& block = blockList[i];
            if (!block.hasNext)
            {
                int next = block.nextLoopIndex;
                while (next > 0)
                {
                    const Block& bNext = blockList[next];
                    int distance = abs(bNext.startTime - block.startTime - multiLoopOffset);
                    if (distance <= maxOffsetMargin && bNext.previousLoopIndex == -1)
                        connectionList.push_back({ distance, (int)i, next });
                    next = bNext.nextLoopIndex;
                }
            }
        }
        numNewConnections = MakeConnections(blockList, connectionList, NT_OTHER, loop == 1);
        if (params.verbose && numNewConnections)
            printf("Added %d good connections at a distance of %d tape loops\n", numNewConnections, loop);
    }
#endif

    if (params.verbose)
        PrintConnections(blockList);


    size_t top = blockList.size();
    while (top >= 100 && !blockList[top - 1].hasNext)
        top--;

    // Finally create master list of all unique blocks and place any remaing unmatched blocks
    //unordered_map<int, int> previous;
    vector<UniqueBlock> masterList;
    int iFirst = 0;
    while (iFirst < blockList.size() && !blockList[iFirst].hasNext)
        iFirst++;
    if (iFirst < blockList.size())
    {
        int j = blockList[iFirst].nextLoopIndex;
        for (int i = iFirst; i < j; i++)
            masterList.emplace_back(UniqueBlock(i, &blockList[i]));
        masterList.emplace_back(UniqueBlock(j, &blockList[j]));    // Special ending block to simplify wrap-around logic
        int loopDistance = blockList[j].startTime - blockList[iFirst].startTime;
        for (; j < blockList.size(); j++)
        {
            //Block& block = blockList[j];
            UniqueBlock jBlock(j, &blockList[j]);
            jBlock.startTime -= loopDistance;
            jBlock.endTime -= loopDistance;
            int numSearches = 2;
            int k = -1;
            while (--numSearches >= 0)
            {
                k = lower_bound(masterList.begin(), masterList.end(), jBlock) - masterList.begin();
                //[&blockList](const UniqueBlock& x, const UniqueBlock& y) { return x.startTime < y.startTime;}) - masterList.begin();
                int end = masterList.size();
                if (k == end || (k == end - 1 && !(jBlock.Overlaps(masterList[end - 2]))))
                {
                    // Wrap around
                    int extraDistance = masterList.back().startTime - masterList.front().startTime;
                    loopDistance += extraDistance;
                    jBlock.startTime -= extraDistance;
                    jBlock.endTime -= extraDistance;
                }
                else
                    break;
            }
            if (k == 0)
                k = 1;
            int previous = k - 1;
            int next = k;
            if (k > 0 && jBlock.startTime - masterList[k - 1].startTime < masterList[k].startTime - jBlock.startTime)
                k--;

            // TODO: a very small block like the OPD block header could appear to be after the following block due to misalignment
            bool bOverlap = true;
            if (blockList[j].previousLoopIndex != -1 && masterList[k].pLastBlock->hasNext && masterList[k].pLastBlock->nextLoopIndex == j)
            {
                // k already likely to be correct
            }
            else if (jBlock.Overlaps(masterList[previous]))
                k = previous;
            else if (jBlock.Overlaps(masterList[next]))
                k = next;
            else if (true)
            {
                if (params.verbose)
                    printf("Insert %d [%d - %d] between %d [%d - %d] and %d [%d - %d]\n",
                        j, jBlock.startTime / 1000, jBlock.endTime / 1000,
                        masterList[previous].pLastBlock->dbgId,
                        masterList[previous].startTime / 1000, masterList[previous].endTime / 1000,
                        masterList[next].pLastBlock->dbgId,
                        masterList[next].startTime / 1000, masterList[next].endTime / 1000);
                // In between or with only small overlap
                // Insert block in between
                // if(jBlock.startTime > masterList[previous].endTime && jBlock.endTime < masterList[next].startTime)
                masterList.insert(masterList.begin() + next, jBlock);
                blockList[j].gapLen = jBlock.startTime - masterList[previous].endTime;
                masterList[next].pFirstBlock->gapLen = masterList[next].startTime - jBlock.endTime;
                bOverlap = false;
            }
            if (bOverlap)
            {
                int catchupFactor = (blockList[j].previousLoopIndex >= 0 && masterList[k].pLastBlock->nextLoopIndex == j) ? 2 : 10;
                loopDistance += (jBlock.startTime - masterList[k].startTime) / catchupFactor;
                masterList[k].AddBlock(j, &blockList[j], params.verbose);
            }
        }
    }
    masterList.pop_back();  // Remove the extra block at the end


#if 0
    // Non-exact matches in-between pairs of exact matches
    connectionList.clear();
    int lastMatch = -1;
    for (size_t i = 0; i < top - 1; i++)
    {
        Block& block = blockList[i];
        if (block.hasNext)
            lastMatch = i;
        else if (lastMatch >= 0)
        {
            int iFirst = i;
            while (i + 1 < top && !blockList[i + 1].hasNext)
                i++;
            if (i < top - 1)
            {
                int iNextFirst = blockList[lastMatch].nextLoopIndex + 1;
                int iNextLast = blockList[i + 1].nextLoopIndex - 1;
                if (iNextFirst <= iNextLast)
                {
                    // Match based on time offset
                    MatchForward(blockList,
                        iFirst, i, iNextFirst, iNextLast,
                        blockList[lastMatch].startTime,
                        blockList[iNextFirst - 1].startTime,
                        connectionList);
                }
            }
        }
    }
    numNewConnections = MakeConnections(blockList, connectionList, NT_OTHER, false);
    if (params.verbose && numNewConnections)
        printf("Added %d sandwitched connections\n", numNewConnections);
#endif
#if 0
    // Adjust next loop pointers
    for (Block& block : blockList)
    {
        int next = block.nextLoopIndex;
        if (next > 0)
        {
            int currentDelta = blockList[next].startTime - block.startTime;
            while (blockList[next].nextLoopIndex > 0)
            {
                int skipNext = blockList[next].nextLoopIndex;
                int delta = blockList[skipNext].startTime - block.startTime;
                if (abs(delta - offset) >= abs(currentDelta - offset))
                    break;
                next = skipNext;
                currentDelta = delta;
            }
            block.nextLoopIndex = next;
        }
    }
#endif
#if 0
    // Find location of median value
    int i = 0, j = 0;
    for (i = 0; i < blockList.size(); i++)
    {
        j = blockList[i].nextLoopIndex;
        if (j >= 0 && blockList[j].startTime - blockList[i].startTime == offset)
            break;
    }
#endif
#if 0
    for (Block& block : blockList)
    {
        block.nextLoopIndex = -1;
        block.previousLoopIndex = -1;
    }
    blockList[i].nextLoopIndex = j;

    MatchForward(blockList, i, j, offset);
    MatchBackward(blockList, i, j, offset);
#endif
#if 0
    PrintConnections(blockList);
    if (params.verbose)
    {
        int numNotConnected = 0;
        for (size_t i = 1; i < top - 1; i++)
            if (!blockList[i].hasNext)
                numNotConnected++;
        if (numNotConnected)
            printf("%d blocks not connected to next loop\n", numNotConnected);
        else
            printf("All blocks have been connected to next loop!\n");
    }
#endif

#if 0
    int first = blockList[0].nextLoopIndex >= 0 ? 0 : 1;
    int i = first;
    while (i < blockList.size() && blockList[i].previousLoopIndex == -1)
    {
        Block merged = MergeSameBlock(blockList, i);
        result.push_back(merged);
        i++;
    }
    int last = i - 1;
#endif

    for (const UniqueBlock& ub : masterList)
    {
        Block merged = MergeSameBlock(blockList, ub.firstBlockIndex, result.size(), params);
        result.push_back(merged);
    }

    return result;
}