#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "folder_icons.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <iterator>

namespace mm {
HIMAGELIST CreateFolderImages(UINT dpi, bool dark) {
    using namespace Gdiplus;
    const int size = std::max(12, MulDiv(18, static_cast<int>(std::min<UINT>(dpi ? dpi : 96, 768)), 96));
    HIMAGELIST images = ImageList_Create(size, size, ILC_COLOR32, 2, 0);
    if (!images) return nullptr;
    ImageList_SetBkColor(images, CLR_NONE);
    for (int state = 0; state < 2; ++state) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = size;
        info.bmiHeader.biHeight = -size;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!dib || !pixels) { if (dib) DeleteObject(dib); ImageList_Destroy(images); return nullptr; }
        {
            // Draw directly into a premultiplied-alpha DIB; GetHBITMAP would
            // introduce an unnecessary alpha/background conversion here.
            Bitmap bitmap(size, size, size * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(pixels));
            Graphics graphics(&bitmap);
            graphics.Clear(Color(0, 0, 0, 0));
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
            graphics.ScaleTransform(size / 18.0f, size / 18.0f);
            SolidBrush back(dark ? Color(255, 188, 143, 66) : Color(255, 229, 178, 68));
            SolidBrush front(dark ? Color(255, 226, 181, 91) : Color(255, 249, 204, 100));
            Pen outline(dark ? Color(255, 133, 102, 53) : Color(255, 159, 119, 47), 0.85f);
            outline.SetLineJoin(LineJoinRound);
            const PointF rear[] = {{1.8f, 14.7f}, {1.8f, 3.8f}, {2.4f, 3.1f},
                {6.6f, 3.1f}, {8.3f, 4.9f}, {15.5f, 4.9f}, {16.1f, 5.5f}, {16.1f, 14.7f}};
            GraphicsPath rearPath;
            rearPath.AddPolygon(rear, static_cast<INT>(std::size(rear)));
            graphics.FillPath(&back, &rearPath);
            graphics.DrawPath(&outline, &rearPath);
            const PointF closed[] = {{1.8f, 6.8f}, {16.1f, 6.8f}, {16.1f, 14.4f},
                {15.4f, 15.1f}, {2.5f, 15.1f}, {1.8f, 14.4f}};
            const PointF open[] = {{1.6f, 7.9f}, {6.8f, 7.9f}, {8.1f, 6.9f},
                {17.0f, 6.9f}, {14.9f, 15.1f}, {2.9f, 15.1f}};
            GraphicsPath frontPath;
            frontPath.AddPolygon(state ? open : closed, static_cast<INT>(std::size(closed)));
            graphics.FillPath(&front, &frontPath);
            graphics.DrawPath(&outline, &frontPath);
            graphics.Flush(FlushIntentionSync);
        }
        const int added = ImageList_Add(images, dib, nullptr);
        DeleteObject(dib);
        if (added != state) { ImageList_Destroy(images); return nullptr; }
    }
    return images;
}
}
