// app_context.h — 单窗口 Win32 应用的共享句柄、资源与当前文档
#pragma once

#include <windows.h>
#include <commctrl.h>

#include "document_state.h"

namespace dl {

struct AppContext {
    HWND hMain = nullptr, hNav = nullptr, hStatus = nullptr;
    HWND hPageTitle = nullptr, hFileLbl = nullptr;
    HWND hOpen = nullptr, hPaste = nullptr, hFilterToggle = nullptr, hCloseLog = nullptr;
    HWND hBookmarks = nullptr;
    HWND hTagBox = nullptr, hGrepBox = nullptr, hSinceBox = nullptr, hUntilBox = nullptr;
    HWND hTagLabel = nullptr, hGrepLabel = nullptr, hSinceLabel = nullptr, hUntilLabel = nullptr;
    HWND hApplyFilter = nullptr, hClearFilter = nullptr, hSearchHistory = nullptr;
    HWND hSummary = nullptr, hTimeline = nullptr, hOutage = nullptr, hMetric = nullptr;
    HWND hTags = nullptr, hRaw = nullptr, hChart = nullptr, hExport = nullptr, hDash = nullptr;
    HWND hFindings = nullptr, hUnparsed = nullptr;
    HIMAGELIST hTableRows = nullptr;

    HFONT hFontUI = nullptr, hFontMono = nullptr, hFontTitle = nullptr, hFontSmall = nullptr;
    HFONT hFontHero = nullptr, hFontTileVal = nullptr, hFontTileLbl = nullptr, hFontSect = nullptr;

    DocumentState document;
    int dpi = 96;
};

AppContext& App();

inline int Scale(int logical) { return MulDiv(logical, App().dpi, 96); }
inline int ScaleFont(int logicalNegative) {
    return -MulDiv(-logicalNegative, App().dpi, 96);
}
inline int S(int logical) { return Scale(logical); }
inline int SF(int logicalNegative) { return ScaleFont(logicalNegative); }

} // namespace dl
