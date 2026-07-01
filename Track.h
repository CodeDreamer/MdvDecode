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

public:
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

        return (n >= 2) ? 1 : 0;
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