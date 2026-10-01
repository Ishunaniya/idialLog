// 最小 Win32 GUI 烟测：启动真实 exe，等待后台加载完成，并核对导航、指标列与设置持久化。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

std::wstring TextOf(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, &text[0], length + 1);
    text.resize(static_cast<size_t>(copied));
    return text;
}

bool Contains(HWND window, const wchar_t* text) {
    return TextOf(window).find(text) != std::wstring::npos;
}

void PrintWide(const char* name, const std::wstring& value) {
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string text(name);
    text += ": ";
    const size_t offset = text.size();
    text.resize(offset + static_cast<size_t>(length));
    if (length > 0)
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                            &text[offset], length, nullptr, nullptr);
    text += "\r\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text.data(),
              static_cast<DWORD>(text.size()), &written, nullptr);
}

std::wstring ClipboardText() {
    if (!OpenClipboard(nullptr)) return L"";
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    const auto* value = handle ? static_cast<const wchar_t*>(GlobalLock(handle)) : nullptr;
    std::wstring result = value ? value : L"";
    if (value) GlobalUnlock(handle);
    CloseClipboard();
    return result;
}

// 仅在显式指定目录时保存实际窗口截图，便于核验自绘文字与小窗口布局。
void Capture(HWND window, const wchar_t* name) {
    wchar_t directory[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"DIALLOG_UI_CAPTURE", directory, MAX_PATH)) return;
    CreateDirectoryW(directory, nullptr);
    HWND notice = FindWindowW(L"dialModernNotice", nullptr);
    if (notice && GetWindow(notice, GW_OWNER) == window) SendMessageW(notice, WM_LBUTTONUP, 0, 0);
    Sleep(100);
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    RECT rect{}; GetWindowRect(window, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    HDC screen = GetWindowDC(window), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, screen, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
    GetDIBits(screen, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    BITMAPFILEHEADER header{}; header.bfType = 0x4D42;
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
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, screen);
}

struct WindowSearch { DWORD processId = 0; HWND window = nullptr; const wchar_t* className = L"dialLogMainCls"; };

BOOL CALLBACK FindProcessWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0; GetWindowThreadProcessId(window, &processId);
    wchar_t className[64]{}; GetClassNameW(window, className, 64);
    if (processId == search.processId && std::wstring(className) == search.className) {
        search.window = window; return FALSE;
    }
    return TRUE;
}

HWND WaitForMain(DWORD processId, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    while (GetTickCount() - start < timeoutMs) {
        WindowSearch search{processId, nullptr};
        EnumWindows(FindProcessWindow, reinterpret_cast<LPARAM>(&search));
        if (search.window) return search.window;
        Sleep(50);
    }
    return nullptr;
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
        if (!dialog) Sleep(50);
    }
    if (!dialog) { PrintWide("font-dialog-missing", name); return false; }
    HWND family = GetDlgItem(dialog, 0x470); // 标准 CHOOSEFONT 的 cmb1
    if (!family) { SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0); return false; }
    const wchar_t* candidates[] = {logFace ? name : L"Noto Sans CJK SC", name,
                                  logFace ? L"Liberation Mono" : L"Liberation Sans"};
    std::wstring chosen;
    for (const wchar_t* candidate : candidates) {
        const LRESULT index = SendMessageW(family, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(candidate));
        if (index != CB_ERR) {
            SendMessageW(family, CB_SETCURSEL, index, 0); chosen = candidate; break;
        }
    }
    if (chosen.empty()) {
        PrintWide("font-not-listed", name); SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0); return false;
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
        if (status == ERROR_SUCCESS && std::wstring(saved) == chosen) return true;
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
        if (label && close && !Contains(label, L"未加载") && TextOf(close) == L"关闭日志") return true;
        Sleep(50);
    }
    return false;
}

std::wstring CreateLargeLog() {
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"dlg", 0, path)) return L"";
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) return L"";
    const std::string line =
        "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | Cell:1D8DE0B | CSQ:18 | RX_PKT:100\r\n";
    std::string block;
    while (block.size() < 1024 * 1024) block += line;
    bool good = true;
    for (int index = 0; index < 64; ++index) {
        DWORD written = 0;
        if (!WriteFile(file, block.data(), static_cast<DWORD>(block.size()), &written, nullptr) ||
            written != block.size()) { good = false; break; }
    }
    CloseHandle(file);
    if (!good) { DeleteFileW(path); return L""; }
    return path;
}

bool DropFile(HWND window, const std::wstring& path) {
    struct DropFilesHeader { DWORD pFiles; POINT point; BOOL nonClient; BOOL wide; };
    const size_t bytes = sizeof(DropFilesHeader) + (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory) return false;
    auto* drop = static_cast<DropFilesHeader*>(GlobalLock(memory));
    if (!drop) { GlobalFree(memory); return false; }
    drop->pFiles = sizeof(DropFilesHeader); drop->wide = TRUE;
    auto* name = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(drop) + drop->pFiles);
    memcpy(name, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    SendMessageW(window, WM_DROPFILES, reinterpret_cast<WPARAM>(memory), 0);
    return true; // 接收方 DragFinish 负责释放 HDROP。
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc != 3) return 90;
    // 新输入必须不继承旧会话筛选；故意写入必定不匹配的条件验证该回归点。
    HKEY settings = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\dialLog", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &settings, nullptr) == ERROR_SUCCESS) {
        const wchar_t stale[] = L"__stale_filter_must_not_apply__";
        for (const wchar_t* name : {L"TagFilter", L"GrepFilter", L"SinceFilter", L"UntilFilter"})
            RegSetValueExW(settings, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(stale), sizeof(stale));
        const wchar_t interfaceFace[] = L"Microsoft YaHei UI", logFace[] = L"Consolas";
        const DWORD filtersExpanded = 1;
        RegSetValueExW(settings, L"FiltersExpanded", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&filtersExpanded), sizeof(filtersExpanded));
        RegSetValueExW(settings, L"UiFont", 0, REG_SZ, reinterpret_cast<const BYTE*>(interfaceFace), sizeof(interfaceFace));
        RegSetValueExW(settings, L"LogFont", 0, REG_SZ, reinterpret_cast<const BYTE*>(logFace), sizeof(logFace));
        RegCloseKey(settings);
    }
    std::wstring command = L"\"" + std::wstring(argv[1]) + L"\" \"" + argv[2] + L"\"";
    LocalFree(argv);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &process)) return 91;
    CloseHandle(process.hThread);

    HWND window = WaitForMain(process.dwProcessId, 30000);
    if (!window) { TerminateProcess(process.hProcess, 92); CloseHandle(process.hProcess); return 1; }
    auto finish = [&](int code) {
        if (IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
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
    if (Contains(GetDlgItem(window, 1010), L"未加载")) return finish(3);
    SetWindowPos(window, nullptr, 0, 0, 1280, 800, SWP_NOMOVE | SWP_NOZORDER);
    UpdateWindow(window);

    const wchar_t* titles[] = {L"概览", L"诊断结论", L"事件时间线", L"断网记录",
                               L"信号指标", L"标签统计", L"原始日志", L"未识别行", L"小区分析"};
    for (int page = 0; page < 9; ++page) {
        SendMessageW(window, WM_APP + 41, page, 0);
        UpdateWindow(window);
        if (TextOf(GetDlgItem(window, 1024)) != titles[page]) return finish(10 + page);
    }

    SendMessageW(window, WM_APP + 41, 0, 0);
    Capture(window, L"overview-wide");
    HWND dashboard = GetDlgItem(window, 1022);
    RECT dashboardRect{}; GetClientRect(dashboard, &dashboardRect);
    SendMessageW(dashboard, WM_LBUTTONUP, 0, MAKELPARAM(dashboardRect.right - 95, 24));
    const std::wstring overviewText = ClipboardText();
    if (overviewText.find(L"首次联网后运行期可用率：") == std::wstring::npos ||
        overviewText.find(L"全程服务可达率：") == std::wstring::npos ||
        overviewText.find(L"2026-06-30") == std::wstring::npos ||
        overviewText.find(L"1359 个样本") == std::wstring::npos ||
        overviewText.find(L"断网 36 次 · 已恢复事件累计 34m02s · 最长 3m20s") == std::wstring::npos) {
        PrintWide("overview-clipboard", overviewText); return finish(43);
    }
    HWND summary = GetDlgItem(window, 1011);
    RECT summaryRect{}; GetClientRect(summary, &summaryRect);
    SendMessageW(summary, WM_LBUTTONUP, 0, MAKELPARAM(summaryRect.right - 76, 32));
    const std::wstring cardText = ClipboardText();
    if (cardText.find(L"可用率与观测范围") != 0 || cardText.find(L"关键事件计数") != std::wstring::npos) return finish(44);
    SendMessageW(window, WM_APP + 41, 1, 0);
    Capture(window, L"findings-wide");
    HWND findings = GetDlgItem(window, 1019);
    RECT findingsRect{}; GetClientRect(findings, &findingsRect);
    SendMessageW(findings, WM_LBUTTONUP, 0, MAKELPARAM(findingsRect.right - 100, 35));
    const std::wstring findingsText = ClipboardText();
    if (findingsText.find(L"诊断结论") != 0 || findingsText.find(L"来源平台：") == std::wstring::npos ||
        findingsText.find(L"依据：") == std::wstring::npos || findingsText.find(L"建议：") == std::wstring::npos ||
        findingsText.find(L"第 ") == std::wstring::npos) return finish(45);

    SendMessageW(window, WM_APP + 41, 4, 0);
    Capture(window, L"metrics-wide");
    HWND metrics = GetDlgItem(window, 1014);
    HWND chart = GetDlgItem(window, 1017);
    HWND header = ListView_GetHeader(metrics);
    if (!header || Header_GetItemCount(header) != 22) return finish(20);
    if (!IsWindowVisible(chart)) return finish(22);
    if (ListView_GetColumnWidth(metrics, 0) < 180) return finish(46);
    RECT chartRect{}; GetClientRect(chart, &chartRect);
    SendMessageW(chart, WM_MOUSEMOVE, 0, MAKELPARAM(chartRect.right - 155, 85));
    Capture(window, L"metrics-hover");
    SendMessageW(chart, WM_MOUSELEAVE, 0, 0);
    SetWindowPos(window, nullptr, 0, 0, 1280, 650, SWP_NOMOVE | SWP_NOZORDER);
    GetClientRect(chart, &chartRect);
    if (chartRect.bottom < 160 || chartRect.bottom >= 77 + 3 * 70) return finish(51);
    Capture(window, L"metrics-compressed");
    SetWindowPos(window, nullptr, 0, 0, 1280, 800, SWP_NOMOVE | SWP_NOZORDER);
    HWND chartMode = GetDlgItem(window, 1047), splitMode = GetDlgItem(window, 1048);
    HWND tableMode = GetDlgItem(window, 1049);
    if (!chartMode || !splitMode || !tableMode || !IsWindowVisible(splitMode)) return finish(33);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1049, BN_CLICKED),
                 reinterpret_cast<LPARAM>(tableMode));
    if (!IsWindowVisible(metrics) || IsWindowVisible(chart)) return finish(34);
    RECT metricPage{}, metricClient{};
    GetWindowRect(metrics, &metricPage);
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&metricPage), 2);
    GetClientRect(window, &metricClient);
    if (metricPage.right < metricClient.right - 30 || metricPage.bottom < metricClient.bottom - 70)
        return finish(35);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1048, BN_CLICKED),
                 reinterpret_cast<LPARAM>(splitMode));
    if (!IsWindowVisible(metrics) || !IsWindowVisible(chart)) return finish(36);

    SendMessageW(window, WM_APP + 41, 6, 0);
    HWND raw = GetDlgItem(window, 1016);
    if (!raw) return finish(30);
    if (!ListView_GetHeader(raw)) return finish(39);
    if (ListView_GetItemCount(raw) < 1) return finish(40);
    if (!(GetWindowLongPtrW(raw, GWL_STYLE) & LVS_OWNERDATA)) return finish(41);
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
    if (ListView_GetSelectedCount(raw) < 1) return finish(42);
    SendMessageW(raw, WM_KEYDOWN, VK_RETURN, 0);
    HWND detail = GetDlgItem(window, 1112);
    if (!detail || !IsWindowVisible(detail) || SendMessageW(detail, WM_GETTEXTLENGTH, 0, 0) < 20) return finish(37);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1111, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(window, 1111)));
    if (IsWindowVisible(detail)) return finish(38);
    SendMessageW(window, WM_APP + 41, 8, 0);
    HWND cells = GetDlgItem(window, 1026);
    if (!cells || Header_GetItemCount(ListView_GetHeader(cells)) != 15 ||
        ListView_GetItemCount(cells) < 1) return finish(31);
    SetWindowPos(window, nullptr, 0, 0, 860, 640, SWP_NOMOVE | SWP_NOZORDER);
    UpdateWindow(window);
    RECT pageTitle{}, exportButton{};
    GetWindowRect(GetDlgItem(window, 1024), &pageTitle);
    GetWindowRect(GetDlgItem(window, 1005), &exportButton);
    if (pageTitle.right > exportButton.left) return finish(32);
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
    if (chartRect.bottom < 77 + 3 * 70) return finish(50);
    if (!GetDlgItem(window, 1135) || !IsWindowVisible(GetDlgItem(window, 1135))) return finish(47);

    if (!ChooseTypeface(window, process.dwProcessId, false, L"Arial")) return finish(48);
    if (!ChooseTypeface(window, process.dwProcessId, true, L"Courier New")) return finish(49);
    Capture(window, L"fonts-selected");

    // 大日志后台任务应能立即取消，且不能覆盖已经成功加载的旧文档。
    const std::wstring oldLabel = TextOf(GetDlgItem(window, 1010));
    const int oldMetricCount = ListView_GetItemCount(metrics);
    const std::wstring largeLog = CreateLargeLog();
    if (largeLog.empty() || !DropFile(window, largeLog)) return finish(26);
    HWND closeButton = GetDlgItem(window, 1030);
    const DWORD cancelStart = GetTickCount();
    while (TextOf(closeButton) != L"取消加载" && GetTickCount() - cancelStart < 5000) Sleep(10);
    if (TextOf(closeButton) != L"取消加载") { DeleteFileW(largeLog.c_str()); return finish(27); }
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1030, BN_CLICKED),
                 reinterpret_cast<LPARAM>(closeButton));
    const DWORD cancelWait = GetTickCount();
    while (TextOf(closeButton) != L"关闭日志" && GetTickCount() - cancelWait < 20000) Sleep(20);
    DeleteFileW(largeLog.c_str());
    if (TextOf(closeButton) != L"关闭日志") return finish(28);
    const std::wstring labelAfterCancel = TextOf(GetDlgItem(window, 1010));
    if (labelAfterCancel != oldLabel || ListView_GetItemCount(metrics) != oldMetricCount) {
        PrintWide("label before cancel", oldLabel);
        PrintWide("label after cancel", labelAfterCancel);
        return finish(29);
    }

    // 展开筛选、应用一次搜索，并验证历史使用 REG_MULTI_SZ 落盘。
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1023, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(window, 1023)));
    HWND grep = GetDlgItem(window, 1007);
    SetWindowTextW(grep, L"SDK");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1003, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(window, 1003)));
    Sleep(150);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\dialLog", 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return finish(23);
    DWORD type = 0, bytes = 0;
    const LONG history = RegQueryValueExW(key, L"SearchHistory", nullptr, &type, nullptr, &bytes);
    RegCloseKey(key);
    if (history != ERROR_SUCCESS || type != REG_MULTI_SZ || bytes <= 2 * sizeof(wchar_t)) return finish(24);

    PostMessageW(window, WM_CLOSE, 0, 0);
    const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exitCode == 0 ? 0 : 25;
}
