// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina
//
// Smooth-curve fit of the cross-revolution distance D(t) — i.e. the observed
// tape-loop duration as a function of trace time — from phase-2 confirmed
// hash-connections. Used by the phase-3 merge pass to compute L(t): the
// per-block shift needed to bring blocks into the rev-0 coordinate frame.
//
// The physical picture: motor speed varies smoothly across the trace (tape
// spin-up on ZX, near-constant on stable QL/OPD captures). A running
// per-block correction can't respond fast enough on spin-up tapes, and a
// stale rev-0-span estimate loses accuracy over many revs. Precomputing a
// spline of D(t) from the phase-2 chains and driving phase 3 from it avoids
// both problems.

#pragma once

#include <vector>

struct FitPoint
{
    double x;         // src.startTime (sample units)
    double y;         // cross-rev distance = dst.startTime - src.startTime
    int weight;       // 1 = NT_OTHER match, 2 = NT_STRONG match
    double fitted;    // filled by FitDistanceCurve
    double residual;  // y - fitted
    bool isOutlier;   // set in the 3-sigma pass
};

// Fit a natural cubic spline to (x, y, weight) samples with knots roughly at
// every knotSpacing on the x axis. Knots are dropped where the ±knotSpacing
// window contains too few samples, so unused-tape regions do not distort the
// fit. One 3-sigma outlier rejection pass. Fills the fitted/residual/isOutlier
// fields of each input point and returns the knot data (kx, ky) plus spline
// coefficients (sb, sc, sd) for later evaluation.
//
// Returns true if a usable spline was built (>=2 knots).
bool FitDistanceCurve(std::vector<FitPoint>& pts, double knotSpacing,
    std::vector<double>& kxOut, std::vector<double>& kyOut,
    std::vector<double>& sbOut, std::vector<double>& scOut, std::vector<double>& sdOut);

// Evaluate a fitted spline at time t. Uses linear extrapolation past the ends
// of the knot range.
double EvalDistanceSpline(const std::vector<double>& kx, const std::vector<double>& ky,
    const std::vector<double>& sb, const std::vector<double>& sc, const std::vector<double>& sd,
    double t);

// Turns per-block observed shifts (one sample per hash-linked block, from
// walking each block's previousLoopIndex chain back to its rev-0 root) into
// L(t) via direct linear interpolation. No rev boundaries, no running
// per-block catchup — the shift comes straight out of interpolation.
//
// Each sample (t, shift) is an EXACT observation: at trace time t there is a
// block whose canonical rev-0 equivalent is at time t - shift. So samples
// have no fit noise. Interpolating between them gives per-block precision
// for blocks not directly in a chain, tracking sub-rev motor variation.
class LoopDistanceState
{
public:
    // sampleT, sampleShift: parallel arrays sorted by sampleT.
    //   sampleShift[i] = 0 for rev-0 chain roots;
    //   sampleShift[i] = t_i - chain_root_time for chain descendants.
    void Init(const std::vector<double>& sampleT, const std::vector<double>& sampleShift);

    // Returns integer sample-unit shift L(t) via linear interpolation.
    // Extrapolates as constant at the ends.
    int Query(double t) const;

private:
    std::vector<double> sampleT;
    std::vector<double> sampleShift;
};

// Linear interpolation between (x, y) samples, x-sorted. Extrapolates as
// constant at the ends. Utility used by the diagnostic code paths.
double InterpDistance(const std::vector<double>& sampleT, const std::vector<double>& sampleD, double t);
