// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include "FitDistance.h"
#include <algorithm>
#include <cmath>
#include <utility>

using std::vector;
using std::pair;

// Natural cubic spline through (kx[i], ky[i]) knots. Standard tridiagonal
// solve; coefficients are stored per segment so eval is a single Horner-like
// evaluation.
static void BuildNaturalCubicSpline(const vector<double>& kx, const vector<double>& ky,
    vector<double>& b, vector<double>& c, vector<double>& d)
{
    int n = (int)kx.size();
    b.assign(n, 0); c.assign(n, 0); d.assign(n, 0);
    if (n < 2) return;
    if (n == 2)
    {
        double h = kx[1] - kx[0];
        b[0] = (ky[1] - ky[0]) / h;
        return;
    }
    vector<double> h(n - 1);
    for (int i = 0; i < n - 1; i++) h[i] = kx[i + 1] - kx[i];
    vector<double> alpha(n, 0);
    for (int i = 1; i < n - 1; i++)
        alpha[i] = 3.0 * ((ky[i + 1] - ky[i]) / h[i] - (ky[i] - ky[i - 1]) / h[i - 1]);
    vector<double> l(n), mu(n), z(n);
    l[0] = 1; mu[0] = 0; z[0] = 0;
    for (int i = 1; i < n - 1; i++)
    {
        l[i] = 2 * (kx[i + 1] - kx[i - 1]) - h[i - 1] * mu[i - 1];
        mu[i] = h[i] / l[i];
        z[i] = (alpha[i] - h[i - 1] * z[i - 1]) / l[i];
    }
    l[n - 1] = 1; z[n - 1] = 0; c[n - 1] = 0;
    for (int j = n - 2; j >= 0; j--)
    {
        c[j] = z[j] - mu[j] * c[j + 1];
        b[j] = (ky[j + 1] - ky[j]) / h[j] - h[j] * (c[j + 1] + 2 * c[j]) / 3.0;
        d[j] = (c[j + 1] - c[j]) / (3.0 * h[j]);
    }
}

double EvalDistanceSpline(const vector<double>& kx, const vector<double>& ky,
    const vector<double>& b, const vector<double>& c, const vector<double>& d, double x)
{
    int n = (int)kx.size();
    if (n == 0) return 0;
    if (n == 1 || x <= kx[0]) return ky[0] + b[0] * (x - kx[0]);
    if (x >= kx[n - 1]) return ky[n - 1] + b[n - 1] * (x - kx[n - 1]);
    int lo = 0, hi = n - 1;
    while (hi - lo > 1)
    {
        int mid = (lo + hi) / 2;
        if (kx[mid] > x) hi = mid; else lo = mid;
    }
    double dx = x - kx[lo];
    return ky[lo] + b[lo] * dx + c[lo] * dx * dx + d[lo] * dx * dx * dx;
}

// Weighted median of samples in a window, ignoring outliers if requested.
// Returns the sample count via outN so callers can drop undersupported knots
// (e.g., in the unused-tape region of a ZX/GST cart).
static double WeightedMedianInWindow(const vector<FitPoint>& pts,
    double xCenter, double halfWindow, bool skipOutliers, int& outN)
{
    outN = 0;
    vector<pair<double, int>> vw;   // (value, weight)
    for (const FitPoint& p : pts)
    {
        if (skipOutliers && p.isOutlier) continue;
        if (p.x < xCenter - halfWindow || p.x > xCenter + halfWindow) continue;
        vw.push_back({ p.y, p.weight });
    }
    outN = (int)vw.size();
    if (vw.empty()) return 0.0;
    std::sort(vw.begin(), vw.end(), [](const pair<double, int>& a, const pair<double, int>& b)
        { return a.first < b.first; });
    long long total = 0;
    for (auto& e : vw) total += e.second;
    long long half = total / 2;
    long long acc = 0;
    for (auto& e : vw)
    {
        acc += e.second;
        if (acc >= half) return e.first;
    }
    return vw.back().first;
}

bool FitDistanceCurve(vector<FitPoint>& pts, double knotSpacing,
    vector<double>& kxOut, vector<double>& kyOut,
    vector<double>& sbOut, vector<double>& scOut, vector<double>& sdOut)
{
    kxOut.clear(); kyOut.clear();
    sbOut.clear(); scOut.clear(); sdOut.clear();
    if (pts.size() < 4) return false;

    double xMin = pts.front().x, xMax = pts.back().x;
    for (const FitPoint& p : pts)
    {
        if (p.x < xMin) xMin = p.x;
        if (p.x > xMax) xMax = p.x;
    }
    if (xMax <= xMin) return false;

    // Candidate knot positions at every knotSpacing, with a small overhang so
    // boundary samples stay inside the spline's valid range.
    vector<double> candidateX;
    for (double x = xMin - knotSpacing / 2; x <= xMax + knotSpacing / 2; x += knotSpacing)
        candidateX.push_back(x);

    // Drop knots without enough local support so unused-tape regions don't
    // seed duplicate-value knots that would distort the spline.
    const int MIN_SAMPLES_PER_KNOT = 3;

    auto buildKnots = [&](bool skipOutliers)
    {
        kxOut.clear(); kyOut.clear();
        for (double xC : candidateX)
        {
            int n = 0;
            double m = WeightedMedianInWindow(pts, xC, knotSpacing, skipOutliers, n);
            if (n >= MIN_SAMPLES_PER_KNOT && m > 0)
            {
                kxOut.push_back(xC);
                kyOut.push_back(m);
            }
        }
        if (kxOut.size() < 4)
        {
            // Fall back to a coarse uniform layout if support was sparse.
            kxOut.clear(); kyOut.clear();
            int nSteps = std::max(3, (int)candidateX.size() / 4);
            for (int i = 0; i <= nSteps; i++)
            {
                double xC = xMin + (xMax - xMin) * i / nSteps;
                int n = 0;
                double m = WeightedMedianInWindow(pts, xC, (xMax - xMin) / nSteps, skipOutliers, n);
                if (m > 0)
                {
                    kxOut.push_back(xC);
                    kyOut.push_back(m);
                }
            }
        }
    };

    // Pass 1: no outlier filter, build spline, mark outliers via residual RMS.
    for (FitPoint& p : pts) p.isOutlier = false;
    buildKnots(false);
    if (kxOut.size() < 2) return false;
    BuildNaturalCubicSpline(kxOut, kyOut, sbOut, scOut, sdOut);
    double sumSq = 0.0;
    for (FitPoint& p : pts)
    {
        p.fitted = EvalDistanceSpline(kxOut, kyOut, sbOut, scOut, sdOut, p.x);
        p.residual = p.y - p.fitted;
        sumSq += p.residual * p.residual;
    }
    double sigma = std::sqrt(sumSq / pts.size());
    for (FitPoint& p : pts)
        if (std::fabs(p.residual) > 3.0 * sigma)
            p.isOutlier = true;

    // Pass 2: refit excluding outliers.
    buildKnots(true);
    if (kxOut.size() < 2) return false;
    BuildNaturalCubicSpline(kxOut, kyOut, sbOut, scOut, sdOut);
    for (FitPoint& p : pts)
    {
        p.fitted = EvalDistanceSpline(kxOut, kyOut, sbOut, scOut, sdOut, p.x);
        p.residual = p.y - p.fitted;
    }
    return true;
}


// ============================================================================
// LoopDistanceState — L(t) via linear interpolation of raw hash-pair samples.
// ============================================================================

double InterpDistance(const vector<double>& sampleT, const vector<double>& sampleD, double t)
{
    if (sampleT.empty()) return 0;
    if (t <= sampleT.front()) return sampleD.front();
    if (t >= sampleT.back()) return sampleD.back();
    auto it = std::upper_bound(sampleT.begin(), sampleT.end(), t);
    int idx1 = (int)(it - sampleT.begin());
    int idx0 = idx1 - 1;
    double span = sampleT[idx1] - sampleT[idx0];
    if (span <= 0) return sampleD[idx0];
    double frac = (t - sampleT[idx0]) / span;
    return sampleD[idx0] + frac * (sampleD[idx1] - sampleD[idx0]);
}

void LoopDistanceState::Init(const vector<double>& t, const vector<double>& shift)
{
    sampleT = t;
    sampleShift = shift;
}

int LoopDistanceState::Query(double t) const
{
    // Direct linear interpolation between chain-derived (t, shift) samples.
    return (int)InterpDistance(sampleT, sampleShift, t);
}
