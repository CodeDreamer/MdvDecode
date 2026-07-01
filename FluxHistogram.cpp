// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include "MdvDecode.h"

void RunningAverage(vector<int>& data)
{
    int saved = data[0];
    for (int i = 0; i < data.size() - 1; i++)
    {
        int newVal = saved + data[i] + data[i + 1];
        saved = data[i];
        data[i] = newVal;
    }
}

bool ComputeCentroid(const vector<int>& hist, int from, int to, int& centroid)
{
    __int64 sum1 = 0;
    __int64 sum2 = 0;
    for (int x = from; x <= to; x++)
    {
        sum1 += x * hist[x];
        sum2 += hist[x];
    }
    if (sum2 == 0)
        return false;
    centroid = (sum1 + sum2 / 2 + 1) / sum2;
    return true;
}

HistResult ProcessHist(const vector<int>& hist, int theoreticalPeriod)
{
    vector<int> origHist = hist;

    int x1 = std::max_element(hist.begin() + 3 * theoreticalPeriod / 16, hist.begin() + theoreticalPeriod * 11 / 16) - hist.begin();
    int x2 = std::max_element(hist.begin() + 13 * theoreticalPeriod / 16, hist.begin() + theoreticalPeriod * 23 / 16) - hist.begin();

    // Now find minimum in-between
    int firstMin = x1;
    int mid = x1;
    int minValue = hist[x1];
    int maxLen = 1;

    for (int x = x1 + 1; x < x2; x++)
    {
        int v = hist[x];
        if (v < minValue)
        {
            minValue = v;
            firstMin = x;
            mid = x;
            maxLen = 1;
        }
        else if (v == minValue)
        {
            int len = 1 + x - firstMin;
            if (len > maxLen)
            {
                maxLen = len;
                mid = (x + firstMin + 1) / 2;
            }
        }
        else
        {
            firstMin = x + 1;
        }
    }

    // Experiment: approximate centroid:
    int delta = 3 * theoreticalPeriod / 16;
    int centroid;
    if (!ComputeCentroid(origHist, x2 - delta, x2 + delta, centroid))
        return { (float)theoreticalPeriod, INT_MAX, 0.5f * theoreticalPeriod, mid };
    int lowCentroid;
    if (!ComputeCentroid(origHist, max(1, x1 - delta), x1 + delta, lowCentroid))
        lowCentroid = 0;
    //printf("%d, %d, %d, %d, %d\n", x1, x2, centroid, mid, hist[mid]);

    return { (float)centroid, hist[mid], (float)lowCentroid, mid };
}

HistResult MergeHistResult(HistResult hr1, HistResult hr2)
{
    float result;
    if (hr1.quality < hr2.quality)
        result = hr1.period;
    else if (hr2.quality < hr1.quality)
        result = hr2.period;
    else
        result = (hr1.period + hr2.period) * 0.5f;
    int quality = min(hr1.quality, hr2.quality);
    return { result, quality };
}


HistResult DoHistogramAndImproveFlux(const vector<int>& flux, vector<int>& result)
{
    static int count = 0;

    const int NUM_BIN = 500;
    vector<int> histEven(NUM_BIN);
    vector<int> histOdd(NUM_BIN);
    result.clear();

    // Note: skip first sample
    for (size_t i = 1; i < flux.size(); i++)
    {
        int f = flux[i];
        f = min(f, NUM_BIN - 1);
        if (i & 1)
            histOdd[f]++;
        else
            histEven[f]++;
    }

    for (int i = 0; i < 10; i++)
    {
        RunningAverage(histOdd);
        RunningAverage(histEven);
    }

    //printf("Block %d odd: ", count / 2);
    HistResult hrOdd = ProcessHist(histOdd, 240);
    //printf("Block %d even: ", count / 2);
    HistResult hrEven = ProcessHist(histEven, 240);

    count++;

    if (hrOdd.quality != INT_MAX && hrOdd.shortPeriod != 0 &&
        hrEven.quality != INT_MAX && hrEven.shortPeriod)
    {
        result = RegularizeFlux(flux, hrEven, hrOdd);
    }

    return MergeHistResult(hrOdd, hrEven);
}


enum FluxLen
{
    FLUX_TOO_SHORT,
    FLUX_SHORT,
    FLUX_MID,
    FLUX_LONG,
    FLUX_TOO_LONG
};

class FluxClassifier
{
    int tooLowThreshold;
    int lowThreshold;
    int highThreshold;
    int tooHighThreshold;

public:
    FluxClassifier(const HistResult& hr)
    {
        float mid = (hr.period + hr.shortPeriod) * 0.5;
        mid = (mid + hr.mid) * 0.5;
        lowThreshold = (int)((mid * 2 + hr.shortPeriod) / 3 + 0.5);
        highThreshold = (int)((mid * 2 + hr.period) / 3 + 0.5);
        tooLowThreshold = hr.shortPeriod * 3 / 4;
        tooHighThreshold = hr.period * 5 / 4;
    }

    FluxLen Classify(int flux)
    {
        if (flux <= lowThreshold)
            return flux < tooLowThreshold ? FLUX_TOO_SHORT : FLUX_SHORT;
        if (flux >= highThreshold)
            return flux > tooHighThreshold ? FLUX_TOO_LONG : FLUX_LONG;
        return FLUX_MID;
    }
};

void RegularizeContiguous(vector<int>& flux, vector<FluxLen>& fluxLen, size_t start, size_t end, bool isLowQuality)
{
    int totMid = 0;
    for (size_t i = start; i <= end; i++)
    {
        int numMid = 0;
        int numShort = 0;
        int lastMid;
        while (i <= end && fluxLen[i] < FLUX_LONG)
        {
            if (fluxLen[i] <= FLUX_SHORT)
                numShort++;
            else
            {
                lastMid = i;
                numMid++;
            }
            i++;
        }

        if (numMid == 1 && i != end)
        {
            fluxLen[lastMid] = (numShort & 1) ? FLUX_SHORT : FLUX_LONG;    // short pulses come in pairs
        }
        else
            totMid += numMid;
    }


    if (totMid || isLowQuality)
    {
        // TODO: partially equalize durations
        int xx = 1;
    }
    else
    {
        // Completely rewrite flux using average durations
        __int64 duration = 0;
        int count[5] = { 0 };
        for (size_t i = start; i <= end; i++)
        {
            count[fluxLen[i]]++;
            duration += flux[i];
        }
        int totCount = count[FLUX_SHORT] + count[FLUX_TOO_SHORT] + 2 * (count[FLUX_LONG] + count[FLUX_TOO_LONG]);
        int halfTotCount = totCount / 2;
        __int64 lastTime = 0;
        int currentCount = 0;
        for (size_t i = start; i <= end; i++)
        {
            currentCount += (fluxLen[i] >= FLUX_LONG) ? 2 : 1;
            __int64 time = (duration * currentCount + halfTotCount) / totCount;
            flux[i] = time - lastTime;
            lastTime = time;
        }
    }
}

vector<int> RegularizeFlux(const vector<int>& flux, const HistResult& hrEven, const HistResult& hrOdd)
{
    FluxClassifier fluxClassEven(hrEven);
    FluxClassifier fluxClassOdd(hrOdd);

    vector<FluxLen> fluxLen(flux.size());
    for (size_t i = 1; i < flux.size(); i++)
    {
        int f = flux[i];
        if (i & 1)
            fluxLen[i] = fluxClassOdd.Classify(f);
        else
            fluxLen[i] = fluxClassEven.Classify(f);
    }

    size_t start = 0;
    while (start < flux.size() && fluxLen[start] != FLUX_SHORT  && fluxLen[start] != FLUX_LONG)
        start++;

    size_t end = flux.size() - 1;
    while (end > start && fluxLen[end] != FLUX_SHORT && fluxLen[end] != FLUX_LONG)
        end--;

    bool isLowQuality = hrEven.quality > 0 || hrOdd.quality > 0;
    vector<int> result(flux);
    size_t startSegment = start;
    bool hasStartSegment = true;
    size_t lastGood = start;
    for (size_t i = start; i <= end; i++)
    {
        FluxLen fl = fluxLen[i];
        if (fl == FLUX_SHORT || fl == FLUX_LONG)
        {
            if (!hasStartSegment)
            {
                startSegment = i;
                hasStartSegment = true;
            }
            lastGood = i;
        }
        else if (fl != FLUX_MID)
        {
            if (hasStartSegment && startSegment + 10 <= lastGood)
                RegularizeContiguous(result, fluxLen, startSegment, lastGood, isLowQuality);
            hasStartSegment = false;
        }
    }
    if (hasStartSegment && startSegment + 10 <= lastGood)
        RegularizeContiguous(result, fluxLen, startSegment, lastGood, isLowQuality);

    return result;
}