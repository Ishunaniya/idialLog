#include "workspace_window.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <set>
#include <limits>
#include <cstdio>
#include <cwchar>
#include <cmath>
#include <stdexcept>
#include "app_context.h"
#include "win_file_io.h"
#include "win_text.h"
#include "load_controller.h"
#include "source_workspace.h"
#include "ui_pages.h"
#include "text_view.h"
#include "text_catalog.h"
#include "modern_shell.h"
#include "theme.h"
#include "sha256.h"
#include "log_time.h"
#include "log_analysis.h"
#include "signal_chart.h"
#include "report_chart.h"
#include "incident_export.h"
#include "incident_review.h"
#include "version.h"
namespace dl {
namespace {
HWND g_workspace=nullptr,g_editor=nullptr;int g_page=0;
bool g_writable=true,g_devicesChanged=false;
ReviewRecord g_edit;
std::vector<std::string> g_reviewKeys;
struct Scope {std::wstring label;std::string device;std::size_t source=SIZE_MAX;};
std::vector<Scope> g_scopes;
std::string g_compareText;
SignalChartSeries g_signalA,g_signalB;
long long g_a0=0,g_a1=0,g_b0=0,g_b1=0;
constexpr int tab=1400,list=1401,device=1402,firmware=1403,configuration=1404,save=1405,hint=1406,
    deviceLabel=1407,firmwareLabel=1408,configurationLabel=1409,
    scopeA=1410,scopeB=1411,startA=1412,endA=1413,startB=1414,endB=1415,
    run=1416,copy=1417,exportHtml=1418,relative=1419,result=1420,plot=1421,metric=1422;
std::wstring StorePath(){wchar_t folder[MAX_PATH]{};if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA|CSIDL_FLAG_CREATE,nullptr,SHGFP_TYPE_CURRENT,folder)))return {};
    std::wstring root=std::wstring(folder)+L"\\dialLog";CreateDirectoryW(root.c_str(),nullptr);root+=L"\\workspaces";CreateDirectoryW(root.c_str(),nullptr);
    std::vector<std::string> hashes;for(const auto& s:App().document.sources)hashes.push_back(s.workspaceKey());std::sort(hashes.begin(),hashes.end());Sha256 digest;for(const auto& h:hashes)digest.update(h);return root+L"\\"+U8ToW(digest.finish())+L".json";}
std::string LineIdentity(std::size_t line){const auto& d=App().document;for(const auto& s:d.sources)if(s.first<s.last&&line>=d.lines[s.first].lineNo&&line<=d.lines[s.last-1].lineNo)return s.workspaceKey()+":"+std::to_string(line-s.rawLineOffset);return "unknown:"+std::to_string(line);}
HWND Control(HWND w,const wchar_t* cls,const wchar_t* text,DWORD style,int id){HWND c=CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,10,10,w,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(App().hFontUI),TRUE);return c;}
void Move(HWND w,int id,int x,int y,int width,int height){MoveWindow(GetDlgItem(w,id),S(x),S(y),S(width),S(height),TRUE);}
std::string Escape(const std::string& text){std::string out;for(char c:text){switch(c){case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;case '"':out+="&quot;";break;default:out+=c;}}return out;}
bool Date(HWND w,int id,long long& t){const auto text=WToU8(GetText(GetDlgItem(w,id)));int y,mo,d,h,m,s;char trailing;
    if(std::sscanf(text.c_str(),"%d-%d-%d %d:%d:%d%c",&y,&mo,&d,&h,&m,&s,&trailing)!=6||y<1970||y>2099||mo<1||mo>12||d<1||d>31||h<0||h>23||m<0||m>59||s<0||s>59)return false;
    t=mkEpoch(y,mo,d,h,m,s);return fmtTime(t,"FULL")==text;}
LogView ScopeRows(std::size_t index){LogView rows;auto& d=App().document;if(index>=g_scopes.size())return rows;rows.reserve(d.lines.size());for(const auto& row:d.lines)rows.push_back(&row);const auto& scope=g_scopes[index];
    if(scope.source!=SIZE_MAX)d.restrictToSource(rows,scope.source);else rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const LogLine* row){auto index=static_cast<std::size_t>(row-d.lines.data());auto source=std::upper_bound(d.sources.begin(),d.sources.end(),index,[](std::size_t n,const SourceSummary& s){return n<s.last;});if(source==d.sources.end()||index<source->first)return true;auto found=d.workspace.devices.find(source->workspaceKey());return found==d.workspace.devices.end()||found->second.device!=scope.device;}),rows.end());return rows;}
std::string ScopeDescription(std::size_t index){if(index>=g_scopes.size())return {};const auto& scope=g_scopes[index];std::string text=WToU8(scope.label);const auto& d=App().document;for(std::size_t i=0;i<d.sources.size();++i){auto found=d.workspace.devices.find(d.sources[i].workspaceKey());if(found==d.workspace.devices.end())continue;if(scope.source!=i&&!(scope.source==SIZE_MAX&&found->second.device==scope.device))continue;const auto& m=found->second;text+="\n"+WToU8(d.sources[i].label)+" | "+UiText8(TextId::workspace_device)+": "+m.device+" | "+UiText8(TextId::workspace_firmware)+": "+m.firmware+" | "+UiText8(TextId::workspace_configuration)+": "+m.configuration;}return text;}
void FillScopes(){g_scopes.clear();auto& d=App().document;std::set<std::string> groups;
    for(std::size_t i=0;i<d.sources.size();++i){auto m=d.workspace.devices.find(d.sources[i].workspaceKey());std::string key=m==d.workspace.devices.end()?"":m->second.device;g_scopes.push_back({d.sources[i].label,key,i});if(!key.empty())groups.insert(key);}
    for(const auto& key:groups)g_scopes.push_back({UiText(TextId::workspace_group)+U8ToW(key),key,SIZE_MAX});
    for(int id:{scopeA,scopeB}){auto c=GetDlgItem(g_workspace,id);SendMessageW(c,CB_RESETCONTENT,0,0);for(const auto& s:g_scopes)SendMessageW(c,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s.label.c_str()));SendMessageW(c,CB_SETCURSEL,0,0);}}
void DefaultPeriod(int combo,int start,int end){auto index=SendDlgItemMessageW(g_workspace,combo,CB_GETCURSEL,0,0);auto rows=ScopeRows(static_cast<std::size_t>(index));long long a=0,b=0;if(!rows.empty()){a=b=rows.front()->t;for(const auto* r:rows){a=std::min(a,r->t);b=std::max(b,r->t);}}SetWindowTextW(GetDlgItem(g_workspace,start),U8ToW(fmtTime(a,"FULL")).c_str());SetWindowTextW(GetDlgItem(g_workspace,end),U8ToW(fmtTime(b,"FULL")).c_str());}
const ChartSeries& Series(const SignalChartSeries& s,int mode){return mode==1?s.rsrp:mode==2?s.rsrq:mode==3?s.snr10:mode==4?s.rssi:s.csq;}
void Calculate(){long long a0,a1,b0,b1;if(!Date(g_workspace,startA,a0)||!Date(g_workspace,endA,a1)||!Date(g_workspace,startB,b0)||!Date(g_workspace,endB,b1)||a1<a0||b1<b0){ShowModernNotice(UiText(TextId::workspace_invalid_time),L"",ModernNoticeKind::Warning);return;}
    g_a0=a0;g_a1=a1;g_b0=b0;g_b1=b1;
    auto ai=static_cast<std::size_t>(SendDlgItemMessageW(g_workspace,scopeA,CB_GETCURSEL,0,0)),bi=static_cast<std::size_t>(SendDlgItemMessageW(g_workspace,scopeB,CB_GETCURSEL,0,0));if(ai>=g_scopes.size()||bi>=g_scopes.size())return;
    auto ar=ScopeRows(ai),br=ScopeRows(bi);auto a=comparePeriod(ar,g_a0,g_a1),b=comparePeriod(br,g_b0,g_b1);
    const bool same=!g_scopes[ai].device.empty()&&g_scopes[ai].device==g_scopes[bi].device;
    g_compareText=periodComparisonText(a,b,ScopeDescription(ai)+" "+fmtTime(g_a0,"FULL")+" → "+fmtTime(g_a1,"FULL"),ScopeDescription(bi)+" "+fmtTime(g_b0,"FULL")+" → "+fmtTime(g_b1,"FULL"),same);
    auto signal = [&](const LogView& rows, long long start, long long end) {
        const auto values = buildMetrics(rows);
        MetricView view;
        for (const auto& metric : values) if (metric.t >= start && metric.t <= end) view.push_back(&metric);
        return reportedSignalSeries(view);
    };
    g_signalA=signal(ar,g_a0,g_a1);g_signalB=signal(br,g_b0,g_b1);SetWindowTextW(GetDlgItem(g_workspace,result),U8ToW(g_compareText).c_str());InvalidateRect(GetDlgItem(g_workspace,plot),nullptr,FALSE);}
struct PlotRuntime {
    ULONG_PTR token = 0;
    PlotRuntime() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) token = 0;
    }
    ~PlotRuntime() { if (token) Gdiplus::GdiplusShutdown(token); }
};
LRESULT CALLBACK PlotProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_ERASEBKGND) return 1;
    if (message != WM_PAINT) return DefWindowProcW(window, message, wp, lp);
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    RECT bounds{};
    GetClientRect(window, &bounds);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(1L, bounds.right), std::max(1L, bounds.bottom));
    auto oldBitmap = SelectObject(dc, bitmap);
    auto oldFont = SelectObject(dc, App().hFontSmall);
    FillSolid(dc, bounds, th::surface);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, th::inkPri);
    if (g_compareText.empty()) {
        DrawTextW(dc, UiText(TextId::workspace_compare_empty), -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        const int mode = static_cast<int>(SendDlgItemMessageW(g_workspace, metric, CB_GETCURSEL, 0, 0));
        const bool align = SendDlgItemMessageW(g_workspace, relative, BM_GETCHECK, 0, 0) == BST_CHECKED;
        ChartSeries a = Series(g_signalA, mode), b = Series(g_signalB, mode);
        if (align) {
            for (auto& point : a) point.first -= g_a0;
            for (auto& point : b) point.first -= g_b0;
        }
        const long long start = align ? 0 : std::min(g_a0, g_b0);
        const long long end = align ? std::max(g_a1 - g_a0, g_b1 - g_b0) : std::max(g_a1, g_b1);
        long long low = mode == 0 ? 0 : mode == 1 ? -140 : mode == 2 ? -25 : mode == 3 ? -200 : -120;
        long long high = mode == 0 ? 31 : mode == 1 ? -40 : mode == 2 ? 0 : mode == 3 ? 300 : -20;
        for (const auto* series : {&a, &b}) for (const auto& point : *series) {
            low = std::min(low, static_cast<long long>(point.second));
            high = std::max(high, static_cast<long long>(point.second));
        }
        if (low == high) ++high;
        const int left = S(56), right = std::max(left + 1, static_cast<int>(bounds.right) - S(18));
        const int top = S(25), bottom = std::max(top + 1, static_cast<int>(bounds.bottom) - S(48));
        for (int i = 0; i <= 4; ++i) {
            const int y = top + (bottom - top) * i / 4;
            const auto value = high - (high - low) * i / 4;
            wchar_t label[48]{};
            if (mode == 3) std::swprintf(label, 48, L"%.1f", value / 10.0);
            else std::swprintf(label, 48, L"%lld", value);
            TextOutW(dc, S(4), y - S(7), label, lstrlenW(label));
            HPEN pen = CreatePen(PS_SOLID, 1, th::border);
            auto oldPen = SelectObject(dc, pen);
            MoveToEx(dc, left, y, nullptr); LineTo(dc, right, y);
            SelectObject(dc, oldPen); DeleteObject(pen);
        }
        static PlotRuntime runtime;
        int index = 0;
        for (const auto* input : {&a, &b}) {
            ChartSeries points;
            downsampleChartSeries(*input, start, end, std::max(1, right - left), points);
            const auto gaps = chartSampleGaps(*input, 600);
            const COLORREF color = index ? th::s2_green : th::s1_blue;
            Gdiplus::Graphics graphics(dc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            const Gdiplus::Color ink(255, GetRValue(color), GetGValue(color), GetBValue(color));
            Gdiplus::Pen pen(ink, std::max(1.5f, App().dpi / 96.0f * 1.5f));
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            if (index) pen.SetDashStyle(Gdiplus::DashStyleDash);
            Gdiplus::SolidBrush dots(ink);
            long long previous = 0;
            Gdiplus::PointF last;
            bool first = true;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const auto& point = points[i];
                const Gdiplus::PointF at(
                    left + static_cast<float>((static_cast<long double>(point.first) - start) /
                        std::max(1.0L, static_cast<long double>(end) - start) * (right - left)),
                    bottom - static_cast<float>((static_cast<long double>(point.second) - low) /
                        (high - low) * (bottom - top)));
                const auto gap = std::lower_bound(gaps.begin(), gaps.end(), previous,
                    [](const ChartGap& g, long long t) { return g.first < t; });
                const bool begin = first || (gap != gaps.end() && gap->first < point.first);
                if (!begin) graphics.DrawLine(&pen, last, at);
                if (points.size() <= 60 || begin || i + 1 == points.size()) {
                    const float radius = S(points.size() == 1 ? 3 : 2);
                    graphics.FillEllipse(&dots, at.X - radius, at.Y - radius, radius * 2, radius * 2);
                }
                last = at; previous = point.first; first = false;
            }
            ++index;
        }
        TextOutW(dc, left, S(4), L"A", 1);
        TextOutW(dc, left + S(58), S(4), L"B", 1);
        {
            Gdiplus::Graphics legend(dc);
            legend.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const auto color = [](COLORREF value) { return Gdiplus::Color(255, GetRValue(value), GetGValue(value), GetBValue(value)); };
            Gdiplus::Pen aPen(color(th::s1_blue), 2.0f), bPen(color(th::s2_green), 2.0f);
            bPen.SetDashStyle(Gdiplus::DashStyleDash);
            legend.DrawLine(&aPen, left + S(17), S(12), left + S(44), S(12));
            legend.DrawLine(&bPen, left + S(75), S(12), left + S(102), S(12));
        }
        const std::wstring title = mode == 3 ? L"SNR dB" : mode == 0 ? L"CSQ" :
            mode == 2 ? L"RSRQ dB" : mode == 1 ? L"RSRP dBm" : L"RSSI dBm";
        TextOutW(dc, left + S(118), S(4), title.c_str(), title.size());
        const std::wstring aText = align ? L"0 s" : U8ToW(fmtTime(start, "FULL"));
        const std::wstring bText = align ? std::to_wstring(end) + L" s" : U8ToW(fmtTime(end, "FULL"));
        TextOutW(dc, left, bottom + S(10), aText.c_str(), aText.size());
        RECT label{left, bottom + S(10), right, bounds.bottom};
        DrawTextW(dc, bText.c_str(), -1, &label, DT_RIGHT | DT_SINGLELINE);
    }
    SelectObject(dc, oldFont);
    BitBlt(target, 0, 0, bounds.right, bounds.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap); DeleteDC(dc);
    EndPaint(window, &paint);
    return 0;
}
void FillList(){HWND table=GetDlgItem(g_workspace,list);ListView_DeleteAllItems(table);while(Header_GetItemCount(ListView_GetHeader(table)))ListView_DeleteColumn(table,0);g_reviewKeys.clear();
    if(g_page==0){LvAddCol(table,0,UiText(TextId::workspace_source),S(270));LvAddCol(table,1,UiText(TextId::workspace_device),S(220));LvAddCol(table,2,UiText(TextId::workspace_firmware),S(160));for(std::size_t i=0;i<App().document.sources.size();++i){const auto& source=App().document.sources[i];LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=const_cast<wchar_t*>(source.label.c_str());ListView_InsertItem(table,&item);auto found=App().document.workspace.devices.find(source.workspaceKey());if(found!=App().document.workspace.devices.end()){auto a=U8ToW(found->second.device),b=U8ToW(found->second.firmware);ListView_SetItemText(table,item.iItem,1,const_cast<wchar_t*>(a.c_str()));ListView_SetItemText(table,item.iItem,2,const_cast<wchar_t*>(b.c_str()));}}}
    else {LvAddCol(table,0,UiText(TextId::workspace_status),S(120));LvAddCol(table,1,UiText(TextId::workspace_reviews),S(450));LvAddCol(table,2,UiText(TextId::workspace_note),S(500));for(const auto& pair:App().document.workspace.reviews){g_reviewKeys.push_back(pair.first);int row=static_cast<int>(g_reviewKeys.size()-1);auto status=U8ToW(reviewStatusText(pair.second.status)),title=U8ToW(GeneratedText(pair.second.title)),note=U8ToW(pair.second.note);LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=row;item.pszText=const_cast<wchar_t*>(status.c_str());ListView_InsertItem(table,&item);ListView_SetItemText(table,row,1,const_cast<wchar_t*>(title.c_str()));ListView_SetItemText(table,row,2,const_cast<wchar_t*>(note.c_str()));}}
    if(ListView_GetItemCount(table))ListView_SetItemState(table,0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);}
void Layout(){if(!g_workspace)return;RECT r{};GetClientRect(g_workspace,&r);int width=MulDiv(r.right,96,App().dpi),height=MulDiv(r.bottom,96,App().dpi);Move(g_workspace,tab,16,12,width-32,30);Move(g_workspace,hint,16,52,width-32,64);
    for(int id:{list,device,firmware,configuration,save,deviceLabel,firmwareLabel,configurationLabel})ShowWindow(GetDlgItem(g_workspace,id),g_page==0?SW_SHOW:g_page==1&&id==list?SW_SHOW:SW_HIDE);
    for(int id:{scopeA,scopeB,startA,endA,startB,endB,run,copy,exportHtml,relative,result,plot,metric,1423,1424,1425,1426})ShowWindow(GetDlgItem(g_workspace,id),g_page==2?SW_SHOW:SW_HIDE);
    SetWindowTextW(GetDlgItem(g_workspace,hint),UiText(g_page==0?TextId::workspace_device_hint:g_page==1?TextId::workspace_review_hint:TextId::workspace_compare_hint));
    if(g_page==0){Move(g_workspace,list,16,120,width-32,height-350);int y=height-218;Move(g_workspace,deviceLabel,16,y,230,26);Move(g_workspace,device,250,y,width-266,28);Move(g_workspace,firmwareLabel,16,y+36,230,26);Move(g_workspace,firmware,250,y+36,width-266,28);Move(g_workspace,configurationLabel,16,y+74,230,26);Move(g_workspace,configuration,250,y+74,width-266,86);Move(g_workspace,save,width-136,height-46,120,30);}
    if(g_page==1)Move(g_workspace,list,16,120,width-32,height-136);
    if(g_page==2){int col=(width-40)/2;Move(g_workspace,scopeA,16,120,col,300);Move(g_workspace,scopeB,24+col,120,col,300);int edit=(col-8)/2;Move(g_workspace,1423,16,150,edit,20);Move(g_workspace,1424,24+edit,150,edit,20);Move(g_workspace,1425,24+col,150,edit,20);Move(g_workspace,1426,32+col+edit,150,edit,20);Move(g_workspace,startA,16,176,edit,28);Move(g_workspace,endA,24+edit,176,edit,28);Move(g_workspace,startB,24+col,176,edit,28);Move(g_workspace,endB,32+col+edit,176,edit,28);Move(g_workspace,run,16,214,180,32);Move(g_workspace,copy,204,214,180,32);Move(g_workspace,exportHtml,392,214,180,32);Move(g_workspace,relative,16,254,260,28);Move(g_workspace,metric,width-216,254,200,300);int chartHeight=std::max(130,(height-310)/2);Move(g_workspace,plot,16,296,width-32,chartHeight);Move(g_workspace,result,16,304+chartHeight,width-32,std::max(60,height-chartHeight-320));}}
void EditDevice(){int row=ListView_GetNextItem(GetDlgItem(g_workspace,list),-1,LVNI_SELECTED);if(row<0||static_cast<std::size_t>(row)>=App().document.sources.size())return;const auto& s=App().document.sources[row];auto found=App().document.workspace.devices.find(s.workspaceKey());DeviceMetadata m=found==App().document.workspace.devices.end()?DeviceMetadata{}:found->second;SetWindowTextW(GetDlgItem(g_workspace,device),U8ToW(m.device).c_str());SetWindowTextW(GetDlgItem(g_workspace,firmware),U8ToW(m.firmware).c_str());SetWindowTextW(GetDlgItem(g_workspace,configuration),U8ToW(m.configuration).c_str());}
LRESULT CALLBACK EditorProc(HWND w,UINT msg,WPARAM wp,LPARAM lp){switch(msg){case WM_CREATE:{Control(w,L"STATIC",UiText(TextId::workspace_review_hint),SS_LEFT,1450);auto c=Control(w,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,1451);for(auto id:{TextId::review_pending,TextId::review_confirmed,TextId::review_handled})SendMessageW(c,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(UiText(id)));SendMessageW(c,CB_SETCURSEL,static_cast<int>(g_edit.status),0);Control(w,L"EDIT",U8ToW(g_edit.note).c_str(),ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,1452);SendDlgItemMessageW(w,1452,EM_SETLIMITTEXT,8000,0);Control(w,L"BUTTON",UiText(TextId::workspace_save),BS_PUSHBUTTON|WS_TABSTOP,1453);ApplyModernTheme(w);return 0;}
    case WM_SIZE:{RECT r{};GetClientRect(w,&r);int width=MulDiv(r.right,96,App().dpi),height=MulDiv(r.bottom,96,App().dpi);Move(w,1450,16,12,width-32,70);Move(w,1451,16,88,240,300);Move(w,1452,16,132,width-32,height-194);Move(w,1453,width-136,height-46,120,30);return 0;}
    case WM_COMMAND:if(LOWORD(wp)==1453){g_edit.note=WToU8(GetText(GetDlgItem(w,1452)));g_edit.status=static_cast<ReviewStatus>(SendDlgItemMessageW(w,1451,CB_GETCURSEL,0,0));auto& records=App().document.workspace.reviews;auto old=records.find(g_edit.key);bool had=old!=records.end();ReviewRecord previous=had?old->second:ReviewRecord{};records[g_edit.key]=g_edit;if(SaveWorkspaceRecords()){DestroyWindow(w);RefreshIncidentAnnotation();if(g_workspace&&g_page==1)FillList();InvalidateRect(App().hFindings,nullptr,FALSE);}else{if(had)records[g_edit.key]=previous;else records.erase(g_edit.key);}return 0;}break;
    case WM_CLOSE:DestroyWindow(w);return 0;case WM_DESTROY:g_editor=nullptr;return 0;case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={S(600),S(420)};return 0;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:return reinterpret_cast<LRESULT>(ModernControlBrush(msg,reinterpret_cast<HDC>(wp),reinterpret_cast<HWND>(lp)));case WM_ERASEBKGND:{RECT r{};GetClientRect(w,&r);FillSolid(reinterpret_cast<HDC>(wp),r,th::surface);return 1;}}
    return DefWindowProcW(w,msg,wp,lp);}
std::string CompareHtml(){std::string html="<!doctype html><html lang=\""+std::string(IsEnglish()?"en-US":"zh-CN")+"\"><meta charset=\"utf-8\"><title>"+Escape(UiText8(TextId::workspace_compare))+"</title><style>body{font-family:Segoe UI,sans-serif;margin:24px;background:#f5f7fa;color:#223043}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:white;padding:16px}.signal-chart{background:white;padding:12px}.chart-scroll{overflow:auto}svg{min-width:880px;width:100%}.chart-grid{stroke:#dce3ed}.chart-label{font-size:13px;fill:#526072}.chart-axis{fill:none;stroke:#9aa8b9}.sample.dense{opacity:0}.sample:hover{opacity:1}</style><h1>"+Escape(UiText8(TextId::workspace_compare))+" · v"+DL_VER_STR+"</h1><pre>"+Escape(g_compareText)+"</pre>";
    // Both curves share an axis. Relative alignment retains originals in hover titles.
    const bool align=SendDlgItemMessageW(g_workspace,relative,BM_GETCHECK,0,0)==BST_CHECKED;
    for(int mode=0;mode<5;++mode){ChartSeries a=Series(g_signalA,mode),b=Series(g_signalB,mode);ReportChartOptions o;o.english=IsEnglish();o.title=mode==0?"CSQ":mode==1?"RSRP":mode==2?"RSRQ":mode==3?"SNR":"RSSI";o.unit=mode==0?"":mode==1||mode==4?"dBm":"dB";o.low=mode==0?0:mode==1?-140:mode==2?-25:mode==3?-200:-120;o.high=mode==0?31:mode==1?-40:mode==2?0:mode==3?300:-20;o.scaled10=mode==3;o.color="#2a78d6";
        if(align){for(auto& p:a)p.first-=g_a0;for(auto& p:b)p.first-=g_b0;}o.start=align?0:std::min(g_a0,g_b0);o.end=align?std::max(g_a1-g_a0,g_b1-g_b0):std::max(g_a1,g_b1);for(const auto* s:{&a,&b})for(const auto& p:*s){o.low=std::min(o.low,p.second);o.high=std::max(o.high,p.second);}o.comparison=b;o.comparisonMode=true;o.relative=align;o.originalStart=g_a0;o.comparisonOriginalStart=g_b0;html+=renderReportChart(a,{},o).html;}
    return html+reportChartScript(IsEnglish())+"</html>";}
LRESULT CALLBACK WorkspaceProc(HWND w,UINT msg,WPARAM wp,LPARAM lp){switch(msg){case WM_CREATE:{g_workspace=w;HWND t=Control(w,WC_TABCONTROLW,L"",WS_TABSTOP,tab);for(auto id:{TextId::workspace_devices,TextId::workspace_reviews,TextId::workspace_compare}){TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<wchar_t*>(UiText(id));TabCtrl_InsertItem(t,TabCtrl_GetItemCount(t),&item);}TabCtrl_SetCurSel(t,g_page);Control(w,L"STATIC",L"",SS_LEFT|SS_NOPREFIX,hint);auto l=Control(w,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP,list);ListView_SetExtendedListViewStyle(l,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
        for(auto entry:{std::pair<int,TextId>{deviceLabel,TextId::workspace_device},{firmwareLabel,TextId::workspace_firmware},{configurationLabel,TextId::workspace_configuration}})Control(w,L"STATIC",UiText(entry.second),SS_CENTERIMAGE,entry.first);
        for(int id:{device,firmware,configuration}){Control(w,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|(id==configuration?ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL:0),id);SendDlgItemMessageW(w,id,EM_SETLIMITTEXT,id==configuration?1000:120,0);}Control(w,L"BUTTON",UiText(TextId::workspace_save),BS_PUSHBUTTON|WS_TABSTOP,save);
        for(int id:{scopeA,scopeB})Control(w,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,id);
        for(int id:{startA,endA,startB,endB}){auto c=Control(w,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,id);SendMessageW(c,EM_SETLIMITTEXT,19,0);SendMessageW(c,EM_SETCUEBANNER,FALSE,reinterpret_cast<LPARAM>(UiText(id==startA||id==startB?TextId::workspace_start:TextId::workspace_end)));}
        for(auto entry:{std::pair<int,TextId>{run,TextId::workspace_run},{copy,TextId::workspace_copy},{exportHtml,TextId::workspace_export}})Control(w,L"BUTTON",UiText(entry.second),BS_PUSHBUTTON|WS_TABSTOP,entry.first);
        for(int id:{1423,1424,1425,1426})Control(w,L"STATIC",UiText(id==1423||id==1425?TextId::workspace_start:TextId::workspace_end),SS_CENTERIMAGE,id);
        Control(w,L"BUTTON",UiText(TextId::workspace_relative),BS_AUTOCHECKBOX|WS_TABSTOP,relative);SendDlgItemMessageW(w,relative,BM_SETCHECK,BST_CHECKED,0);
        auto edit=Control(w,L"EDIT",L"",ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP,result);SendMessageW(edit,EM_SETLIMITTEXT,0,0);SendMessageW(edit,WM_SETFONT,reinterpret_cast<WPARAM>(App().hFontMono),TRUE);
        auto cb=Control(w,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP,metric);for(const wchar_t* label:{L"CSQ",L"RSRP",L"RSRQ",L"SNR",L"RSSI"})SendMessageW(cb,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));SendMessageW(cb,CB_SETCURSEL,0,0);
        WNDCLASSW cls{};cls.lpfnWndProc=PlotProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"dialWorkspacePlot";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&cls);Control(w,cls.lpszClassName,L"",0,plot);FillScopes();DefaultPeriod(scopeA,startA,endA);DefaultPeriod(scopeB,startB,endB);FillList();ApplyModernTheme(w);Layout();return 0;}
    case WM_SIZE:Layout();return 0;case WM_NOTIFY:{auto h=reinterpret_cast<NMHDR*>(lp);if(h->idFrom==tab&&h->code==TCN_SELCHANGE){g_page=TabCtrl_GetCurSel(GetDlgItem(w,tab));FillList();Layout();return 0;}if(h->idFrom==list&&h->code==LVN_ITEMCHANGED&&g_page==0){EditDevice();return 0;}if(h->idFrom==list&&h->code==NM_DBLCLK&&g_page==1){int row=ListView_GetNextItem(GetDlgItem(w,list),-1,LVNI_SELECTED);if(row>=0&&static_cast<std::size_t>(row)<g_reviewKeys.size()){const auto& record=App().document.workspace.reviews.at(g_reviewKeys[row]);EditManualReview(record.key,record.title);}return 0;}break;}
    case WM_COMMAND:{int id=LOWORD(wp);if(id>=startA&&id<=endB&&HIWORD(wp)==EN_CHANGE){g_compareText.clear();g_signalA={};g_signalB={};SetWindowTextW(GetDlgItem(w,result),L"");InvalidateRect(GetDlgItem(w,plot),nullptr,FALSE);return 0;}if(id==save){int row=ListView_GetNextItem(GetDlgItem(w,list),-1,LVNI_SELECTED);if(row<0||static_cast<std::size_t>(row)>=App().document.sources.size())return 0;const auto& s=App().document.sources[row];auto previous=App().document.workspace;std::string deviceId=WToU8(GetText(GetDlgItem(w,device)));auto begin=deviceId.find_first_not_of(" \t\r\n"),end=deviceId.find_last_not_of(" \t\r\n");deviceId=begin==std::string::npos?"":deviceId.substr(begin,end-begin+1);
        App().document.workspace.devices[s.workspaceKey()]={deviceId,WToU8(GetText(GetDlgItem(w,firmware))),WToU8(GetText(GetDlgItem(w,configuration)))};if(!SaveWorkspaceRecords()){App().document.workspace=std::move(previous);return 0;}g_devicesChanged=true;g_compareText.clear();g_signalA={};g_signalB={};SetWindowTextW(GetDlgItem(w,result),L"");FillScopes();FillList();ShowModernNotice(UiText(TextId::workspace_saved),L"",ModernNoticeKind::Success);return 0;}
        if(id==run){Calculate();return 0;}if(id==copy){if(!g_compareText.empty())CopyWindowText(w,U8ToW(g_compareText));return 0;}if(id==metric||id==relative){InvalidateRect(GetDlgItem(w,plot),nullptr,FALSE);return 0;}
        if(id==scopeA||id==scopeB){if(HIWORD(wp)==CBN_SELCHANGE)DefaultPeriod(id,id==scopeA?startA:startB,id==scopeA?endA:endB);return 0;}
        if(id==exportHtml){if(g_compareText.empty())Calculate();if(g_compareText.empty())return 0;wchar_t path[MAX_PATH]=L"diallog_comparison.html";OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=App().hMain;dialog.lpstrFile=path;dialog.nMaxFile=MAX_PATH;dialog.lpstrFilter=L"HTML\0*.html\0\0";dialog.lpstrDefExt=L"html";dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST;EnableWindow(w,FALSE);bool accepted=GetSaveFileNameW(&dialog)!=FALSE;if(IsWindow(w))EnableWindow(w,TRUE);if(accepted&&g_workspace==w){std::wstring error;if(!WriteFileBytesAtomic(path,CompareHtml(),error))ShowModernNotice(UiText(TextId::workspace_error),error.c_str(),ModernNoticeKind::Error);}return 0;}break;}
    case WM_CLOSE:{bool refresh=g_devicesChanged;DestroyWindow(w);if(refresh){auto& d=App().document;if(d.sourceMode==DocumentState::SourceMode::Device){bool exists=false;for(const auto& pair:d.workspace.devices)if(pair.second.device==d.selectedDevice)exists=true;if(!exists)d.sourceMode=DocumentState::SourceMode::Independent;}RefreshAll();}return 0;}case WM_DESTROY:g_devicesChanged=false;g_workspace=nullptr;g_compareText.clear();g_signalA={};g_signalB={};return 0;case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={S(860),S(680)};return 0;
    case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:return reinterpret_cast<LRESULT>(ModernControlBrush(msg,reinterpret_cast<HDC>(wp),reinterpret_cast<HWND>(lp)));case WM_ERASEBKGND:{RECT r{};GetClientRect(w,&r);FillSolid(reinterpret_cast<HDC>(wp),r,th::surface);return 1;}}
    return DefWindowProcW(w,msg,wp,lp);}
}
void LoadWorkspaceRecords(){App().document.workspace={};g_writable=true;auto path=StorePath();if(path.empty())return;if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES)return;try{std::string bytes;std::wstring error;if(!ReadBoundedFile(path,bytes,error,16*1024*1024))throw std::runtime_error("Workspace read failed");App().document.workspace=decodeWorkspace(bytes);}catch(const std::exception& e){g_writable=false;ShowModernNotice(UiText(TextId::workspace_error),U8ToW(e.what()).c_str(),ModernNoticeKind::Error);}}
bool SaveWorkspaceRecords(){try{auto path=StorePath();std::wstring error;if(!g_writable||path.empty()||!WriteFileBytesAtomic(path,encodeWorkspace(App().document.workspace),error)){ShowModernNotice(UiText(TextId::workspace_error),error.c_str(),ModernNoticeKind::Error);return false;}return true;}catch(const std::exception& e){ShowModernNotice(UiText(TextId::workspace_error),U8ToW(e.what()).c_str(),ModernNoticeKind::Error);return false;}}
void CloseWorkspaceWindows(){if(g_editor)DestroyWindow(g_editor);if(g_workspace)DestroyWindow(g_workspace);}
void ShowWorkspace(int page){if(LoadInProgress()||App().document.sources.empty())return;if(g_workspace){g_page=page;TabCtrl_SetCurSel(GetDlgItem(g_workspace,tab),page);FillList();Layout();SetForegroundWindow(g_workspace);return;}g_page=page;WNDCLASSW cls{};cls.lpfnWndProc=WorkspaceProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"dialWorkspace";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&cls);g_workspace=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,UiText(TextId::workspace_title),WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,S(1080),S(830),App().hMain,nullptr,cls.hInstance,nullptr);}
void EditManualReview(const std::string& key,const std::string& title){if(g_editor)DestroyWindow(g_editor);auto found=App().document.workspace.reviews.find(key);g_edit=found==App().document.workspace.reviews.end()?ReviewRecord{key,title,"",ReviewStatus::Pending}:found->second;WNDCLASSW cls{};cls.lpfnWndProc=EditorProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"dialManualReview";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);RegisterClassW(&cls);g_editor=CreateWindowExW(WS_EX_TOOLWINDOW,cls.lpszClassName,U8ToW(title).c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE,CW_USEDEFAULT,CW_USEDEFAULT,S(760),S(540),App().hMain,nullptr,cls.hInstance,nullptr);if(g_editor)SetFocus(GetDlgItem(g_editor,1452));}
std::string IncidentReviewKey(const Outage& outage){return sha256("incident:"+LineIdentity(outage.startLine)+":"+std::to_string(outage.start));}
std::string FindingReviewKey(const Finding& f){Sha256 hash;hash.update("finding:"+f.title+":"+f.detail);for(const auto& e:f.ev){hash.update(LineIdentity(e.lineNo));hash.update(e.text);}return hash.finish();}
std::string WorkspaceReviewText(){std::string out;for(const auto& pair:App().document.workspace.reviews)out+=reviewRecordText(pair.second)+"\n";return out;}
bool RouteWorkspaceMessage(MSG& message){for(HWND w:{g_editor,g_workspace})if(w&&(message.hwnd==w||IsChild(w,message.hwnd))){if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){SendMessageW(w,WM_CLOSE,0,0);return true;}if(message.message==WM_KEYDOWN&&message.wParam==VK_RETURN&&w==g_workspace&&g_page==1&&message.hwnd==GetDlgItem(w,list)){int row=ListView_GetNextItem(message.hwnd,-1,LVNI_SELECTED);if(row>=0&&static_cast<std::size_t>(row)<g_reviewKeys.size()){const auto& record=App().document.workspace.reviews.at(g_reviewKeys[row]);EditManualReview(record.key,record.title);}return true;}if(message.message==WM_KEYDOWN&&message.wParam=='A'&&(GetKeyState(VK_CONTROL)&0x8000)){wchar_t cls[32]{};GetClassNameW(message.hwnd,cls,32);if(lstrcmpW(cls,L"Edit")==0){SendMessageW(message.hwnd,EM_SETSEL,0,-1);return true;}}if(!IsDialogMessageW(w,&message)){TranslateMessage(&message);DispatchMessageW(&message);}return true;}return false;}
void ImportEvidencePackage(){if(LoadInProgress())return;wchar_t path[MAX_PATH]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=App().hMain;dialog.lpstrFile=path;dialog.nMaxFile=MAX_PATH;dialog.lpstrFilter=L"ZIP\0*.zip\0\0";dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;dialog.lpstrTitle=UiText(TextId::package_import);if(!GetOpenFileNameW(&dialog))return;try{std::string bytes;std::wstring error;if(!ReadBoundedFile(path,bytes,error,kMaxInputBytes))throw std::runtime_error(WToU8(error));auto package=readEvidencePackage(bytes);if(package.originals.empty()){ShowSelectableText(UiText(TextId::package_report),std::wstring(UiText(TextId::package_no_original))+L"\r\n\r\n"+U8ToW(package.report));return;}std::string().swap(bytes);package={};LoadFiles({path});}catch(const std::exception& e){ShowModernNotice(UiText(TextId::package_invalid),U8ToW(e.what()).c_str(),ModernNoticeKind::Error);}}
}
