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
    // EDIT 的内容需要标准消息封送；其他控件直接读取窗口文字，避免为轮询
    // 后台加载按钮而向目标 UI 线程同步发送消息。
    wchar_t className[32]{}; GetClassNameW(window, className, 32);
    const bool edit = _wcsicmp(className, L"EDIT") == 0;
    const int length = edit ? static_cast<int>(SendMessageW(window, WM_GETTEXTLENGTH, 0, 0))
                            : GetWindowTextLengthW(window);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = edit ? static_cast<int>(SendMessageW(window, WM_GETTEXT, length + 1, reinterpret_cast<LPARAM>(&text[0])))
                            : GetWindowTextW(window, &text[0], length + 1);
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
void Capture(HWND window, const wchar_t* name, bool refresh = true) {
    wchar_t directory[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"DIALLOG_UI_CAPTURE", directory, MAX_PATH)) return;
    CreateDirectoryW(directory, nullptr);
    HWND notice = FindWindowW(L"dialModernNotice", nullptr);
    if (notice && GetWindow(notice, GW_OWNER) == window) SendMessageW(notice, WM_LBUTTONUP, 0, 0);
    if (refresh) {
        Sleep(100);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
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
    if (processId == search.processId && std::wstring(className) == search.className && IsWindowVisible(window)) {
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

bool AppearanceFitsNavigation(HWND window) {
    HWND button=GetDlgItem(window,1135), navigation=GetDlgItem(window,1001);
    RECT settings{}, sidebar{};
    if (!button || !navigation || !IsWindowVisible(button) ||
        !GetWindowRect(button,&settings) || !GetWindowRect(navigation,&sidebar)) return false;
    POINT center{(settings.left+settings.right)/2,(settings.top+settings.bottom)/2};
    return settings.left>sidebar.left && settings.right<sidebar.right &&
        settings.top>sidebar.top+(sidebar.bottom-sidebar.top)/2 && settings.bottom<sidebar.bottom &&
        WindowFromPoint(center)==button;
}

bool CheckAppearanceMenu(HWND window,DWORD processId,const wchar_t* captureName) {
    PostMessageW(GetDlgItem(window,1135),BM_CLICK,0,0);
    WindowSearch search{processId,nullptr,L"#32768"};
    const DWORD start=GetTickCount();
    while (!search.window && GetTickCount()-start<3000) {
        EnumWindows(FindProcessWindow,reinterpret_cast<LPARAM>(&search));
        if (!search.window) Sleep(25);
    }
    if (!search.window) return false;
    RECT menu{}, button{};
    GetWindowRect(search.window,&menu);GetWindowRect(GetDlgItem(window,1135),&button);
    const bool above=menu.bottom<=button.top && menu.top>=0;
    Capture(window,captureName);
    Capture(search.window,(std::wstring(captureName)+L"-popup").c_str());
    SendMessageW(window,WM_CANCELMODE,0,0);
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

std::string ReadBytes(const std::wstring& path) {
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE) return {};
    DWORD size=GetFileSize(file,nullptr),read=0;
    std::string data(size,'\0');
    if(size) ReadFile(file,&data[0],size,&read,nullptr);
    CloseHandle(file);data.resize(read);return data;
}

BOOL CALLBACK FindFilenameControl(HWND child,LPARAM parameter) {
    const int id=GetDlgCtrlID(child);
    if(id==0x480 || id==0x47c) {
        *reinterpret_cast<HWND*>(parameter)=child;return FALSE;
    }
    return TRUE;
}

HWND FilenameControl(HWND dialog) {
    HWND control=nullptr;
    EnumChildWindows(dialog,FindFilenameControl,reinterpret_cast<LPARAM>(&control));
    return control;
}

bool ChooseFile(HWND window,DWORD processId,int command,const std::wstring& path) {
    PostMessageW(window,WM_COMMAND,command,0);
    HWND dialog=nullptr;
    const DWORD start=GetTickCount();
    while(!dialog && GetTickCount()-start<15000) {
        WindowSearch search{processId,nullptr,L"#32770"};
        EnumWindows(FindProcessWindow,reinterpret_cast<LPARAM>(&search));
        if(search.window && FilenameControl(search.window)) dialog=search.window;
        if(!dialog) Sleep(50);
    }
    if(!dialog) {PrintWide("export-dialog-missing",path);return false;}
    // Explorer dialogs may pump messages while their initial shell folder is still being built.
    Sleep(750);
    HWND filename=FilenameControl(dialog);
    SetWindowTextW(filename,path.c_str());
    SendMessageW(dialog,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(filename),CBN_EDITCHANGE),reinterpret_cast<LPARAM>(filename));
    Capture(dialog,command==1040?L"open-cancellation-file-dialog":L"english-export-dialog");
    SendMessageW(GetDlgItem(dialog,IDOK),BM_CLICK,0,0);
    const DWORD closing=GetTickCount();
    while(IsWindow(dialog) && GetTickCount()-closing<10000) Sleep(50);
    if(IsWindow(dialog)) {
        Capture(dialog,L"file-dialog-did-not-close");
        PrintWide("file-dialog-did-not-close",TextOf(filename));
        PostMessageW(dialog,WM_COMMAND,IDCANCEL,0);return false;
    }
    return true;
}

bool ExportTo(HWND window,DWORD processId,int command,const std::wstring& path) {
    if(!ChooseFile(window,processId,command,path)) return false;
    const DWORD wait=GetTickCount();
    while(GetTickCount()-wait<15000) {
        if(!ReadBytes(path).empty()) return true;
        Sleep(50);
    }
    PrintWide("export-file-missing",path);return false;
}

int MultiSourceSmoke(const std::wstring& executable) {
    wchar_t directory[MAX_PATH]{},temporary[MAX_PATH]{};
    if(!GetTempPathW(MAX_PATH,directory) || !GetTempFileNameW(directory,L"dls",0,temporary)) return 100;
    DeleteFileW(temporary);
    const std::wstring base=temporary;
    const std::wstring paths[]={base+L"_A.log",base+L"_B_现场.log"};
    const std::string input[]={
        "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | CSQ:18 | RSRP:-95 | RSRQ:-8 SNR:120 RSSI:-65\r\n"
        "[2026-08-03 10:00:01] [SDK] Ping failed, fault timer started\r\n"
        "[2026-08-03 10:00:03] [SDK] Network recovered after 2s\r\n",
        "[2026-08-03 10:00:00] [HEARTBEAT] CH:SIM | CSQ:8 | RSRP:-115 | RSRQ:-19 SNR:-20 RSSI:-100 | OPER:现场\r\n"
        "[2026-08-03 10:00:02] [SDK] Ping failed, fault timer started\r\n"
        "[2026-08-03 10:00:09] [SDK] Network recovered after 7s\r\n"};
    for(int i=0;i<2;++i) {
        HANDLE file=CreateFileW(paths[i].c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
        if(file==INVALID_HANDLE_VALUE) return 101;
        DWORD written=0;WriteFile(file,input[i].data(),static_cast<DWORD>(input[i].size()),&written,nullptr);CloseHandle(file);
        if(written!=input[i].size()) return 102;
    }
    std::wstring command=L"\""+executable+L"\" --tab=4 \""+paths[0]+L"\" \""+paths[1]+L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(),command.end());mutableCommand.push_back(0);
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,mutableCommand.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&process)) return 103;
    CloseHandle(process.hThread);
    HWND window=WaitForMain(process.dwProcessId,90000);
    auto finish=[&](int code) {
        if(window && IsWindow(window)) PostMessageW(window,WM_CLOSE,0,0);
        if(WaitForSingleObject(process.hProcess,10000)!=WAIT_OBJECT_0) TerminateProcess(process.hProcess,code);
        CloseHandle(process.hProcess);
        for(const auto& path:paths) DeleteFileW(path.c_str());
        for(const wchar_t* extension:{L".md",L".html",L".csv"}) DeleteFileW((base+extension).c_str());
        return code;
    };
    if(!window || !WaitForLoad(window,90000)) return finish(104);
    HWND metrics=GetDlgItem(window,1014);
    if(ListView_GetItemCount(metrics)!=1 || !Contains(GetDlgItem(window,1010),L"_A.log")) return finish(105);
    SendMessageW(window,WM_COMMAND,34001,0);
    if(ListView_GetItemCount(metrics)!=1 || !Contains(GetDlgItem(window,1010),L"_B_现场.log")) return finish(106);
    SendMessageW(window,WM_APP+41,6,0);
    HWND raw=GetDlgItem(window,1016);
    if(ListView_GetItemCount(raw)!=3) return finish(107);
    RECT header{};POINT origin{};GetWindowRect(ListView_GetHeader(raw),&header);ClientToScreen(raw,&origin);
    const int rowY=header.bottom-origin.y+10;
    SendMessageW(raw,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(24,rowY));SendMessageW(raw,WM_LBUTTONUP,0,MAKELPARAM(24,rowY));
    SendMessageW(raw,WM_COPY,0,0);
    if(ClipboardText().find(L"4\t2026-08-03 10:00:00")==std::wstring::npos || ClipboardText().find(L"OPER:现场")==std::wstring::npos) {
        PrintWide("source-B-raw-copy",ClipboardText());return finish(108);
    }
    SendMessageW(window,WM_COMMAND,33001,0);
    SendMessageW(window,WM_APP+41,4,0);
    if(ListView_GetItemCount(metrics)!=2) return finish(109);
    SendMessageW(window,WM_COMMAND,34001,0);
    SetWindowTextW(GetDlgItem(window,1008),L"10:00:01");SetWindowTextW(GetDlgItem(window,1009),L"10:00:03");
    SendMessageW(window,WM_COMMAND,1003,0);
    if(ListView_GetItemCount(metrics)!=0) return finish(110);
    SendMessageW(window,WM_COMMAND,1141,0);
    if(ListView_GetItemCount(metrics)!=1 || !Contains(GetDlgItem(window,1010),L"_B_现场.log")) return finish(111);
    SetWindowPos(window,nullptr,0,0,1280,850,SWP_NOMOVE|SWP_NOZORDER);
    HWND signalChart=GetDlgItem(window,1017);
    RedrawWindow(signalChart,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
    RECT signalRect{};GetClientRect(signalChart,&signalRect);
    const int signalSection=(signalRect.bottom-77)/3;
    SendMessageW(signalChart,WM_LBUTTONDOWN,0,MAKELPARAM(signalRect.right-150,26+signalSection+11));
    if(!Contains(signalChart,L"RSSI") || !Contains(signalChart,L"1 点")) {
        PrintWide("rssi-chart-label",TextOf(signalChart));Capture(window,L"rssi-switch-failure");return finish(125);
    }
    SendMessageW(signalChart,WM_MOUSEMOVE,0,MAKELPARAM(signalRect.right/2,26+signalSection+45));
    Capture(window,L"rssi-reported-hover");
    SendMessageW(signalChart,WM_LBUTTONUP,0,0);
    SendMessageW(window,WM_COMMAND,1171,0);
    if(!Contains(signalChart,L"RSSI") || !Contains(signalChart,L"1 points") ||
       !Contains(GetDlgItem(window,1135),L"Language / Fonts")) return finish(126);
    SendMessageW(window,WM_COMMAND,33000,0);
    WindowSearch search{process.dwProcessId,nullptr,L"dialSourceComparison"};EnumWindows(FindProcessWindow,reinterpret_cast<LPARAM>(&search));
    if(!search.window || SendDlgItemMessageW(search.window,1160,CB_GETCOUNT,0,0)!=2) return finish(112);
    SendMessageW(search.window,WM_COMMAND,1166,0);
    const auto comparison=ClipboardText();
    if(comparison.find(L"-95 / -95 / -95")==std::wstring::npos || comparison.find(L"-115 / -115 / -115")==std::wstring::npos ||
       comparison.find(L"18 / 18 / 18")==std::wstring::npos || comparison.find(L"8 / 8 / 8")==std::wstring::npos) {
        PrintWide("source-comparison-copy",comparison);return finish(113);
    }
    Capture(search.window,L"english-two-source-comparison");SendMessageW(search.window,WM_CLOSE,0,0);
    Capture(window,L"english-two-source-metrics");
    SendMessageW(window,WM_COMMAND,32004,0);
    if(!ExportTo(window,process.dwProcessId,1044,base+L".md")) return finish(114);
    const auto markdown=ReadBytes(base+L".md");
    if(markdown.find("Advice:")==std::string::npos || markdown.find("检查天线")!=std::string::npos ||
       markdown.find("_B_现场.log")==std::string::npos || markdown.find("Current-source history summary")==std::string::npos) {
        PrintWide("markdown-export-failure",L"Advice/source scope/original source name");return finish(115);
    }
    if(!ExportTo(window,process.dwProcessId,1046,base+L".html")) return finish(116);
    const auto html=ReadBytes(base+L".html");
    if(html.find("lang=\"en-US\"")==std::string::npos || html.find("_B_现场.log")==std::string::npos || html.find("检查天线")!=std::string::npos) return finish(117);
    if(!ExportTo(window,process.dwProcessId,1045,base+L".csv")) return finish(118);
    const auto csv=ReadBytes(base+L".csv");
    if(csv.find("detailed_at_stage")==std::string::npos || csv.find("现场")==std::string::npos ||
       csv.find("-115")==std::string::npos || csv.find("-100")==std::string::npos) return finish(119);
    // Exercise the real DPI message with its rectangle in the target process.
    // An owned comparison must not keep the font that the main window deletes.
    SendMessageW(window,WM_COMMAND,33000,0);
    search.window=nullptr;EnumWindows(FindProcessWindow,reinterpret_cast<LPARAM>(&search));
    if(!search.window) return finish(120);
    RECT suggested{};GetWindowRect(window,&suggested);
    void* remoteRect=VirtualAllocEx(process.hProcess,nullptr,sizeof(suggested),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    SIZE_T copied=0;
    if(!remoteRect || !WriteProcessMemory(process.hProcess,remoteRect,&suggested,sizeof(suggested),&copied) || copied!=sizeof(suggested)) {
        if(remoteRect) VirtualFreeEx(process.hProcess,remoteRect,0,MEM_RELEASE);
        return finish(121);
    }
    const HWND oldComparison=search.window;
    SendMessageW(window,WM_DPICHANGED,MAKEWPARAM(144,144),reinterpret_cast<LPARAM>(remoteRect));
    VirtualFreeEx(process.hProcess,remoteRect,0,MEM_RELEASE);
    if(IsWindow(oldComparison) || ListView_GetItemCount(metrics)!=1 || !Contains(GetDlgItem(window,1010),L"_B_现场.log")) return finish(122);
    SendMessageW(window,WM_COMMAND,33000,0);
    search.window=nullptr;EnumWindows(FindProcessWindow,reinterpret_cast<LPARAM>(&search));
    if(!search.window) return finish(123);
    SendMessageW(search.window,WM_COMMAND,1166,0);
    if(ClipboardText()!=comparison) return finish(124);
    Capture(search.window,L"english-source-comparison-after-dpi");
    SendMessageW(search.window,WM_CLOSE,0,0);
    SendMessageW(window,WM_COMMAND,1170,0);SendMessageW(window,WM_COMMAND,32005,0);
    return finish(0);
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
        const DWORD filtersExpanded = 1, english = 0, metricColumns = (1u << 22) - 1;
        RegSetValueExW(settings,L"English",0,REG_DWORD,reinterpret_cast<const BYTE*>(&english),sizeof(english));
        RegSetValueExW(settings,L"MetricColumns",0,REG_DWORD,reinterpret_cast<const BYTE*>(&metricColumns),sizeof(metricColumns));
        RegSetValueExW(settings, L"FiltersExpanded", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&filtersExpanded), sizeof(filtersExpanded));
        RegSetValueExW(settings, L"UiFont", 0, REG_SZ, reinterpret_cast<const BYTE*>(interfaceFace), sizeof(interfaceFace));
        RegSetValueExW(settings, L"LogFont", 0, REG_SZ, reinterpret_cast<const BYTE*>(logFace), sizeof(logFace));
        RegCloseKey(settings);
    }
    const std::wstring executable=argv[1];
    std::wstring command = L"\"" + executable + L"\" \"" + argv[2] + L"\"";
    LocalFree(argv);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &startup, &process)) return 91;
    CloseHandle(process.hThread);

    HWND window = WaitForMain(process.dwProcessId, 90000);
    if (!window) { DWORD startCode=0; GetExitCodeProcess(process.hProcess,&startCode); PrintWide("startup-exit-code",std::to_wstring(startCode)); TerminateProcess(process.hProcess, 92); CloseHandle(process.hProcess); return 1; }
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
    SetWindowPos(window, nullptr, 0, 0, 1280, 800, SWP_NOZORDER);
    UpdateWindow(window);
    if (!AppearanceFitsNavigation(window)) return finish(130);
    if (!CheckAppearanceMenu(window,process.dwProcessId,L"appearance-menu-wide")) return finish(131);

    const wchar_t* titles[] = {L"概览", L"诊断结论", L"事件时间线", L"断网记录",
                               L"信号指标", L"标签统计", L"原始日志", L"未识别行", L"小区分析"};
    for (int page = 0; page < 9; ++page) {
        SendMessageW(window, WM_APP + 41, page, 0);
        UpdateWindow(window);
        if (TextOf(GetDlgItem(window, 1024)) != titles[page]) return finish(10 + page);
        if (page == 2 && Header_GetItemCount(ListView_GetHeader(GetDlgItem(window, 1012))) != 4)
            return finish(52);
        if (page == 3 && Header_GetItemCount(ListView_GetHeader(GetDlgItem(window, 1013))) != 6)
            return finish(53);
        if ((page == 2 || page == 3 || page == 6) && !IsWindowVisible(GetDlgItem(window, 1139)))
            return finish(54);
        if (!IsWindowVisible(GetDlgItem(window, 1140)) || !IsWindowVisible(GetDlgItem(window, 1141)) ||
            IsWindowEnabled(GetDlgItem(window, 1141))) return finish(60);
        if (page == 2 || page == 3 || page == 5 || page == 6)
            Capture(window, page == 2 ? L"timeline-wide" : page == 3 ? L"outages-wide" :
                            page == 5 ? L"tags-wide" : L"raw-wide");
    }

    SendMessageW(window, WM_APP + 41, 2, 0);
    HWND timeline = GetDlgItem(window, 1012);
    RECT timelineHeader{}; POINT timelineOrigin{};
    GetWindowRect(ListView_GetHeader(timeline), &timelineHeader); ClientToScreen(timeline, &timelineOrigin);
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
    SCROLLINFO compactFindingsRange{};compactFindingsRange.cbSize=sizeof(compactFindingsRange);compactFindingsRange.fMask=SIF_RANGE;
    const bool haveCompactRange=GetScrollInfo(findings,SB_VERT,&compactFindingsRange)!=FALSE;
    SendMessageW(findings,WM_LBUTTONUP,0,MAKELPARAM(findingsRect.right-210,35));
    Capture(window,L"findings-expanded");
    SCROLLINFO expandedFindingsRange=compactFindingsRange;
    const bool haveExpandedRange=GetScrollInfo(findings,SB_VERT,&expandedFindingsRange)!=FALSE;
    const bool readableRanges=haveCompactRange && haveExpandedRange &&
        compactFindingsRange.nMax>0 && expandedFindingsRange.nMax>0;
    if(!readableRanges) PrintWide("finding-scroll-range",L"Cross-process range unavailable; expansion is checked in captures.");
    SendMessageW(findings,WM_LBUTTONUP,0,MAKELPARAM(findingsRect.right-100,35));
    // Wine may reject GetScrollInfo on another process's custom window.
    // Full-copy equality remains mandatory; captured expanded content is reviewed.
    if(ClipboardText()!=findingsText || (readableRanges &&
        expandedFindingsRange.nMax<=compactFindingsRange.nMax)) {
        PrintWide("finding-expand-ranges",std::to_wstring(compactFindingsRange.nMax)+L" -> "+std::to_wstring(expandedFindingsRange.nMax));
        PrintWide("finding-copy-before",findingsText);PrintWide("finding-copy-after",ClipboardText());return finish(127);
    }
    SendMessageW(findings,WM_LBUTTONUP,0,MAKELPARAM(findingsRect.right-210,35));
    Capture(window,L"findings-compact-restored");

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
    RECT splitChart{}; GetClientRect(chart, &splitChart);
    const DWORD switchStart = GetTickCount();
    for (int repeat = 0; repeat < 8; ++repeat) {
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1047, BN_CLICKED), reinterpret_cast<LPARAM>(chartMode));
        GetClientRect(chart, &chartRect);
        if (chartRect.bottom <= splitChart.bottom || IsWindowVisible(metrics)) return finish(55);
        if (repeat == 7) Capture(window, L"metrics-chart-immediate", false);
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(1048, BN_CLICKED), reinterpret_cast<LPARAM>(splitMode));
        GetClientRect(chart, &chartRect);
        if (chartRect.bottom != splitChart.bottom || !IsWindowVisible(metrics)) return finish(56);
        if (repeat == 7) Capture(window, L"metrics-split-immediate", false);
    }
    PrintWide("chart-switch-16-total-ms", std::to_wstring(GetTickCount() - switchStart));
    const int section = (chartRect.bottom - 77) / 3;
    SendMessageW(chart, WM_LBUTTONDOWN, 0, MAKELPARAM(chartRect.right - 215, 26 + section + 11));
    if(!Contains(chart,L"RSRQ")) return finish(128);
    UpdateWindow(chart); Capture(window, L"metrics-rsrq");
    SendMessageW(chart, WM_LBUTTONDOWN, 0, MAKELPARAM(chartRect.right - 280, 26 + section + 11));
    if(!Contains(chart,L"RSRP")) return finish(129);
    UpdateWindow(chart);

    // EG25 真机：按完整时间轴框选前约 34 分钟，包含前三次完整断网。
    const int plotLeft = 58, plotY = 61;
    const int fullMetricCount = ListView_GetItemCount(metrics);
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
    Capture(window, L"range-selected");
    SendMessageW(window, WM_APP + 41, 3, 0);
    if (ListView_GetItemCount(GetDlgItem(window, 1013)) != 3) return finish(62);
    SendMessageW(window, WM_APP + 41, 2, 0);
    PrintWide("selected-timeline-count", std::to_wstring(ListView_GetItemCount(timeline)));
    if (ListView_GetItemCount(timeline) != 64) return finish(70);
    Capture(window, L"range-timeline");
    SendMessageW(window, WM_APP + 41, 1, 0);
    Capture(window, L"range-findings");
    GetClientRect(findings, &findingsRect);
    SendMessageW(findings, WM_LBUTTONUP, 0, MAKELPARAM(findingsRect.right - 100, 35));
    if (ClipboardText().find(L"选区：2026-06-30 00:00:26") == std::wstring::npos ||
        ClipboardText().find(L"解析审计基于整份输入") == std::wstring::npos ||
        ClipboardText().find(L"当前区间汇总") == std::wstring::npos ||
        ClipboardText().find(L"全量日志历史汇总") != std::wstring::npos) return finish(63);
    SendMessageW(window, WM_APP + 41, 4, 0); UpdateWindow(chart);

    // 反向再框选到故障起点后、恢复前：不能把区间内未见恢复写成全局未恢复。
    SendMessageW(chart, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(plotLeft + 150, plotY));
    SendMessageW(chart, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(plotLeft, plotY));
    SendMessageW(chart, WM_LBUTTONUP, 0, MAKELPARAM(plotLeft, plotY));
    if (ListView_GetItemCount(metrics) != 13 || !Contains(GetDlgItem(window, 1140), L"2026-06-30 00:06:41")) return finish(64);
    SendMessageW(window, WM_APP + 41, 3, 0);
    HWND outages = GetDlgItem(window, 1013);
    if (ListView_GetItemCount(outages) != 1) return finish(65);
    RECT outageHeader{}; POINT outageOrigin{}; GetWindowRect(ListView_GetHeader(outages), &outageHeader);
    ClientToScreen(outages, &outageOrigin);
    const int outageRowY = outageHeader.bottom - outageOrigin.y + 10;
    SendMessageW(outages, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(24, outageRowY));
    SendMessageW(outages, WM_LBUTTONUP, 0, MAKELPARAM(24, outageRowY));
    SendMessageW(outages, WM_COPY, 0, 0);
    if (ClipboardText().find(L"区间内未见恢复") == std::wstring::npos) return finish(66);
    Capture(window, L"range-outage-boundary");

    // 恢复入口跨页面可用，恢复后所有指标和断网都回到原计数。
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1141, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1141)));
    if (ListView_GetItemCount(outages) != 36 || IsWindowEnabled(GetDlgItem(window, 1141)) ||
        !Contains(GetDlgItem(window, 1140), L"全范围")) return finish(67);
    SendMessageW(window, WM_APP + 41, 4, 0); UpdateWindow(chart);
    if (ListView_GetItemCount(metrics) != fullMetricCount) return finish(68);
    SendMessageW(chart, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(plotLeft, plotY));
    SendMessageW(chart, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(plotLeft + 40, plotY));
    SendMessageW(chart, WM_KEYDOWN, VK_ESCAPE, 0);
    SendMessageW(chart, WM_LBUTTONUP, 0, MAKELPARAM(plotLeft + 40, plotY));
    if (IsWindowEnabled(GetDlgItem(window, 1141)) || ListView_GetItemCount(metrics) != fullMetricCount) return finish(69);

    // 消息条件与图表选区独立，恢复全范围仅清除时间条件。
    SetWindowTextW(GetDlgItem(window, 1007), L"SDK");
    SetWindowTextW(GetDlgItem(window, 1008), L"00:00:00");
    SetWindowTextW(GetDlgItem(window, 1009), L"00:35:00");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1003, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1003)));
    if (!Contains(GetDlgItem(window, 1140), L"时间条件") || !IsWindowEnabled(GetDlgItem(window, 1141))) return finish(71);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1141, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1141)));
    if (TextOf(GetDlgItem(window, 1007)) != L"SDK" || !TextOf(GetDlgItem(window, 1008)).empty() ||
        !TextOf(GetDlgItem(window, 1009)).empty() || IsWindowEnabled(GetDlgItem(window, 1141))) return finish(72);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1004, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1004)));
    SetWindowTextW(GetDlgItem(window, 1008), L"12:00:00");
    SetWindowTextW(GetDlgItem(window, 1009), L"12:01:00");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1003, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1003)));
    SendMessageW(window, WM_APP + 41, 6, 0);
    if (ListView_GetItemCount(GetDlgItem(window, 1016)) != 0 || !IsWindowEnabled(GetDlgItem(window, 1141))) return finish(73);
    Capture(window, L"range-empty");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1141, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1141)));
    if (ListView_GetItemCount(GetDlgItem(window, 1016)) != 2860) return finish(74);

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
    SendMessageW(raw, WM_COPY, 0, 0);
    const auto rawText = ClipboardText();
    if (rawText.find(L"2\t2026-06-30 00:00:26\t") != 0 ||
        rawText.find(L"1D8DE0D -> 1D8DE0B") == std::wstring::npos) return finish(57);
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
    if (!AppearanceFitsNavigation(window)) return finish(132);
    if (!CheckAppearanceMenu(window,process.dwProcessId,L"appearance-menu-narrow")) return finish(133);

    // Font enumeration is tested separately when a shared Wine host is memory constrained.
    wchar_t skipFontDialogs[2]{};
    if (!GetEnvironmentVariableW(L"DIALLOG_UI_SKIP_FONT_DIALOGS",skipFontDialogs,2)) {
        if (!ChooseTypeface(window, process.dwProcessId, false, L"Arial")) return finish(48);
        if (!ChooseTypeface(window, process.dwProcessId, true, L"Courier New")) return finish(49);
        Capture(window, L"fonts-selected");
    }

    // 取消另一份日志的加载应同时保留当前选区及其分析结果。
    UpdateWindow(chart);
    SendMessageW(chart, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(plotLeft, plotY));
    SendMessageW(chart, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(plotLeft + 40, plotY));
    SendMessageW(chart, WM_LBUTTONUP, 0, MAKELPARAM(plotLeft + 40, plotY));
    if (!IsWindowEnabled(GetDlgItem(window, 1141))) return finish(75);
    const std::wstring oldRangeLabel = TextOf(GetDlgItem(window, 1140));

    // 大日志后台任务应能立即取消，且不能覆盖已经成功加载的旧文档。
    const std::wstring oldLabel = TextOf(GetDlgItem(window, 1010));
    const int oldMetricCount = ListView_GetItemCount(metrics);
    const std::wstring largeLog = CreateLargeLog();
    if (largeLog.empty() || !ChooseFile(window,process.dwProcessId,1040,largeLog)) return finish(26);
    HWND closeButton = GetDlgItem(window, 1030);
    const DWORD cancelStart = GetTickCount();
    while (TextOf(closeButton) != L"取消加载" && GetTickCount() - cancelStart < 5000) Sleep(10);
    if (TextOf(closeButton) != L"取消加载") {
        PrintWide("cancel-button", TextOf(closeButton));
        PrintWide("load-label", TextOf(GetDlgItem(window, 1010)));
        PrintWide("load-status", TextOf(GetDlgItem(window, 1011)));
        DeleteFileW(largeLog.c_str()); return finish(27);
    }
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
    if (TextOf(GetDlgItem(window, 1140)) != oldRangeLabel || !IsWindowEnabled(GetDlgItem(window, 1141))) return finish(76);
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1141, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1141)));

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

    // 成功载入新文档则清除旧选区和条件；失败/取消上面已经验证为保留。
    SetWindowTextW(GetDlgItem(window, 1008), L"00:00:00");
    SetWindowTextW(GetDlgItem(window, 1009), L"00:10:00");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1003, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(window, 1003)));
    if (!IsWindowEnabled(GetDlgItem(window, 1141))) return finish(77);
    // 初始成功载入已将样本放到最近文件首项；用实际菜单命令打开，避免
    // 第二次跨进程合成 HDROP 的句柄封送影响“新文档重置”的断言。
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(1050, 0), 0);
    if (!WaitForLoad(window, 180000)) return finish(77);
    const DWORD reloadStart = GetTickCount();
    while (GetTickCount() - reloadStart < 30000 &&
           (IsWindowEnabled(GetDlgItem(window, 1141)) || ListView_GetItemCount(metrics) != fullMetricCount ||
            !TextOf(GetDlgItem(window, 1007)).empty())) Sleep(20);
    if (IsWindowEnabled(GetDlgItem(window, 1141)) || !Contains(GetDlgItem(window, 1140), L"全范围") ||
        !TextOf(GetDlgItem(window, 1007)).empty() || !TextOf(GetDlgItem(window, 1008)).empty() ||
        !TextOf(GetDlgItem(window, 1009)).empty() || ListView_GetItemCount(metrics) != fullMetricCount) {
        PrintWide("reload-range", TextOf(GetDlgItem(window, 1140)));
        PrintWide("reload-label", TextOf(GetDlgItem(window, 1010)));
        PrintWide("reload-grep", TextOf(GetDlgItem(window, 1007)));
        PrintWide("reload-metric-count", std::to_wstring(ListView_GetItemCount(metrics)));
        return finish(78);
    }

    SetWindowPos(window,nullptr,0,0,1280,800,SWP_NOMOVE|SWP_NOZORDER);
    SendMessageW(window,WM_APP+41,4,0);
    SendMessageW(window,WM_COMMAND,32004,0);
    if (ListView_GetColumnWidth(metrics,0)<=0 || ListView_GetColumnWidth(metrics,5)!=0 ||
        ListView_GetColumnWidth(metrics,19)<=0 || Header_GetItemCount(ListView_GetHeader(metrics))!=22 ||
        ListView_GetItemCount(metrics)!=fullMetricCount) return finish(79);
    RECT metricHeader{};POINT metricOrigin{};GetWindowRect(ListView_GetHeader(metrics),&metricHeader);ClientToScreen(metrics,&metricOrigin);
    const int metricRowY=metricHeader.bottom-metricOrigin.y+10;
    SendMessageW(metrics,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(24,metricRowY));
    SendMessageW(metrics,WM_LBUTTONUP,0,MAKELPARAM(24,metricRowY));
    SendMessageW(metrics,WM_KEYDOWN,VK_RETURN,0);
    if (!Contains(GetDlgItem(window,1112),L"CSQ") || !Contains(GetDlgItem(window,1112),L"RSRP")) return finish(80);
    Capture(window,L"columns-at-health");
    SendMessageW(window,WM_COMMAND,1111,0);
    SendMessageW(window,WM_COMMAND,32000,0); Capture(window,L"columns-common");
    SendMessageW(window,WM_COMMAND,32005,0);
    SetWindowTextW(GetDlgItem(window,1008),L"00:00:00");SetWindowTextW(GetDlgItem(window,1009),L"00:35:00");
    SendMessageW(window,WM_COMMAND,1003,0);
    const int chineseRangeCount=ListView_GetItemCount(metrics);
    SendMessageW(window,WM_COMMAND,1171,0);
    if (!Contains(GetDlgItem(window,1024),L"Signal metrics") || !Contains(GetDlgItem(window,1141),L"Reset range") ||
        !Contains(GetDlgItem(window,1140),L"Time conditions") || ListView_GetItemCount(metrics)!=chineseRangeCount) return finish(81);
    Capture(window,L"english-metrics-range");
    SendMessageW(window,WM_COMMAND,1141,0);
    if (ListView_GetItemCount(metrics)!=fullMetricCount) return finish(82);
    SendMessageW(window,WM_APP+41,1,0);
    Capture(window,L"english-findings");
    RECT englishFindingRect{};GetClientRect(GetDlgItem(window,1019),&englishFindingRect);
    SendMessageW(GetDlgItem(window,1019),WM_LBUTTONUP,0,MAKELPARAM(englishFindingRect.right-100,35));
    const auto englishFindings=ClipboardText();
    if (englishFindings.find(L"Basis:")==std::wstring::npos || englishFindings.find(L"Advice:")==std::wstring::npos ||
        englishFindings.find(L"Weak signal")==std::wstring::npos || englishFindings.find(L"CH:ROAMLINK")==std::wstring::npos ||
        englishFindings.find(L"全量日志历史汇总")!=std::wstring::npos) {PrintWide("english-findings",englishFindings);return finish(83);}
    Capture(window,L"english-findings");
    SendMessageW(window,WM_APP+41,0,0);
    Capture(window,L"english-overview");
    RECT englishDashRect{};GetClientRect(GetDlgItem(window,1022),&englishDashRect);
    SendMessageW(GetDlgItem(window,1022),WM_LBUTTONUP,0,MAKELPARAM(englishDashRect.right-95,24));
    if (ClipboardText().find(L"Outages 36")==std::wstring::npos || ClipboardText().find(L"1359 samples")==std::wstring::npos) return finish(84);
    Capture(window,L"english-overview");
    SetWindowPos(window,nullptr,0,0,860,640,SWP_NOMOVE|SWP_NOZORDER);
    if (!AppearanceFitsNavigation(window) ||
        !CheckAppearanceMenu(window,process.dwProcessId,L"appearance-menu-english-narrow")) return finish(134);
    Capture(window,L"english-overview-narrow");
    SendMessageW(window,WM_APP+41,4,0);Capture(window,L"english-metrics-narrow");
    SendMessageW(window,WM_COMMAND,33000,0);
    WindowSearch comparisonSearch{process.dwProcessId,nullptr,L"dialSourceComparison"};
    EnumWindows(FindProcessWindow,reinterpret_cast<LPARAM>(&comparisonSearch));
    if (!comparisonSearch.window || ListView_GetItemCount(GetDlgItem(comparisonSearch.window,1162))!=16) return finish(85);
    Capture(comparisonSearch.window,L"english-source-comparison");
    SendMessageW(comparisonSearch.window,WM_CLOSE,0,0);
    // A single-source comparison also needs fresh statistics when its time range changes.
    SetWindowTextW(GetDlgItem(window,1008),L"00:00:00");SetWindowTextW(GetDlgItem(window,1009),L"00:34:41");
    SendMessageW(window,WM_COMMAND,1003,0);
    SendMessageW(window,WM_COMMAND,33000,0);
    comparisonSearch.window=nullptr;EnumWindows(FindProcessWindow,reinterpret_cast<LPARAM>(&comparisonSearch));
    if(!comparisonSearch.window) return finish(87);
    SendMessageW(comparisonSearch.window,WM_COMMAND,1166,0);
    if(ClipboardText().find(L"Metric samples\t68\t68\r\n")==std::wstring::npos) {
        PrintWide("single-source-range-comparison",ClipboardText());return finish(88);
    }
    SendMessageW(comparisonSearch.window,WM_CLOSE,0,0);
    SendMessageW(window,WM_COMMAND,1141,0);
    SendMessageW(window,WM_COMMAND,1170,0);
    if (!Contains(GetDlgItem(window,1024),L"信号指标") || ListView_GetItemCount(metrics)!=fullMetricCount) return finish(86);

    PostMessageW(window, WM_CLOSE, 0, 0);
    const DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exitCode == 0 ? MultiSourceSmoke(executable) : 25;
}
