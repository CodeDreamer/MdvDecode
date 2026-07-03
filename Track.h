// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#pragma once

class Track
{
private:
    vector<int> track;
    size_t index;
    float currentTime;
    float bitPeriod;
    float expectedTime;
    float midTime;
    int nBits;
    bool validTime;
    // Runtime debug flag: when true, StartPhaseLock / FindPreamble draw the
    // flux + detected bit boundaries to a JPG. Off by default; the caller
    // enables it selectively for a single chunk of interest.
    bool debug = false;
    const char* debugTag = "";

    // Bit-alignment collection for post-read flux pictures around a specific
    // byte offset. When enabled, ReadBit appends expectedTime per bit call so
    // the caller can slice a window and hand it to DrawErrorNamed. Costs one
    // boolean branch per ReadBit; off by default.
    bool collectAlignment = false;
    vector<int> alignmentBuffer;
    vector<int> bitValueBuffer;   // 0/1 read per cell, paired with alignmentBuffer

public:
    void SetDebug(bool d, const char* tag = "") { debug = d; debugTag = tag; }
    void EnableAlignmentCollection(bool b)
    {
        collectAlignment = b;
        if (b) { alignmentBuffer.clear(); bitValueBuffer.clear(); }
    }
    const vector<int>& GetAlignmentBuffer() const { return alignmentBuffer; }
    const vector<int>& GetBitValueBuffer() const { return bitValueBuffer; }
    const vector<int>& GetFlux() const { return track; }
    Track(const vector<int>& t, float startPeriod)
        : track(t)
    {
        Restart(startPeriod);
    }

    void ReplaceFlux(const vector<int>& newFlux)
    {
        track = newFlux;
    }

    void Restart(float startPeriod)
    {
        index = 0;
        currentTime = 0;
        bitPeriod = startPeriod;
        expectedTime = 0;
        nBits = 0;
    }

    bool EndOfTrack()
    {
        return index >= track.size();
    }

    float GetPeriod()
    {
        return bitPeriod;
    }

    void SetPeriod(float p)
    {
        bitPeriod = p;
        expectedTime = currentTime;
        nBits = 0;
    }

    float GetTime()
    {
        return currentTime;
    }

    float GetExpectedTime()
    {
        return expectedTime;
    }

    float GetMidTime()
    {
        return midTime;
    }

    void ResetExpectedTime()
    {
        expectedTime = currentTime;
    }

    void AdjustExpectedTime(float adjust)
    {
        expectedTime += adjust;
    }

    float NextTime()
    {
        EndOfTrack() ? FLT_MAX : track[index] + currentTime;
    }

    void Advance()
    {
        currentTime += track[index++];
    }

    int AdvanceTo(float t)
    {
        int numTransitions = 0;
        while (index < track.size() && abs(track[index] + currentTime - t) < abs(currentTime - t))
        {
            Advance();
            numTransitions++;
        }
        return numTransitions;
    }

    int ReadBit()
    {
        if (collectAlignment)
            alignmentBuffer.push_back((int)(expectedTime + 0.5f));
        int n;
        if (++nBits <= 5)
        {
            // First bits may be unreliable
            n = AdvanceTo(currentTime + bitPeriod);
            expectedTime = currentTime;
            validTime = false;
        }
        else
        {
            float startTime = currentTime;
            expectedTime += bitPeriod;
            n = AdvanceTo(expectedTime);

            float elapsed = currentTime - startTime;
            validTime = elapsed > bitPeriod * 0.7 && elapsed < bitPeriod * 1.3;
            if (validTime)
            {
                bitPeriod = 0.95 * bitPeriod + 0.05 * elapsed;              // Update period
                expectedTime = 0.8 * expectedTime + 0.2 * currentTime;    // Update phase
            }
        }

        int bit = (n >= 2) ? 1 : 0;
        if (collectAlignment)
            bitValueBuffer.push_back(bit);
        return bit;
    }

    bool ReadByte(BYTE& b)
    {
        for (int i = 0; i < 8; i++)
        {
            if (EndOfTrack())
                return false;
            if (i == 4)
                midTime = validTime ? expectedTime : 0;
            b >>= 1;
            b |= ReadBit() << 7;
        }
        return true;
    }

    bool StartPhaseLock(float maxDeviation, int numBytes);

    bool FindPreamble(int numZeros, int numOnes, int maxSearchLen, vector<BYTE>& preamble, int* pStartTime);
};