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
    for (const Block& b: blocks)
    {
        //printf("  %d, id=%d\n", b.order, b.masterId);
        if (b.order < lastOrder)
        {
            //printf("start loop at %d\n", b.order);
            numWraps++;
            if (verbose)
            {
                int deltaFromLastWrap = (lastWrapTime == INT_MIN) ? 0 : b.startTime - lastWrapTime;
                printf("wrap at dbgId=%d (idx=%d): startTime=%d masterId=%d order=%d (prev order=%d) delta=%d\n",
                    b.dbgId, dbgIndex, b.startTime, b.masterId, b.order, lastOrder, deltaFromLastWrap);
                lastWrapTime = b.startTime;
            }
            hasDistance = false;
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
        printf("Pass 1 collected %zu distance entries for %d wraps\n", distance.size(), numWraps);

    // Pass 2 does distance[++rowId] once per wrap. If distance is short of
    // numWraps entries, pass 2 will read past the end. This indicates the
    // merge left the block ordering inconsistent (e.g. poor Spectrum merge
    // quality on tapes with lots of overwritten sectors) and rendering would
    // be misleading anyway. Bail out with a warning.
    if ((int)distance.size() < numWraps)
    {
        printf("Warning: -jpg skipped, block ordering inconsistent (%d wraps, %zu distance entries)\n",
            numWraps, distance.size());
        return;
    }

    int maxDist =  *std::max_element(distance.begin(), distance.end());
    double xScale = (double)(width - margin * 2) / maxDist;
    int numRows = distance.size();
    double yScale = (double)(height - margin * 2) / (1.0 + (numRows - 1) * VERT_SPACE);

    lastOrder = INT_MAX;
    int rowId = -1;
    Gdiplus::Rect r;
    dbgIndex = 0;
    for (const Block& b : blocks)
    {
        if (b.order < lastOrder)
        {
            //printf("start loop at %d, row %d\n", b.order, rowId + 1);
            //if (rowId >= (int)distance.size() - 1)
            //    break;
            startTime += distance[++rowId];
            r.Y = (int)(yScale * rowId * VERT_SPACE + 0.5) + margin;
            r.Height = (int)(yScale + 0.5);

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