// 最小 Win32 GUI 烟测：启动真实 exe，等待后台加载完成，并核对导航、指标列与设置持久化。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "../../version.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

std::wstring TextOf(HWND window) {
    // EDIT 的内容需要标准消息封送；其他控件直接读取窗口文字，避免为轮询
    // 后台加载按钮而向目标 UI 线程同步发送消息。
    wchar_t className[32]{};
    GetClassNameW(window, className, 32);
    const bool edit = _wcsicmp(className, L"EDIT") == 0 || _wcsicmp(className, L"COMBOBOX") == 0;
    const int length =
        edit ? static_cast<int>(SendMessageW(window, WM_GETTEXTLENGTH, 0, 0)) : GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied =
        edit ? static_cast<int>(SendMessageW(window, WM_GETTEXT, length + 1, reinterpret_cast<LPARAM>(&text[0])))
             : GetWindowTextW(window, &text[0], length + 1);
    text.resize(static_cast<size_t>(copied));
    return text;
}

bool Contains(HWND window, const wchar_t* text) {
    return TextOf(window).find(text) != std::wstring::npos;
}

void PrintWide(const char* name, const std::wstring& value) {
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string text(name);
    text += ": ";
    const size_t offset = text.size();
    text.resize(offset + static_cast<size_t>(length));
    if (length > 0)
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &text[offset], length, nullptr,
                            nullptr);
    text += "\r\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

std::wstring ClipboardText() {
    if (!OpenClipboard(nullptr))
        return L"";
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    const auto* value = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
    std::wstring result = value ? value : L"";
    if (value)
        GlobalUnlock(handle);
    CloseClipboard();
    return result;
}

std::vector<DWORD> ClientPixels(HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    HDC screen = GetDC(window), dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right, rect.bottom);
    auto previous = SelectObject(dc, bitmap);
    BitBlt(dc, 0, 0, rect.right, rect.bottom, screen, 0, 0, SRCCOPY);
    SelectObject(dc, previous);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<DWORD> pixels(static_cast<size_t>(rect.right) * rect.bottom);
    const bool read = GetDIBits(screen, bitmap, 0, rect.bottom, pixels.data(), &info, DIB_RGB_COLORS) != 0;
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(window, screen);
    if (!read)
        pixels.clear();
    for (auto& pixel : pixels)
        pixel &= 0xffffff;  // The unused alpha byte is not part of a GDI screenshot.
    return pixels;
}

bool CheckHoverRestore(HWND window, POINT inside, const wchar_t* label) {
    struct RestorePointer {
        POINT saved{};

        RestorePointer() {
            GetCursorPos(&saved);
        }

        ~RestorePointer() {
            SetCursorPos(saved.x, saved.y);
        }
    } pointer;

    POINT position{2, 2};
    ClientToScreen(window, &position);
    SetCursorPos(position.x, position.y);
    Sleep(30);

    SendMessageW(window, WM_MOUSELEAVE, 0, 0);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    const auto before = ClientPixels(window);
    if (before.empty())
        return false;

    RECT client{}, dirty{};
    GetClientRect(window, &client);
    position = inside;
    ClientToScreen(window, &position);
    SetCursorPos(position.x, position.y);
    const DWORD started = GetTickCount();
    int observedRegions = 0;
    for (int i = 0; i < 80; ++i) {
        SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(inside.x + i % 16, inside.y));
        if (GetUpdateRect(window, &dirty, FALSE)) {
            ++observedRegions;
            // Pending hover paints may coalesce; they must still cover less
            // than the complete control. A flushed queue is also valid.
            if (EqualRect(&dirty, &client))
                return false;
        }
        UpdateWindow(window);
    }
    PrintWide("hover-move-ms", std::wstring(label) + L": " + std::to_wstring(GetTickCount() - started));
    PrintWide("hover-observed-regions", std::wstring(label) + L": " + std::to_wstring(observedRegions));

    const auto hovered = ClientPixels(window);
    if (hovered == before)
        return false;  // A suppressed repaint must not hide the actual cursor.
    SendMessageW(window, WM_MOUSELEAVE, 0, 0);
    UpdateWindow(window);
    if (ClientPixels(window) != before)
        return false;  // No cursor, tooltip or old highlight may remain.

    // Empty space has no hover state to animate or repaint.
    position = {2, 2};
    ClientToScreen(window, &position);
    SetCursorPos(position.x, position.y);
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(2, 2));
    UpdateWindow(window);
    if (ClientPixels(window) != before)
        return false;
    PrintWide("hover-restored", std::wstring(label) + L": PASS");
    return true;
}

// 仅在显式指定目录时保存实际窗口截图，便于核验自绘文字与小窗口布局。
void Capture(HWND window, const wchar_t* name, bool refresh = true) {
    // Settle painting before subsequent clicks, even when image saving is off.
    // Self-drawn copy buttons measure their hit rectangles during WM_PAINT.
    if (!IsWindow(window))
        return;
    HWND notice = FindWindowW(L"dialModernNotice", nullptr);
    DWORD targetProcess = 0, noticeProcess = 0;
    GetWindowThreadProcessId(window, &targetProcess);
    if (notice)
        GetWindowThreadProcessId(notice, &noticeProcess);
    if (notice && noticeProcess == targetProcess)
        SendMessageW(notice, WM_LBUTTONUP, 0, 0);
    if (refresh) {
        Sleep(100);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    wchar_t directory[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"DIALLOG_UI_CAPTURE", directory, MAX_PATH))
        return;
    CreateDirectoryW(directory, nullptr);
    RECT rect{};
    GetWindowRect(window, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    // Capture the composed desktop: a child DC contains undefined pixels where
    // an overlapping tooltip obscures it, which can produce a false black box.
    HDC screen = GetDC(nullptr), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY);
    SelectObject(memory, old);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    GetDIBits(screen, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    std::wstring path = std::wstring(directory) + L"\\" + name + L".bmp";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, &header, sizeof(header), &written, nullptr);
        WriteFile(file, &info.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr);
        WriteFile(file, pixels.data(), static_cast<DWORD>(pixels.size()), &written, nullptr);
        CloseHandle(file);
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

struct WindowSearch {
    DWORD processId = 0;
    HWND window = nullptr;
    const wchar_t* className = L"dialLogMainCls";
};

bool CheckMainFrame(HWND window) {
    HWND caption = FindWindowExW(window, nullptr, L"dialMainCaption", nullptr);
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
    if (contrast.dwFlags & HCF_HIGHCONTRASTON)
        return caption && !IsWindowVisible(caption);
    HWND navigation = GetDlgItem(window, 1001);
    if (!caption || !IsWindowVisible(caption) || !navigation)
        return false;
    auto waitState = [window](bool minimized, bool maximized) {
        for (int attempt = 0; attempt < 200; ++attempt) {
            if ((IsIconic(window) != FALSE) == minimized && (IsZoomed(window) != FALSE) == maximized)
                return true;
            Sleep(10);
        }
        return false;
    };
    SendMessageW(window, WM_SYSCOMMAND, SC_RESTORE, 0);
    if (!waitState(false, false))
        return false;
    SetWindowPos(window, nullptr, 0, 0, 1280, 800, SWP_NOZORDER);
    Capture(window, L"caption-initial");
    RECT nav{}, title{}, bounds{};
    GetWindowRect(navigation, &nav);
    GetWindowRect(caption, &title);
    GetWindowRect(window, &bounds);
    if (nav.left < title.left || nav.left - title.left > 16 || nav.top != title.bottom) {
        PrintWide("caption-geometry", std::to_wstring(nav.left) + L"," + std::to_wstring(nav.top) + L" / " +
                                          std::to_wstring(title.left) + L"," + std::to_wstring(title.bottom));
        return false;
    }
    HDC dc = GetDC(nullptr);
    // The left caption must continue the navigation color; the right caption
    // must match the original header rather than tinting the complete row.
    const COLORREF leftCaption = GetPixel(dc, nav.right - 18, title.top + (title.bottom - title.top) / 2);
    const COLORREF leftNav = GetPixel(dc, nav.right - 18, nav.top + 4);
    const COLORREF rightCaption = GetPixel(dc, nav.right + 150, title.top + 4);
    const COLORREF rightHeader = GetPixel(dc, nav.right + 150, title.bottom + 4);
    ReleaseDC(nullptr, dc);
    if (leftCaption == CLR_INVALID || leftCaption != leftNav || rightCaption != rightHeader ||
        leftCaption == rightCaption) {
        PrintWide("caption-colors", std::to_wstring(leftCaption) + L"," + std::to_wstring(leftNav) + L" / " +
                                        std::to_wstring(rightCaption) + L"," + std::to_wstring(rightHeader));
        return false;
    }
    const LPARAM dragPoint = MAKELPARAM(title.left + (title.right - title.left) / 2, title.top + 16);
    if (SendMessageW(window, WM_NCHITTEST, 0, dragPoint) != HTCAPTION ||
        SendMessageW(window, WM_NCHITTEST, 0, MAKELPARAM(bounds.left + 1, bounds.top + 1)) != HTTOPLEFT) {
        PrintWide("caption-hit-test", std::to_wstring(SendMessageW(window, WM_NCHITTEST, 0, dragPoint)) + L" / " +
                                          std::to_wstring(SendMessageW(window, WM_NCHITTEST, 0,
                                                                       MAKELPARAM(bounds.left + 1, bounds.top + 1))));
        return false;
    }

    HWND maximize = GetDlgItem(caption, 39002);
    SendMessageW(navigation, WM_LBUTTONUP, 0, 0);
    GUITHREADINFO focusBefore{};
    focusBefore.cbSize = sizeof(focusBefore);
    const DWORD guiThread = GetWindowThreadProcessId(window, nullptr);
    if (!GetGUIThreadInfo(guiThread, &focusBefore) || focusBefore.hwndFocus != navigation)
        return false;
    RECT maximizeBounds{};
    GetWindowRect(maximize, &maximizeBounds);
    const LPARAM maximizePoint =
        MAKELPARAM((maximizeBounds.left + maximizeBounds.right) / 2, (maximizeBounds.top + maximizeBounds.bottom) / 2);
    if (SendMessageW(window, WM_NCHITTEST, 0, maximizePoint) != HTMAXBUTTON)
        return false;
    SendMessageW(window, WM_NCLBUTTONDOWN, HTMAXBUTTON, maximizePoint);
    SendMessageW(
        maximize, WM_LBUTTONUP, 0,
        MAKELPARAM((maximizeBounds.right - maximizeBounds.left) / 2, (maximizeBounds.bottom - maximizeBounds.top) / 2));
    if (!waitState(false, true))
        return false;
    GUITHREADINFO focusAfter{};
    focusAfter.cbSize = sizeof(focusAfter);
    if (!GetGUIThreadInfo(guiThread, &focusAfter) || focusAfter.hwndFocus != focusBefore.hwndFocus)
        return false;
    Capture(caption, L"caption-maximized");
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    GetWindowRect(caption, &title);
    if (title.left < monitor.rcWork.left || title.right > monitor.rcWork.right || title.top < monitor.rcWork.top) {
        PrintWide("caption-maximized-bounds", std::to_wstring(title.left) + L"," + std::to_wstring(title.top) + L"," +
                                                  std::to_wstring(title.right));
        return false;
    }
    SendMessageW(GetDlgItem(caption, 39002), BM_CLICK, 0, 0);
    if (!waitState(false, false))
        return false;
    SendMessageW(GetDlgItem(caption, 39001), BM_CLICK, 0, 0);
    if (!waitState(true, false))
        return false;
    SendMessageW(window, WM_SYSCOMMAND, SC_RESTORE, 0);
    if (!waitState(false, false))
        return false;
    Capture(window, L"caption-restored");
    PrintWide("caption-pass", L"split background, native drag/resize hit targets, maximize/restore and minimize");
    return true;
}

BOOL CALLBACK FindProcessWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    wchar_t className[64]{};
    GetClassNameW(window, className, 64);
    if (processId == search.processId && std::wstring(className) == search.className && IsWindowVisible(window)) {
        search.window = window;
        return FALSE;
    }
    return TRUE;
}

HWND WaitForMain(DWORD processId, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        WindowSearch search{processId, nullptr};
        EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
        if (search.window)
            return search.window;
        Sleep(50);
    }
    return nullptr;
}

bool AppearanceFitsNavigation(HWND window) {
    HWND button = GetDlgItem(window, 1135), navigation = GetDlgItem(window, 1001);
    RECT settings{}, sidebar{};
    if (!button || !navigation || !IsWindowVisible(button) || !GetWindowRect(button, &settings) ||
        !GetWindowRect(navigation, &sidebar))
        return false;
    POINT center{(settings.left + settings.right) / 2, (settings.top + settings.bottom) / 2};
    return settings.left > sidebar.left && settings.right < sidebar.right &&
           settings.top > sidebar.top + (sidebar.bottom - sidebar.top) / 2 && settings.bottom < sidebar.bottom &&
           WindowFromPoint(center) == button;
}

bool CheckAppearanceMenu(HWND window, DWORD processId, const wchar_t* captureName) {
    PostMessageW(GetDlgItem(window, 1135), BM_CLICK, 0, 0);
    WindowSearch search{processId, nullptr, L"#32768"};
    const DWORD start = GetTickCount();
    while (!search.window && GetTickCount() - start < 3000) {
        EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
        if (!search.window)
            Sleep(25);
    }
    if (!search.window)
        return false;
    RECT menu{}, button{};
    GetWindowRect(search.window, &menu);
    GetWindowRect(GetDlgItem(window, 1135), &button);
    bool above = menu.bottom <= button.top && menu.top >= 0;
    const DWORD placement = GetTickCount();
    while (!above && GetTickCount() - placement < 1500) {
        Sleep(25);
        GetWindowRect(search.window, &menu);
        GetWindowRect(GetDlgItem(window, 1135), &button);
        above = menu.bottom <= button.top && menu.top >= 0;
    }
    Capture(window, captureName);
    Capture(search.window, (std::wstring(captureName) + L"-popup").c_str());
    SendMessageW(window, WM_CANCELMODE, 0, 0);
    return above;
}

bool ChooseTypeface(HWND window, DWORD processId, bool logFace, const wchar_t* name) {
    PostMessageW(window, WM_COMMAND, MAKEWPARAM(logFace ? 1137 : 1136, 0), 0);
    const DWORD start = GetTickCount();
    HWND dialog = nullptr;
    while (!dialog && GetTickCount() - start < 10000) {
        WindowSearch search{processId, nullptr, L"#32770"};
        EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
        // 模态框可先被枚举到，再完成 WM_INITDIALOG 和字体列表填充。
        if (search.window && IsWindowVisible(search.window) && GetDlgItem(search.window, 0x470))
            dialog = search.window;
        if (!dialog)
            Sleep(50);
    }
    if (!dialog) {
        PrintWide("font-dialog-missing", name);
        return false;
    }
    HWND family = GetDlgItem(dialog, 0x470);  // 标准 CHOOSEFONT 的 cmb1
    if (!family) {
        SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
        return false;
    }
    const wchar_t* candidates[] = {logFace ? name : L"Noto Sans CJK SC", name,
                                   logFace ? L"Liberation Mono" : L"Liberation Sans"};
    std::wstring chosen;
    for (const wchar_t* candidate : candidates) {
        const LRESULT index =
            SendMessageW(family, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(candidate));
        if (index != CB_ERR) {
            SendMessageW(family, CB_SETCURSEL, index, 0);
            chosen = candidate;
            break;
        }
    }
    if (chosen.empty()) {
        PrintWide("font-not-listed", name);
        SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
        return false;
    }
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(0x470, CBN_SELCHANGE), reinterpret_cast<LPARAM>(family));
    Capture(dialog, logFace ? L"font-dialog-log" : L"font-dialog-ui");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    wchar_t saved[LF_FACESIZE]{};
    const DWORD wait = GetTickCount();
    while (GetTickCount() - wait < 10000) {
        DWORD bytes = sizeof(saved);
        const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\dialLog", logFace ? L"LogFont" : L"UiFont",
                                            RRF_RT_REG_SZ, nullptr, saved, &bytes);
        if (status == ERROR_SUCCESS && std::wstring(saved) == chosen)
            return true;
        Sleep(50);
    }
    PrintWide("font-saved", saved);
    return false;
}

bool WaitForLoad(HWND window, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        // 枚举到主窗口时 WM_CREATE 可能还没建完子控件；不能永久缓存空句柄。
        HWND label = GetDlgItem(window, 1010);
        HWND close = GetDlgItem(window, 1030);
        DWORD_PTR idle = 0;
        const bool settled = SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &idle) != 0;
        if (settled && label && close && !Contains(label, L"未加载") && TextOf(close) == L"关闭日志")
            return true;
        Sleep(50);
    }
    return false;
}

std::wstring CreateLargeLog() {
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"dlg", 0, path))
        return L"";
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return L"";
    const std::string line = "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | Cell:1D8DE0B | CSQ:18 | RX_PKT:100\r\n";
    std::string block;
    while (block.size() < 1024 * 1024)
        block += line;
    bool good = true;
    for (int index = 0; index < 64; ++index) {
        DWORD written = 0;
        if (!WriteFile(file, block.data(), static_cast<DWORD>(block.size()), &written, nullptr) ||
            written != block.size()) {
            good = false;
            break;
        }
    }
    CloseHandle(file);
    if (!good) {
        DeleteFileW(path);
        return L"";
    }
    return path;
}

std::string ReadBytes(const std::wstring& path) {
    HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    DWORD size = GetFileSize(file, nullptr), read = 0;
    std::string data(size, '\0');
    if (size)
        ReadFile(file, &data[0], size, &read, nullptr);
    CloseHandle(file);
    data.resize(read);
    return data;
}

BOOL CALLBACK FindFilenameControl(HWND child, LPARAM parameter) {
    const int id = GetDlgCtrlID(child);
    if (id == 0x480 || id == 0x47c) {
        *reinterpret_cast<HWND*>(parameter) = child;
        return FALSE;
    }
    return TRUE;
}

HWND FilenameControl(HWND dialog) {
    HWND control = nullptr;
    EnumChildWindows(dialog, FindFilenameControl, reinterpret_cast<LPARAM>(&control));
    return control;
}

BOOL CALLBACK FindInnerEdit(HWND child, LPARAM parameter) {
    wchar_t cls[64]{};
    GetClassNameW(child, cls, 64);
    if (_wcsicmp(cls, L"EDIT") == 0) {
        *reinterpret_cast<HWND*>(parameter) = child;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK FindFileDialog(HWND candidate, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(candidate, &processId);
    wchar_t className[64]{};
    GetClassNameW(candidate, className, 64);
    if (processId == search.processId && IsWindowVisible(candidate) && lstrcmpW(className, L"#32770") == 0 &&
        FilenameControl(candidate)) {
        search.window = candidate;
        return FALSE;
    }
    return TRUE;
}

bool ChooseFile(HWND window, DWORD processId, int command, const std::wstring& path) {
    if (!IsWindow(window)) {
        PrintWide("file-dialog-owner-destroyed", path);
        return false;
    }
    PostMessageW(window, WM_COMMAND, command, 0);
    HWND dialog = nullptr;
    const DWORD start = GetTickCount();
    while (!dialog && GetTickCount() - start < 15000) {
        WindowSearch search{processId, nullptr, L"#32770"};
        EnumWindows(FindFileDialog, reinterpret_cast<LPARAM>(&search));
        dialog = search.window;
        if (!dialog)
            Sleep(50);
    }
    if (!dialog) {
        PrintWide("export-dialog-missing", path);
        PrintWide("file-dialog-owner-state", !IsWindow(window)         ? L"destroyed"
                                             : IsWindowEnabled(window) ? L"enabled"
                                                                       : L"disabled");
        if (IsWindow(window))
            Capture(window, L"file-dialog-missing-owner");
        WindowSearch pending{processId, nullptr, L"#32770"};
        EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&pending));
        if (pending.window) {
            PrintWide("file-dialog-pending-title", TextOf(pending.window));
            Capture(pending.window, L"file-dialog-missing-pending");
        }
        return false;
    }
    if (command == 1310 && IsWindowEnabled(window)) {
        PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
        return false;
    }
    // Explorer dialogs may pump messages while their initial shell folder is still being built.
    Sleep(750);
    SetForegroundWindow(dialog);
    HWND filename = FilenameControl(dialog);
    HWND filenameEdit = nullptr;
    EnumChildWindows(filename, FindInnerEdit, reinterpret_cast<LPARAM>(&filenameEdit));
    SetWindowTextW(filenameEdit ? filenameEdit : filename, path.c_str());
    Capture(dialog, command == 1040 ? L"open-cancellation-file-dialog" : L"english-export-dialog");
    SendMessageW(GetDlgItem(dialog, IDOK), BM_CLICK, 0, 0);
    const DWORD closing = GetTickCount();
    while (IsWindow(dialog) && GetTickCount() - closing < 10000)
        Sleep(50);
    if (IsWindow(dialog)) {
        Capture(dialog, L"file-dialog-did-not-close");
        PrintWide("file-dialog-did-not-close", TextOf(filenameEdit ? filenameEdit : filename));
        PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
        return false;
    }
    return true;
}

bool ExportTo(HWND window, DWORD processId, int command, const std::wstring& path) {
    if (!ChooseFile(window, processId, command, path))
        return false;
    const DWORD wait = GetTickCount();
    while (GetTickCount() - wait < 15000) {
        if (!ReadBytes(path).empty())
            return true;
        Sleep(50);
    }
    PrintWide("export-file-missing", path);
    return false;
}

HWND OwnedWindow(DWORD processId, const wchar_t* name) {
    WindowSearch search{processId, nullptr, name};
    EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.window) {
        auto parent = WaitForMain(processId, 100);
        if (parent)
            EnumChildWindows(parent, FindProcessWindow, reinterpret_cast<LPARAM>(&search));
    }
    return search.window;
}

void SelectFirstRow(HWND list) {
    RECT header{};
    POINT origin{};
    GetWindowRect(ListView_GetHeader(list), &header);
    ClientToScreen(list, &origin);
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(24, header.bottom - origin.y + 10));
    SendMessageW(list, WM_LBUTTONUP, 0, MAKELPARAM(24, header.bottom - origin.y + 10));
}

int CheckIncident(HWND window, DWORD processId, bool secondSource = false) {
    SendMessageW(window, WM_APP + 41, 3, 0);
    HWND outages = GetDlgItem(window, 1013);
    SelectFirstRow(outages);
    SendMessageW(window, WM_COMMAND, 1180, 0);
    HWND review = OwnedWindow(processId, L"dialIncidentReview");
    if (!review)
        return 140;
    if (secondSource) {
        Sleep(650);  // New-input clearing must not leave a delayed refresh that closes the review.
        if (!IsWindow(review)) {
            PrintWide("incident-closed-after-new-load", L"650 ms after opening");
            return 163;
        }
    }
    HWND list = GetDlgItem(review, 1306);
    const auto summary = TextOf(GetDlgItem(review, 1304));
    if (summary.find(secondSource ? L"7s" : L"30s") == std::wstring::npos ||
        summary.find(secondSource ? L"原文件行 2 · 合并行 5" : L"原文件行 25 · 合并行 25") == std::wstring::npos) {
        PrintWide("incident-summary", summary);
        return 141;
    }
    if (ListView_GetItemCount(list) != (secondSource ? 2 : 9) || Header_GetItemCount(ListView_GetHeader(list)) != 6)
        return 142;
    Capture(review, secondSource ? L"incident-source-B" : L"incident-events-zh");
    HWND tab = GetDlgItem(review, 1305);
    // Click the actual tab: WM_NOTIFY is restricted to same-process senders.
    SendMessageW(tab, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(150, 12));
    SendMessageW(tab, WM_LBUTTONUP, 0, MAKELPARAM(150, 12));
    if (ListView_GetItemCount(list) != (secondSource ? 1 : 22) || Header_GetItemCount(ListView_GetHeader(list)) != 13) {
        PrintWide("incident-metric-count", std::to_wstring(ListView_GetItemCount(list)) + L" columns=" +
                                               std::to_wstring(Header_GetItemCount(ListView_GetHeader(list))));
        Capture(review, L"incident-metrics-failure");
        return 144;
    }
    Capture(review, secondSource ? L"incident-source-B-metrics" : L"incident-metrics-zh");
    if (!secondSource) {
        SetWindowPos(review, nullptr, 0, 0, 780, 700, SWP_NOMOVE | SWP_NOZORDER);
        Capture(review, L"incident-metrics-narrow-zh");
        SetWindowPos(review, nullptr, 0, 0, 1120, 880, SWP_NOMOVE | SWP_NOZORDER);
    }
    SendMessageW(review, WM_COMMAND, 1309, 0);
    const auto copied = ClipboardText();
    if (copied.find(L"事件复盘") == std::wstring::npos ||
        copied.find(L"不能直接当作单次事故根因") == std::wstring::npos)
        return 145;
    if (!secondSource) {
        SendMessageW(review, WM_COMMAND, 1301, 0);
        if (ListView_GetItemCount(list) != 20 || !Contains(GetDlgItem(review, 1304), L"原文件行 103"))
            return 146;
        SendMessageW(review, WM_COMMAND, 1300, 0);
        if (ListView_GetItemCount(list) != 22)
            return 147;
        wchar_t path[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"DIALLOG_UI_EVIDENCE_PATH", path, MAX_PATH)) {
            DeleteFileW(path);
            if (!ExportTo(review, processId, 1310, path) || !IsWindowEnabled(review))
                return 148;
        }
        // A model refresh must close every borrowed review before replacing data.
        SendMessageW(window, WM_COMMAND, 1003, 0);
        if (IsWindow(review))
            return 149;
        SendMessageW(window, WM_APP + 41, 3, 0);
        SelectFirstRow(outages);
        SendMessageW(window, WM_COMMAND, 1180, 0);
        review = OwnedWindow(processId, L"dialIncidentReview");
        if (!review)
            return 157;
        SendMessageW(review, WM_COMMAND, 1311, 0);
        if (IsWindow(review) || !Contains(GetDlgItem(window, 1024), L"原始日志"))
            return 158;
        SendMessageW(GetDlgItem(window, 1016), WM_COPY, 0, 0);
        if (ClipboardText().find(L"8\t2026-06-30 00:01:50") == std::wstring::npos)
            return 159;
    } else {
        // Source switching must also invalidate the old review.
        SendMessageW(window, WM_COMMAND, 34000, 0);
        if (IsWindow(review))
            return 150;
        SendMessageW(window, WM_COMMAND, 34001, 0);
    }
    return 0;
}

int WorkspaceSmoke(HWND window, DWORD processId, const std::wstring& base) {
    SetWindowPos(window, nullptr, 0, 0, 1400, 1100, SWP_NOMOVE | SWP_NOZORDER);
    SendMessageW(window, WM_COMMAND, 33002, 0);
    HWND work = OwnedWindow(processId, L"dialWorkspace");
    if (!work || ListView_GetItemCount(GetDlgItem(work, 1401)) != 2)
        return 180;
    SelectFirstRow(GetDlgItem(work, 1401));
    SetWindowTextW(GetDlgItem(work, 1402), L"现场设备 A");
    SetWindowTextW(GetDlgItem(work, 1403), L"FW-A");
    SetWindowTextW(GetDlgItem(work, 1404), L"APN=before");
    SendMessageW(work, WM_COMMAND, 1405, 0);
    work = OwnedWindow(processId, L"dialWorkspace");
    if (!work)
        return 229;
    HWND list = GetDlgItem(work, 1401);
    SelectFirstRow(list);
    SendMessageW(list, WM_KEYDOWN, VK_DOWN, 0);
    SendMessageW(list, WM_KEYUP, VK_DOWN, 0);
    SetWindowTextW(GetDlgItem(work, 1402), L"现场设备 B");
    SetWindowTextW(GetDlgItem(work, 1403), L"FW-B");
    SetWindowTextW(GetDlgItem(work, 1404), L"APN=after");
    SendMessageW(work, WM_COMMAND, 1405, 0);
    work = OwnedWindow(processId, L"dialWorkspace");
    if (!work)
        return 230;
    Capture(work, L"workspace-devices-zh");
    SendMessageW(work, WM_COMMAND, 1461, 0);
    HWND tabs = GetDlgItem(work, 1400);
    if (TabCtrl_GetCurSel(tabs) != 1 || !IsWindowVisible(GetDlgItem(work, 1410)))
        return 181;
    // Rounded tab painting must preserve the native keyboard selection and notifications.
    SendMessageW(tabs, WM_KEYDOWN, VK_LEFT, 0);
    SendMessageW(tabs, WM_KEYUP, VK_LEFT, 0);
    if (TabCtrl_GetCurSel(tabs) != 0 || !IsWindowVisible(GetDlgItem(work, 1473)))
        return 250;
    SendMessageW(tabs, WM_KEYDOWN, VK_RIGHT, 0);
    SendMessageW(tabs, WM_KEYUP, VK_RIGHT, 0);
    if (TabCtrl_GetCurSel(tabs) != 1 || !IsWindowVisible(GetDlgItem(work, 1410)))
        return 251;
    SendDlgItemMessageW(work, 1411, CB_SETCURSEL, 1, 0);
    SendMessageW(work, WM_COMMAND, MAKEWPARAM(1411, CBN_SELCHANGE), reinterpret_cast<LPARAM>(GetDlgItem(work, 1411)));
    SendMessageW(work, WM_COMMAND, 1416, 0);
    const auto comparison = TextOf(GetDlgItem(work, 1420));
    PrintWide("workspace-comparison", comparison);
    if (comparison.find(L"-35.000") == std::wstring::npos || comparison.find(L"FW-A") == std::wstring::npos ||
        comparison.find(L"FW-B") == std::wstring::npos ||
        comparison.find(L"不同设备或身份未确认") == std::wstring::npos)
        return 182;
    SendMessageW(work, WM_COMMAND, 1417, 0);
    if (ClipboardText() != comparison)
        return 183;
    SendDlgItemMessageW(work, 1422, CB_SETCURSEL, 4, 0);
    SendMessageW(work, WM_COMMAND, MAKEWPARAM(1422, CBN_SELCHANGE), reinterpret_cast<LPARAM>(GetDlgItem(work, 1422)));
    Capture(work, L"workspace-comparison-rssi-zh");
    SetWindowPos(window, nullptr, 0, 0, 1000, 1000, SWP_NOMOVE | SWP_NOZORDER);
    Capture(work, L"workspace-comparison-narrow-zh");
    HWND plot = GetDlgItem(work, 1421);
    if (!CheckHoverRestore(plot, POINT{80, 100}, L"workbench"))
        return 252;
    RECT plotRect{};
    GetClientRect(plot, &plotRect);
    SendMessageW(plot, WM_MOUSEMOVE, 0, MAKELPARAM(56, 100));
    const auto allSignals = TextOf(plot);
    PrintWide("workspace-hover", allSignals);
    for (const wchar_t* field : {L"RSRP", L"RSRQ", L"SNR", L"RSSI", L"CSQ", L"2026-08-03", L"Δ(B−A)", L"12.0 dB",
                                 L"-2.0 dB", L"-95 dBm", L"-8 dB", L"-100 dBm"})
        if (allSignals.find(field) == std::wstring::npos)
            return 210;
    POINT at{plotRect.right / 2, 100};
    ClientToScreen(plot, &at);
    SendMessageW(plot, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), MAKELPARAM(at.x, at.y));
    if (!IsWindowEnabled(GetDlgItem(work, 1463)))
        return 211;
    SendMessageW(plot, WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(200, 100));
    SendMessageW(plot, WM_MOUSEMOVE, MK_MBUTTON, MAKELPARAM(250, 100));
    SendMessageW(plot, WM_MBUTTONUP, 0, MAKELPARAM(250, 100));
    SendMessageW(work, WM_COMMAND, 1463, 0);
    if (IsWindowEnabled(GetDlgItem(work, 1463)))
        return 212;
    SendMessageW(plot, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(120, 100));
    SendMessageW(plot, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(400, 100));
    SendMessageW(plot, WM_LBUTTONUP, 0, MAKELPARAM(400, 100));
    if (!IsWindowEnabled(GetDlgItem(work, 1463)))
        return 213;
    SendMessageW(work, WM_COMMAND, 1464, 0);
    if (!Contains(GetDlgItem(window, 1010), L"_A.log") || !IsWindowEnabled(GetDlgItem(window, 1141)))
        return 214;
    SendMessageW(window, WM_COMMAND, 1141, 0);
    SendMessageW(window, WM_COMMAND, 33002, 0);
    work = OwnedWindow(processId, L"dialWorkspace");
    SendMessageW(work, WM_COMMAND, 1461, 0);
    if (!ExportTo(work, processId, 1418, base + L".comparison.html"))
        return 184;
    SendMessageW(window, WM_APP + 41, 9, 0);
    work = OwnedWindow(processId, L"dialWorkspace");
    HWND top = GetDlgItem(work, 1400);
    SendMessageW(top, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(40, 12));
    SendMessageW(top, WM_LBUTTONUP, 0, MAKELPARAM(40, 12));
    if (ListView_GetItemCount(GetDlgItem(work, 1401)) != 2 || ListView_GetItemCount(GetDlgItem(work, 1482)) != 2 ||
        !Contains(GetDlgItem(work, 1483), L"缺少明确证据"))
        return 215;
    SendMessageW(work, WM_COMMAND, 1484, 0);
    if (ClipboardText().find(L"恢复动作") == std::wstring::npos)
        return 216;
    SetWindowTextW(GetDlgItem(work, 1486), L"5");
    Sleep(100);
    if (ListView_GetItemCount(GetDlgItem(work, 1401)) != 1)
        return 217;
    SetWindowTextW(GetDlgItem(work, 1486), L"0");
    SendMessageW(work, WM_COMMAND, 1477, 0);
    SendDlgItemMessageW(work, 1471, CB_SETCURSEL, 2, 0);
    SendMessageW(work, WM_COMMAND, MAKEWPARAM(1471, CBN_SELCHANGE), 0);
    if (ListView_GetItemCount(GetDlgItem(work, 1401)) != 1)
        return 218;
    SendDlgItemMessageW(work, 1471, CB_SETCURSEL, 0, 0);
    SendMessageW(work, WM_COMMAND, MAKEWPARAM(1471, CBN_SELCHANGE), 0);
    SendMessageW(work, WM_COMMAND, 1479, 0);
    if (ClipboardText().find(L"duration_s") == std::wstring::npos)
        return 219;
    HWND board = GetDlgItem(work, 1401);
    SelectFirstRow(board);
    RECT boardHeader{};
    POINT boardOrigin{};
    GetWindowRect(ListView_GetHeader(board), &boardHeader);
    ClientToScreen(board, &boardOrigin);
    SendMessageW(board, WM_LBUTTONDOWN, MK_CONTROL | MK_LBUTTON,
                 MAKELPARAM(24, boardHeader.bottom - boardOrigin.y + 36));
    SendMessageW(board, WM_LBUTTONUP, MK_CONTROL, MAKELPARAM(24, boardHeader.bottom - boardOrigin.y + 36));
    if (ListView_GetSelectedCount(board) != 2)
        return 231;
    SendMessageW(work, WM_COMMAND, 1478, 0);
    SendDlgItemMessageW(work, 1471, CB_SETCURSEL, 3, 0);
    SendMessageW(work, WM_COMMAND, MAKEWPARAM(1471, CBN_SELCHANGE), 0);
    if (ListView_GetItemCount(board) != 2)
        return 232;
    SendDlgItemMessageW(work, 1471, CB_SETCURSEL, 0, 0);
    SendMessageW(work, WM_COMMAND, MAKEWPARAM(1471, CBN_SELCHANGE), 0);
    PostMessageW(board, WM_KEYDOWN, VK_F2, 0);
    HWND shortcutEditor = nullptr;
    const auto shortcutStart = GetTickCount();
    while (!shortcutEditor && GetTickCount() - shortcutStart < 5000) {
        shortcutEditor = OwnedWindow(processId, L"dialManualReview");
        if (!shortcutEditor)
            Sleep(50);
    }
    if (!shortcutEditor)
        return 236;
    SetWindowTextW(GetDlgItem(shortcutEditor, 1452), L"F2 现场人工备注");
    SendMessageW(shortcutEditor, WM_COMMAND, 1453, 0);
    if (IsWindow(shortcutEditor) || !Contains(GetDlgItem(work, 1483), L"F2 现场人工备注"))
        return 237;
    Capture(work, L"workspace-event-chain-zh");
    SendMessageW(work, WM_COMMAND, 1462, 0);
    SetWindowTextW(GetDlgItem(work, 1501), L"2026-08-03 10:00:00");
    SetWindowTextW(GetDlgItem(work, 1502), L"2026-08-03 10:00:15");
    SendDlgItemMessageW(work, 1503, CB_SETCURSEL, 3, 0);
    SendMessageW(work, WM_COMMAND, MAKEWPARAM(1503, CBN_SELCHANGE), 0);
    SetWindowTextW(GetDlgItem(work, 1504), L"4");
    SendMessageW(work, WM_COMMAND, 1505, 0);
    if (!Contains(GetDlgItem(work, 1420), L"started_outages") ||
        !Contains(GetDlgItem(work, 1420), L"2026-08-03 10:00:12"))
        return 220;
    plot = GetDlgItem(work, 1421);
    SendMessageW(plot, WM_MOUSEMOVE, 0, MAKELPARAM(56, 100));
    if (!Contains(plot, L"n=1") || !Contains(plot, L"P50"))
        return 221;
    if (!ExportTo(work, processId, 1507, base + L".trend.html"))
        return 222;
    Capture(work, L"workspace-trends-zh");
    SendMessageW(work, WM_COMMAND, 1461, 0);
    SendDlgItemMessageW(work, 1411, CB_SETCURSEL, 1, 0);
    SendMessageW(work, WM_COMMAND, MAKEWPARAM(1411, CBN_SELCHANGE), 0);
    SendMessageW(work, WM_COMMAND, 1416, 0);
    plot = GetDlgItem(work, 1421);
    SendMessageW(plot, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(400, 100));
    if (!IsWindowEnabled(GetDlgItem(work, 1463)))
        return 233;
    top = GetDlgItem(work, 1400);
    SendMessageW(top, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(260, 12));
    SendMessageW(top, WM_LBUTTONUP, 0, MAKELPARAM(260, 12));
    if (TabCtrl_GetCurSel(top) != 2)
        return 223;
    SetWindowTextW(GetDlgItem(work, 1490), L"跨来源复核会话");
    const auto sessionZip = base + L".session.zip";
    if (!ExportTo(work, processId, 1491, sessionZip))
        return 224;
    DeleteFileW((base + L"_A.log").c_str());
    DeleteFileW((base + L"_B_现场.log").c_str());
    SendMessageW(window, WM_COMMAND, 1030, 0);
    SendMessageW(window, WM_APP + 41, 9, 0);
    work = OwnedWindow(processId, L"dialWorkspace");
    if (!work || !IsWindowEnabled(GetDlgItem(work, 1492)))
        return 234;
    if (!ChooseFile(work, processId, 1492, sessionZip))
        return 225;
    Sleep(1000);
    if (!WaitForLoad(window, 90000))
        return 226;
    work = OwnedWindow(processId, L"dialWorkspace");
    if (!work || !Contains(GetDlgItem(work, 1490), L"跨来源复核会话") || !Contains(GetDlgItem(work, 1493), L"2")) {
        PrintWide("session-workbench", work ? L"visible" : L"missing");
        PrintWide("session-name", TextOf(GetDlgItem(work, 1490)));
        PrintWide("session-info", TextOf(GetDlgItem(work, 1493)));
        PrintWide("session-status", TextOf(GetDlgItem(window, 1020)));
        PrintWide("session-source", TextOf(GetDlgItem(window, 1010)));
        Capture(window, L"workspace-session-restore-failure");
        return 227;
    }
    Capture(work, L"workspace-session-restored-zh");
    SendMessageW(work, WM_COMMAND, 1461, 0);
    if (!IsWindowEnabled(GetDlgItem(work, 1463)))
        return 235;
    SendMessageW(work, WM_COMMAND, 1416, 0);
    if (!Contains(GetDlgItem(work, 1420), L"-35.000"))
        return 228;
    SendMessageW(work, WM_CLOSE, 0, 0);
    SendMessageW(window, WM_APP + 41, 4, 0);
    SendMessageW(window, WM_COMMAND, 30000, 0);
    if (ListView_GetItemCount(GetDlgItem(window, 1014)) != 1 || !Contains(GetDlgItem(window, 1010), L"现场设备 A")) {
        PrintWide("restored-device-A", TextOf(GetDlgItem(window, 1010)));
        PrintWide("restored-metrics-count", std::to_wstring(ListView_GetItemCount(GetDlgItem(window, 1014))));
        return 185;
    }
    SendMessageW(window, WM_COMMAND, 30001, 0);
    if (ListView_GetItemCount(GetDlgItem(window, 1014)) != 1 || !Contains(GetDlgItem(window, 1010), L"现场设备 B"))
        return 186;
    SendMessageW(window, WM_APP + 41, 3, 0);
    SelectFirstRow(GetDlgItem(window, 1013));
    SendMessageW(window, WM_COMMAND, 1180, 0);
    HWND review = OwnedWindow(processId, L"dialIncidentReview");
    if (!review)
        return 187;
    SendMessageW(review, WM_COMMAND, 1315, 0);
    HWND editor = OwnedWindow(processId, L"dialManualReview");
    if (!editor)
        return 188;
    SendDlgItemMessageW(editor, 1451, CB_SETCURSEL, 1, 0);
    SetWindowTextW(GetDlgItem(editor, 1452), L"现场已确认；原始证据未改动 <script> & RSSI=-100");
    SendMessageW(editor, WM_COMMAND, 1453, 0);
    if (IsWindow(editor) || !Contains(GetDlgItem(review, 1304), L"现场已确认"))
        return 189;
    SendMessageW(review, WM_COMMAND, 1309, 0);
    if (ClipboardText().find(L"现场已确认") == std::wstring::npos)
        return 190;
    Capture(review, L"workspace-manual-review-zh");
    SelectFirstRow(GetDlgItem(review, 1306));
    SendMessageW(review, WM_COMMAND, 1312, 0);
    const std::wstring zip = base + L".evidence.zip";
    if (!ExportTo(review, processId, 1310, zip))
        return 191;
    SendMessageW(review, WM_CLOSE, 0, 0);
    wchar_t repeatsText[16]{};
    const int repeats = GetEnvironmentVariableW(L"DIALLOG_UI_REVIEW_REPEATS", repeatsText, 16)
                            ? static_cast<int>(wcstol(repeatsText, nullptr, 10))
                            : 0;
    for (int round = 0; round < repeats && round < 100; ++round) {
        if (!IsWindow(window))
            return 252;
        Capture(window, L"review-repeat-main");
        SendMessageW(window, WM_COMMAND, 1180, 0);
        review = OwnedWindow(processId, L"dialIncidentReview");
        if (!review)
            return 253;
        SendMessageW(review, WM_COMMAND, 1309, 0);
        DeleteFileW(zip.c_str());
        if (!ExportTo(review, processId, 1310, zip))
            return 254;
        SendMessageW(review, WM_CLOSE, 0, 0);
        PrintWide("review-export-close-repeat", std::to_wstring(round + 1));
    }
    if (!ChooseFile(window, processId, 33003, zip))
        return 192;
    Sleep(1000);
    if (!WaitForLoad(window, 90000))
        return 193;
    SendMessageW(window, WM_APP + 41, 4, 0);
    if (!Contains(GetDlgItem(window, 1010), L"现场设备 B") || ListView_GetItemCount(GetDlgItem(window, 1014)) != 1) {
        PrintWide("workspace-import-label", TextOf(GetDlgItem(window, 1010)));
        PrintWide("workspace-import-count", std::to_wstring(ListView_GetItemCount(GetDlgItem(window, 1014))));
        PrintWide("workspace-import-status", TextOf(GetDlgItem(window, 1020)));
        Capture(window, L"workspace-import-failure");
        return 194;
    }
    SendMessageW(window, WM_APP + 41, 3, 0);
    SelectFirstRow(GetDlgItem(window, 1013));
    SendMessageW(window, WM_COMMAND, 1180, 0);
    review = OwnedWindow(processId, L"dialIncidentReview");
    if (!review || !Contains(GetDlgItem(review, 1304), L"现场已确认") || !Contains(GetDlgItem(review, 1304), L"7s"))
        return 195;
    SendMessageW(review, WM_CLOSE, 0, 0);
    SendMessageW(window, WM_COMMAND, 1171, 0);
    SendMessageW(window, WM_COMMAND, 33002, 0);
    work = OwnedWindow(processId, L"dialWorkspace");
    if (!work || !Contains(work, L"Review workbench"))
        return 196;
    Capture(work, L"workspace-devices-en");
    SendMessageW(work, WM_CLOSE, 0, 0);
    SendMessageW(window, WM_COMMAND, 1170, 0);
    PrintWide("workspace-pass", L"device groups, period deltas, RSSI overlay, manual review persistence, original ZIP "
                                L"import, bookmarks and bilingual windows");
    return 0;
}

int MultiSourceSmoke(const std::wstring& executable) {
    wchar_t directory[MAX_PATH]{}, temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"dls", 0, temporary))
        return 100;
    DeleteFileW(temporary);
    const std::wstring base = temporary;
    const std::wstring paths[] = {base + L"_A.log", base + L"_B_现场.log"};
    const std::string input[] = {
        "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | CSQ:18 | RSRP:-95 | RSRQ:-8 SNR:120 RSSI:-65\r\n"
        "[2026-08-03 10:00:01] [SDK] Ping failed, fault timer started\r\n"
        "[2026-08-03 10:00:03] [SDK] Network recovered after 2s\r\n",
        "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | CSQ:8 | RSRP:-115 | RSRQ:-19 SNR:-20 RSSI:-100 | OPER:现场\r\n"
        "[2026-08-03 10:00:02] [SDK] Ping failed, fault timer started\r\n"
        "[2026-08-03 10:00:09] [SDK] Network recovered after 7s\r\n"};
    for (int i = 0; i < 2; ++i) {
        HANDLE file = CreateFileW(paths[i].c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return 101;
        DWORD written = 0;
        WriteFile(file, input[i].data(), static_cast<DWORD>(input[i].size()), &written, nullptr);
        CloseHandle(file);
        if (written != input[i].size())
            return 102;
    }
    std::wstring command = L"\"" + executable + L"\" --tab=4 \"" + paths[0] + L"\" \"" + paths[1] + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process))
        return 103;
    CloseHandle(process.hThread);
    HWND window = WaitForMain(process.dwProcessId, 90000);
    auto finish = [&](int code) {
        if (window && IsWindow(window))
            PostMessageW(window, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0)
            TerminateProcess(process.hProcess, code);
        CloseHandle(process.hProcess);
        for (const auto& path : paths)
            DeleteFileW(path.c_str());
        for (const wchar_t* extension : {L".md", L".html", L".csv"})
            DeleteFileW((base + extension).c_str());
        return code;
    };
    if (!window || !WaitForLoad(window, 90000))
        return finish(104);
    wchar_t workspaceOnly[4]{};
    if (GetEnvironmentVariableW(L"DIALLOG_UI_WORKSPACE_ONLY", workspaceOnly, 4)) {
        int code = WorkspaceSmoke(window, process.dwProcessId, base);
        return finish(code);
    }
    HWND metrics = GetDlgItem(window, 1014);
    if (ListView_GetItemCount(metrics) != 1 || !Contains(GetDlgItem(window, 1010), L"_A.log"))
        return finish(105);
    SendMessageW(window, WM_COMMAND, 34001, 0);
    if (ListView_GetItemCount(metrics) != 1 || !Contains(GetDlgItem(window, 1010), L"_B_现场.log"))
        return finish(106);
    SendMessageW(window, WM_APP + 41, 6, 0);
    HWND raw = GetDlgItem(window, 1016);
    if (ListView_GetItemCount(raw) != 3)
        return finish(107);
    RECT header{};
    POINT origin{};
    GetWindowRect(ListView_GetHeader(raw), &header);
    ClientToScreen(raw, &origin);
    const int rowY = header.bottom - origin.y + 10;
    SendMessageW(raw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(24, rowY));
    SendMessageW(raw, WM_LBUTTONUP, 0, MAKELPARAM(24, rowY));
    SendMessageW(raw, WM_COPY, 0, 0);
    if (ClipboardText().find(L"4\t2026-08-03 10:00:00") == std::wstring::npos ||
        ClipboardText().find(L"OPER:现场") == std::wstring::npos) {
        PrintWide("source-B-raw-copy", ClipboardText());
        return finish(108);
    }
    const int reviewResult = CheckIncident(window, process.dwProcessId, true);
    if (reviewResult)
        return finish(reviewResult);
    SendMessageW(window, WM_COMMAND, 33001, 0);
    SendMessageW(window, WM_APP + 41, 4, 0);
    if (ListView_GetItemCount(metrics) != 2)
        return finish(109);
    SendMessageW(window, WM_COMMAND, 34001, 0);
    SetWindowTextW(GetDlgItem(window, 1008), L"10:00:01");
    SetWindowTextW(GetDlgItem(window, 1009), L"10:00:03");
    SendMessageW(window, WM_COMMAND, 1003, 0);
    if (ListView_GetItemCount(metrics) != 0)
        return finish(110);
    SendMessageW(window, WM_COMMAND, 1141, 0);
    if (ListView_GetItemCount(metrics) != 1 || !Contains(GetDlgItem(window, 1010), L"_B_现场.log"))
        return finish(111);
    SetWindowPos(window, nullptr, 0, 0, 1280, 850, SWP_NOMOVE | SWP_NOZORDER);
    HWND signalChart = GetDlgItem(window, 1017);
    RedrawWindow(signalChart, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    RECT signalRect{};
    GetClientRect(signalChart, &signalRect);
    const int signalSection = (signalRect.bottom - 77) / 3;
    SendMessageW(signalChart, WM_LBUTTONDOWN, 0, MAKELPARAM(signalRect.right - 150, 26 + signalSection + 11));
    if (!Contains(signalChart, L"RSSI") || !Contains(signalChart, L"1 点")) {
        PrintWide("rssi-chart-label", TextOf(signalChart));
        Capture(window, L"rssi-switch-failure");
        return finish(125);
    }
    SendMessageW(signalChart, WM_MOUSEMOVE, 0, MAKELPARAM(signalRect.right / 2, 26 + signalSection + 45));
    Capture(window, L"rssi-reported-hover");
    SendMessageW(signalChart, WM_LBUTTONUP, 0, 0);
    SendMessageW(window, WM_COMMAND, 1171, 0);
    if (!Contains(signalChart, L"RSSI") || !Contains(signalChart, L"1 points") ||
        !Contains(GetDlgItem(window, 1135), L"Language / Fonts"))
        return finish(126);
    SendMessageW(window, WM_APP + 41, 3, 0);
    SelectFirstRow(GetDlgItem(window, 1013));
    SendMessageW(window, WM_COMMAND, 1180, 0);
    HWND englishReview = OwnedWindow(process.dwProcessId, L"dialIncidentReview");
    if (!englishReview || !Contains(GetDlgItem(englishReview, 1304), L"Source line 2 · Merged line 5") ||
        !Contains(GetDlgItem(englishReview, 1304), L"7s"))
        return finish(154);
    Capture(englishReview, L"incident-events-en");
    SendMessageW(englishReview, WM_COMMAND, 1309, 0);
    if (ClipboardText().find(L"Incident review") == std::wstring::npos ||
        ClipboardText().find(L"_B_现场.log") == std::wstring::npos)
        return finish(155);
    SendMessageW(englishReview, WM_CLOSE, 0, 0);
    SendMessageW(window, WM_APP + 41, 4, 0);
    SendMessageW(window, WM_COMMAND, 1181, 0);
    HWND englishGuide = OwnedWindow(process.dwProcessId, L"dialSelectableText");
    if (!englishGuide || !Contains(GetDlgItem(englishGuide, 1200), L"not causes of outages"))
        return finish(156);
    Capture(englishGuide, L"signal-guide-en");
    SendMessageW(englishGuide, WM_CLOSE, 0, 0);
    SendMessageW(window, WM_COMMAND, 33000, 0);
    WindowSearch search{process.dwProcessId, nullptr, L"dialSourceComparison"};
    EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.window || SendDlgItemMessageW(search.window, 1160, CB_GETCOUNT, 0, 0) != 2)
        return finish(112);
    SendMessageW(search.window, WM_COMMAND, 1166, 0);
    const auto comparison = ClipboardText();
    if (comparison.find(L"-95 / -95 / -95") == std::wstring::npos ||
        comparison.find(L"-115 / -115 / -115") == std::wstring::npos ||
        comparison.find(L"18 / 18 / 18") == std::wstring::npos || comparison.find(L"8 / 8 / 8") == std::wstring::npos ||
        comparison.find(L"-65 / -65.0 / -65 dBm") == std::wstring::npos ||
        comparison.find(L"-100 / -100.0 / -100 dBm") == std::wstring::npos) {
        PrintWide("source-comparison-copy", comparison);
        return finish(113);
    }
    Capture(search.window, L"english-two-source-comparison");
    SendMessageW(search.window, WM_CLOSE, 0, 0);
    Capture(window, L"english-two-source-metrics");
    SendMessageW(window, WM_COMMAND, 32004, 0);
    if (!ExportTo(window, process.dwProcessId, 1044, base + L".md"))
        return finish(114);
    const auto markdown = ReadBytes(base + L".md");
    if (markdown.find("Advice:") == std::string::npos || markdown.find("检查天线") != std::string::npos ||
        markdown.find("_B_现场.log") == std::string::npos ||
        markdown.find("Current-source history summary") == std::string::npos) {
        PrintWide("markdown-export-failure", L"Advice/source scope/original source name");
        return finish(115);
    }
    if (!ExportTo(window, process.dwProcessId, 1046, base + L".html"))
        return finish(116);
    const auto html = ReadBytes(base + L".html");
    if (html.find("lang=\"en-US\"") == std::string::npos || html.find("_B_现场.log") == std::string::npos ||
        html.find("检查天线") != std::string::npos)
        return finish(117);
    if (html.find("RSSI") == std::string::npos || html.find("Samples 1 · Drawn 1") == std::string::npos ||
        html.find("4096 horizontal buckets") == std::string::npos ||
        html.find("dialLog v" DL_VER_STR) == std::string::npos)
        return finish(164);
    if (!ExportTo(window, process.dwProcessId, 1045, base + L".csv"))
        return finish(118);
    const auto csv = ReadBytes(base + L".csv");
    if (csv.find("detailed_at_stage") == std::string::npos || csv.find("现场") == std::string::npos ||
        csv.find("-115") == std::string::npos || csv.find("-100") == std::string::npos)
        return finish(119);
    // Exercise the real DPI message with its rectangle in the target process.
    // An owned comparison must not keep the font that the main window deletes.
    SendMessageW(window, WM_COMMAND, 33000, 0);
    search.window = nullptr;
    EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.window)
        return finish(120);
    RECT suggested{};
    GetWindowRect(window, &suggested);
    void* remoteRect =
        VirtualAllocEx(process.hProcess, nullptr, sizeof(suggested), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    SIZE_T copied = 0;
    if (!remoteRect || !WriteProcessMemory(process.hProcess, remoteRect, &suggested, sizeof(suggested), &copied) ||
        copied != sizeof(suggested)) {
        if (remoteRect)
            VirtualFreeEx(process.hProcess, remoteRect, 0, MEM_RELEASE);
        return finish(121);
    }
    const HWND oldComparison = search.window;
    SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(remoteRect));
    VirtualFreeEx(process.hProcess, remoteRect, 0, MEM_RELEASE);
    if (IsWindow(oldComparison) || ListView_GetItemCount(metrics) != 1 ||
        !Contains(GetDlgItem(window, 1010), L"_B_现场.log"))
        return finish(122);
    SendMessageW(window, WM_COMMAND, 33000, 0);
    search.window = nullptr;
    EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.window)
        return finish(123);
    SendMessageW(search.window, WM_COMMAND, 1166, 0);
    if (ClipboardText() != comparison)
        return finish(124);
    Capture(search.window, L"english-source-comparison-after-dpi");
    SendMessageW(search.window, WM_CLOSE, 0, 0);
    SendMessageW(window, WM_COMMAND, 1170, 0);
    SendMessageW(window, WM_COMMAND, 32005, 0);
    return finish(0);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc != 3)
        return 90;
    // 新输入必须不继承旧会话筛选；故意写入必定不匹配的条件验证该回归点。
    HKEY settings = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\dialLog", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &settings,
                        nullptr) == ERROR_SUCCESS) {
        const wchar_t stale[] = L"__stale_filter_must_not_apply__";
        for (const wchar_t* name : {L"TagFilter", L"GrepFilter", L"SinceFilter", L"UntilFilter"})
            RegSetValueExW(settings, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(stale), sizeof(stale));
        const wchar_t interfaceFace[] = L"Microsoft YaHei UI", logFace[] = L"Consolas";
        const DWORD filtersExpanded = 1, english = 0, metricColumns = (1u << 22) - 1;
        RegSetValueExW(settings, L"English", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&english), sizeof(english));
        RegSetValueExW(settings, L"MetricColumns", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&metricColumns),
                       sizeof(metricColumns));
        RegSetValueExW(settings, L"FiltersExpanded", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&filtersExpanded),
                       sizeof(filtersExpanded));
        RegSetValueExW(settings, L"UiFont", 0, REG_SZ, reinterpret_cast<const BYTE*>(interfaceFace),
                       sizeof(interfaceFace));
        RegSetValueExW(settings, L"LogFont", 0, REG_SZ, reinterpret_cast<const BYTE*>(logFace), sizeof(logFace));
        RegCloseKey(settings);
    }
    const std::wstring executable = argv[1];
    std::wstring command = L"\"" + executable + L"\" \"" + argv[2] + L"\"";
    LocalFree(argv);
    wchar_t multiOnly[4]{};
    if (GetEnvironmentVariableW(L"DIALLOG_UI_MULTI_ONLY", multiOnly, 4) ||
        GetEnvironmentVariableW(L"DIALLOG_UI_WORKSPACE_ONLY", multiOnly, 4))
        return MultiSourceSmoke(executable);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                        &process))
        return 91;
    CloseHandle(process.hThread);

    HWND window = WaitForMain(process.dwProcessId, 90000);
    if (!window) {
        DWORD startCode = 0;
        GetExitCodeProcess(process.hProcess, &startCode);
        PrintWide("startup-exit-code", std::to_wstring(startCode));
        TerminateProcess(process.hProcess, 92);
        CloseHandle(process.hProcess);
        return 1;
    }
    auto finish = [&](int code) {
        if (IsWindow(window))
            PostMessageW(window, WM_CLOSE, 0, 0);
        if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, static_cast<UINT>(code));
            WaitForSingleObject(process.hProcess, 2000);
        }
        CloseHandle(process.hProcess);
        return code;
    };
    // Wine 首次加载 CJK 字体可能延迟界面消息；断言仍核对实际文档和剪贴板内容。
    if (!WaitForLoad(window, 180000)) {
        PrintWide("load-timeout-label", TextOf(GetDlgItem(window, 1010)));
        PrintWide("load-timeout-action", TextOf(GetDlgItem(window, 1030)));
        return finish(2);
    }
    if (Contains(GetDlgItem(window, 1010), L"未加载"))
        return finish(3);
    if (!Contains(window, L"dialLog v" DL_VER_WSTR))
        return finish(170);
    if (!CheckMainFrame(window))
        return finish(240);
    wchar_t captionOnly[4]{};
    if (GetEnvironmentVariableW(L"DIALLOG_UI_CAPTION_ONLY", captionOnly, 4)) {
        HWND caption = FindWindowExW(window, nullptr, L"dialMainCaption", nullptr);
        SendMessageW(GetDlgItem(caption, 39003), BM_CLICK, 0, 0);
        if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0)
            return finish(241);
        return finish(0);
    }
    wchar_t fontsOnly[4]{};
    if (GetEnvironmentVariableW(L"DIALLOG_UI_FONT_ONLY", fontsOnly, 4)) {
        SetWindowPos(window, nullptr, 0, 0, 860, 640, SWP_NOMOVE | SWP_NOZORDER);
        if (!ChooseTypeface(window, process.dwProcessId, false, L"Arial"))
            return finish(48);
        if (!ChooseTypeface(window, process.dwProcessId, true, L"Courier New"))
            return finish(49);
        SendMessageW(window, WM_COMMAND, 1171, 0);
        SendMessageW(window, WM_COMMAND, 1170, 0);
        SendMessageW(window, WM_COMMAND, 1050, 0);
        if (!WaitForLoad(window, 90000))
            return finish(238);
        SendMessageW(window, WM_APP + 41, 4, 0);
        if (ListView_GetItemCount(GetDlgItem(window, 1014)) != 1399)
            return finish(239);
        Capture(window, L"fonts-isolated-reload");
        PrintWide("font-pass", L"UI/log font persistence, language switch and document reload");
        return finish(0);
    }
    SetWindowPos(window, nullptr, 0, 0, 1280, 800, SWP_NOZORDER);
    UpdateWindow(window);
    if (!AppearanceFitsNavigation(window))
        return finish(130);
    if (!CheckAppearanceMenu(window, process.dwProcessId, L"appearance-menu-wide"))
        return finish(131);

    const wchar_t* titles[] = {L"概览",     L"诊断结论", L"事件时间线", L"断网记录", L"信号指标",
                               L"标签统计", L"原始日志", L"未识别行",   L"小区分析"};
    for (int page = 0; page < 9; ++page) {
        SendMessageW(window, WM_APP + 41, page, 0);
        UpdateWindow(window);
        if (TextOf(GetDlgItem(window, 1024)) != titles[page])
            return finish(10 + page);
        if (page == 2 && Header_GetItemCount(ListView_GetHeader(GetDlgItem(window, 1012))) != 4)
            return finish(52);
        if (page == 3 && Header_GetItemCount(ListView_GetHeader(GetDlgItem(window, 1013))) != 6)
            return finish(53);
        if ((page == 2 || page == 3 || page == 6) && !IsWindowVisible(GetDlgItem(window, 1139)))
            return finish(54);
        if (!IsWindowVisible(GetDlgItem(window, 1140)) || !IsWindowVisible(GetDlgItem(window, 1141)) ||
            IsWindowEnabled(GetDlgItem(window, 1141)))
            return finish(60);
        if (page == 2 || page == 3 || page == 5 || page == 6)
            Capture(window, page == 2   ? L"timeline-wide"
                            : page == 3 ? L"outages-wide"
                            : page == 5 ? L"tags-wide"
                                        : L"raw-wide");
    }

    wchar_t skipNewWindows[4]{};
    wchar_t focusOnly[4]{};
    if (GetEnvironmentVariableW(L"DIALLOG_UI_FOCUS_ONLY", focusOnly, 4)) {
        wchar_t directory[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"DIALLOG_UI_CAPTURE", directory, MAX_PATH))
            return finish(167);
        SendMessageW(window, WM_APP + 41, 1, 0);
        HWND pane = GetDlgItem(window, 1019), edit = GetDlgItem(window, 1007);
        SendMessageW(edit, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(12, 12));
        SendMessageW(edit, WM_LBUTTONUP, 0, MAKELPARAM(12, 12));
        UpdateWindow(pane);
        Capture(pane, L"findings-focus-top");
        for (int i = 0; i < 5; ++i)
            SendMessageW(pane, WM_VSCROLL, SB_PAGEDOWN, 0);
        UpdateWindow(pane);
        Capture(pane, L"findings-focus-before");
        const std::wstring base = std::wstring(directory) + L"\\findings-focus-";
        const auto scrolled = ReadBytes(base + L"before.bmp");
        if (scrolled.empty() || scrolled == ReadBytes(base + L"top.bmp"))
            return finish(168);
        SendMessageW(pane, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(8, 8));
        SendMessageW(pane, WM_LBUTTONUP, 0, MAKELPARAM(8, 8));
        UpdateWindow(pane);
        Capture(pane, L"findings-focus-after");
        if (scrolled != ReadBytes(base + L"after.bmp")) {
            PrintWide("findings-pointer-focus", L"Mouse focus changed the scrolled contents");
            return finish(169);
        }
        return finish(0);
    }
    const bool skipNew = GetEnvironmentVariableW(L"DIALLOG_UI_SKIP_NEW_WINDOWS", skipNewWindows, 4) != 0;
    wchar_t newScope[16]{};
    GetEnvironmentVariableW(L"DIALLOG_UI_NEW_SCOPE", newScope, 16);
    wchar_t reportOnly[4]{};
    GetEnvironmentVariableW(L"DIALLOG_UI_REPORT_ONLY", reportOnly, 4);
    if (!skipNew && std::wstring(newScope) != L"text") {
        const int reviewResult = CheckIncident(window, process.dwProcessId);
        if (reviewResult)
            return finish(reviewResult);
    }
    SendMessageW(window, WM_APP + 41, 2, 0);
    HWND timeline = GetDlgItem(window, 1012);
    RECT timelineHeader{};
    POINT timelineOrigin{};
    GetWindowRect(ListView_GetHeader(timeline), &timelineHeader);
    ClientToScreen(timeline, &timelineOrigin);
    const int timelineRowY = timelineHeader.bottom - timelineOrigin.y + 10;
    SendMessageW(timeline, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(24, timelineRowY));
    SendMessageW(timeline, WM_LBUTTONUP, 0, MAKELPARAM(24, timelineRowY));
    SendMessageW(timeline, WM_COPY, 0, 0);
    if (ClipboardText() != L"2026-06-30 00:01:50\tSDK 事件\tSDK\tDataCall disconnected | profile=1 err=0x0\r\n")
        return finish(58);
    SendMessageW(timeline, WM_KEYDOWN, VK_RETURN, 0);
    if (TextOf(GetDlgItem(window, 1112)).find(L"DataCall disconnected | profile=1 err=0x0") == std::wstring::npos) {
        PrintWide("timeline-detail", TextOf(GetDlgItem(window, 1112)));
        return finish(59);
    }
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1111, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1111)));

    SendMessageW(window, WM_APP + 41, 0, 0);
    Capture(window, L"overview-wide");
    HWND dashboard = GetDlgItem(window, 1022);
    RECT dashboardRect{};
    GetClientRect(dashboard, &dashboardRect);
    SendMessageW(dashboard, WM_LBUTTONUP, 0, MAKELPARAM(dashboardRect.right - 95, 24));
    const std::wstring overviewText = ClipboardText();
    if (overviewText.find(L"首次联网后运行期可用率：") == std::wstring::npos ||
        overviewText.find(L"全程服务可达率：") == std::wstring::npos ||
        overviewText.find(L"2026-06-30") == std::wstring::npos ||
        overviewText.find(L"1359 个样本") == std::wstring::npos ||
        overviewText.find(L"RSSI 有效样本: 516") == std::wstring::npos ||
        overviewText.find(L"断网 36 次 · 已恢复事件累计 34m02s · 最长 3m20s") == std::wstring::npos) {
        PrintWide("overview-clipboard", overviewText);
        return finish(43);
    }
    HWND summary = GetDlgItem(window, 1011);
    RECT summaryRect{};
    GetClientRect(summary, &summaryRect);
    SendMessageW(summary, WM_LBUTTONUP, 0, MAKELPARAM(summaryRect.right - 76, 32));
    const std::wstring cardText = ClipboardText();
    if (cardText.find(L"可用率与观测范围") != 0 || cardText.find(L"关键事件计数") != std::wstring::npos)
        return finish(44);
    SendMessageW(window, WM_APP + 41, 1, 0);
    Capture(window, L"findings-wide");
    HWND findings = GetDlgItem(window, 1019);
    RECT findingsRect{};
    GetClientRect(findings, &findingsRect);
    SendMessageW(findings, WM_LBUTTONUP, 0, MAKELPARAM(findingsRect.right - 100, 35));
    const std::wstring findingsText = ClipboardText();
    if (findingsText.find(L"诊断结论") != 0 || findingsText.find(L"来源平台：") == std::wstring::npos ||
        findingsText.find(L"依据：") == std::wstring::npos || findingsText.find(L"建议：") == std::wstring::npos ||
        findingsText.find(L"第 ") == std::wstring::npos ||
        findingsText.find(L"RSSI 有效样本: 516") == std::wstring::npos)
        return finish(45);
    SCROLLINFO compactFindingsRange{};
    compactFindingsRange.cbSize = sizeof(compactFindingsRange);
    compactFindingsRange.fMask = SIF_RANGE;
    const bool haveCompactRange = GetScrollInfo(findings, SB_VERT, &compactFindingsRange) != FALSE;
    SendMessageW(findings, WM_LBUTTONUP, 0, MAKELPARAM(findingsRect.right - 210, 35));
    Capture(window, L"findings-expanded");
    SCROLLINFO expandedFindingsRange = compactFindingsRange;
    const bool haveExpandedRange = GetScrollInfo(findings, SB_VERT, &expandedFindingsRange) != FALSE;
    const bool readableRanges =
        haveCompactRange && haveExpandedRange && compactFindingsRange.nMax > 0 && expandedFindingsRange.nMax > 0;
    if (!readableRanges)
        PrintWide("finding-scroll-range", L"Cross-process range unavailable; expansion is checked in captures.");
    SendMessageW(findings, WM_LBUTTONUP, 0, MAKELPARAM(findingsRect.right - 100, 35));
    // Wine may reject GetScrollInfo on another process's custom window.
    // Full-copy equality remains mandatory; captured expanded content is reviewed.
    if (ClipboardText() != findingsText ||
        (readableRanges && expandedFindingsRange.nMax <= compactFindingsRange.nMax)) {
        PrintWide("finding-expand-ranges",
                  std::to_wstring(compactFindingsRange.nMax) + L" -> " + std::to_wstring(expandedFindingsRange.nMax));
        PrintWide("finding-copy-before", findingsText);
        PrintWide("finding-copy-after", ClipboardText());
        return finish(127);
    }
    SendMessageW(findings, WM_LBUTTONUP, 0, MAKELPARAM(findingsRect.right - 210, 35));
    Capture(window, L"findings-compact-restored");
    if (!skipNew && std::wstring(newScope) != L"review") {
        SendMessageW(findings, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(32, 250));
        SendMessageW(findings, WM_KEYDOWN, VK_HOME, 0);
        SendMessageW(findings, WM_KEYDOWN, VK_F2, 0);
        HWND selectable = OwnedWindow(process.dwProcessId, L"dialSelectableText");
        if (!selectable || TextOf(GetDlgItem(selectable, 1200)).find(L"依据：") == std::wstring::npos ||
            TextOf(GetDlgItem(selectable, 1200)).find(L"证据：") == std::wstring::npos)
            return finish(151);
        const auto selectedText = TextOf(GetDlgItem(selectable, 1200));
        SendDlgItemMessageW(selectable, 1200, EM_SETSEL, 0, -1);
        SendDlgItemMessageW(selectable, 1200, WM_COPY, 0, 0);
        if (ClipboardText() != selectedText)
            return finish(152);
        Capture(selectable, L"findings-selectable-text");
        // Real EG25 fixture: the first finding's first evidence is source line 24.
        if (selectedText.find(L"第 24 行") == std::wstring::npos) {
            PrintWide("selected-finding-jump-text", selectedText);
            return finish(160);
        }
        const unsigned long expectedLine = 24;
        SendMessageW(selectable, WM_COMMAND, 1202, 0);
        if (IsWindow(selectable) || !Contains(GetDlgItem(window, 1024), L"原始日志"))
            return finish(161);
        SendMessageW(GetDlgItem(window, 1016), WM_COPY, 0, 0);
        if (ClipboardText().find(std::to_wstring(expectedLine) + L"\t") != 0)
            return finish(162);
        SendMessageW(window, WM_APP + 41, 4, 0);
        SendMessageW(window, WM_COMMAND, 1181, 0);
        selectable = OwnedWindow(process.dwProcessId, L"dialSelectableText");
        if (!selectable || !Contains(GetDlgItem(selectable, 1200), L"RSSI（dBm）"))
            return finish(153);
        Capture(selectable, L"signal-guide-zh");
        SendMessageW(selectable, WM_CLOSE, 0, 0);
    }

    SendMessageW(window, WM_APP + 41, 4, 0);
    Capture(window, L"metrics-wide");
    HWND metrics = GetDlgItem(window, 1014);
    HWND chart = GetDlgItem(window, 1017);
    HWND header = ListView_GetHeader(metrics);
    if (!header || Header_GetItemCount(header) != 22)
        return finish(20);
    if (!IsWindowVisible(chart))
        return finish(22);
    if (ListView_GetColumnWidth(metrics, 0) < 180)
        return finish(46);
    wchar_t reportBase[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"DIALLOG_UI_REPORT_BASE", reportBase, MAX_PATH)) {
        for (bool english : {false, true}) {
            SendMessageW(window, WM_COMMAND, english ? 1171 : 1170, 0);
            const auto path = std::wstring(reportBase) + (english ? L".en.html" : L".zh.html");
            DeleteFileW(path.c_str());
            if (!ExportTo(window, process.dwProcessId, 1046, path))
                return finish(165);
            const auto report = ReadBytes(path);
            if (report.find("dialLog v" DL_VER_STR) == std::string::npos || report.find("RSSI") == std::string::npos ||
                report.find("data-samples=\"1359\" data-drawn=\"1359\"") == std::string::npos)
                return finish(166);
        }
        SendMessageW(window, WM_COMMAND, 1170, 0);
    }
    if (!CheckHoverRestore(chart, POINT{180, 85}, L"signal-chart"))
        return finish(253);
    HWND navigation = FindWindowExW(window, nullptr, L"dialModernNavigation", nullptr);
    RECT navRect{};
    GetClientRect(navigation, &navRect);
    if (!navigation || !CheckHoverRestore(navigation, POINT{80, navRect.bottom < 780 ? 110 : 120}, L"navigation"))
        return finish(254);
    RECT chartRect{};
    GetClientRect(chart, &chartRect);
    SendMessageW(chart, WM_MOUSEMOVE, 0, MAKELPARAM(chartRect.right - 155, 85));
    Capture(window, L"metrics-hover");
    SendMessageW(chart, WM_MOUSELEAVE, 0, 0);
    SetWindowPos(window, nullptr, 0, 0, 1280, 650, SWP_NOMOVE | SWP_NOZORDER);
    GetClientRect(chart, &chartRect);
    if (chartRect.bottom < 160 || chartRect.bottom >= 77 + 3 * 70)
        return finish(51);
    Capture(window, L"metrics-compressed");
    SetWindowPos(window, nullptr, 0, 0, 1280, 800, SWP_NOMOVE | SWP_NOZORDER);
    HWND chartMode = GetDlgItem(window, 1047), splitMode = GetDlgItem(window, 1048);
    HWND tableMode = GetDlgItem(window, 1049);
    if (!chartMode || !splitMode || !tableMode || !IsWindowVisible(splitMode))
        return finish(33);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1049, BN_CLICKED), reinterpret_cast<LPARAM>(tableMode));
    if (!IsWindowVisible(metrics) || IsWindowVisible(chart))
        return finish(34);
    RECT metricPage{}, metricClient{};
    GetWindowRect(metrics, &metricPage);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&metricPage), 2);
    GetClientRect(window, &metricClient);
    if (metricPage.right < metricClient.right - 30 || metricPage.bottom < metricClient.bottom - 70)
        return finish(35);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1048, BN_CLICKED), reinterpret_cast<LPARAM>(splitMode));
    if (!IsWindowVisible(metrics) || !IsWindowVisible(chart))
        return finish(36);
    RECT splitChart{};
    GetClientRect(chart, &splitChart);
    const DWORD switchStart = GetTickCount();
    for (int repeat = 0; repeat < 8; ++repeat) {
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1047, BN_CLICKED), reinterpret_cast<LPARAM>(chartMode));
        GetClientRect(chart, &chartRect);
        if (chartRect.bottom <= splitChart.bottom || IsWindowVisible(metrics))
            return finish(55);
        if (repeat == 7)
            Capture(window, L"metrics-chart-immediate", false);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1048, BN_CLICKED), reinterpret_cast<LPARAM>(splitMode));
        GetClientRect(chart, &chartRect);
        if (chartRect.bottom != splitChart.bottom || !IsWindowVisible(metrics))
            return finish(56);
        if (repeat == 7)
            Capture(window, L"metrics-split-immediate", false);
    }
    PrintWide("chart-switch-16-total-ms", std::to_wstring(GetTickCount() - switchStart));
    const int section = (chartRect.bottom - 77) / 3;
    SendMessageW(chart, WM_LBUTTONDOWN, 0, MAKELPARAM(chartRect.right - 215, 26 + section + 11));
    if (!Contains(chart, L"RSRQ"))
        return finish(128);
    UpdateWindow(chart);
    Capture(window, L"metrics-rsrq");
    SendMessageW(chart, WM_LBUTTONDOWN, 0, MAKELPARAM(chartRect.right - 280, 26 + section + 11));
    if (!Contains(chart, L"RSRP"))
        return finish(129);
    UpdateWindow(chart);

    // EG25 真机：按完整时间轴框选前约 34 分钟，包含前三次完整断网。
    const int plotLeft = 58, plotY = 61;
    const int fullMetricCount = ListView_GetItemCount(metrics);
    SendMessageW(chart, WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(plotLeft + 220, plotY));
    RECT beforeResize{};
    GetClientRect(chart, &beforeResize);
    SendMessageW(chart, WM_SIZE, 0, MAKELPARAM(beforeResize.right, beforeResize.bottom));
    SendMessageW(chart, WM_MBUTTONUP, 0, MAKELPARAM(plotLeft + 250, plotY));
    if (IsWindowEnabled(GetDlgItem(window, 1141)) || ListView_GetItemCount(metrics) != fullMetricCount)
        return finish(200);
    UpdateWindow(chart);
    POINT wheelPoint{plotLeft + 160, plotY};
    ClientToScreen(chart, &wheelPoint);
    SendMessageW(chart, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(wheelPoint.x, wheelPoint.y));
    if (!IsWindowEnabled(GetDlgItem(window, 1141)) || ListView_GetItemCount(metrics) >= fullMetricCount)
        return finish(196);
    const auto zoomRange = TextOf(GetDlgItem(window, 1140));
    UpdateWindow(chart);
    SendMessageW(chart, WM_MOUSEWHEEL, MAKEWPARAM(MK_SHIFT, -WHEEL_DELTA), MAKELPARAM(wheelPoint.x, wheelPoint.y));
    if (TextOf(GetDlgItem(window, 1140)) == zoomRange)
        return finish(197);
    UpdateWindow(chart);
    const auto panRange = TextOf(GetDlgItem(window, 1140));
    SendMessageW(chart, WM_MBUTTONDOWN, MK_MBUTTON, MAKELPARAM(plotLeft + 220, plotY));
    SendMessageW(chart, WM_MOUSEMOVE, MK_MBUTTON, MAKELPARAM(plotLeft + 250, plotY));
    SendMessageW(chart, WM_MBUTTONUP, 0, MAKELPARAM(plotLeft + 250, plotY));
    if (TextOf(GetDlgItem(window, 1140)) == panRange)
        return finish(198);
    SendMessageW(window, WM_COMMAND, 1141, 0);
    UpdateWindow(chart);
    if (ListView_GetItemCount(metrics) != fullMetricCount || IsWindowEnabled(GetDlgItem(window, 1141)))
        return finish(199);
    PrintWide("native-wheel-pan-reset", L"PASS linked metric selection and source-bounded range");
    SendMessageW(chart, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(plotLeft, plotY));
    SendMessageW(chart, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(plotLeft + 40, plotY));
    Capture(window, L"range-preview");
    SendMessageW(chart, WM_LBUTTONUP, 0, MAKELPARAM(plotLeft + 40, plotY));
    const int selectedMetricCount = ListView_GetItemCount(metrics);
    if (!IsWindowEnabled(GetDlgItem(window, 1141)) || selectedMetricCount != 68 ||
        !Contains(GetDlgItem(window, 1140), L"2026-06-30 00:00:26") ||
        !Contains(GetDlgItem(window, 1140), L"2026-06-30 00:34:41"))
        return finish(61);
    PrintWide("selected-metric-count", std::to_wstring(selectedMetricCount));
    if (reportBase[0]) {
        const auto path = std::wstring(reportBase) + L".range.html";
        DeleteFileW(path.c_str());
        if (!ExportTo(window, process.dwProcessId, 1046, path))
            return finish(167);
        const auto report = ReadBytes(path);
        // The 68 metric rows include one row without CSQ; it must not become a point.
        if (report.find("data-samples=\"67\" data-drawn=\"67\"") == std::string::npos ||
            report.find("2026-06-30 00:34:41") == std::string::npos)
            return finish(168);
        if (reportOnly[0])
            return finish(0);
    }
    Capture(window, L"range-selected");
    SendMessageW(window, WM_APP + 41, 3, 0);
    if (ListView_GetItemCount(GetDlgItem(window, 1013)) != 3)
        return finish(62);
    SendMessageW(window, WM_APP + 41, 2, 0);
    PrintWide("selected-timeline-count", std::to_wstring(ListView_GetItemCount(timeline)));
    if (ListView_GetItemCount(timeline) != 64)
        return finish(70);
    Capture(window, L"range-timeline");
    SendMessageW(window, WM_APP + 41, 1, 0);
    Capture(window, L"range-findings");
    GetClientRect(findings, &findingsRect);
    SendMessageW(findings, WM_LBUTTONUP, 0, MAKELPARAM(findingsRect.right - 100, 35));
    if (ClipboardText().find(L"选区：2026-06-30 00:00:26") == std::wstring::npos ||
        ClipboardText().find(L"解析审计基于整份输入") == std::wstring::npos ||
        ClipboardText().find(L"当前区间汇总") == std::wstring::npos ||
        ClipboardText().find(L"全量日志历史汇总") != std::wstring::npos)
        return finish(63);
    SendMessageW(window, WM_APP + 41, 4, 0);
    UpdateWindow(chart);

    // 反向再框选到故障起点后、恢复前：不能把区间内未见恢复写成全局未恢复。
    SendMessageW(chart, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(plotLeft + 150, plotY));
    SendMessageW(chart, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(plotLeft, plotY));
    SendMessageW(chart, WM_LBUTTONUP, 0, MAKELPARAM(plotLeft, plotY));
    if (ListView_GetItemCount(metrics) != 13 || !Contains(GetDlgItem(window, 1140), L"2026-06-30 00:06:41"))
        return finish(64);
    SendMessageW(window, WM_APP + 41, 3, 0);
    HWND outages = GetDlgItem(window, 1013);
    if (ListView_GetItemCount(outages) != 1)
        return finish(65);
    RECT outageHeader{};
    POINT outageOrigin{};
    GetWindowRect(ListView_GetHeader(outages), &outageHeader);
    ClientToScreen(outages, &outageOrigin);
    const int outageRowY = outageHeader.bottom - outageOrigin.y + 10;
    SendMessageW(outages, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(24, outageRowY));
    SendMessageW(outages, WM_LBUTTONUP, 0, MAKELPARAM(24, outageRowY));
    SendMessageW(outages, WM_COPY, 0, 0);
    if (ClipboardText().find(L"区间内未见恢复") == std::wstring::npos)
        return finish(66);
    Capture(window, L"range-outage-boundary");

    // 恢复入口跨页面可用，恢复后所有指标和断网都回到原计数。
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1141, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1141)));
    if (ListView_GetItemCount(outages) != 36 || IsWindowEnabled(GetDlgItem(window, 1141)) ||
        !Contains(GetDlgItem(window, 1140), L"全范围"))
        return finish(67);
    SendMessageW(window, WM_APP + 41, 4, 0);
    UpdateWindow(chart);
    if (ListView_GetItemCount(metrics) != fullMetricCount)
        return finish(68);
    SendMessageW(chart, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(plotLeft, plotY));
    SendMessageW(chart, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(plotLeft + 40, plotY));
    SendMessageW(chart, WM_KEYDOWN, VK_ESCAPE, 0);
    SendMessageW(chart, WM_LBUTTONUP, 0, MAKELPARAM(plotLeft + 40, plotY));
    if (IsWindowEnabled(GetDlgItem(window, 1141)) || ListView_GetItemCount(metrics) != fullMetricCount)
        return finish(69);

    // 消息条件与图表选区独立，恢复全范围仅清除时间条件。
    SetWindowTextW(GetDlgItem(window, 1007), L"SDK");
    SetWindowTextW(GetDlgItem(window, 1008), L"00:00:00");
    SetWindowTextW(GetDlgItem(window, 1009), L"00:35:00");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1003, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1003)));
    if (!Contains(GetDlgItem(window, 1140), L"时间条件") || !IsWindowEnabled(GetDlgItem(window, 1141)))
        return finish(71);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1141, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1141)));
    if (TextOf(GetDlgItem(window, 1007)) != L"SDK" || !TextOf(GetDlgItem(window, 1008)).empty() ||
        !TextOf(GetDlgItem(window, 1009)).empty() || IsWindowEnabled(GetDlgItem(window, 1141)))
        return finish(72);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1004, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1004)));
    SetWindowTextW(GetDlgItem(window, 1008), L"12:00:00");
    SetWindowTextW(GetDlgItem(window, 1009), L"12:01:00");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1003, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1003)));
    SendMessageW(window, WM_APP + 41, 6, 0);
    if (ListView_GetItemCount(GetDlgItem(window, 1016)) != 0 || !IsWindowEnabled(GetDlgItem(window, 1141)))
        return finish(73);
    Capture(window, L"range-empty");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1141, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1141)));
    if (ListView_GetItemCount(GetDlgItem(window, 1016)) != 2860)
        return finish(74);

    SendMessageW(window, WM_APP + 41, 6, 0);
    HWND raw = GetDlgItem(window, 1016);
    if (!raw)
        return finish(30);
    if (!ListView_GetHeader(raw))
        return finish(39);
    if (ListView_GetItemCount(raw) < 1)
        return finish(40);
    if (!(GetWindowLongPtrW(raw, GWL_STYLE) & LVS_OWNERDATA))
        return finish(41);
    // 跨进程直接发送 LVM_SETITEMSTATE 在部分 Wine 版本不会封送 LVITEM；模拟用户点击
    // 首行既覆盖真实交互，也避免测试依赖该兼容性细节。
    RECT rawHeader{};
    POINT rawOrigin{};
    GetWindowRect(ListView_GetHeader(raw), &rawHeader);
    ClientToScreen(raw, &rawOrigin);
    const LONG measuredRowY = rawHeader.bottom - rawOrigin.y + 10L;
    const int firstRowY = static_cast<int>(measuredRowY > 1 ? measuredRowY : 1);
    SendMessageW(raw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(24, firstRowY));
    SendMessageW(raw, WM_LBUTTONUP, 0, MAKELPARAM(24, firstRowY));
    if (ListView_GetSelectedCount(raw) < 1)
        return finish(42);
    SendMessageW(raw, WM_COPY, 0, 0);
    const auto rawText = ClipboardText();
    if (rawText.find(L"2\t2026-06-30 00:00:26\t") != 0 || rawText.find(L"1D8DE0D -> 1D8DE0B") == std::wstring::npos)
        return finish(57);
    SendMessageW(raw, WM_KEYDOWN, VK_RETURN, 0);
    HWND detail = GetDlgItem(window, 1112);
    if (!detail || !IsWindowVisible(detail) || SendMessageW(detail, WM_GETTEXTLENGTH, 0, 0) < 20)
        return finish(37);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1111, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1111)));
    if (IsWindowVisible(detail))
        return finish(38);
    SendMessageW(window, WM_APP + 41, 8, 0);
    HWND cells = GetDlgItem(window, 1026);
    if (!cells || Header_GetItemCount(ListView_GetHeader(cells)) != 17 || ListView_GetItemCount(cells) < 1)
        return finish(31);
    RECT cellHeader{};
    POINT cellOrigin{};
    GetWindowRect(ListView_GetHeader(cells), &cellHeader);
    ClientToScreen(cells, &cellOrigin);
    const int cellRowY = cellHeader.bottom - cellOrigin.y + 10;
    SendMessageW(cells, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(24, cellRowY));
    SendMessageW(cells, WM_LBUTTONUP, 0, MAKELPARAM(24, cellRowY));
    SendMessageW(cells, WM_COPY, 0, 0);
    const auto cellCopy = ClipboardText();
    std::size_t cellTabs = 0;
    for (const auto ch : cellCopy)
        if (ch == L'\t')
            ++cellTabs;
    if (cellTabs != 16 || cellCopy.find(L"\t-\t0\r\n") == std::wstring::npos) {
        PrintWide("cell-rssi-missing-copy", cellCopy);
        return finish(163);
    }
    bool haveRssiCell = false;
    for (int row = 1; row < ListView_GetItemCount(cells); ++row) {
        SendMessageW(cells, WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(cells, WM_COPY, 0, 0);
        const auto observedCopy = ClipboardText();
        if (observedCopy.find(L" / -") != std::wstring::npos) {
            haveRssiCell = true;
            break;
        }
    }
    if (!haveRssiCell)
        return finish(164);
    SetWindowPos(window, nullptr, 0, 0, 860, 640, SWP_NOMOVE | SWP_NOZORDER);
    UpdateWindow(window);
    RECT pageTitle{}, exportButton{};
    GetWindowRect(GetDlgItem(window, 1024), &pageTitle);
    GetWindowRect(GetDlgItem(window, 1005), &exportButton);
    if (pageTitle.right > exportButton.left)
        return finish(32);
    Capture(window, L"cells-narrow");
    for (int page : {0, 1, 4}) {
        SendMessageW(window, WM_APP + 41, page, 0);
        Capture(window, page == 0 ? L"overview-narrow" : page == 1 ? L"findings-narrow" : L"metrics-narrow");
    }
    // 展开的筛选栏在小窗口占用图表空间；收起后必须显示完整三图区。
    if (IsWindowVisible(GetDlgItem(window, 1006)))
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1023, BN_CLICKED),
                     reinterpret_cast<LPARAM>(GetDlgItem(window, 1023)));
    GetClientRect(chart, &chartRect);
    Capture(window, L"metrics-narrow-collapsed");
    // 顶部/时间轴/页脚占 77px，每个图区至少留 70px（含标题）。
    if (chartRect.bottom < 77 + 3 * 70)
        return finish(50);
    if (!GetDlgItem(window, 1135) || !IsWindowVisible(GetDlgItem(window, 1135)))
        return finish(47);
    if (!AppearanceFitsNavigation(window))
        return finish(132);
    if (!CheckAppearanceMenu(window, process.dwProcessId, L"appearance-menu-narrow"))
        return finish(133);

    // Font enumeration is tested separately when a shared Wine host is memory constrained.
    wchar_t skipFontDialogs[2]{};
    if (!GetEnvironmentVariableW(L"DIALLOG_UI_SKIP_FONT_DIALOGS", skipFontDialogs, 2)) {
        if (!ChooseTypeface(window, process.dwProcessId, false, L"Arial"))
            return finish(48);
        if (!ChooseTypeface(window, process.dwProcessId, true, L"Courier New"))
            return finish(49);
        Capture(window, L"fonts-selected");
    }

    // 取消另一份日志的加载应同时保留当前选区及其分析结果。
    UpdateWindow(chart);
    SendMessageW(chart, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(plotLeft, plotY));
    SendMessageW(chart, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(plotLeft + 40, plotY));
    SendMessageW(chart, WM_LBUTTONUP, 0, MAKELPARAM(plotLeft + 40, plotY));
    if (!IsWindowEnabled(GetDlgItem(window, 1141)))
        return finish(75);
    const std::wstring oldRangeLabel = TextOf(GetDlgItem(window, 1140));

    // 大日志后台任务应能立即取消，且不能覆盖已经成功加载的旧文档。
    const std::wstring oldLabel = TextOf(GetDlgItem(window, 1010));
    const int oldMetricCount = ListView_GetItemCount(metrics);
    const std::wstring largeLog = CreateLargeLog();
    if (largeLog.empty() || !ChooseFile(window, process.dwProcessId, 1040, largeLog))
        return finish(26);
    HWND closeButton = GetDlgItem(window, 1030);
    const DWORD cancelStart = GetTickCount();
    while (TextOf(closeButton) != L"取消加载" && GetTickCount() - cancelStart < 5000)
        Sleep(10);
    if (TextOf(closeButton) != L"取消加载") {
        PrintWide("cancel-button", TextOf(closeButton));
        PrintWide("load-label", TextOf(GetDlgItem(window, 1010)));
        PrintWide("load-status", TextOf(GetDlgItem(window, 1011)));
        DeleteFileW(largeLog.c_str());
        return finish(27);
    }
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1030, BN_CLICKED), reinterpret_cast<LPARAM>(closeButton));
    const DWORD cancelWait = GetTickCount();
    while (TextOf(closeButton) != L"关闭日志" && GetTickCount() - cancelWait < 20000)
        Sleep(20);
    DeleteFileW(largeLog.c_str());
    if (TextOf(closeButton) != L"关闭日志")
        return finish(28);
    const std::wstring labelAfterCancel = TextOf(GetDlgItem(window, 1010));
    if (labelAfterCancel != oldLabel || ListView_GetItemCount(metrics) != oldMetricCount) {
        PrintWide("label before cancel", oldLabel);
        PrintWide("label after cancel", labelAfterCancel);
        return finish(29);
    }
    if (TextOf(GetDlgItem(window, 1140)) != oldRangeLabel || !IsWindowEnabled(GetDlgItem(window, 1141)))
        return finish(76);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1141, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1141)));

    // 展开筛选、应用一次搜索，并验证历史使用 REG_MULTI_SZ 落盘。
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1023, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1023)));
    HWND grep = GetDlgItem(window, 1007);
    SetWindowTextW(grep, L"SDK");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1003, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1003)));
    Sleep(150);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\dialLog", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return finish(23);
    DWORD type = 0, bytes = 0;
    const LONG history = RegQueryValueExW(key, L"SearchHistory", nullptr, &type, nullptr, &bytes);
    RegCloseKey(key);
    if (history != ERROR_SUCCESS || type != REG_MULTI_SZ || bytes <= 2 * sizeof(wchar_t))
        return finish(24);

    // 成功载入新文档则清除旧选区和条件；失败/取消上面已经验证为保留。
    SetWindowTextW(GetDlgItem(window, 1008), L"00:00:00");
    SetWindowTextW(GetDlgItem(window, 1009), L"00:10:00");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1003, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1003)));
    if (!IsWindowEnabled(GetDlgItem(window, 1141)))
        return finish(77);
    // 初始成功载入已将样本放到最近文件首项；用实际菜单命令打开，避免
    // 第二次跨进程合成 HDROP 的句柄封送影响“新文档重置”的断言。
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1050, 0), 0);
    if (!WaitForLoad(window, 180000))
        return finish(77);
    const DWORD reloadStart = GetTickCount();
    while (GetTickCount() - reloadStart < 30000 &&
           (IsWindowEnabled(GetDlgItem(window, 1141)) || ListView_GetItemCount(metrics) != fullMetricCount ||
            !TextOf(GetDlgItem(window, 1007)).empty()))
        Sleep(20);
    if (IsWindowEnabled(GetDlgItem(window, 1141)) || !Contains(GetDlgItem(window, 1140), L"全范围") ||
        !TextOf(GetDlgItem(window, 1007)).empty() || !TextOf(GetDlgItem(window, 1008)).empty() ||
        !TextOf(GetDlgItem(window, 1009)).empty() || ListView_GetItemCount(metrics) != fullMetricCount) {
        PrintWide("reload-range", TextOf(GetDlgItem(window, 1140)));
        PrintWide("reload-label", TextOf(GetDlgItem(window, 1010)));
        PrintWide("reload-grep", TextOf(GetDlgItem(window, 1007)));
        PrintWide("reload-metric-count", std::to_wstring(ListView_GetItemCount(metrics)));
        return finish(78);
    }

    SetWindowPos(window, nullptr, 0, 0, 1280, 800, SWP_NOMOVE | SWP_NOZORDER);
    SendMessageW(window, WM_APP + 41, 4, 0);
    SendMessageW(window, WM_COMMAND, 32004, 0);
    if (ListView_GetColumnWidth(metrics, 0) <= 0 || ListView_GetColumnWidth(metrics, 5) != 0 ||
        ListView_GetColumnWidth(metrics, 19) <= 0 || Header_GetItemCount(ListView_GetHeader(metrics)) != 22 ||
        ListView_GetItemCount(metrics) != fullMetricCount) {
        PrintWide("column-preset-state", L"time=" + std::to_wstring(ListView_GetColumnWidth(metrics, 0)) + L" csq=" +
                                             std::to_wstring(ListView_GetColumnWidth(metrics, 5)) + L" at=" +
                                             std::to_wstring(ListView_GetColumnWidth(metrics, 19)) + L" columns=" +
                                             std::to_wstring(Header_GetItemCount(ListView_GetHeader(metrics))) +
                                             L" rows=" + std::to_wstring(ListView_GetItemCount(metrics)) +
                                             L" expected=" + std::to_wstring(fullMetricCount));
        return finish(79);
    }
    RECT metricHeader{};
    POINT metricOrigin{};
    GetWindowRect(ListView_GetHeader(metrics), &metricHeader);
    ClientToScreen(metrics, &metricOrigin);
    const int metricRowY = metricHeader.bottom - metricOrigin.y + 10;
    SendMessageW(metrics, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(24, metricRowY));
    SendMessageW(metrics, WM_LBUTTONUP, 0, MAKELPARAM(24, metricRowY));
    SendMessageW(metrics, WM_KEYDOWN, VK_RETURN, 0);
    if (!Contains(GetDlgItem(window, 1112), L"CSQ") || !Contains(GetDlgItem(window, 1112), L"RSRP"))
        return finish(80);
    Capture(window, L"columns-at-health");
    SendMessageW(window, WM_COMMAND, 1111, 0);
    SendMessageW(window, WM_COMMAND, 32000, 0);
    Capture(window, L"columns-common");
    SendMessageW(window, WM_COMMAND, 32005, 0);
    SetWindowTextW(GetDlgItem(window, 1008), L"00:00:00");
    SetWindowTextW(GetDlgItem(window, 1009), L"00:35:00");
    SendMessageW(window, WM_COMMAND, 1003, 0);
    const int chineseRangeCount = ListView_GetItemCount(metrics);
    SendMessageW(window, WM_COMMAND, 1171, 0);
    if (!Contains(GetDlgItem(window, 1024), L"Signal metrics") || !Contains(GetDlgItem(window, 1141), L"Reset range") ||
        !Contains(GetDlgItem(window, 1140), L"Time conditions") || ListView_GetItemCount(metrics) != chineseRangeCount)
        return finish(81);
    Capture(window, L"english-metrics-range");
    SendMessageW(window, WM_COMMAND, 1141, 0);
    if (ListView_GetItemCount(metrics) != fullMetricCount)
        return finish(82);
    SendMessageW(window, WM_APP + 41, 1, 0);
    Capture(window, L"english-findings");
    RECT englishFindingRect{};
    GetClientRect(GetDlgItem(window, 1019), &englishFindingRect);
    SendMessageW(GetDlgItem(window, 1019), WM_LBUTTONUP, 0, MAKELPARAM(englishFindingRect.right - 100, 35));
    const auto englishFindings = ClipboardText();
    if (englishFindings.find(L"Basis:") == std::wstring::npos ||
        englishFindings.find(L"Advice:") == std::wstring::npos ||
        englishFindings.find(L"Weak signal") == std::wstring::npos ||
        englishFindings.find(L"CH:ROAMLINK") == std::wstring::npos ||
        englishFindings.find(L"全量日志历史汇总") != std::wstring::npos) {
        PrintWide("english-findings", englishFindings);
        return finish(83);
    }
    Capture(window, L"english-findings");
    SendMessageW(window, WM_APP + 41, 0, 0);
    Capture(window, L"english-overview");
    RECT englishDashRect{};
    GetClientRect(GetDlgItem(window, 1022), &englishDashRect);
    SendMessageW(GetDlgItem(window, 1022), WM_LBUTTONUP, 0, MAKELPARAM(englishDashRect.right - 95, 24));
    if (ClipboardText().find(L"Outages 36") == std::wstring::npos ||
        ClipboardText().find(L"1359 samples") == std::wstring::npos ||
        ClipboardText().find(L"Valid RSSI samples: 516") == std::wstring::npos)
        return finish(84);
    Capture(window, L"english-overview");
    SetWindowPos(window, nullptr, 0, 0, 860, 640, SWP_NOMOVE | SWP_NOZORDER);
    if (!AppearanceFitsNavigation(window) ||
        !CheckAppearanceMenu(window, process.dwProcessId, L"appearance-menu-english-narrow"))
        return finish(134);
    Capture(window, L"english-overview-narrow");
    SendMessageW(window, WM_APP + 41, 4, 0);
    Capture(window, L"english-metrics-narrow");
    SendMessageW(window, WM_COMMAND, 33000, 0);
    WindowSearch comparisonSearch{process.dwProcessId, nullptr, L"dialSourceComparison"};
    EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&comparisonSearch));
    if (!comparisonSearch.window || ListView_GetItemCount(GetDlgItem(comparisonSearch.window, 1162)) != 18)
        return finish(85);
    Capture(comparisonSearch.window, L"english-source-comparison");
    SendMessageW(comparisonSearch.window, WM_CLOSE, 0, 0);
    // A single-source comparison also needs fresh statistics when its time range changes.
    SetWindowTextW(GetDlgItem(window, 1008), L"00:00:00");
    SetWindowTextW(GetDlgItem(window, 1009), L"00:34:41");
    SendMessageW(window, WM_COMMAND, 1003, 0);
    SendMessageW(window, WM_COMMAND, 33000, 0);
    comparisonSearch.window = nullptr;
    EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&comparisonSearch));
    if (!comparisonSearch.window)
        return finish(87);
    SendMessageW(comparisonSearch.window, WM_COMMAND, 1166, 0);
    if (ClipboardText().find(L"Metric samples\t68\t68\r\n") == std::wstring::npos ||
        ClipboardText().find(L"Valid RSSI samples\t30\t30\r\n") == std::wstring::npos) {
        PrintWide("single-source-range-comparison", ClipboardText());
        return finish(88);
    }
    SendMessageW(comparisonSearch.window, WM_CLOSE, 0, 0);
    SendMessageW(window, WM_COMMAND, 1141, 0);
    SendMessageW(window, WM_COMMAND, 1170, 0);
    if (!Contains(GetDlgItem(window, 1024), L"信号指标") || ListView_GetItemCount(metrics) != fullMetricCount)
        return finish(86);

    PostMessageW(window, WM_CLOSE, 0, 0);
    const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exitCode == 0 ? MultiSourceSmoke(executable) : 25;
}
