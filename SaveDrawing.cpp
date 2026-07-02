// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Daniele Terdina

#include <Windows.h>
#include <GdiPlus.h>
#include <GdiPlusFlat.h>
#include <vector>
#include <assert.h>
#include "MdvDecode.h"
using namespace Gdiplus;
using namespace Gdiplus::DllExports;
using namespace std;

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT  num = 0;          // number of image encoders
    UINT  size = 0;         // size of the image encoder array in bytes

    ImageCodecInfo* pImageCodecInfo = NULL;

    GetImageEncodersSize(&num, &size);
    if (size == 0)
        return -1;  // Failure

    pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == NULL)
        return -1;  // Failure

    GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j)
    {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0)
        {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;  // Success
        }
    }

    free(pImageCodecInfo);
    return -1;  // Failure
}

void SaveJpeg(HBITMAP hBmp, LPCWSTR lpszFilename, ULONG uQuality)
{
    GpBitmap* pBitmap;
    GdipCreateBitmapFromHBITMAP(hBmp, NULL, &pBitmap);

    CLSID imageCLSID;
    GetEncoderClsid(L"image/jpeg", &imageCLSID);

    EncoderParameters encoderParams;
    encoderParams.Count = 1;
    encoderParams.Parameter[0].NumberOfValues = 1;
    encoderParams.Parameter[0].Guid = EncoderQuality;
    encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
    encoderParams.Parameter[0].Value = &uQuality;

    GdipSaveImageToFile(pBitmap, lpszFilename, &imageCLSID, &encoderParams);
}

struct GdiplusInit {
    GdiplusInit() {
        GdiplusStartupInput inp;
        GdiplusStartupOutput outp;
        Status result = GdiplusStartup(&token_, &inp, &outp);
        int i = 0;
    }
    ~GdiplusInit() {
        GdiplusShutdown(token_);
    }
private:
    ULONG_PTR token_;
};

static GdiplusInit initGraphics;

void DrawError(const vector<int>& fluxData, const vector<int>& alignment)
{
    static int id = 0;

    const int width = 4000;
    const int height = 120;
    const int waveHeight = 30;
    //Create a bitmap
    Bitmap bmp(width, height, PixelFormat32bppARGB);
    Graphics g(&bmp);
    Pen blackPen(Color(255, 0, 0, 0), 3);
    Pen lightGreyPen(Color(255, 200, 200, 200), 3);
    SolidBrush whiteBrush(Color(255, 255, 255, 255));


    size_t start = 0;
    int timeStart = 0;
    if (!alignment.empty())
    {
        int time = 0;
        while (start < fluxData.size() && time + fluxData[start] < alignment[0])
            time += fluxData[start++];

        for (int i = 0; i < 4 && start > 0; i++)
            time -= fluxData[--start];
        timeStart = time;
    }

    // Erase background
    g.FillRectangle(&whiteBrush, 0, 0, width, height);

    for (int t : alignment)
    {
        int x = (t - timeStart) / 6;
        g.DrawLine(&lightGreyPen, x, 0, x, height);
    }

    const int waveHigh = (height - waveHeight) / 2;
    const int waveLow = height - waveHeight;
    bool state = true;
    int lastX = 0;
    int time = 0;
    for (size_t i = start; i < fluxData.size(); i++)
    {
        time += fluxData[i];
        g.DrawLine(&blackPen, lastX, waveLow, lastX, waveHigh);
        int y = state ? waveHigh : waveLow;
        int x = (time - timeStart) / 6;
        g.DrawLine(&blackPen, lastX, y, x, y);
        if (x > width)
            break;
        lastX = x;
        state = !state;
    }

    HBITMAP hBitmap;
    bmp.GetHBITMAP(Color::Black, &hBitmap);
    SaveJpeg(hBitmap, L"error.jpg", 80);
}

void DrawAllBlocks(const vector<Block>& blocks, FileSystem *pFileSys, int firstBlock, const char* jpgPath, bool verbose)
{
    const int width = 11000;
    const int height = 450;
    const int margin = 30;
    const float VERT_SPACE = 1.8;

    //Create a bitmap
    Bitmap bmp(width, height, PixelFormat32bppARGB);
    Graphics g(&bmp);
    Pen blackPen(Color(255, 0, 0, 0), 3);
    Pen lightGreyPen(Color(255, 200, 200, 200), 3);
    Pen greenPen(Color(255, 16, 216, 16), 3);
    SolidBrush whiteBrush(Color(255, 255, 255, 255));
    SolidBrush blackSemitransparentBrush(Color(128, 0, 0, 0));
    SolidBrush lightRedBrush(Color(255, 255, 128, 128));
    SolidBrush lightGreenBrush(Color(255, 128, 255, 128));

    // Erase background
    g.FillRectangle(&whiteBrush, 0, 0, width, height);

    g.DrawLine(&lightGreyPen, margin, margin, margin, height - margin);

    // Estimate typical rev-to-rev block distance (median of hasNext deltas)
    // so we can suppress spurious wraps that occur far too close in time to
    // the previous one (e.g. when the physical tape junction sits mid-rev
    // and a block just after junction has a lower order than one just
    // before, followed shortly by a real rev-boundary block also with lower
    // order — two apparent wraps per rev).
    int medianRevDelta = 0;
    {
        vector<int> deltas;
        deltas.reserve(64);
        for (size_t i = 0; i < blocks.size() && deltas.size() < 64; i++)
            if (blocks[i].hasNext)
                deltas.push_back(blocks[blocks[i].nextLoopIndex].startTime - blocks[i].startTime);
        if (!deltas.empty())
        {
            std::sort(deltas.begin(), deltas.end());
            medianRevDelta = deltas[deltas.size() / 2];
        }
    }
    int minWrapSpacing = medianRevDelta / 2;   // must move >=half a rev forward

    vector<int> distance;
    distance.push_back(0);
    int lastOrder = INT_MAX;
    int sum = 0;
    bool foundSector0 = false;
    bool hasDistance = false;
    int startTime = INT_MIN;

    int dbgIndex = 0;
    int lastWrapTime = INT_MIN;
    int numWraps = 0;
    bool firstIter = true;
    for (const Block& b: blocks)
    {
        if (b.order < lastOrder)
        {
            bool tooSoon = (!firstIter && lastWrapTime != INT_MIN &&
                             (int64_t)(b.startTime - lastWrapTime) < minWrapSpacing);
            if (tooSoon)
            {
                // Spurious wrap — a block came in out of monotonic order but
                // we're still in the same physical rev. Ignore for row-count
                // purposes but keep lastOrder in sync so we can still detect
                // the real next boundary.
                if (verbose)
                    printf("wrap suppressed at dbgId=%d (idx=%d): startTime=%d order=%d (prev order=%d) delta=%d < %d\n",
                        b.dbgId, dbgIndex, b.startTime, b.order, lastOrder,
                        b.startTime - lastWrapTime, minWrapSpacing);
            }
            else
            {
                // If the row we're leaving had no hasNext blocks (nothing
                // contributed a distance for its transition), fall back to
                // the raw wrap-to-wrap time delta so the distance array
                // stays aligned with the wrap count. Without this, pass 2
                // would apply the NEXT row's distance at the wrong wrap
                // and render subsequent rows off-screen. Reference for the
                // delta is the last real wrap, or the very first block if
                // this is the first real wrap.
                if (!firstIter && !hasDistance)
                {
                    int prevRowStart = (lastWrapTime != INT_MIN) ? lastWrapTime : blocks.begin()->startTime;
                    int fallback = b.startTime - prevRowStart;
                    distance.push_back(fallback);
                    sum += fallback;
                    // Note: hasDistance stays false — a real hasNext block
                    // in this row can still push distance[++] on its own.
                }
                if (!firstIter)
                {
                    numWraps++;
                    if (verbose)
                    {
                        int deltaFromLastWrap = (lastWrapTime == INT_MIN) ? 0 : b.startTime - lastWrapTime;
                        printf("wrap at dbgId=%d (idx=%d): startTime=%d masterId=%d order=%d (prev order=%d) delta=%d\n",
                            b.dbgId, dbgIndex, b.startTime, b.masterId, b.order, lastOrder, deltaFromLastWrap);
                    }
                    lastWrapTime = b.startTime;   // only real wraps update lastWrapTime
                }
                firstIter = false;
                hasDistance = false;
            }
        }
        if (!foundSector0 && b.order == 0)
        {
            foundSector0 = true;
            startTime = b.startTime - b.gapLen - sum;
        }
        if (!hasDistance && b.hasNext)
        {
            int d = blocks[b.nextLoopIndex].startTime - b.startTime;
            distance.push_back(d);
            sum += d;
            hasDistance = true;
        }
        lastOrder = b.order;

        dbgIndex++;
    }
    if (verbose)
        printf("Pass 1 collected %zu distance entries for %d wraps (min spacing=%d)\n",
            distance.size(), numWraps, minWrapSpacing);

    // Pass 2 does distance[++rowId] once per wrap. If distance is short of
    // numWraps entries, we'd read past the end and either crash or produce
    // odd geometry. Rather than skipping the whole jpg, pad with the median
    // of what we have so subsequent rows are just placed at typical spacing.
    if ((int)distance.size() < numWraps + 1 && distance.size() > 1)
    {
        vector<int> sortedD(distance.begin() + 1, distance.end());   // skip the leading 0
        std::sort(sortedD.begin(), sortedD.end());
        int fill = sortedD.empty() ? 0 : sortedD[sortedD.size() / 2];
        while ((int)distance.size() < numWraps + 1)
            distance.push_back(fill);
        if (verbose)
            printf("Padded distance array to %zu entries (median=%d) for jpg rendering\n",
                distance.size(), fill);
    }

    int maxDist =  *std::max_element(distance.begin(), distance.end());
    double xScale = (double)(width - margin * 2) / maxDist;
    int numRows = distance.size();
    double yScale = (double)(height - margin * 2) / (1.0 + (numRows - 1) * VERT_SPACE);

    lastOrder = INT_MAX;
    int rowId = -1;
    int lastWrapTimeP2 = INT_MIN;
    bool firstIterP2 = true;
    Gdiplus::Rect r;
    dbgIndex = 0;
    for (const Block& b : blocks)
    {
        if (b.order < lastOrder)
        {
            // Wrap-suppression: skip if the previous REAL wrap was less than
            // half a rev ago (spurious dip from a mid-rev tape junction).
            // The very first iteration is the artificial "block 0 < INT_MAX"
            // trigger — that one always applies (row 0 setup with distance[0]=0).
            bool tooSoon = (!firstIterP2 && lastWrapTimeP2 != INT_MIN &&
                             (int64_t)(b.startTime - lastWrapTimeP2) < minWrapSpacing);
            if (!tooSoon && rowId + 1 < (int)distance.size())
            {
                startTime += distance[++rowId];
                r.Y = (int)(yScale * rowId * VERT_SPACE + 0.5) + margin;
                r.Height = (int)(yScale + 0.5);
                if (!firstIterP2)
                    lastWrapTimeP2 = b.startTime;
                firstIterP2 = false;
            }
        }
        lastOrder = b.order;

        r.X = (int)(xScale * (b.startTime - startTime) + 0.5) + margin;
        r.Width = (int)(xScale * (b.endTime - b.startTime) + 0.5);

        if (b.hasNext)
        {
            const Block& b2 = blocks[b.nextLoopIndex];
            int xTo = (int)(xScale * ((b2.startTime + b2.endTime) * 0.5 - startTime - distance[rowId + 1]) + 0.5) + margin;
            int yTo = (int)(yScale * (rowId + 1) * VERT_SPACE + 0.5) + margin;
            g.DrawLine((b.nextType == NT_STRONG) ? &greenPen : &lightGreyPen, r.X + r.Width / 2, r.Y + r.Height, xTo, yTo);
        }

        //if (pFileSys)
        {
            //bool isGood = pFileSys->IsGoodBlock(&b.data[0], b.data.size());
            //assert(isGood == b.isGood);
            SolidBrush* pBrush = (b.isGood && b.mergeQuality != MQ_BAD) ? &lightGreenBrush : &lightRedBrush;
            if (!b.isGood || b.mergeQuality == MQ_PERFECT)
            {
                // Fill entire rectangle (bad sector, or all copies are good and match each other)
                g.FillRectangle(pBrush, r);
            }
            else
            {
                // Fill triangle
                Point triangle[3];
                triangle[0].X = r.X;
                triangle[0].Y = triangle[1].Y = r.Y + r.Height;
                triangle[1].X = triangle[2].X = r.X + r.Width;
                triangle[2].Y = r.Y;
                g.FillPolygon(pBrush, triangle, 3);
            }
            if (b.sectorMapType == SMT_MARKED_BAD)
            {
                // Mark known bad sectors with an X
                g.DrawLine(&blackPen, r.X, r.Y, r.X + r.Width, r.Y + r.Height);
                g.DrawLine(&blackPen, r.X, r.Y + r.Height, r.X + r.Width, r.Y);
            }
            else if (b.sectorMapType == SMT_MAP || (b.sectorMapType == SMT_FILE && b.data.size() >= MIN_SECTOR_SIZE))
            {
                // Mark known used sectors with a dot
                REAL x = r.X + r.Width * 0.5f - 4;
                REAL y = r.Y + r.Height * 0.5f - 4;
                REAL radius = min(r.Width, r.Height) * 0.25f;
                g.FillEllipse(&blackSemitransparentBrush, x, y, radius, radius);
            }
        }
        g.DrawRectangle(&blackPen, r);

        dbgIndex++;
    }

    HBITMAP hBitmap;
    bmp.GetHBITMAP(Color::Black, &hBitmap);

    WCHAR wPath[MAX_PATH];
    size_t converted = 0;
    mbstowcs_s(&converted, wPath, jpgPath, _TRUNCATE);
    SaveJpeg(hBitmap, wPath, 80);
}

// =============================================================================
// Phase 2 diagnostic layout
// =============================================================================
//
// Visualizes blockList right after MakeConnections (hash-based chain
// scaffolding) and BEFORE the Overlaps-based placement pass. Layout:
//   Y = startTime / offset  (rev index; ~7 rows for a 6.1-rev tape)
//   X = startTime % offset  (position within one revolution)
//
// Block fill colors reveal scaffolding quality:
//   green   = block is in a confirmed chain of length >= numLoops-1  (solid scaffold)
//   yellow  = block is in a shorter chain (2..numLoops-2)             (partial)
//   red     = block is isolated (chain of length 1)                   (no scaffold)
//   pale    = block has a hash-match pointer but the connection was not confirmed
//
// Lines show the connections:
//   solid green    = confirmed unambiguous match (NT_STRONG)
//   dashed green   = confirmed loop=1 match (NT_OTHER)
//   thin gray      = hash-match, not confirmed
//
// For blocks that look like ZX Spectrum headers (small block, header flag set),
// the sector number (data[1]) is printed inside the block.
void DrawPhase2Layout(const vector<Block>& blockList, int offset, int traceFreq, const char* jpgPath, bool verbose)
{
    if (blockList.empty() || offset <= 0)
        return;

    // Estimate number of revs from the maximum startTime
    int maxStart = 0;
    for (const Block& b : blockList)
        if (b.startTime > maxStart) maxStart = b.startTime;
    int numRevs = maxStart / offset + 1;
    if (numRevs < 1) numRevs = 1;
    if (numRevs > 20) numRevs = 20;  // sanity cap

    // Compute chain length that each block belongs to
    vector<int> chainLen(blockList.size(), 1);
    vector<bool> isChainHead(blockList.size(), true);
    for (size_t i = 0; i < blockList.size(); i++)
        if (blockList[i].hasNext && blockList[i].nextLoopIndex >= 0)
            isChainHead[blockList[i].nextLoopIndex] = false;
    for (size_t i = 0; i < blockList.size(); i++)
    {
        if (!isChainHead[i]) continue;
        int len = 1;
        int cur = (int)i;
        while (blockList[cur].hasNext && blockList[cur].nextLoopIndex >= 0)
        {
            cur = blockList[cur].nextLoopIndex;
            len++;
        }
        // Walk again and assign length to every node in this chain
        cur = (int)i;
        chainLen[cur] = len;
        while (blockList[cur].hasNext && blockList[cur].nextLoopIndex >= 0)
        {
            cur = blockList[cur].nextLoopIndex;
            chainLen[cur] = len;
        }
    }

    // Layout constants
    const int margin = 40;
    const int rowHeight = 60;
    const int labelBand = 18;   // strip above each row for sector-number labels
    const int rowGap = 8;
    const int height = margin * 2 + numRevs * (rowHeight + labelBand + rowGap);
    const int width = 11000;
    const double xScale = (double)(width - margin * 2) / offset;

    Bitmap bmp(width, height, PixelFormat32bppARGB);
    Graphics g(&bmp);
    Pen blackPen(Color(255, 0, 0, 0), 2);
    Pen thinGrayPen(Color(255, 180, 180, 180), 1);
    Pen strongGreenPen(Color(255, 16, 160, 16), 2);
    Pen otherGreenPen(Color(255, 16, 160, 16), 1);
    otherGreenPen.SetDashStyle(DashStyleDash);
    Pen lightGridPen(Color(255, 220, 220, 220), 1);
    SolidBrush whiteBrush(Color(255, 255, 255, 255));
    SolidBrush blackBrush(Color(255, 0, 0, 0));
    SolidBrush solidGreenBrush(Color(255, 128, 220, 128));
    SolidBrush yellowBrush(Color(255, 240, 220, 128));
    SolidBrush redBrush(Color(255, 240, 128, 128));
    SolidBrush paleBrush(Color(255, 220, 220, 220));

    g.FillRectangle(&whiteBrush, 0, 0, width, height);

    // Row baselines and labels
    Gdiplus::Font labelFont(L"Consolas", 10, FontStyleRegular, UnitPixel);
    for (int r = 0; r < numRevs; r++)
    {
        int y = margin + r * (rowHeight + labelBand + rowGap) + labelBand;
        g.DrawLine(&lightGridPen, margin, y, width - margin, y);
        WCHAR label[32];
        swprintf_s(label, L"rev %d", r);
        g.DrawString(label, -1, &labelFont, PointF((REAL)(margin - 34), (REAL)y), &blackBrush);
    }

    // Consider a chain "solid" if it covers at least (numRevs - 1) blocks —
    // allows one dropped revolution.
    const int solidLen = numRevs > 1 ? numRevs - 1 : numRevs;

    Gdiplus::Font sectorFont(L"Consolas", 12, FontStyleBold, UnitPixel);
    int drawnSectorLabels = 0;

    for (size_t i = 0; i < blockList.size(); i++)
    {
        const Block& b = blockList[i];
        int rev = b.startTime / offset;
        if (rev < 0 || rev >= numRevs) continue;
        int tInRev = b.startTime % offset;
        int rectX = margin + (int)(xScale * tInRev + 0.5);
        int rectW = (int)(xScale * (b.endTime - b.startTime) + 0.5);
        if (rectW < 1) rectW = 1;
        int rowTop = margin + rev * (rowHeight + labelBand + rowGap);
        int rectY = rowTop + labelBand;

        SolidBrush* pFill;
        if (chainLen[i] >= solidLen) pFill = &solidGreenBrush;
        else if (chainLen[i] >= 2)   pFill = &yellowBrush;
        else if (b.nextLoopIndex >= 0 || (i > 0 && blockList[i - 1].nextLoopIndex == (int)i))
            pFill = &paleBrush;
        else                          pFill = &redBrush;
        g.FillRectangle(pFill, rectX, rectY, rectW, rowHeight);
        g.DrawRectangle(&blackPen, rectX, rectY, rectW, rowHeight);

        // Label the block with its sector number iff it looks structurally
        // like a real sector header:
        //   OPD    - 14 bytes starting with 0xFF, sector number at byte 1
        //   ZX Spectrum - 15 bytes with byte 0's flag bit set, sector number at byte 1
        // The size floor also excludes OPD's 4-byte "block header" (which
        // holds fileNum/blockNum, not the sector number) so its byte 1 doesn't
        // draw an overlapping bogus label next to the real sector-header one.
        bool isOpdSectorHeader = b.data.size() == 14 && b.data[0] == 0xFF;
        bool isZxSectorHeader  = b.data.size() == 15 && (b.data[0] & 1) == 1;
        if (isOpdSectorHeader || isZxSectorHeader)
        {
            WCHAR num[8];
            swprintf_s(num, L"%d", b.data[1]);
            // Leader tick from label to block
            g.DrawLine(&thinGrayPen, rectX + rectW / 2, rowTop + labelBand - 2, rectX + rectW / 2, rectY);
            g.DrawString(num, -1, &sectorFont, PointF((REAL)(rectX - 6), (REAL)rowTop), &blackBrush);
            drawnSectorLabels++;
        }
    }

    // Connection lines: draw AFTER blocks so they sit on top of fills.
    for (size_t i = 0; i < blockList.size(); i++)
    {
        const Block& b = blockList[i];
        if (b.nextLoopIndex < 0 || b.nextLoopIndex >= (int)blockList.size()) continue;
        const Block& n = blockList[b.nextLoopIndex];
        int rev1 = b.startTime / offset;
        int rev2 = n.startTime / offset;
        if (rev1 < 0 || rev1 >= numRevs || rev2 < 0 || rev2 >= numRevs) continue;
        int tIn1 = b.startTime % offset;
        int tIn2 = n.startTime % offset;
        int x1 = margin + (int)(xScale * (tIn1 + (b.endTime - b.startTime) / 2) + 0.5);
        int y1 = margin + rev1 * (rowHeight + labelBand + rowGap) + labelBand + rowHeight;
        int x2 = margin + (int)(xScale * (tIn2 + (n.endTime - n.startTime) / 2) + 0.5);
        int y2 = margin + rev2 * (rowHeight + labelBand + rowGap) + labelBand;

        Pen* pPen;
        if (b.hasNext && b.nextType == NT_STRONG) pPen = &strongGreenPen;
        else if (b.hasNext)                       pPen = &otherGreenPen;
        else                                      pPen = &thinGrayPen;
        g.DrawLine(pPen, x1, y1, x2, y2);
    }

    // Legend
    Gdiplus::Font legendFont(L"Consolas", 12, FontStyleRegular, UnitPixel);
    WCHAR summary[256];
    swprintf_s(summary,
        L"Phase 2 layout: %d blocks  offset=%d samples (%.2fs @ %d MHz)  numRevs=%d  sectorNums shown for %d blocks",
        (int)blockList.size(), offset, (double)offset / traceFreq, traceFreq / 1000000,
        numRevs, drawnSectorLabels);
    g.DrawString(summary, -1, &legendFont, PointF(margin, 8), &blackBrush);

    if (verbose)
        printf("DrawPhase2Layout: numRevs=%d bitmap=%dx%d headerLabels=%d\n",
            numRevs, width, height, drawnSectorLabels);

    HBITMAP hBitmap;
    bmp.GetHBITMAP(Color::Black, &hBitmap);

    WCHAR wPath[MAX_PATH];
    size_t converted = 0;
    mbstowcs_s(&converted, wPath, jpgPath, _TRUNCATE);
    SaveJpeg(hBitmap, wPath, 80);
}