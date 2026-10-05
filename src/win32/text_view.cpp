#include "text_view.h"
#include <commctrl.h>
#include <cstring>
#include <algorithm>
#include "app_context.h"
#include "workspace_window.h"
#include "ui_pages.h"
#include "modern_shell.h"
#include "theme.h"
#include "text_catalog.h"
namespace dl {
namespace {
HWND g_textWindow=nullptr;
std::string g_reviewKey,g_reviewTitle;
std::vector<TextEvidence> g_evidence;
constexpr int body=1200,evidenceList=1201,jump=1202,bookmark=1203,hint=1204;
void Follow(bool mark) {
    const int row=ListView_GetNextItem(GetDlgItem(g_textWindow,evidenceList),-1,LVNI_SELECTED);
    if(row<0 || static_cast<std::size_t>(row)>=g_evidence.size())return;
    const auto evidence=g_evidence[row];
    if(mark) { ToggleEvidenceBookmark(evidence.line,evidence.text);return; }
    CloseSelectableText();JumpToRawLine(evidence.line);SetForegroundWindow(App().hMain);
}
LRESULT CALLBACK TextProc(HWND window,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_CREATE: {
        auto control=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int id) {
            HWND child=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,10,10,window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(App().hFontUI),TRUE);return child;
        };
        control(L"STATIC",UiText(TextId::text_hint),SS_LEFT|SS_NOPREFIX,hint);
        HWND edit=control(L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,body);
        SendMessageW(edit,EM_SETLIMITTEXT,0,0);
        HWND list=control(WC_LISTVIEWW,L"",WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,evidenceList);
        ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        LvAddCol(list,0,UiText(TextId::incident_global_line),S(100));LvAddCol(list,1,UiText(TextId::incident_events),S(650));
        control(L"BUTTON",UiText(TextId::incident_jump),BS_PUSHBUTTON|WS_TABSTOP,jump);
        control(L"BUTTON",UiText(TextId::incident_bookmark),BS_PUSHBUTTON|WS_TABSTOP,bookmark);
        control(L"BUTTON",UiText(TextId::workspace_edit_review),BS_PUSHBUTTON|WS_TABSTOP,1205);
        ApplyModernTheme(window);return 0;
    }
    case WM_SIZE: {
        RECT r{};GetClientRect(window,&r);const int width=std::max(1,static_cast<int>(r.right)-S(32));
        const bool has=!g_evidence.empty(),review=!g_reviewKey.empty();const int listH=has?std::min(S(180),static_cast<int>(r.bottom)/3):0;
        MoveWindow(GetDlgItem(window,hint),S(16),S(12),width,S(48),TRUE);
        MoveWindow(GetDlgItem(window,body),S(16),S(68),width,std::max(1,static_cast<int>(r.bottom)-S(84)-((has||review)?listH+S(46):0)),TRUE);
        const int top=static_cast<int>(r.bottom)-S(62)-listH;
        MoveWindow(GetDlgItem(window,evidenceList),S(16),top,width,listH,TRUE);
        MoveWindow(GetDlgItem(window,jump),S(16),r.bottom-S(50),S(155),S(32),TRUE);
        MoveWindow(GetDlgItem(window,bookmark),S(184),r.bottom-S(50),S(155),S(32),TRUE);
        for(int id:{evidenceList,jump,bookmark})ShowWindow(GetDlgItem(window,id),has?SW_SHOW:SW_HIDE);
        MoveWindow(GetDlgItem(window,1205),r.right-S(240),r.bottom-S(50),S(224),S(32),TRUE);ShowWindow(GetDlgItem(window,1205),review?SW_SHOW:SW_HIDE);
        ListView_SetColumnWidth(GetDlgItem(window,evidenceList),1,std::max(S(200),width-S(120)));return 0;
    }
    case WM_COMMAND: if(LOWORD(wp)==1205){EditManualReview(g_reviewKey,g_reviewTitle);return 0;}if(LOWORD(wp)==jump || LOWORD(wp)==bookmark){Follow(LOWORD(wp)==bookmark);return 0;}break;
    case WM_NOTIFY: if(reinterpret_cast<NMHDR*>(lp)->code==NM_DBLCLK){Follow(false);return 0;}break;
    case WM_ERASEBKGND:{RECT r{};GetClientRect(window,&r);FillSolid(reinterpret_cast<HDC>(wp),r,th::surface);return 1;}
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(ModernControlBrush(msg,reinterpret_cast<HDC>(wp),reinterpret_cast<HWND>(lp)));
    case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={S(650),S(450)};return 0;
    case WM_CLOSE:DestroyWindow(window);return 0;
    case WM_DESTROY:g_textWindow=nullptr;std::vector<TextEvidence>().swap(g_evidence);return 0;
    }
    return DefWindowProcW(window,msg,wp,lp);
}
}
bool CopyWindowText(HWND owner,const std::wstring& text) {
    if(!OpenClipboard(owner))return false;
    const SIZE_T bytes=(text.size()+1)*sizeof(wchar_t);HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
    if(!memory){CloseClipboard();return false;}
    void* output=GlobalLock(memory);if(!output){GlobalFree(memory);CloseClipboard();return false;}
    std::memcpy(output,text.c_str(),bytes);GlobalUnlock(memory);
    EmptyClipboard();if(!SetClipboardData(CF_UNICODETEXT,memory)){GlobalFree(memory);CloseClipboard();return false;}
    CloseClipboard();return true;
}
void CloseSelectableText(){if(g_textWindow)DestroyWindow(g_textWindow);}
void ShowSelectableText(const std::wstring& title,const std::wstring& text,const std::vector<TextEvidence>& evidence,const std::string& reviewKey,const std::string& reviewTitle) {
    CloseSelectableText();g_evidence=evidence;g_reviewKey=reviewKey;g_reviewTitle=reviewTitle;
    WNDCLASSW cls{};cls.lpfnWndProc=TextProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"dialSelectableText";
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&cls);
    g_textWindow=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,title.c_str(),WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,S(940),S(720),App().hMain,nullptr,cls.hInstance,nullptr);
    if(!g_textWindow)return;
    SetWindowTextW(GetDlgItem(g_textWindow,body),text.c_str());
    HWND list=GetDlgItem(g_textWindow,evidenceList);
    for(std::size_t i=0;i<g_evidence.size();++i){std::wstring line=std::to_wstring(g_evidence[i].line);LVITEMW item{};
        item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=const_cast<wchar_t*>(line.c_str());ListView_InsertItem(list,&item);
        ListView_SetItemText(list,item.iItem,1,const_cast<wchar_t*>(g_evidence[i].text.c_str()));}
    if(!g_evidence.empty())ListView_SetItemState(list,0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    ShowWindow(g_textWindow,SW_SHOW);SetFocus(GetDlgItem(g_textWindow,body));
}
bool RouteSelectableTextMessage(MSG& message) {
    if(!g_textWindow || (message.hwnd!=g_textWindow && !IsChild(g_textWindow,message.hwnd)))return false;
    if(message.message==WM_KEYDOWN) {
        if(message.wParam==VK_ESCAPE){CloseSelectableText();return true;}
        if(message.wParam=='A' && (GetKeyState(VK_CONTROL)&0x8000) && GetFocus()==GetDlgItem(g_textWindow,body)) {
            SendDlgItemMessageW(g_textWindow,body,EM_SETSEL,0,-1);return true;}
        if(message.wParam==VK_RETURN && GetFocus()==GetDlgItem(g_textWindow,evidenceList)){Follow(false);return true;}
    }
    if(!IsDialogMessageW(g_textWindow,&message)){TranslateMessage(&message);DispatchMessageW(&message);}return true;
}
void ShowSignalGuide(){ShowSelectableText(UiText(TextId::signal_guide),UiText(TextId::signal_guide_body));}
} // namespace dl
