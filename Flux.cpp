// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include "MdvDecode.h"

void RemoveSpurious(vector<BYTE>& data, int maxSpuriousLen, BYTE trackMask)
{
    size_t lastTriggerTime = 0;
    size_t spuriousStart = 0;
    size_t spuriousEnd = 0;
    int numSpurious = 0;

    for (size_t i = 1; i < data.size(); i++)
    {
        BYTE trigger = (data[i] ^ data[i - 1]) & trackMask;
        if (trigger)
        {
            if (i - lastTriggerTime <= maxSpuriousLen && lastTriggerTime != 0)
            {
                if (!numSpurious)
                    spuriousStart = lastTriggerTime;
                numSpurious++;
                spuriousEnd = i;
            }
            else if (numSpurious)
            {
                BYTE prevValue = (data[spuriousStart] & trackMask) ^ trackMask;
                if (numSpurious & 1)
                {
                    for (size_t j = spuriousStart; j < spuriousEnd; j++)
                        data[j] = (data[j] & ~trackMask) | prevValue;
                }
                else
                {
                    size_t mid = (spuriousEnd + spuriousStart) / 2;
                    for (size_t j = spuriousStart; j < spuriousEnd; j++)
                    {
                        if (j == mid)
                            prevValue = prevValue ^ trackMask;
                        data[j] = (data[j] & ~trackMask) | prevValue;
                    }
                }
                numSpurious = 0;
            }
            lastTriggerTime = i;
        }
    }
}

vector<Chunk> FluxChunks(const vector<BYTE>& data, int minGapLen, BYTE track1Mask, BYTE track2Mask)
{
    vector<Chunk> chunkList;
    Chunk chunk;
    size_t lastCh1 = 0;
    size_t lastCh2 = 0;
    size_t last = 0;
    size_t chunkStart = 0;
    BYTE prev = data[0];
    bool firstEvent = true;
    chunk.gapLen = 0;
    BYTE tracksMask = track1Mask | track2Mask;

    for (size_t i = 0; i < data.size(); i++)
    {
        BYTE b = data[i];
        BYTE trigger = (b ^ prev) & tracksMask;
        prev = b;
        if (trigger)
        {
            if (firstEvent || i - last >= minGapLen)
            {
                // We look for gaps present in both tracks, but maybe the ZX8302 only uses track1?

                if (!chunk.track1.empty() || !chunk.track2.empty())
                {
                    chunk.dataLen = last - chunkStart;
                    chunkList.push_back(chunk);
                    chunk.gapLen = 0;
                }
                else
                {
                    // Example: two gaps with single isolated transition in-between
                    // Gap len will be added
                    int xx = 0;
                }

                chunk.gapLen += i - last;
                chunkStart = i;
                last = lastCh1 = lastCh2 = i;
                chunk.track1.clear();
                chunk.track2.clear();
                firstEvent = false;
            }
            else
            {
                last = i;
                if (trigger & track1Mask)
                {
                    chunk.track1.push_back(i - lastCh1);
                    lastCh1 = last;
                }
                if (trigger & track2Mask)
                {
                    chunk.track2.push_back(i - lastCh2);
                    lastCh2 = last;
                }
            }
        }
    }

    if (!chunk.track1.empty() || !chunk.track2.empty())
    {
        chunk.dataLen = data.size() - chunkStart;
        chunkList.push_back(chunk);
    }
    if (data.size() - last >= minGapLen)
    {
        chunk.gapLen = data.size() - last;
        chunk.dataLen = 0;
        chunk.track1.clear();
        chunk.track2.clear();
        chunkList.push_back(chunk);
    }

    return chunkList;
}