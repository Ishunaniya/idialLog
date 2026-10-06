#pragma once

#include <windows.h>
#include <array>

#include "app_context.h"
#include "text_catalog.h"
#include "theme.h"

namespace dl {

// Keep the bitmap selected only while its owning DC is alive. Fonts and other
// temporary objects must be restored by the caller before reusing this DC.
class PaintBuffer {
  public:
    PaintBuffer() = default;
    PaintBuffer(const PaintBuffer&) = delete;
    PaintBuffer& operator=(const PaintBuffer&) = delete;

    ~PaintBuffer() {
        reset();
    }

    void reset() {
        if (dc_) {
            SelectObject(dc_, previous_);
            DeleteObject(bitmap_);
            DeleteDC(dc_);
        }

        dc_ = nullptr;
        bitmap_ = nullptr;
        previous_ = nullptr;
        width_ = height_ = 0;
    }

    bool ensure(HDC reference, int width, int height) {
        if (dc_ && width_ == width && height_ == height)
            return true;

        reset();
        if (width <= 0 || height <= 0)
            return false;

        HDC dc = CreateCompatibleDC(reference);
        HBITMAP bitmap = dc ? CreateCompatibleBitmap(reference, width, height) : nullptr;
        if (!bitmap) {
            if (dc)
                DeleteDC(dc);
            return false;
        }

        auto previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) {
            DeleteObject(bitmap);
            DeleteDC(dc);
            return false;
        }

        dc_ = dc;
        bitmap_ = bitmap;
        previous_ = previous;
        width_ = width;
        height_ = height;
        return true;
    }

    HDC dc() const {
        return dc_;
    }

    void copyTo(HDC destination, const RECT& area) const {
        BitBlt(destination, area.left, area.top, area.right - area.left, area.bottom - area.top, dc_, area.left,
               area.top, SRCCOPY);
    }

  private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    int width_ = 0, height_ = 0;
};

// Cache identity includes settings that can change without reloading log data.
struct PaintAppearance {
    int dpi = 0;
    unsigned long long fontRevision = 0;
    bool english = false, dark = false, highContrast = false;
    std::array<HFONT, 3> fonts{};
    std::array<COLORREF, 23> colors{};

    bool operator==(const PaintAppearance& other) const {
        return dpi == other.dpi && fontRevision == other.fontRevision && english == other.english &&
               dark == other.dark && highContrast == other.highContrast && fonts == other.fonts &&
               colors == other.colors;
    }
};

inline PaintAppearance CurrentPaintAppearance() {
    return {App().dpi,
            App().fontRevision,
            IsEnglish(),
            th::dark,
            th::highContrast,
            {App().hFontUI, App().hFontSmall, App().hFontSect},
            {th::surface,    th::page,      th::border,   th::grid,       th::axis,      th::accent,
             th::accentSoft, th::inkPri,    th::inkSec,   th::inkMuted,   th::warning,   th::good,
             th::outageBand, th::s1_blue,   th::s2_green, th::s3_magenta, th::s4_yellow, th::s5_aqua,
             th::s6_orange,  th::s7_violet, th::s8_red,   th::nav,        th::hover}};
}

}  // namespace dl
