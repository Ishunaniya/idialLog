// load_controller.cpp — 日志来源加载、筛选刷新和导出协调
#include "load_controller.h"

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "app_settings.h"
#include "app_context.h"
#include "chartmodel.h"
#include "log_analysis.h"
#include "log_filter.h"
#include "log_parser.h"
#include "log_time.h"
#include "memoryutil.h"
#include "modern_shell.h"
#include "signal_quality.h"
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
volatile LONG g_loadActive = 0;
volatile LONG g_loadCancel = 0;
volatile LONG g_loadShutdown = 0;
HANDLE g_loadThread = nullptr;
DWORD g_loadCompletedAt = 0;

struct LoadRequest {
    HWND owner = nullptr;
    std::vector<std::wstring> paths;
    std::string tag, grep, since, until;
};

struct LoadResult {
    DocumentState document;
    std::vector<std::wstring> openedPaths;
    std::wstring label, warning, error;
    bool cancelled = false;
    bool badRegex = false;
    bool success = false;
};

} // namespace

static void PresentAnalysis(bool bad) {
    MarkAllPagesDirty();
    RenderPage(CurrentPage());

    std::wstring st = FmtW(L"  筛选后 %d / 共 %d 行   ·   断网 %d 次   ·   启动证据 %d",
                           (int)App().document.filtered.size(), (int)App().document.lines.size(),
                           (int)App().document.outages.size(), (int)App().document.sessions.size());
    st += L"   ·   平台: " + U8ToW(App().document.platform.name);
    const MetricRow* latestCell = nullptr;
    for (const MetricRow& metric : App().document.metrics)
        if (!metric.cellId.empty() && (!latestCell || metric.t > latestCell->t ||
            (metric.t == latestCell->t && metric.lineNo > latestCell->lineNo))) latestCell = &metric;
    if (latestCell)
        st += FmtW(L"   ·   最近小区 ID: %s（共 %d 个）",
                   U8ToW(latestCell->cellId.str()).c_str(),
                   static_cast<int>(App().document.cellAnalysis.cells.size()));
    else
        st += L"   ·   小区 ID: 日志未提供";
    if (App().document.audit.unparsed == 0 && App().document.audit.nulBytes == 0)
        st += L"   ·   未识别 0 行、NUL 0 字节";
    else
        st += FmtW(L"   ·   ⚠ 未识别 %d 行(%.2f%%)，NUL %d 字节/%d 行(见审计页)",
                   (int)App().document.audit.unparsed, App().document.audit.unparsedRatio() * 100.0,
                   (int)App().document.audit.nulBytes, (int)App().document.audit.nulLines);
    if (bad) st += L"   ·   ⚠ 正则非法,已忽略该条件";
    SetWindowTextW(App().hStatus, st.c_str());
    if (bad && !g_regexWasBad)
        ShowModernNotice(L"正则表达式无效", L"已暂时忽略“消息正则”条件，其他筛选仍然生效。",
                         ModernNoticeKind::Warning, 6000);
    g_regexWasBad = bad;
}

void RefreshAll() {
    if (App().document.lines.empty()) { SetWindowTextW(App().hStatus, L"尚未加载日志。"); return; }
    ResetVirtualTables();
    bool bad = false;
    App().document.filtered = applyFilterView(App().document.lines, WToU8(GetText(App().hTagBox)), WToU8(GetText(App().hGrepBox)),
                             WToU8(GetText(App().hSinceBox)), WToU8(GetText(App().hUntilBox)), &bad);
    App().document.outages = collectOutages(App().document.filtered);
    App().document.metrics = buildMetrics(App().document.filtered);
    RebuildMetricQuickFilterView();
    App().document.cellAnalysis = analyzeCells(App().document.filtered, App().document.metrics,
                                               App().document.outages);

    // 结论基于**筛选后**的视图,与各页展示保持一致
    App().document.findings = analyze(App().document.filtered, App().document.outages,
                                      App().document.metrics, App().document.platform,
                                      App().document.audit, &App().document.cellAnalysis);

    PresentAnalysis(bad);
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
    lbl += FmtW(L"   (%d 行, %d 个启动证据, %s)", (int)App().document.lines.size(), (int)App().document.sessions.size(),
                U8ToW(App().document.platform.name).c_str());
    SetWindowTextW(App().hFileLbl, lbl.c_str());
    RefreshAll();
}

struct WorkerProgress {
    HWND owner = nullptr;
    int base = 0;
    int span = 0;
    int stage = 0;
    int last = -1;
};

static bool LoadCancelled() {
    return InterlockedCompareExchange(&g_loadCancel, 0, 0) != 0;
}

static void PostLoadProgress(HWND owner, int percent, int stage) {
    if (owner) PostMessageW(owner, WM_APP_LOAD_PROGRESS,
                            static_cast<WPARAM>(std::max(0, std::min(100, percent))),
                            static_cast<LPARAM>(stage));
}

static bool ObserveRead(void* context, size_t done, size_t total) {
    auto& progress = *static_cast<WorkerProgress*>(context);
    if (LoadCancelled()) return false;
    int percent = progress.base;
    if (total) percent += static_cast<int>((static_cast<unsigned long long>(done) * progress.span) / total);
    if (percent != progress.last) {
        progress.last = percent;
        PostLoadProgress(progress.owner, percent, progress.stage);
    }
    return true;
}

static void AddLoadProblem(std::wstring& text, const std::wstring& path, const std::wstring& reason) {
    if (!text.empty()) text += L"\n";
    text += L"• " + FileNameOf(path) + L"：" + (reason.empty() ? L"未知错误" : reason);
}

static void DeliverLoadResult(HWND owner, std::unique_ptr<LoadResult>& result) {
    result->cancelled = result->cancelled || LoadCancelled();
    if (InterlockedCompareExchange(&g_loadShutdown, 0, 0) ||
        !PostMessageW(owner, WM_APP_LOAD_COMPLETE, 0,
                      reinterpret_cast<LPARAM>(result.get()))) return;
    result.release();
}

static DWORD WINAPI LoadWorker(void* parameter) {
    std::unique_ptr<LoadRequest> request(static_cast<LoadRequest*>(parameter));
    std::unique_ptr<LoadResult> result(new (std::nothrow) LoadResult);
    if (!result) {
        if (!InterlockedCompareExchange(&g_loadShutdown, 0, 0))
            PostMessageW(request->owner, WM_APP_LOAD_COMPLETE, 0, 0);
        return 0;
    }

    try {

    // 逐份探测 → 跨时基混合防护 → 按首时间戳定序 → 增量解析。
    // 拖入顺序(资源管理器多选)与文件对话框返回顺序都不保证按时间,而 parseLines 不排序,
    // 顺序拼接会让时间线/断网/可用率全错(v1.3.0 及之前的行为)。定序与时基判定逻辑均在
    // logmodel(orderByTime / detectMix),此处只做 I/O、排除、增量投喂、提示。
    std::vector<LoadSource> sources;
    size_t batchTextBytes = 0;
    std::wstring sourceProblems;
    for (size_t pathIndex = 0; pathIndex < request->paths.size(); ++pathIndex) {
        if (LoadCancelled()) { result->cancelled = true; break; }
        const auto& p = request->paths[pathIndex];
        PostLoadProgress(request->owner,
                         2 + static_cast<int>(pathIndex * 24 / std::max<size_t>(1, request->paths.size())), 0);
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
                WorkerProgress progress{request->owner, 3, 20, 0, -1};
                loaded = ReadPathExpand(p, sub, subLabels, subTextBytes, readErr,
                                        &progress, ObserveRead);
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
            if (LoadCancelled()) { result->cancelled = true; break; }
            AddLoadProblem(sourceProblems, p, readErr);
        } else {
            result->openedPaths.push_back(p);
        }
    }
    if (result->cancelled) { DeliverLoadResult(request->owner, result); return 0; }
    if (sources.empty()) {
        result->error = sourceProblems.empty() ? L"没有可读取的日志来源。" : sourceProblems;
        DeliverLoadResult(request->owner, result); return 0;
    }
    if (!sourceProblems.empty()) result->warning = L"部分文件未载入：\n" + sourceProblems;
    PostLoadProgress(request->owner, 28, 1);

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
        if (!result->warning.empty()) result->warning += L"\n\n";
        result->warning += msg;

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
    if (sources.empty()) {
        result->error = L"时基检查后没有可分析的日志。";
        DeliverLoadResult(request->owner, result); return 0;
    }

    std::vector<size_t> ord = orderByTime(probes);

    bool reordered = false;
    int noTs = 0;
    for (size_t i = 0; i < ord.size(); ++i) {
        if (ord[i] != i) reordered = true;
        if (!firstTimestamp(probes[ord[i]], nullptr)) noTs++;
    }

    // 普通文件从磁盘分块投喂，压缩条目移动投喂；旧文档在新结果成功前保持可用。
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
    bool parseOk = true;
    std::wstring parseErr;
    struct SourceRange { std::wstring label; size_t first = 0, last = 0; };
    std::vector<SourceRange> ranges;
    try {
        StreamingLogParser parser(result->document.lines, result->document.sessions, reserveHint);
        for (size_t orderIndex = 0; orderIndex < ord.size(); ++orderIndex) {
            if (LoadCancelled()) { parseOk = false; parseErr = L"操作已取消"; break; }
            const size_t i = ord[orderIndex];
            parser.beginFile();
            LoadSource& source = sources[i];
            SourceRange range{source.label, result->document.lines.size(), result->document.lines.size()};
            if (source.streamPlain) {
                WorkerProgress progress{
                    request->owner,
                    35 + static_cast<int>(orderIndex * 48 / std::max<size_t>(1, ord.size())),
                    static_cast<int>(48 / std::max<size_t>(1, ord.size())), 2, -1};
                if (!ReadPlainLines(
                        source.path, std::numeric_limits<size_t>::max(),
                        [&](std::string line) { parser.pushLine(std::move(line)); },
                        nullptr, parseErr, &progress, ObserveRead)) {
                    parseOk = false;
                    break;
                }
            } else {
                for (size_t lineIndex = 0; lineIndex < source.lines.size(); ++lineIndex) {
                    if ((lineIndex & 4095U) == 0 && LoadCancelled()) {
                        parseOk = false; parseErr = L"操作已取消"; break;
                    }
                    parser.pushLine(std::move(source.lines[lineIndex]));
                }
                releaseVector(source.lines);
            }
            if (!parseOk) break;
            range.last = result->document.lines.size();
            ranges.push_back(std::move(range));
        }
        if (parseOk) parser.finish(&result->document.audit);
    } catch (const std::bad_alloc&) {
        parseOk = false;
        parseErr = L"内存不足,无法解析所选日志";
    }
    if (!parseOk) {
        result->cancelled = LoadCancelled();
        if (!result->cancelled) result->error = parseErr;
        DeliverLoadResult(request->owner, result); return 0;
    }

    PostLoadProgress(request->owner, 85, 3);
    result->document.platform = detectPlatform(result->document.lines);
    result->document.filtered = applyFilterView(result->document.lines, request->tag, request->grep,
                                                 request->since, request->until, &result->badRegex);
    result->document.outages = collectOutages(result->document.filtered);
    result->document.metrics = buildMetrics(result->document.filtered);
    result->document.metricView.reserve(result->document.metrics.size());
    for (const MetricRow& metric : result->document.metrics)
        result->document.metricView.push_back(&metric);
    result->document.cellAnalysis = analyzeCells(result->document.filtered, result->document.metrics,
                                                 result->document.outages);
    result->document.findings = analyze(result->document.filtered, result->document.outages,
                                        result->document.metrics, result->document.platform,
                                        result->document.audit, &result->document.cellAnalysis);

    if (LoadCancelled()) {
        result->cancelled = true;
        DeliverLoadResult(request->owner, result); return 0;
    }

    // 多文件不只“拼在一起”，同时保留每份来源的可比较摘要，供概览和报告使用。
    for (const auto& range : ranges) {
        if (LoadCancelled()) {
            result->cancelled = true;
            DeliverLoadResult(request->owner, result); return 0;
        }
        LogView view;
        view.reserve(range.last - range.first);
        for (size_t index = range.first; index < range.last; ++index)
            view.push_back(&result->document.lines[index]);
        auto metrics = buildMetrics(view);
        auto outages = collectOutages(view);
        SourceSummary summary;
        summary.label = range.label;
        summary.parsedLines = view.size();
        summary.metricRows = metrics.size();
        summary.outages = outages.size();
        long long rsrpTotal = 0; size_t rsrpCount = 0;
        for (const auto& metric : metrics) if (metric.rsrp < 0) { rsrpTotal += metric.rsrp; ++rsrpCount; }
        if (rsrpCount) {
            summary.averageRsrp = static_cast<int>(rsrpTotal / static_cast<long long>(rsrpCount));
            summary.hasAverageRsrp = true;
        }
        result->document.sources.push_back(std::move(summary));
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
    lbl += FmtW(L"   (%d 行, %d 个启动证据, %s)", (int)result->document.lines.size(),
                (int)result->document.sessions.size(), U8ToW(result->document.platform.name).c_str());
    result->label = std::move(lbl);
    result->success = true;
    PostLoadProgress(request->owner, 100, 3);

    DeliverLoadResult(request->owner, result);
    } catch (const std::bad_alloc&) {
        result->error = L"内存不足，后台加载未完成；当前分析结果保持不变。";
        DeliverLoadResult(request->owner, result);
    } catch (...) {
        result->error = L"后台加载遇到未预期错误；当前分析结果保持不变。";
        DeliverLoadResult(request->owner, result);
    }
    return 0;
}

void LoadFiles(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    if (LoadInProgress()) {
        ShowModernNotice(L"日志正在加载", L"可点击“取消加载”，再选择另一批日志。",
                         ModernNoticeKind::Info);
        return;
    }
    std::unique_ptr<LoadRequest> request(new (std::nothrow) LoadRequest);
    if (!request) {
        MessageBoxW(App().hMain, L"内存不足，无法创建加载任务。", L"错误", MB_ICONERROR);
        return;
    }
    request->owner = App().hMain;
    request->paths = paths;
    request->tag = WToU8(GetText(App().hTagBox));
    request->grep = WToU8(GetText(App().hGrepBox));
    request->since = WToU8(GetText(App().hSinceBox));
    request->until = WToU8(GetText(App().hUntilBox));
    InterlockedExchange(&g_loadCancel, 0);
    InterlockedExchange(&g_loadShutdown, 0);
    InterlockedExchange(&g_loadActive, 1);
    g_loadThread = CreateThread(nullptr, 0, LoadWorker, request.get(), 0, nullptr);
    if (!g_loadThread) {
        InterlockedExchange(&g_loadActive, 0);
        MessageBoxW(App().hMain, L"无法启动后台加载线程。", L"错误", MB_ICONERROR);
        return;
    }
    request.release();
    SetWindowTextW(App().hCloseLog, L"取消加载");
    SetShellBusy(true, L"正在检查日志来源… 0%");
    SetShellProgress(0);
}

bool LoadInProgress() {
    return InterlockedCompareExchange(&g_loadActive, 0, 0) != 0;
}

void CancelLoad() {
    if (!LoadInProgress()) return;
    InterlockedExchange(&g_loadCancel, 1);
    SetWindowTextW(App().hCloseLog, L"正在取消…");
    EnableWindow(App().hCloseLog, FALSE);
    SetShellProgress(0, L"正在取消加载；当前分析结果会保留…");
}

bool ConsumeLoadActionClick() {
    if (LoadInProgress()) {
        CancelLoad();
        return true;
    }
    // “取消加载”和“关闭日志”复用一个按钮。后台完成消息可能恰好排在已经按下的
    // 取消点击之前；短暂吞掉这次滞后的点击，避免它按新语义误清空文档。
    return g_loadCompletedAt != 0 && GetTickCount() - g_loadCompletedAt < 750;
}

bool HandleLoadControllerMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_APP_LOAD_PROGRESS) {
        if (!LoadInProgress() || LoadCancelled()) return true;
        const int percent = static_cast<int>(wparam);
        const int stage = static_cast<int>(lparam);
        const wchar_t* name = stage == 0 ? L"检查与展开" : stage == 1 ? L"校验时间轴" :
                              stage == 2 ? L"解析日志" : L"生成分析";
        const std::wstring text = FmtW(L"正在%s… %d%%   ·   可点击“取消加载”保留当前结果", name, percent);
        SetShellProgress(percent, text.c_str());
        return true;
    }
    if (message != WM_APP_LOAD_COMPLETE) return false;

    std::unique_ptr<LoadResult> result(reinterpret_cast<LoadResult*>(lparam));
    // worker 可能已投递完成消息、但 UI 尚未处理时用户点击取消；此时仍应尊重取消，
    // 不能因为结果已经算完就覆盖当前文档。
    const bool cancelRequested = LoadCancelled();
    if (g_loadThread) { CloseHandle(g_loadThread); g_loadThread = nullptr; }
    InterlockedExchange(&g_loadActive, 0);
    g_loadCompletedAt = GetTickCount();
    SetShellBusy(false);
    SetWindowTextW(App().hCloseLog, L"关闭日志");
    EnableWindow(App().hCloseLog, TRUE);

    if (!result) {
        MessageBoxW(App().hMain, L"后台加载时内存不足，当前分析结果未改变。", L"加载失败", MB_ICONERROR);
        return true;
    }
    if (result->cancelled || cancelRequested) {
        ShowModernNotice(L"已取消加载", L"当前日志与分析结果保持不变。", ModernNoticeKind::Info);
        return true;
    }
    if (!result->success) {
        MessageBoxW(App().hMain,
                    (result->error.empty() ? L"日志加载失败，当前分析结果未改变。" : result->error.c_str()),
                    L"加载失败", MB_ICONERROR);
        return true;
    }

    ResetVirtualTables();
    ReleaseLoadedData();
    App().document.swap(result->document);
    RebuildMetricQuickFilterView();
    SetWindowTextW(App().hFileLbl, result->label.c_str());
    PresentAnalysis(result->badRegex);
    RememberRecentFiles(result->openedPaths);
    RefreshNavigation();
    if (!result->warning.empty())
        MessageBoxW(App().hMain, result->warning.c_str(), L"日志已载入（有提示）", MB_ICONWARNING | MB_OK);
    else
        ShowModernNotice(L"日志分析完成", result->label.c_str(), ModernNoticeKind::Success, 5000);
    return true;
}

void ShutdownLoadController() {
    if (!LoadInProgress()) return;
    InterlockedExchange(&g_loadShutdown, 1);
    InterlockedExchange(&g_loadCancel, 1);
    if (g_loadThread) {
        WaitForSingleObject(g_loadThread, INFINITE);
        CloseHandle(g_loadThread); g_loadThread = nullptr;
    }
    MSG message{};
    while (PeekMessageW(&message, App().hMain, WM_APP_LOAD_COMPLETE, WM_APP_LOAD_COMPLETE, PM_REMOVE))
        delete reinterpret_cast<LoadResult*>(message.lParam);
    InterlockedExchange(&g_loadActive, 0);
}

// 从剪贴板粘贴日志文本分析(SSH 里 cat 日志后直接选中复制的场景,手上没有文件)
void DoPaste() {
    if (LoadInProgress()) {
        ShowModernNotice(L"日志正在加载", L"请先取消当前加载任务。", ModernNoticeKind::Info);
        return;
    }
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
    if (LoadInProgress()) {
        ShowModernNotice(L"日志正在加载", L"请先取消当前加载任务。", ModernNoticeKind::Info);
        return;
    }
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
        out += "timestamp,ch,cell_id,pci,tac,csq,tmax,consec_fail,rx_pkt,drx,rsrp,rsrq,snr_db,rssi,srv,rat,deny,oper,lte_engineering_quality,at_telemetry_timeout,at_basic_probe,detailed_at_stage\r\n";
        for (const auto& m : App().document.metrics) {
            for (size_t column = 0; column < kMetricColumnCount; ++column) {
                // 程序生成的时间公式保留完整年月日；CH/Cell/RAT/OPER 等日志文本列会先
                // 中和 =,+,-,@ 前缀，避免导入电子表格后被当作公式执行。
                out += csv(metricCsvCellText(m, column));
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

void DoExportReport() {
    if (App().document.lines.empty()) {
        ShowModernNotice(L"没有可导出的报告", L"请先加载并分析日志。", ModernNoticeKind::Info);
        return;
    }
    wchar_t file[MAX_PATH] = L"diallog_report.md";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = App().hMain;
    ofn.lpstrFilter = L"Markdown (*.md)\0*.md\0文本文件 (*.txt)\0*.txt\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"md";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;

    auto mdSafe = [](std::wstring value) {
        for (size_t index = 0; index < value.size(); ++index) {
            if (value[index] == L'|') { value.insert(index, 1, L'\\'); ++index; }
            else if (value[index] == L'\r' || value[index] == L'\n') value[index] = L' ';
        }
        return value;
    };
    std::string out = "\xEF\xBB\xBF";
    auto add = [&](const std::wstring& line = L"") { out += WToU8(line); out += "\r\n"; };
    try {
        SYSTEMTIME now{}; GetLocalTime(&now);
        add(L"# dialLog 诊断报告"); add();
        add(FmtW(L"> 生成时间：%04d-%02d-%02d %02d:%02d:%02d  ·  本地离线分析",
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond));
        add(); add(L"## 分析摘要"); add();
        add(FmtW(L"- 平台：%s", U8ToW(App().document.platform.name).c_str()));
        add(FmtW(L"- 日志：筛选后 %d / 解析 %d 行，日志打开 %d 次，有证据的进程启动 %d 次",
                 static_cast<int>(App().document.filtered.size()), static_cast<int>(App().document.lines.size()),
                 static_cast<int>(App().document.audit.logOpened),
                 static_cast<int>(App().document.sessions.size())));
        if (!App().document.lines.empty()) {
            long long firstTime = App().document.lines.front().t;
            long long lastTime = firstTime;
            for (const auto& line : App().document.lines) {
                firstTime = std::min(firstTime, line.t);
                lastTime = std::max(lastTime, line.t);
            }
            add(FmtW(L"- 日志时间：%s → %s", U8ToW(fmtTime(firstTime, "FULL")).c_str(),
                     U8ToW(fmtTime(lastTime, "FULL")).c_str()));
            const ObservationStats observation = observationStats(App().document.lines);
            add(FmtW(L"- 观测覆盖：实际 %s / 日历跨度 %s（%.2f%%）；授时跳变切段 %d 处",
                     U8ToW(fmtDur(observation.observedSpan)).c_str(),
                     U8ToW(fmtDur(observation.calendarSpan)).c_str(), observation.coveragePercent,
                     static_cast<int>(observation.clockDiscontinuities)));
        }
        add(FmtW(L"- 断网：%d 次；解析遗漏：%d 行（%.2f%%）；NUL：%d 字节/%d 行",
                 static_cast<int>(App().document.outages.size()), static_cast<int>(App().document.audit.unparsed),
                 App().document.audit.unparsedRatio() * 100.0,
                 static_cast<int>(App().document.audit.nulBytes),
                 static_cast<int>(App().document.audit.nulLines)));

        if (App().document.sources.size() > 1) {
            add(); add(L"## 多日志对比"); add();
            add(L"| 来源 | 解析行 | 断网 | 指标样本 | 平均 RSRP |");
            add(L"|---|---:|---:|---:|---:|");
            for (const auto& source : App().document.sources)
                add(FmtW(L"| %s | %d | %d | %d | %s |", mdSafe(source.label).c_str(),
                         static_cast<int>(source.parsedLines), static_cast<int>(source.outages),
                         static_cast<int>(source.metricRows),
                         source.hasAverageRsrp ? FmtW(L"%d dBm", source.averageRsrp).c_str() : L"—"));
        }

        std::map<std::string, int> cells;
        long long snrTotal = 0; int snrCount = 0, snrMin = 100000, snrMax = -100000;
        for (const auto& metric : App().document.metrics) {
            if (!metric.cellId.empty()) ++cells[metric.cellId.str()];
            if (usesLteEngineeringReference(metric.rat) && metric.snr10 != 100000) {
                snrTotal += metric.snr10; ++snrCount;
                snrMin = std::min(snrMin, metric.snr10); snrMax = std::max(snrMax, metric.snr10);
            }
        }
        add(); add(L"## 信号与小区"); add();
        add(FmtW(L"- 指标样本：%d；识别小区：%d",
                 static_cast<int>(App().document.metrics.size()), static_cast<int>(cells.size())));
        add(L"- LTE 工程参考分档：仅适用于 RAT=LTE（RAT 缺失按兼容的 LTE 日志处理）；不是 3GPP 或运营商统一故障等级");
        add(L"- 综合等级：取 CSQ、RSRP、RSRQ、SNR 中最弱一项，是本软件的保守提示策略");
        add(L"- CSQ：0–9 较差 / 10–14 一般 / 15–19 良好 / 20–31 优秀（99 未知）");
        add(L"- RSRP：<-100 较差 / -100~-91 一般 / -90~-81 良好 / ≥-80 dBm 优秀");
        add(L"- RSRQ：<-20 较差 / -20~-16 一般 / -15~-11 良好 / ≥-10 dB 优秀");
        add(L"- SNR：≤0 较差 / 0.1~12.9 一般 / 13~19.9 良好 / ≥20 dB 优秀（模组上报值，不等同于所有制式的标准化 SINR）");
        if (snrCount)
            add(FmtW(L"- SNR：最低 %.1f / 平均 %.1f / 最高 %.1f dB（%d 个样本）",
                     snrMin / 10.0, snrTotal / (10.0 * snrCount), snrMax / 10.0, snrCount));
        if (!cells.empty()) {
            add(); add(L"| Cell ID | 样本数 |"); add(L"|---|---:|");
            std::vector<std::pair<std::string, int>> ranked(cells.begin(), cells.end());
            std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
                return left.second > right.second;
            });
            for (const auto& cell : ranked)
                add(FmtW(L"| %s | %d |", U8ToW(cell.first).c_str(), cell.second));
        }

        add(); add(L"## 诊断结论"); add();
        if (App().document.findings.empty()) add(L"未形成有证据支撑的结论。");
        for (size_t index = 0; index < App().document.findings.size(); ++index) {
            const auto& finding = App().document.findings[index];
            const wchar_t* level = finding.severity == 2 ? L"严重" : finding.severity == 1 ? L"告警" : L"信息";
            add(FmtW(L"### %d. [%s] %s", static_cast<int>(index + 1), level,
                     U8ToW(finding.title).c_str())); add();
            add(L"- 依据：" + U8ToW(finding.detail));
            add(L"- 建议：" + U8ToW(finding.advice));
            for (const auto& evidence : finding.ev)
                add(FmtW(L"  - 第 %d 行 · %s · %s", static_cast<int>(evidence.lineNo),
                         U8ToW(evidence.ts).c_str(), U8ToW(evidence.text).c_str()));
            add();
        }

        if (!EvidenceBookmarks().empty()) {
            add(L"## 人工证据书签"); add();
            for (const auto& bookmark : EvidenceBookmarks())
                add(FmtW(L"- 第 %d 行 · %s", static_cast<int>(bookmark.lineNo), bookmark.text.c_str()));
        }
    } catch (const std::bad_alloc&) {
        MessageBoxW(App().hMain, L"内存不足，无法生成诊断报告。", L"错误", MB_ICONERROR);
        return;
    }

    std::wstring writeErr;
    if (!WriteFileBytesAtomic(file, out, writeErr)) {
        MessageBoxW(App().hMain, writeErr.c_str(), L"导出失败", MB_ICONERROR);
        return;
    }
    SetWindowTextW(App().hStatus, (std::wstring(L"已导出诊断报告 ") + file).c_str());
    ShowModernNotice(L"诊断报告导出完成", file, ModernNoticeKind::Success, 6000);
}

void DoExportHtml() {
    if (App().document.lines.empty()) {
        ShowModernNotice(L"没有可导出的报告", L"请先加载并分析日志。", ModernNoticeKind::Info);
        return;
    }
    wchar_t file[MAX_PATH] = L"diallog_visual_report.html";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = App().hMain;
    ofn.lpstrFilter = L"HTML (*.html)\0*.html\0\0"; ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH; ofn.lpstrDefExt = L"html";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;

    BusyScope busy(L"正在生成单文件可视化报告…");
    auto escape = [](const std::string& value) {
        std::string output; output.reserve(value.size() + value.size() / 8);
        for (char ch : value) {
            switch (ch) {
            case '&': output += "&amp;"; break;
            case '<': output += "&lt;"; break;
            case '>': output += "&gt;"; break;
            case '"': output += "&quot;"; break;
            case '\'': output += "&#39;"; break;
            default: output += ch; break;
            }
        }
        return output;
    };
    auto oneDecimal = [](int value) {
        char text[32]{}; std::snprintf(text, sizeof(text), "%.1f", value / 10.0); return std::string(text);
    };

    std::string html;
    try {
        long long firstTime = App().document.lines.front().t, lastTime = firstTime;
        for (const LogLine& line : App().document.lines) {
            firstTime = std::min(firstTime, line.t); lastTime = std::max(lastTime, line.t);
        }
        SYSTEMTIME now{}; GetLocalTime(&now);
        char generated[64]{};
        std::snprintf(generated, sizeof(generated), "%04u-%02u-%02u %02u:%02u:%02u",
                      now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

        html = "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
               "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>dialLog 可视化诊断报告</title><style>"
               ":root{color-scheme:light dark;--bg:#f5f7fa;--card:#fff;--ink:#161a21;--muted:#596579;"
               "--line:#dce2ea;--accent:#1769d2;--bad:#c93c43;--warn:#9a6700;--soft:#eaf2ff}"
               "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.55 "
               "system-ui,-apple-system,'Segoe UI','Microsoft YaHei UI',sans-serif}.wrap{max-width:1240px;margin:auto;padding:32px}"
               "header,.card{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:24px;margin-bottom:18px}"
               "h1{margin:0 0 6px;font-size:30px}h2{font-size:20px;margin:0 0 16px}.muted{color:var(--muted)}"
               ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}.kpi{padding:16px;background:var(--soft);border-radius:12px}"
               ".kpi b{display:block;font-size:24px}.charts{display:grid;gap:16px}svg{width:100%;height:auto;background:var(--card);border-radius:10px}"
               "table{border-collapse:collapse;width:100%;display:block;overflow:auto}th,td{border-bottom:1px solid var(--line);padding:9px 12px;text-align:left;white-space:nowrap}"
               "th{position:sticky;top:0;background:var(--card)}.severity2{border-left:5px solid var(--bad)}.severity1{border-left:5px solid var(--warn)}"
               ".finding{padding:14px 16px;margin:10px 0;background:var(--soft);border-radius:10px}.finding h3{margin:0 0 8px}.evidence{font-family:ui-monospace,Consolas,monospace;white-space:normal}"
               "@media(prefers-color-scheme:dark){:root{--bg:#111419;--card:#1c2026;--ink:#f3f5f8;--muted:#aeb8c7;--line:#38414d;--accent:#65a7f3;--bad:#f06c73;--warn:#f0b945;--soft:#19324f}}"
               "@media(forced-colors:active){*{forced-color-adjust:auto}.kpi,.finding{border:1px solid CanvasText}}"
               "@media(max-width:700px){.wrap{padding:14px}header,.card{padding:16px;border-radius:10px}h1{font-size:24px}}"
               "@media print{body{background:white}.wrap{max-width:none;padding:0}.card,header{break-inside:avoid;box-shadow:none}}"
               "</style></head><body><main class=\"wrap\"><header><h1>dialLog 可视化诊断报告</h1><div class=\"muted\">生成于 " +
               std::string(generated) + " · 本地离线分析 · 单文件可归档</div></header>";

        html += "<section class=\"card\"><h2>分析摘要</h2><div class=\"grid\">";
        auto kpi = [&](const char* label, const std::string& value) {
            html += "<div class=\"kpi\"><span class=\"muted\">" + std::string(label) +
                    "</span><b>" + escape(value) + "</b></div>";
        };
        kpi("平台", App().document.platform.name);
        kpi("解析行", std::to_string(App().document.lines.size()));
        kpi("筛选后", std::to_string(App().document.filtered.size()));
        kpi("断网", std::to_string(App().document.outages.size()) + " 次");
        kpi("小区", std::to_string(App().document.cellAnalysis.cells.size()) + " 个");
        kpi("未识别", std::to_string(App().document.audit.unparsed) + " 行");
        kpi("文件损伤", std::to_string(App().document.audit.nulBytes) + " NUL 字节");
        const ObservationStats observation = observationStats(App().document.lines);
        html += "</div><p class=\"muted\">日志时间：" + escape(fmtTime(firstTime, "FULL")) + " → " +
                escape(fmtTime(lastTime, "FULL")) + "；实际观测 " +
                escape(fmtDur(observation.observedSpan)) + " / 日历跨度 " +
                escape(fmtDur(observation.calendarSpan)) + "（覆盖 " +
                oneDecimal(static_cast<int>(observation.coveragePercent * 10.0)) +
                "%）；授时跳变切段 " + std::to_string(observation.clockDiscontinuities) +
                " 处</p></section>";

        ChartSeries csq, rsrp, rsrq, snr;
        csq.reserve(App().document.metrics.size()); rsrp.reserve(App().document.metrics.size());
        rsrq.reserve(App().document.metrics.size()); snr.reserve(App().document.metrics.size());
        for (const MetricRow& metric : App().document.metrics) {
            if (!usesLteEngineeringReference(metric.rat)) continue;
            if (metric.csqVal >= 0) csq.push_back({metric.t, metric.csqVal});
            if (metric.rsrp < 0) rsrp.push_back({metric.t, metric.rsrp});
            if (metric.rsrq < 0) rsrq.push_back({metric.t, metric.rsrq});
            if (metric.snr10 != 100000) snr.push_back({metric.t, metric.snr10});
        }
        sortChartSeriesByTime(csq); sortChartSeriesByTime(rsrp);
        sortChartSeriesByTime(rsrq); sortChartSeriesByTime(snr);
        auto svg = [&](const char* title, const ChartSeries& input, int low, int high,
                       const char* color, bool scaled10,
                       std::initializer_list<std::pair<int, const char*>> guides) {
            if (input.empty()) return std::string("<p class=\"muted\">暂无 ") + title + " 样本。</p>";
            const long long t0 = input.front().first, t1 = input.back().first;
            ChartSeries points; downsampleChartSeries(input, t0, t1, 920, points);
            auto x = [&](long long time) { return 55 + int(double(time - t0) / std::max(1LL, t1 - t0) * 920); };
            auto y = [&](int value) {
                value = std::max(low, std::min(high, value));
                return 215 - int(double(value - low) / std::max(1, high - low) * 170);
            };
            std::string output = "<svg viewBox=\"0 0 1000 250\" role=\"img\" aria-label=\"" +
                escape(title) + " 趋势图\"><text x=\"55\" y=\"24\" fill=\"currentColor\" font-size=\"17\" font-weight=\"600\">" +
                escape(title) + "</text>";
            for (int grid = 0; grid < 4; ++grid) {
                const int value = high - (high - low) * grid / 3, gy = y(value);
                output += "<line x1=\"55\" y1=\"" + std::to_string(gy) + "\" x2=\"975\" y2=\"" +
                          std::to_string(gy) + "\" stroke=\"#9aa6b2\" opacity=\".3\"/><text x=\"5\" y=\"" +
                          std::to_string(gy + 4) + "\" fill=\"currentColor\" opacity=\".7\" font-size=\"12\">" +
                          (scaled10 ? oneDecimal(value) : std::to_string(value)) + "</text>";
            }
            for (const Outage& outage : App().document.outages) {
                const long long end = outage.recovered ? outage.end : t1;
                if (end < t0 || outage.start > t1) continue;
                const int left = x(std::max(t0, outage.start));
                const int right = x(std::min(t1, end));
                output += "<rect x=\"" + std::to_string(left) + "\" y=\"45\" width=\"" +
                          std::to_string(std::max(2, right - left)) +
                          "\" height=\"170\" fill=\"#c93c43\" opacity=\".12\"/>";
            }
            for (const auto& guide : guides) {
                const int guideY = y(guide.first);
                output += "<line x1=\"55\" y1=\"" + std::to_string(guideY) +
                          "\" x2=\"975\" y2=\"" + std::to_string(guideY) +
                          "\" stroke=\"currentColor\" opacity=\".38\"/>"
                          "<text x=\"970\" y=\"" + std::to_string(guideY - 4) +
                          "\" text-anchor=\"end\" fill=\"currentColor\" opacity=\".8\" font-size=\"11\">" +
                          escape(guide.second) + "</text>";
            }
            output += "<polyline fill=\"none\" stroke=\"" + std::string(color) +
                      "\" stroke-width=\"2\" points=\"";
            for (const ChartPoint& point : points)
                output += std::to_string(x(point.first)) + "," + std::to_string(y(point.second)) + " ";
            output += "\"/><text x=\"55\" y=\"238\" fill=\"currentColor\" opacity=\".7\" font-size=\"12\">" +
                      escape(fmtTime(t0, "FULL")) + "</text><text x=\"975\" y=\"238\" text-anchor=\"end\" fill=\"currentColor\" opacity=\".7\" font-size=\"12\">" +
                      escape(fmtTime(t1, "FULL")) + "</text></svg>";
            return output;
        };
        html += "<section class=\"card\"><h2>LTE 信号趋势与工程参考线</h2>"
                "<p class=\"muted\">仅适用于 RAT=LTE（RAT 缺失按兼容的 LTE 日志处理）；分档不是 3GPP 或运营商统一故障等级。"
                "综合等级取四项中的最弱项；SNR 为模组上报值，不等同于所有制式的标准化 SINR。</p>"
                "<div class=\"charts\">" +
                svg("CSQ 信号强度", csq, 0, 31, "#2a78d6", false,
                    {{kCsqFair, "≥10 一般"}, {kCsqGood, "≥15 良好"},
                     {kCsqExcellent, "≥20 优秀"}}) +
                svg("RSRP 覆盖质量 (dBm)", rsrp, -140, -40, "#6656c9", false,
                    {{kRsrpFair, "≥-100 一般"}, {kRsrpGood, "≥-90 良好"},
                     {kRsrpExcellent, "≥-80 优秀"}}) +
                svg("RSRQ 覆盖质量 (dB)", rsrq, -25, 0, "#eb6834", false,
                    {{kRsrqFair, "≥-20 一般"}, {kRsrqGood, "≥-15 良好"},
                     {kRsrqExcellent, "≥-10 优秀"}}) +
                svg("SNR 信噪比 (dB)", snr, -200, 300, "#1baf7a", true,
                    {{kSnrFair10, ">0 一般"}, {kSnrGood10, "≥13 良好"},
                     {kSnrExcellent10, "≥20 优秀"}}) +
                "</div></section>";

        html += "<section class=\"card\"><h2>小区质量画像</h2><table><thead><tr>"
                "<th>Cell ID</th><th>PCI</th><th>TAC</th><th>样本</th><th>占比%</th><th>观测驻留</th>"
                "<th>平均RSRP</th><th>最低RSRP</th><th>平均RSRQ</th><th>平均SNR</th><th>平均CSQ</th>"
                "<th>切入/切出</th><th>断网关联</th><th>判断</th></tr></thead><tbody>";
        for (const CellSummary& cell : App().document.cellAnalysis.cells) {
            html += "<tr>";
            for (std::size_t column = 0; column <= 10; ++column)
                html += "<td>" + escape(cellSummaryCellText(cell, column)) + "</td>";
            html += "<td>" + std::to_string(cell.switchesIn) + "/" + std::to_string(cell.switchesOut) +
                    "</td><td>" + std::to_string(cell.outageStarts) + "</td><td>" +
                    escape(cellSummaryCellText(cell, 14)) + "</td></tr>";
        }
        html += "</tbody></table><p class=\"muted\">断网关联：同一日志来源中，断网前 10 分钟内最近的小区样本；相关不等同因果。"
                "观测驻留只累计相邻且间隔不超过 10 分钟的同小区样本。</p></section>";

        html += "<section class=\"card\"><h2>断网记录</h2><table><thead><tr><th>#</th><th>开始</th><th>恢复</th><th>时长</th><th>证据行</th></tr></thead><tbody>";
        for (std::size_t i = 0; i < App().document.outages.size(); ++i) {
            const Outage& outage = App().document.outages[i];
            html += "<tr><td>" + std::to_string(i + 1) + "</td><td>" + escape(fmtTime(outage.start, "FULL")) +
                    "</td><td>" + (outage.recovered ? escape(fmtTime(outage.end, "FULL")) : "未恢复") +
                    "</td><td>" + (outage.recovered ? escape(fmtDur(outage.dur)) : "-") +
                    "</td><td>" + std::to_string(outage.startLine) + " → " +
                    (outage.endLine ? std::to_string(outage.endLine) : "-") + "</td></tr>";
        }
        html += "</tbody></table></section><section class=\"card\"><h2>诊断结论与证据</h2>";
        if (App().document.findings.empty()) html += "<p class=\"muted\">未形成有证据支撑的结论。</p>";
        for (const Finding& finding : App().document.findings) {
            html += "<article class=\"finding severity" + std::to_string(finding.severity) + "\"><h3>" +
                    escape(finding.title) + "</h3><p><b>依据：</b>" + escape(finding.detail) +
                    "</p><p><b>建议：</b>" + escape(finding.advice) + "</p>";
            for (const Evidence& evidence : finding.ev)
                html += "<p class=\"evidence\">第 " + std::to_string(evidence.lineNo) + " 行 · " +
                        escape(evidence.ts) + " · " + escape(evidence.text) + "</p>";
            html += "</article>";
        }
        if (!EvidenceBookmarks().empty()) {
            html += "<h2>人工证据书签</h2><ul>";
            for (const EvidenceBookmark& bookmark : EvidenceBookmarks())
                html += "<li class=\"evidence\">第 " + std::to_string(bookmark.lineNo) + " 行 · " +
                        escape(WToU8(bookmark.text)) + "</li>";
            html += "</ul>";
        }
        html += "</section><footer class=\"muted\">由 dialLog 本地离线生成；报告不加载外部字体、脚本或网络资源。</footer></main></body></html>";
    } catch (const std::bad_alloc&) {
        MessageBoxW(App().hMain, L"内存不足，无法生成 HTML 报告。", L"错误", MB_ICONERROR); return;
    }
    std::wstring writeError;
    if (!WriteFileBytesAtomic(file, html, writeError)) {
        MessageBoxW(App().hMain, writeError.c_str(), L"导出失败", MB_ICONERROR); return;
    }
    SetWindowTextW(App().hStatus, (std::wstring(L"已导出可视化报告 ") + file).c_str());
    ShowModernNotice(L"HTML 可视化报告导出完成", file, ModernNoticeKind::Success, 6000);
}


} // namespace dl
