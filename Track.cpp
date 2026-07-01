// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include "MdvDecode.h"
#include "Track.h"


bool Track::StartPhaseLock(float maxDeviation, int minNumBytes)
{
    const int period = (int)(bitPeriod + 0.5f);
    const int minPeriod = (int)(bitPeriod * (1.0f - maxDeviation) + 0.5f);
    const int maxPeriod = (int)(bitPeriod * (1.0f + maxDeviation) + 0.5f);
    const int numBits = minNumBytes * 8;

    if (track.size() < numBits)
        return false;

#ifdef _DEBUG
    vector<int> currentAlignment;
    vector<int> bestAlignment;
#endif

    __int64 bestError = INT64_MAX;
    vector<bool> visitedStart(10);
    size_t offset = index;
    int offsetTime = (int)currentTime;

    for (size_t k = 0; k < visitedStart.size(); k++)
    {
        if (visitedStart[k])
            continue;

#ifdef _DEBUG
        currentAlignment.clear();
        currentAlignment.push_back(0);
#endif

        size_t startIndex = offset;
        int time = offsetTime;
        while (startIndex < offset + k)
            time += track[startIndex++];

        __int64 totError = 0;
        int lastTime = time;
        int startTime = time;
        int n = 0;
        size_t i = startIndex + 1;
        while (i < track.size())
        {
            int newTime = time + track[i - 1];
            int duration = newTime - lastTime;
            if (abs(duration + track[i] - period) < abs(duration - period) || duration == 0)
            {
                // We can get closer to the target duration by moving ahead
                i++;
                time = newTime;
            }
            else if (duration >= minPeriod && duration <= maxPeriod)
            {
                // Closest duration looks sensible

                n++;
                lastTime = newTime;
                if (i < visitedStart.size())
                    visitedStart[i] = true;
                __int64 err = duration - period;
                totError += err * err;
#ifdef _DEBUG
                currentAlignment.push_back(lastTime);
                if (currentAlignment.size() > bestAlignment.size())
                    bestAlignment = currentAlignment;
#endif
                if (n >= numBits)
                {
                    if (totError < bestError)
                    {
                        bestError = totError;
                        index = startIndex;
                        currentTime = startTime;
                        bitPeriod = (float)(lastTime - startTime) / n;
                    }
                    break;
                }
                i++;
                time = newTime;
            }
            else
            {
                // Incorrect duration; Try starting later
                n = 0;
                startTime += track[startIndex++];
                lastTime = startTime;
#ifdef _DEBUG
                currentAlignment.clear();
                totError = 0;
#endif
            }
        }
    }

    if (bestError != INT64_MAX)
        return true;
#if 0
    if (bestAlignment.size() < 100)
        return false;

    int leftAnchor = (bestAlignment[5] + bestAlignment[6]) / 2;
    int rightAnchor = (bestAlignment[98] + bestAlignment[99]) / 2;
    int anchorDiff = 98 - 5;
    double duration = (double)(rightAnchor - leftAnchor) / anchorDiff;
    bestAlignment.clear();
    for (int k = -4; k < 100; k++)
        bestAlignment.push_back((int)(((k + 0.5) * duration) + leftAnchor + 0.5));
#endif
#ifdef _DEBUG
    DrawError(track, bestAlignment);
#endif

    return false;
}

BYTE bit_reverse(BYTE b)
{
    BYTE result = 0;
    BYTE mask = 128;
    while (b)
    {
        if (b & 1)
            result |= mask;
        b >>= 1;
        mask >>= 1;
    }
    return result;
}

bool Track::FindPreamble(int numZeros, int numOnes, int maxSearchLen, vector<BYTE>& preamble, int* pStartTime)
{
    numZeros <<= 3;
    numOnes <<= 3;
    maxSearchLen <<= 3;
    preamble.clear();
    float timeOrigin = currentTime;

#ifdef _DEBUG
    vector<int> currentAlignment;
    vector<int> bestAlignment;
#endif

    BYTE b = 0;
    int n = 0;
    int numBitsRead = 0;
    int bitStartTime[8];
    bool found = false;
    while (!found && !EndOfTrack())
    {
        while (!EndOfTrack())
        {
            if ((numBitsRead & 7) == 0 && numBitsRead > 0)
            {
                preamble.push_back(b);
                b = 0;
            }

#ifdef _DEBUG
            currentAlignment.push_back((int)(expectedTime + 0.5));
#endif
            if (numBitsRead < 8)
                bitStartTime[numBitsRead] = (int)(currentTime - timeOrigin + 0.5f);

            numBitsRead++;
            b <<= 1;
            if (ReadBit())
            {
                b |= 1;
                break;
            }
            else
            {
                n++;
            }
        }

        if (n >= numZeros)
        {
            n = 1;
            while (!EndOfTrack() && !found)
            {
#ifdef _DEBUG
                currentAlignment.push_back((int)(expectedTime + 0.5));
#endif

                numBitsRead++;
                b <<= 1;
                if (!ReadBit())
                {
                    break;
                }
                else
                {
                    n++;
                    b |= 1;
                    found = n == numOnes && numBitsRead <= maxSearchLen;
                }
                if ((numBitsRead & 7) == 0)
                {
                    preamble.push_back(b);
                    b = 0;
                }
            }
        }
        n = 1;

#ifdef _DEBUG
        if (!found && numBitsRead < maxSearchLen)
        {
            if (currentAlignment.size() > bestAlignment.size())
                bestAlignment = currentAlignment;
            currentAlignment.clear();
        }
#endif
    }

#ifdef _DEBUG
    if (!found)
        DrawError(track, bestAlignment);
#endif
    //preamble.clear();
    //*pStartTime = 0;


    int numPendingBits = numBitsRead & 7;
    if (numPendingBits)
    {
        // Re-align using correct byte boundary
        int complement = 8 - numPendingBits;
        size_t last = preamble.size() - 1;
        for (size_t i = 0; i < last; i++)
            preamble[i] = (preamble[i] << numPendingBits) | (preamble[i + 1] >> complement);
        preamble[last] = (preamble[last] << numPendingBits) | b;
    }
    *pStartTime = bitStartTime[numPendingBits];

    for (size_t i = 0; i < preamble.size(); i++)
        preamble[i] = bit_reverse(preamble[i]);

    return found;
}