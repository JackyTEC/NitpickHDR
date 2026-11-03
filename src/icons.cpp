// Nitpick — per-monitor HDR control
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Tray icons are drawn at runtime so they're crisp at any DPI and can carry the
// display number. Everything is rendered 4x supersampled with GDI into coverage
// masks, then composited into a 32-bit alpha icon.

#include "icons.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace icons {

namespace {

constexpr int kSuperSample = 4;

// A square white-on-black GDI canvas used as a coverage mask.
struct Canvas
{
    int w;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    uint32_t* px = nullptr;

    explicit Canvas(int width) : w(width)
    {
        BITMAPINFO bi {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -w;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&px), nullptr, 0);
        oldBmp = SelectObject(dc, bmp);
        Clear();
    }
    ~Canvas()
    {
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
    }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    void Clear()
    {
        GdiFlush();
        if (px)
            std::memset(px, 0, static_cast<size_t>(w) * w * 4);
    }

    // Ink bounding box (exclusive right/bottom); empty if nothing drawn.
    RECT InkBounds() const
    {
        GdiFlush();
        RECT r { w, w, 0, 0 };
        for (int y = 0; y < w; ++y)
            for (int x = 0; x < w; ++x)
                if (px[y * w + x] & 0xFF) {
                    r.left = std::min<LONG>(r.left, x);
                    r.top = std::min<LONG>(r.top, y);
                    r.right = std::max<LONG>(r.right, x + 1);
                    r.bottom = std::max<LONG>(r.bottom, y + 1);
                }
        if (r.right <= r.left)
            return RECT {};
        return r;
    }

    // Box-filter down to (w/ss)^2 coverage values in [0,1]
    std::vector<float> Downsample(int ss) const
    {
        GdiFlush();
        const int out = w / ss;
        std::vector<float> cov(static_cast<size_t>(out) * out, 0.f);
        for (int y = 0; y < out; ++y)
            for (int x = 0; x < out; ++x) {
                unsigned sum = 0;
                for (int sy = 0; sy < ss; ++sy)
                    for (int sx = 0; sx < ss; ++sx)
                        sum += px[(y * ss + sy) * w + (x * ss + sx)] & 0xFF;
                cov[y * out + x] = static_cast<float>(sum) / (255.f * ss * ss);
            }
        return cov;
    }
};

HFONT MakeFont(int pixelHeight, int weight)
{
    return CreateFontW(-pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void RenderText(Canvas& c, const std::wstring& text, int fontPx, int weight, int x, int y)
{
    HFONT font = MakeFont(fontPx, weight);
    HGDIOBJ oldFont = SelectObject(c.dc, font);
    SetBkMode(c.dc, TRANSPARENT);
    SetTextColor(c.dc, RGB(255, 255, 255));
    TextOutW(c.dc, x, y, text.c_str(), static_cast<int>(text.size()));
    GdiFlush();
    SelectObject(c.dc, oldFont);
    DeleteObject(font);
}

// Draw `text` so that its *ink* (not the font cell) fits `box`, centred.
void DrawFittedText(Canvas& dst, const std::wstring& text, RECT box, int weight)
{
    const int bw = box.right - box.left, bh = box.bottom - box.top;
    if (bw <= 0 || bh <= 0)
        return;

    Canvas tmp(dst.w * 3);
    int fontPx = std::max(4, bh * 3 / 2);
    RECT ink {};
    for (int iter = 0; iter < 6; ++iter) {
        tmp.Clear();
        RenderText(tmp, text, fontPx, weight, dst.w / 2, dst.w / 2);
        ink = tmp.InkBounds();
        const int iw = ink.right - ink.left, ih = ink.bottom - ink.top;
        if (iw <= 0 || ih <= 0)
            return;
        const double s = std::min(static_cast<double>(bw) / iw, static_cast<double>(bh) / ih);
        if (s >= 0.97 && s <= 1.0)
            break;
        int next = std::max(4, static_cast<int>(std::floor(fontPx * s)));
        if (next == fontPx)
            next = s < 1.0 ? fontPx - 1 : fontPx; // nudge down if still too large
        if (next == fontPx)
            break;
        fontPx = next;
    }
    // Measure once more at the final size
    tmp.Clear();
    RenderText(tmp, text, fontPx, weight, dst.w / 2, dst.w / 2);
    ink = tmp.InkBounds();
    const int iw = ink.right - ink.left, ih = ink.bottom - ink.top;
    const int originX = dst.w / 2 - ink.left; // ink offset relative to text origin
    const int originY = dst.w / 2 - ink.top;
    RenderText(dst, text, fontPx, weight, box.left + (bw - iw) / 2 + originX, box.top + (bh - ih) / 2 + originY);
}

void FillRoundRect(Canvas& c, int l, int t, int r, int b, int radius)
{
    HGDIOBJ oldBrush = SelectObject(c.dc, GetStockObject(WHITE_BRUSH));
    HGDIOBJ oldPen = SelectObject(c.dc, GetStockObject(NULL_PEN));
    // With a NULL pen, RoundRect excludes the right/bottom edge -> +1
    RoundRect(c.dc, l, t, r + 1, b + 1, radius * 2, radius * 2);
    GdiFlush();
    SelectObject(c.dc, oldPen);
    SelectObject(c.dc, oldBrush);
}

struct Pixel
{
    float r = 0, g = 0, b = 0, a = 0; // premultiplied
};

void Over(std::vector<Pixel>& dst, const std::vector<float>& cov, COLORREF color, float opacity)
{
    const float cr = GetRValue(color) / 255.f, cg = GetGValue(color) / 255.f, cb = GetBValue(color) / 255.f;
    for (size_t i = 0; i < dst.size(); ++i) {
        const float a = std::clamp(cov[i], 0.f, 1.f) * opacity;
        dst[i].r = cr * a + dst[i].r * (1 - a);
        dst[i].g = cg * a + dst[i].g * (1 - a);
        dst[i].b = cb * a + dst[i].b * (1 - a);
        dst[i].a = a + dst[i].a * (1 - a);
    }
}

HICON ToIcon(int size, const std::vector<Pixel>& pixels)
{
    BITMAPV5HEADER bi {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color)
        return nullptr;

    auto* out = static_cast<uint32_t*>(bits);
    for (size_t i = 0; i < pixels.size(); ++i) {
        const auto& p = pixels[i];
        // Icons expect straight (non-premultiplied) alpha
        const float a = std::clamp(p.a, 0.f, 1.f);
        auto ch = [a](float v) { return static_cast<uint32_t>(std::lround(std::clamp(a > 0 ? v / a : 0.f, 0.f, 1.f) * 255)); };
        out[i] = (static_cast<uint32_t>(std::lround(a * 255)) << 24) | (ch(p.r) << 16) | (ch(p.g) << 8) | ch(p.b);
    }

    std::vector<BYTE> maskBits(static_cast<size_t>((size + 15) / 16) * 2 * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());

    ICONINFO ii {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

} // namespace

HICON Create(int size, State state, int number, bool darkTaskbar)
{
    size = std::max(size, 12);
    const int ss = kSuperSample;
    const int big = size * ss;

    const COLORREF fg = darkTaskbar ? RGB(245, 245, 245) : RGB(28, 28, 28);
    const COLORREF gold = RGB(255, 185, 0);
    const COLORREF red = RGB(232, 17, 35);

    const int stroke = std::max(1, static_cast<int>(std::lround(size / 16.0))) * ss;
    const int radius = std::max(2, static_cast<int>(std::lround(size * 0.22))) * ss;

    // Shapes
    Canvas outer(big);
    FillRoundRect(outer, 0, 0, big - 1, big - 1, radius);
    Canvas inner(big);
    FillRoundRect(inner, stroke, stroke, big - 1 - stroke, big - 1 - stroke, std::max(ss, radius - stroke));
    auto outerCov = outer.Downsample(ss);
    auto innerCov = inner.Downsample(ss);
    std::vector<float> ringCov(outerCov.size());
    for (size_t i = 0; i < ringCov.size(); ++i)
        ringCov[i] = std::max(0.f, outerCov[i] - innerCov[i]);

    // Label + number
    const std::wstring label = state == State::On ? L"HDR" : L"SDR";
    Canvas labelCanvas(big);
    Canvas numberCanvas(big);
    auto at = [big](double f) { return static_cast<LONG>(std::lround(big * f)); };
    if (number > 0) {
        DrawFittedText(labelCanvas, label, RECT { at(0.12), at(0.13), at(0.88), at(0.50) }, FW_BOLD);
        DrawFittedText(numberCanvas, std::to_wstring(number), RECT { at(0.30), at(0.60), at(0.70), at(0.87) }, FW_BOLD);
    } else {
        DrawFittedText(labelCanvas, label, RECT { at(0.12), at(0.30), at(0.88), at(0.70) }, FW_BOLD);
    }
    auto labelCov = labelCanvas.Downsample(ss);
    auto numberCov = numberCanvas.Downsample(ss);

    std::vector<Pixel> px(static_cast<size_t>(size) * size);
    switch (state) {
    case State::On:
        Over(px, outerCov, gold, 1.f);
        Over(px, labelCov, RGB(0, 0, 0), 1.f);
        Over(px, numberCov, RGB(0, 0, 0), 1.f);
        break;
    case State::Off:
        Over(px, ringCov, fg, 1.f);
        Over(px, labelCov, fg, 1.f);
        Over(px, numberCov, fg, 1.f);
        break;
    case State::Locked:
        Over(px, innerCov, red, 0.30f);
        Over(px, ringCov, red, 1.f);
        Over(px, labelCov, fg, 1.f);
        Over(px, numberCov, fg, 1.f);
        break;
    case State::Unsupported:
        Over(px, ringCov, fg, 0.40f);
        Over(px, labelCov, fg, 0.45f);
        Over(px, numberCov, fg, 0.45f);
        break;
    }
    return ToIcon(size, px);
}

} // namespace icons
