// load_controller.cpp — 日志来源加载、筛选刷新和导出协调
#include "load_controller.h"

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "app_settings.h"
#include "app_context.h"
#include "log_analysis.h"
#include "log_filter.h"
#include "log_parser.h"
#include "memoryutil.h"
#include "modern_shell.h"
#include "tablemodel.h"
#include "ui_pages.h"
#include "win_file_io.h"
#include "win_text.h"

namespace dl {

namespace {

class BusyScope {
public:
    explicit BusyScope(const wchar_t* text) { SetShellBusy(true, text); }
    ~BusyScope() { SetShellBusy(false); }
    BusyScope(const BusyScope&) = delete;
    BusyScope& operator=(const BusyScope&) = delete;
};

bool g_regexWasBad = false;

} // namespace

void RefreshAll() {
    if (App().document.lines.empty()) { SetWindowTextW(App().hStatus, L"尚未加载日志。"); return; }
    ResetVirtualTables();
    bool bad = false;
    App().document.filtered = applyFilterView(App().document.lines, WToU8(GetText(App().hTagBox)), WToU8(GetText(App().hGrepBox)),
                             WToU8(GetText(App().hSinceBox)), WToU8(GetText(App().hUntilBox)), &bad);
    App().document.outages = collectOutages(App().document.filtered);
    App().document.metrics = buildMetrics(App().document.filtered);

    // 结论基于**筛选后**的视图,与各页展示保持一致
    App().document.findings = analyze(App().document.filtered, App().document.outages, App().document.metrics, App().document.platform, App().document.audit);

    MarkAllPagesDirty();
    RenderPage(CurrentPage());

    std::wstring st = FmtW(L"  筛选后 %d / 共 %d 行   ·   断网 %d 次   ·   会话(重启) %d",
                           (int)App().document.filtered.size(), (int)App().document.lines.size(), (int)App().document.outages.size(), (int)App().document.sessions.size());
    st += L"   ·   平台: " + U8ToW(App().document.platform.name);
    // 未识别行占比常显在状态栏:任何一页都能看到覆盖率,而不是藏在某个页里
    if (App().document.audit.unparsed == 0)
        st += L"   ·   未识别 0 行(无遗漏)";
    else
        st += FmtW(L"   ·   ⚠ 未识别 %d 行(%.2f%%,见“未识别行”页)",
                   (int)App().document.audit.unparsed, App().document.audit.unparsedRatio() * 100.0);
    if (bad) st += L"   ·   ⚠ 正则非法,已忽略该条件";
    SetWindowTextW(App().hStatus, st.c_str());
    if (bad && !g_regexWasBad)
        ShowModernNotice(L"正则表达式无效", L"已暂时忽略“消息正则”条件，其他筛选仍然生效。",
                         ModernNoticeKind::Warning, 6000);
    g_regexWasBad = bad;
}

// 载入的公共尾段:移动接管原始行,解析后立即释放,不让 raw 与后续分析结果长期共存。
static void LoadRawLines(std::vector<std::string> raw, const std::wstring& srcLabel,
                         const std::vector<size_t>& fileBoundaries = {}) {
    // raw 已成功读取/合并后才卸载旧日志；读取失败仍保留当前分析。释放旧分析结果后再
    // parse,避免“大旧日志模型 + 新日志原文 + 新分析模型”三者在切换期间重叠。
    ReleaseLoadedData();
    parseLines(raw, App().document.lines, App().document.sessions, &App().document.audit, fileBoundaries);
    releaseVector(raw);
    App().document.platform = detectPlatform(App().document.lines);   // 平台识别用全量行(不受筛选影响)
    std::wstring lbl = srcLabel;
    lbl += FmtW(L"   (%d 行, %d 会话, %s)", (int)App().document.lines.size(), (int)App().document.sessions.size(),
                U8ToW(App().document.platform.name).c_str());
    SetWindowTextW(App().hFileLbl, lbl.c_str());
    RefreshAll();
}

void LoadFiles(const std::vector<std::wstring>& paths) {
    BusyScope busy(L"正在读取并分析日志…");
    // 逐份探测 → 跨时基混合防护 → 按首时间戳定序 → 增量解析。
    // 拖入顺序(资源管理器多选)与文件对话框返回顺序都不保证按时间,而 parseLines 不排序,
    // 顺序拼接会让时间线/断网/可用率全错(v1.3.0 及之前的行为)。定序与时基判定逻辑均在
    // logmodel(orderByTime / detectMix),此处只做 I/O、排除、增量投喂、提示。
    std::vector<LoadSource> sources;
    std::vector<std::wstring> openedPaths;
    size_t batchTextBytes = 0;
    for (const auto& p : paths) {
        std::wstring readErr;
        bool loaded = false;
        try {
            dl::ArchiveKind kind = dl::ARC_NONE;
            size_t pathBytes = 0;
            loaded = InspectFile(p, kind, pathBytes, readErr);
            if (loaded && kind == dl::ARC_NONE) {
                LoadSource source;
                source.streamPlain = true;
                source.path = p;
                source.label = FileNameOf(p);
                source.textBytes = pathBytes;
                loaded = ReadPlainLines(
                    p, 200,
                    [&](std::string line) { source.probe.push_back(std::move(line)); },
                    nullptr, readErr);
                if (loaded && source.probe.empty()) {
                    loaded = false;
                    readErr = L"文件为空";
                }
                if (loaded && source.textBytes > kMaxBatchTextBytes - batchTextBytes) {
                    loaded = false;
                    readErr = L"本次选择的日志文本总量超过 512 MiB";
                }
                if (loaded) {
                    batchTextBytes += source.textBytes;
                    sources.push_back(std::move(source));
                }
            } else if (loaded) {
                // 压缩包仍需先解压，但每个条目随后直接投喂解析器，不再拼出第二份 raw。
                std::vector<std::vector<std::string>> sub;
                std::vector<std::wstring> subLabels;
                size_t subTextBytes = 0;
                loaded = ReadPathExpand(p, sub, subLabels, subTextBytes, readErr);
                if (loaded && subTextBytes > kMaxBatchTextBytes - batchTextBytes) {
                    loaded = false;
                    readErr = L"本次选择的展开后日志文本总量超过 512 MiB";
                }
                if (loaded) {
                    batchTextBytes += subTextBytes;
                    for (size_t i = 0; i < sub.size(); ++i) {
                        LoadSource source;
                        source.label = subLabels[i];
                        source.lines = std::move(sub[i]);
                        const size_t n = std::min<size_t>(source.lines.size(), 200);
                        source.probe.assign(source.lines.begin(), source.lines.begin() + n);
                        sources.push_back(std::move(source));
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            loaded = false;
            readErr = L"内存不足,无法读取、展开或切分日志";
        }
        if (!loaded) {
            MessageBoxW(App().hMain, (L"读取失败:\n" + p + L"\n\n" + readErr).c_str(), L"错误", MB_ICONERROR);
        } else {
            openedPaths.push_back(p);
        }
    }
    if (sources.empty()) return;

    std::vector<std::vector<std::string>> probes;
    probes.reserve(sources.size());
    for (const auto& source : sources) probes.push_back(source.probe);

    // 跨时基混合防护(方案A):同时含墙钟与"时钟未同步"日志时,未同步批与墙钟批不在同一
    // 时间坐标系,混合拼接会把跨度撑成几十年、可用率从"差"翻成"良好"。此处排除未同步批,
    // 只用墙钟批出结论,并明确列出被排除的文件让用户知情。未同步批未销毁,可单独再拖入分析。
    MixReport mix = detectMix(probes);
    std::wstring excludedNote;
    if (mix.mixed) {
        std::wstring names;
        for (size_t i : mix.unsyncedIdx) {
            // 只取文件名,不带路径,提示更短
            const std::wstring& full = sources[i].label;
            size_t slash = full.find_last_of(L"\\/");
            names += L"\n  · " + (slash == std::wstring::npos ? full : full.substr(slash + 1));
        }
        std::wstring msg = FmtW(L"检测到 %d 份日志时钟未同步(时间戳落在 1970 年),\n"
                               L"与其余 %d 份墙钟日志不在同一时间坐标系。\n\n"
                               L"已从本次合并中排除以下未同步日志,仅用墙钟日志出结论:%s\n\n"
                               L"如需查看未同步日志,请单独拖入分析。",
                               (int)mix.unsyncedIdx.size(), (int)mix.wallIdx.size(), names.c_str());
        MessageBoxW(App().hMain, msg.c_str(), L"时钟未同步日志已排除", MB_ICONWARNING | MB_OK);

        // 用墙钟批重建来源/探针,后续定序只在墙钟批内进行
        std::vector<LoadSource> keptSources;
        std::vector<std::vector<std::string>> keptProbes;
        for (size_t i : mix.wallIdx) {
            keptSources.push_back(std::move(sources[i]));
            keptProbes.push_back(std::move(probes[i]));
        }
        sources.swap(keptSources);
        probes.swap(keptProbes);
        excludedNote = FmtW(L",已排除 %d 份未同步日志", (int)mix.unsyncedIdx.size());
    }
    if (sources.empty()) return;   // 理论上不会:mixed 时 wallIdx 必非空,防御性保留

    std::vector<size_t> ord = orderByTime(probes);

    bool reordered = false;
    int noTs = 0;
    for (size_t i = 0; i < ord.size(); ++i) {
        if (ord[i] != i) reordered = true;
        if (!firstTimestamp(probes[ord[i]], nullptr)) noTs++;
    }

    // 读入和探测都成功后再卸载旧日志；普通文件从磁盘分块投喂，压缩条目移动投喂。
    // 不再同时持有 chunks + 合并 raw + LogLine 三份大对象。
    size_t reserveHint = 0;
    size_t plainReserveHint = 0;
    for (const auto& source : sources) {
        if (!source.streamPlain) {
            reserveHint += source.lines.size();
        } else if (!source.probe.empty()) {
            size_t probeBytes = 0;
            for (const auto& line : source.probe) probeBytes += line.size() + 1;
            size_t avg = std::max<size_t>(probeBytes / source.probe.size(), 32);
            plainReserveHint += source.textBytes / avg + 1;
        }
    }
    // 多个短行文件的估算可能很大；统一只预留前 100 万行，其余让 vector 按需增长，
    // 避免仅凭 200 行样本就在解析前一次性申请数 GiB。
    reserveHint += std::min<size_t>(plainReserveHint, 1000000);
    ReleaseLoadedData();
    bool parseOk = true;
    std::wstring parseErr;
    try {
        StreamingLogParser parser(App().document.lines, App().document.sessions, reserveHint);
        for (size_t i : ord) {
            parser.beginFile();
            LoadSource& source = sources[i];
            if (source.streamPlain) {
                if (!ReadPlainLines(
                        source.path, std::numeric_limits<size_t>::max(),
                        [&](std::string line) { parser.pushLine(std::move(line)); },
                        nullptr, parseErr)) {
                    parseOk = false;
                    break;
                }
            } else {
                for (auto& line : source.lines) parser.pushLine(std::move(line));
                releaseVector(source.lines);
            }
        }
        if (parseOk) parser.finish(&App().document.audit);
    } catch (const std::bad_alloc&) {
        parseOk = false;
        parseErr = L"内存不足,无法解析所选日志";
    }
    if (!parseOk) {
        ReleaseLoadedData();
        MessageBoxW(App().hMain, parseErr.c_str(), L"读取失败", MB_ICONERROR);
        return;
    }

    std::wstring lbl;
    if (sources.size() == 1) {
        lbl = sources[0].label + excludedNote;   // 排除后只剩一份时,仍要带上排除提示
    } else {
        // 多文件必须让人看见到底按什么顺序拼的 —— 否则重排是隐形的,出了错也无从察觉
        lbl = FmtW(L"%d 个文件合并", (int)sources.size());
        lbl += reordered ? L"(已按时间重排)" : L"(拖入顺序已是时间顺序)";
        if (noTs > 0) lbl += FmtW(L",其中 %d 份扫不到时间戳→拼在最后", noTs);
        lbl += excludedNote;
    }
    App().document.platform = detectPlatform(App().document.lines);
    lbl += FmtW(L"   (%d 行, %d 会话, %s)", (int)App().document.lines.size(), (int)App().document.sessions.size(),
                U8ToW(App().document.platform.name).c_str());
    SetWindowTextW(App().hFileLbl, lbl.c_str());
    RefreshAll();
    RememberRecentFiles(openedPaths);
}

// 从剪贴板粘贴日志文本分析(SSH 里 cat 日志后直接选中复制的场景,手上没有文件)
void DoPaste() {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        ShowModernNotice(L"剪贴板里没有文本",
                         L"请先复制日志内容，再使用“粘贴日志”或 Ctrl+V。",
                         ModernNoticeKind::Info);
        return;
    }
    if (!OpenClipboard(App().hMain)) {
        ShowModernNotice(L"暂时无法读取剪贴板", L"剪贴板可能正被其他程序占用，请稍后重试。",
                         ModernNoticeKind::Error, 6000);
        return;
    }
    std::wstring w;
    bool clipboardOom = false;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) {
            try { w = p; }
            catch (const std::bad_alloc&) { clipboardOom = true; }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (clipboardOom) {
        MessageBoxW(App().hMain, L"内存不足,无法复制剪贴板文本。", L"错误", MB_ICONERROR);
        return;
    }
    if (w.empty()) {
        ShowModernNotice(L"剪贴板文本为空", L"复制包含时间戳的日志内容后再试。",
                         ModernNoticeKind::Info);
        return;
    }
    // UTF-16 转 UTF-8 最坏每个码点 4 字节,在转换前即执行与文件相同的 512MiB 上限。
    if (w.size() > (size_t)kMaxInputBytes / 4) {
        MessageBoxW(App().hMain, L"剪贴板文本超过 512 MiB 输入限制。", L"内容过大", MB_ICONWARNING);
        return;
    }

    BusyScope busy(L"正在分析剪贴板日志…");

    // 按行切分(兼容 \r\n / \n / \r 三种换行);与文件读取共用纯 C++ 实现。
    std::vector<std::string> raw;
    try {
        std::string u8 = WToU8(w);
        dl::splitTextLines(std::move(u8), raw);
    } catch (const std::bad_alloc&) {
        MessageBoxW(App().hMain, L"内存不足,无法分析剪贴板文本。", L"错误", MB_ICONERROR);
        return;
    }

    std::wstring pasteLabel = FmtW(L"[剪贴板粘贴 %d 行]", (int)raw.size());
    LoadRawLines(std::move(raw), pasteLabel);

    // 粘贴的往往是片段,若一行都没认出来,直接把原因摆出来(而不是让用户对着空界面猜)
    if (App().document.lines.empty()) {
        ShowModernNotice(L"没有识别出日志行",
                         L"请确认复制内容带有行首时间戳；可在“未识别行”页面查看原文。",
                         ModernNoticeKind::Warning, 8000);
    }
}

void DoOpen() {
    std::vector<wchar_t> buf(32768, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = App().hMain;
    ofn.lpstrFilter = L"日志与压缩包 (*.log;*.txt;*.zip;*.gz)\0*.log;*.txt;*.zip;*.gz;*.tar.gz\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrTitle = L"选择 dial 日志(可多选,将合并分析)";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;

    std::vector<std::wstring> paths;
    std::wstring dir = buf.data();
    size_t p = dir.size() + 1;
    if (buf[p] == 0) {           // 单选:buf 即完整路径
        paths.push_back(dir);
    } else {                     // 多选:目录\0文件1\0文件2\0\0
        while (buf[p]) {
            std::wstring f = &buf[p];
            paths.push_back(dir + L"\\" + f);
            p += f.size() + 1;
        }
    }
    LoadFiles(paths);
}

void DoExportCsv() {
    if (App().document.metrics.empty()) {
        ShowModernNotice(L"没有可导出的指标", L"加载包含心跳或信号采样的日志后再导出。",
                         ModernNoticeKind::Info);
        return;
    }
    wchar_t file[MAX_PATH] = L"diallog_metrics.csv";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = App().hMain;
    ofn.lpstrFilter = L"CSV (*.csv)\0*.csv\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"csv";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;

    std::string out;
    try {
        out = "\xEF\xBB\xBF";                      // UTF-8 BOM,Excel 中文不乱码
        auto csv = [](const std::string& s) {
            if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
            std::string q = "\"";
            for (char c : s) { q += c; if (c == '\"') q += '\"'; }
            q += '\"';
            return q;
        };
        out += "time,ch,csq,tmax,consec_fail,rx_pkt,drx,rsrp,rsrq,snr_db,rssi,srv,rat,deny,oper\r\n";
        for (const auto& m : App().document.metrics) {
            for (size_t column = 0; column <= 6; ++column) {
                out += csv(metricCellText(m, column));
                out += ',';
            }
            out += (m.rsrp < 0 ? std::to_string(m.rsrp) : ""); out += ',';
            out += (m.rsrq < 0 ? std::to_string(m.rsrq) : ""); out += ',';
            if (m.snr10 != 100000) { char b[32]; snprintf(b, sizeof(b), "%.1f", m.snr10 / 10.0); out += b; }
            out += ',';
            out += (m.rssiVal < 0 ? std::to_string(m.rssiVal) : ""); out += ',';
            for (size_t column = 11; column < kMetricColumnCount; ++column) {
                out += csv(metricCellText(m, column));
                out += (column + 1 == kMetricColumnCount) ? "\r\n" : ",";
            }
        }
    } catch (const std::bad_alloc&) {
        MessageBoxW(App().hMain, L"内存不足,无法生成 CSV。", L"错误", MB_ICONERROR);
        return;
    }
    std::wstring writeErr;
    if (!WriteFileBytesAtomic(file, out, writeErr)) {
        MessageBoxW(App().hMain, writeErr.c_str(), L"导出失败", MB_ICONERROR);
        return;
    }
    SetWindowTextW(App().hStatus, (std::wstring(L"已导出 ") + file).c_str());
    ShowModernNotice(L"CSV 导出完成", file, ModernNoticeKind::Success, 6000);
}


} // namespace dl
