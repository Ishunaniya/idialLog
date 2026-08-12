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
    GetWindowTextW(window, &text[0], length + 1);
    text.resize(length);
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

struct WindowSearch { DWORD processId = 0; HWND window = nullptr; };

BOOL CALLBACK FindProcessWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD processId = 0; GetWindowThreadProcessId(window, &processId);
    wchar_t className[64]{}; GetClassNameW(window, className, 64);
    if (processId == search.processId && std::wstring(className) == L"dialLogMainCls") {
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

bool WaitForLoad(HWND window, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    HWND label = GetDlgItem(window, 1010);
    HWND close = GetDlgItem(window, 1030);
    while (GetTickCount() - start < timeoutMs) {
        if (!Contains(label, L"未加载") && TextOf(close) == L"关闭日志") return true;
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
    // Wine 首次冷启动可能初始化字体/注册表；真实 Windows 通常远快于此上限。
    if (!WaitForLoad(window, 60000)) return finish(2);
    if (Contains(GetDlgItem(window, 1010), L"未加载")) return finish(3);

    const wchar_t* titles[] = {L"概览", L"诊断结论", L"事件时间线", L"断网记录",
                               L"信号指标", L"标签统计", L"原始日志", L"未识别行"};
    for (int page = 0; page < 8; ++page) {
        SendMessageW(window, WM_APP + 41, page, 0);
        UpdateWindow(window);
        if (TextOf(GetDlgItem(window, 1024)) != titles[page]) return finish(10 + page);
    }

    SendMessageW(window, WM_APP + 41, 4, 0);
    HWND metrics = GetDlgItem(window, 1014);
    HWND header = ListView_GetHeader(metrics);
    if (!header || Header_GetItemCount(header) != 18) return finish(20);
    if (!IsWindowVisible(GetDlgItem(window, 1017))) return finish(22);

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
