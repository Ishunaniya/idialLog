// load_controller.cpp — 日志来源加载、筛选刷新和导出协调
#include "load_controller.h"
#include "rssi_summary.h"
#include "source_workspace.h"
#include "incident_review.h"
#include "text_view.h"

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
#include "signal_chart.h"
#include "report_chart.h"
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
#include "text_catalog.h"
#include "findingmodel.h"
#include "version.h"

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

// 日志文件名/归档目录通常带 YYYYMMDD。仅提取已经存在于来源名称里的年份，供 Android
// MM-DD logcat 补全年；提取失败就保持未定序，绝不拿当前年份或 1970 猜测。
static int YearHintFromLabel(const std::wstring& label) {
    for (size_t i = 0; i + 8 <= label.size(); ++i) {
        int digits[8]{};
        bool allDigits = true;
        for (size_t k = 0; k < 8; ++k) {
            if (label[i + k] < L'0' || label[i + k] > L'9') { allDigits = false; break; }
            digits[k] = label[i + k] - L'0';
        }
        if (!allDigits) continue;
        const int year = digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 + digits[3];
        const int month = digits[4] * 10 + digits[5];
        const int day = digits[6] * 10 + digits[7];
        if (year >= 2000 && year <= 2999 && month >= 1 && month <= 12 && day >= 1 && day <= 31)
            return year;
    }
    return 0;
}

// 一份新输入默认必须以全量日志分析。筛选只属于当前文档，绝不让隐藏的旧条件影响新文档。
static void ResetFiltersForNewInput() {
    // WM_SETTEXT emits EN_CHANGE after the new document has been installed.
    // Use the shell's batched clear so its 450 ms refresh cannot close a new review.
    ClearMainFilters(false);
    if (App().hFilterToggle) {
        SetWindowTextW(App().hFilterToggle, UiText(TextId::ui_0000));
        SetModernButtonActive(App().hFilterToggle, false);
    }
}

} // namespace

static void PresentAnalysis(bool bad) {
    MarkAllPagesDirty();
    RenderPage(CurrentPage());

    std::wstring st = FmtW(UiText(TextId::ui_0001),
                           (int)App().document.filtered.size(), (int)App().document.lines.size(),
                           (int)App().document.outages.size(), (int)App().document.sessions.size());
    st += UiText(TextId::ui_0002) + U8ToW(GeneratedText(App().document.platform.name));
    const MetricRow* latestCell = nullptr;
    for (const MetricRow& metric : App().document.metrics)
        if (!metric.cellId.empty() && (!latestCell || metric.t > latestCell->t ||
            (metric.t == latestCell->t && metric.lineNo > latestCell->lineNo))) latestCell = &metric;
    if (latestCell)
        st += FmtW(UiText(TextId::ui_0003),
                   U8ToW(latestCell->cellId.str()).c_str(),
                   static_cast<int>(App().document.cellAnalysis.cells.size()));
    else
        st += UiText(TextId::ui_0004);
    if (App().document.audit.unparsed == 0 && App().document.audit.nulBytes == 0)
        st += UiText(TextId::ui_0005);
    else
        st += FmtW(UiText(TextId::ui_0006),
                   (int)App().document.audit.unparsed, App().document.audit.unparsedRatio() * 100.0,
                   (int)App().document.audit.nulBytes, (int)App().document.audit.nulLines);
    if (bad) st += UiText(TextId::ui_0007);
    if (HasAnalysisTimeFilter()) st += UiText(TextId::ui_0008);
    SetWindowTextW(App().hStatus, st.c_str());
    UpdateAnalysisTimeRangeControls();
    UpdateSourceControls();
    if (bad && !g_regexWasBad)
        ShowModernNotice(UiText(TextId::ui_0009), UiText(TextId::ui_0010),
                         ModernNoticeKind::Warning, 6000);
    g_regexWasBad = bad;
}

void RefreshPresentation() { PresentAnalysis(g_regexWasBad); }

void RefreshAll() {
    CloseSourceComparison(); CloseIncidentReview(); CloseSelectableText();
    if (App().document.lines.empty() && App().document.sources.empty()) { SetWindowTextW(App().hStatus, UiText(TextId::ui_0011)); return; }
    ResetVirtualTables();
    bool bad = false;
    App().document.filtered = applyFilterView(App().document.lines, WToU8(GetText(App().hTagBox)), WToU8(GetText(App().hGrepBox)),
                             WToU8(GetText(App().hSinceBox)), WToU8(GetText(App().hUntilBox)), &bad);
    App().document.restrictToTimeRange(App().document.filtered);
    if (App().document.sources.size() > 1 || !App().document.comparisons.empty())
        App().document.rebuildComparisons(App().document.filtered);
    if (App().document.sourceMode == DocumentState::SourceMode::Independent)
        App().document.restrictToSource(App().document.filtered, App().document.selectedSource);
    App().document.outages = collectOutages(App().document.filtered);
    App().document.metrics = buildMetrics(App().document.filtered);
    App().document.rebuildSignalObservations();
    RebuildMetricQuickFilterView();
    App().document.cellAnalysis = analyzeCells(App().document.filtered, App().document.metrics,
                                               App().document.outages);

    if (App().document.sources.size() > 1) {
        LogView sourceRows; sourceRows.reserve(App().document.lines.size());
        for (const auto& line : App().document.lines) sourceRows.push_back(&line);
        if (App().document.sourceMode == DocumentState::SourceMode::Independent)
            App().document.restrictToSource(sourceRows, App().document.selectedSource);
        App().document.platform = detectPlatform(sourceRows);
    }

    // 结论基于**筛选后**的视图,与各页展示保持一致
    App().document.findings = analyze(App().document.filtered, App().document.outages,
                                      App().document.metrics, App().document.platform,
                                      App().document.audit, &App().document.cellAnalysis);

    PresentAnalysis(bad);
}

bool HasAnalysisTimeFilter() {
    return App().document.timeRange.active || !GetText(App().hSinceBox).empty() || !GetText(App().hUntilBox).empty();
}

std::wstring AnalysisTimeRangeText() {
    const auto& range = App().document.timeRange;
    if (range.active)
        return UiText(TextId::ui_0012) + U8ToW(fmtTime(range.start, "FULL")) + L" → " + U8ToW(fmtTime(range.end, "FULL"));
    if (HasAnalysisTimeFilter())
        return UiText(TextId::ui_0472) + GetText(App().hSinceBox) + L" → " + GetText(App().hUntilBox);
    return UiText(TextId::ui_0013);
}

std::wstring AnalysisScopedText(const std::string& text) {
    const bool independent = App().document.sources.size() > 1 &&
        App().document.sourceMode == DocumentState::SourceMode::Independent;
    if (!HasAnalysisTimeFilter() && !independent) return U8ToW(GeneratedText(text));
    return U8ToW(findingScopedText(text,HasAnalysisTimeFilter()?TextId::ui_0015:TextId::ui_0539));
}

void UpdateAnalysisTimeRangeControls() {
    const auto& range = App().document.timeRange;
    std::wstring label;
    if (range.active) {
        label = UiText(TextId::ui_0016) + U8ToW(fmtTime(range.start, "FULL")) + UiText(TextId::ui_0473) + U8ToW(fmtTime(range.end, "FULL"));
    } else if (HasAnalysisTimeFilter()) {
        label = AnalysisTimeRangeText() + UiText(TextId::ui_0474);
    } else {
        label = UiText(TextId::ui_0017);
    }
    if (App().hTimeRangeLabel) SetWindowTextW(App().hTimeRangeLabel, label.c_str());
    if (App().hTimeRangeReset) EnableWindow(App().hTimeRangeReset,
        !App().document.lines.empty() && HasAnalysisTimeFilter());
    if (App().hFilterToggle) SendMessageW(App().hMain, WM_APP_SHELL_LAYOUT, 0, 0);
}

void SelectAnalysisTimeRange(long long start, long long end) {
    if (App().document.lines.empty() || start == end) return;
    if (PageDetailVisible()) ClosePageDetail();
    App().document.selectTimeRange(start, end);
    RefreshAll();
}

void RestoreAnalysisTimeRange() {
    App().document.timeRange = DocumentState::TimeRange{};
    for (HWND edit : {App().hSinceBox, App().hUntilBox})
        if (edit) SetWindowTextW(edit, L"");
    RefreshAll();
    UpdateAnalysisTimeRangeControls();
}

// 载入的公共尾段:移动接管原始行,解析后立即释放,不让 raw 与后续分析结果长期共存。
static void LoadRawLines(std::vector<std::string> raw, const std::wstring& srcLabel,
                         const std::vector<size_t>& fileBoundaries = {}) {
    // raw 已成功读取/合并后才卸载旧日志；读取失败仍保留当前分析。释放旧分析结果后再
    // parse,避免“大旧日志模型 + 新日志原文 + 新分析模型”三者在切换期间重叠。
    ResetFiltersForNewInput();
    ReleaseLoadedData();
    parseLines(raw, App().document.lines, App().document.sessions, &App().document.audit, fileBoundaries);
    releaseVector(raw);
    SourceSummary pastedSource; pastedSource.label = srcLabel; pastedSource.last = App().document.lines.size();
    App().document.sources.push_back(std::move(pastedSource));
    App().document.platform = detectPlatform(App().document.lines);   // 平台识别用全量行(不受筛选影响)
    std::wstring lbl = srcLabel;
    lbl += FmtW(UiText(TextId::ui_0018), (int)App().document.lines.size(), (int)App().document.sessions.size(),
                U8ToW(GeneratedText(App().document.platform.name)).c_str());
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
    text += L"• " + FileNameOf(path) + L"：" + (reason.empty() ? UiText(TextId::ui_0019) : reason);
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
                    readErr = UiText(TextId::ui_0020);
                }
                if (loaded && source.textBytes > kMaxBatchTextBytes - batchTextBytes) {
                    loaded = false;
                    readErr = UiText(TextId::ui_0021);
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
                    readErr = UiText(TextId::ui_0022);
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
            readErr = UiText(TextId::ui_0023);
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
        result->error = sourceProblems.empty() ? UiText(TextId::ui_0024) : sourceProblems;
        DeliverLoadResult(request->owner, result); return 0;
    }
    if (!sourceProblems.empty()) result->warning = UiText(TextId::ui_0025) + sourceProblems;
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
        std::wstring msg = FmtW(UiText(TextId::ui_0480),
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
        excludedNote = FmtW(UiText(TextId::ui_0030), (int)mix.unsyncedIdx.size());
    }
    if (sources.empty()) {
        result->error = UiText(TextId::ui_0031);
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
    struct SourceRange { std::wstring label; size_t first = 0, last = 0, rawLineOffset = 0; };
    std::vector<SourceRange> ranges;
    std::size_t rawLineCount = 0;
    try {
        StreamingLogParser parser(result->document.lines, result->document.sessions, reserveHint);
        for (size_t orderIndex = 0; orderIndex < ord.size(); ++orderIndex) {
            if (LoadCancelled()) { parseOk = false; parseErr = UiText(TextId::ui_0032); break; }
            const size_t i = ord[orderIndex];
            LoadSource& source = sources[i];
            parser.beginFile(YearHintFromLabel(source.label));
            SourceRange range{source.label, result->document.lines.size(), result->document.lines.size()};
            range.rawLineOffset = rawLineCount;
            if (source.streamPlain) {
                WorkerProgress progress{
                    request->owner,
                    35 + static_cast<int>(orderIndex * 48 / std::max<size_t>(1, ord.size())),
                    static_cast<int>(48 / std::max<size_t>(1, ord.size())), 2, -1};
                if (!ReadPlainLines(
                        source.path, std::numeric_limits<size_t>::max(),
                        [&](std::string line) { ++rawLineCount; parser.pushLine(std::move(line)); },
                        nullptr, parseErr, &progress, ObserveRead)) {
                    parseOk = false;
                    break;
                }
            } else {
                for (size_t lineIndex = 0; lineIndex < source.lines.size(); ++lineIndex) {
                    if ((lineIndex & 4095U) == 0 && LoadCancelled()) {
                        parseOk = false; parseErr = UiText(TextId::ui_0032); break;
                    }
                    ++rawLineCount; parser.pushLine(std::move(source.lines[lineIndex]));
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
        parseErr = UiText(TextId::ui_0033);
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
    for (const auto& range : ranges) {
        SourceSummary summary; summary.label = range.label;
        summary.first = range.first; summary.last = range.last;
        summary.rawLineOffset = range.rawLineOffset;
        result->document.sources.push_back(std::move(summary));
    }
    if (ranges.size() > 1) result->document.rebuildComparisons(result->document.filtered);
    result->document.restrictToSource(result->document.filtered, 0);
    result->document.outages = collectOutages(result->document.filtered);
    result->document.metrics = buildMetrics(result->document.filtered);
    result->document.rebuildSignalObservations();
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
    for (size_t sourceIndex = 0; sourceIndex < ranges.size(); ++sourceIndex) {
        const auto& range = ranges[sourceIndex];
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
        SourceSummary& summary = result->document.sources[sourceIndex];
        summary.parsedLines = view.size();
        summary.metricRows = metrics.size();
        summary.outages = outages.size();
        long long rsrpTotal = 0; size_t rsrpCount = 0;
        for (const auto& metric : metrics) if (metric.rsrp < 0) { rsrpTotal += metric.rsrp; ++rsrpCount; }
        if (rsrpCount) {
            summary.averageRsrp = static_cast<int>(rsrpTotal / static_cast<long long>(rsrpCount));
            summary.hasAverageRsrp = true;
        }
    }

    std::wstring lbl;
    if (sources.size() == 1) {
        lbl = sources[0].label + excludedNote;   // 排除后只剩一份时,仍要带上排除提示
    } else {
        // 多文件必须让人看见到底按什么顺序拼的 —— 否则重排是隐形的,出了错也无从察觉
        lbl = FmtW(UiText(TextId::ui_0034), (int)sources.size());
        lbl += reordered ? UiText(TextId::ui_0035) : UiText(TextId::ui_0036);
        if (noTs > 0) lbl += FmtW(UiText(TextId::ui_0037), noTs);
        lbl += excludedNote;
    }
    lbl += FmtW(UiText(TextId::ui_0018), (int)result->document.lines.size(),
                (int)result->document.sessions.size(), U8ToW(result->document.platform.name).c_str());
    result->label = std::move(lbl);
    result->success = true;
    PostLoadProgress(request->owner, 100, 3);

    DeliverLoadResult(request->owner, result);
    } catch (const std::bad_alloc&) {
        result->error = UiText(TextId::ui_0038);
        DeliverLoadResult(request->owner, result);
    } catch (...) {
        result->error = UiText(TextId::ui_0039);
        DeliverLoadResult(request->owner, result);
    }
    return 0;
}

void LoadFiles(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    if (LoadInProgress()) {
        ShowModernNotice(UiText(TextId::ui_0040), UiText(TextId::ui_0041),
                         ModernNoticeKind::Info);
        return;
    }
    std::unique_ptr<LoadRequest> request(new (std::nothrow) LoadRequest);
    if (!request) {
        MessageBoxW(App().hMain, UiText(TextId::ui_0042), UiText(TextId::ui_0043), MB_ICONERROR);
        return;
    }
    request->owner = App().hMain;
    request->paths = paths;
    // 拖入/打开另一份日志就是新的分析会话：后台从全量数据构建。但直到加载成功前，
    // 不碰当前界面与筛选，确保取消或失败时旧文档的显示状态保持不变。
    request->tag.clear();
    request->grep.clear();
    request->since.clear();
    request->until.clear();
    InterlockedExchange(&g_loadCancel, 0);
    InterlockedExchange(&g_loadShutdown, 0);
    InterlockedExchange(&g_loadActive, 1);
    g_loadThread = CreateThread(nullptr, 0, LoadWorker, request.get(), 0, nullptr);
    if (!g_loadThread) {
        InterlockedExchange(&g_loadActive, 0);
        MessageBoxW(App().hMain, UiText(TextId::ui_0044), UiText(TextId::ui_0043), MB_ICONERROR);
        return;
    }
    request.release();
    SetWindowTextW(App().hCloseLog, UiText(TextId::ui_0045));
    SetShellBusy(true, UiText(TextId::ui_0046));
    SetShellProgress(0);
}

bool LoadInProgress() {
    return InterlockedCompareExchange(&g_loadActive, 0, 0) != 0;
}

void CancelLoad() {
    if (!LoadInProgress()) return;
    InterlockedExchange(&g_loadCancel, 1);
    SetWindowTextW(App().hCloseLog, UiText(TextId::ui_0047));
    EnableWindow(App().hCloseLog, FALSE);
    SetShellProgress(0, UiText(TextId::ui_0048));
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
        const wchar_t* name = stage == 0 ? UiText(TextId::ui_0049) : stage == 1 ? UiText(TextId::ui_0050) :
                              stage == 2 ? UiText(TextId::ui_0051) : UiText(TextId::ui_0052);
        const std::wstring text = FmtW(UiText(TextId::ui_0053), name, percent);
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
    SetWindowTextW(App().hCloseLog, UiText(TextId::ui_0054));
    EnableWindow(App().hCloseLog, TRUE);

    if (!result) {
        MessageBoxW(App().hMain, UiText(TextId::ui_0055), UiText(TextId::ui_0056), MB_ICONERROR);
        return true;
    }
    if (result->cancelled || cancelRequested) {
        ShowModernNotice(UiText(TextId::ui_0057), UiText(TextId::ui_0058), ModernNoticeKind::Info);
        return true;
    }
    if (!result->success) {
        MessageBoxW(App().hMain,
                    (result->error.empty() ? UiText(TextId::ui_0059) : result->error.c_str()),
                    UiText(TextId::ui_0056), MB_ICONERROR);
        return true;
    }

    ResetVirtualTables();
    ReleaseLoadedData();
    App().document.swap(result->document);
    ResetFiltersForNewInput();
    RebuildMetricQuickFilterView();
    SetWindowTextW(App().hFileLbl, result->label.c_str());
    if (App().document.sources.size() > 1) RefreshAll();
    else PresentAnalysis(result->badRegex);
    RememberRecentFiles(result->openedPaths);
    RefreshNavigation();
    if (!result->warning.empty())
        MessageBoxW(App().hMain, result->warning.c_str(), UiText(TextId::ui_0060), MB_ICONWARNING | MB_OK);
    else
        ShowModernNotice(UiText(TextId::ui_0061), result->label.c_str(), ModernNoticeKind::Success, 5000);
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
        ShowModernNotice(UiText(TextId::ui_0040), UiText(TextId::ui_0062), ModernNoticeKind::Info);
        return;
    }
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        ShowModernNotice(UiText(TextId::ui_0063),
                         UiText(TextId::ui_0064),
                         ModernNoticeKind::Info);
        return;
    }
    if (!OpenClipboard(App().hMain)) {
        ShowModernNotice(UiText(TextId::ui_0065), UiText(TextId::ui_0066),
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
        MessageBoxW(App().hMain, UiText(TextId::ui_0067), UiText(TextId::ui_0043), MB_ICONERROR);
        return;
    }
    if (w.empty()) {
        ShowModernNotice(UiText(TextId::ui_0068), UiText(TextId::ui_0069),
                         ModernNoticeKind::Info);
        return;
    }
    // UTF-16 转 UTF-8 最坏每个码点 4 字节,在转换前即执行与文件相同的 512MiB 上限。
    if (w.size() > (size_t)kMaxInputBytes / 4) {
        MessageBoxW(App().hMain, UiText(TextId::ui_0070), UiText(TextId::ui_0071), MB_ICONWARNING);
        return;
    }

    BusyScope busy(UiText(TextId::ui_0072));

    // 按行切分(兼容 \r\n / \n / \r 三种换行);与文件读取共用纯 C++ 实现。
    std::vector<std::string> raw;
    try {
        std::string u8 = WToU8(w);
        dl::splitTextLines(std::move(u8), raw);
    } catch (const std::bad_alloc&) {
        MessageBoxW(App().hMain, UiText(TextId::ui_0073), UiText(TextId::ui_0043), MB_ICONERROR);
        return;
    }

    std::wstring pasteLabel = FmtW(UiText(TextId::ui_0074), (int)raw.size());
    LoadRawLines(std::move(raw), pasteLabel);

    // 粘贴的往往是片段,若一行都没认出来,直接把原因摆出来(而不是让用户对着空界面猜)
    if (App().document.lines.empty()) {
        ShowModernNotice(UiText(TextId::ui_0075),
                         UiText(TextId::ui_0076),
                         ModernNoticeKind::Warning, 8000);
    }
}

void DoOpen() {
    if (LoadInProgress()) {
        ShowModernNotice(UiText(TextId::ui_0040), UiText(TextId::ui_0062), ModernNoticeKind::Info);
        return;
    }
    std::vector<wchar_t> buf(32768, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = App().hMain;
    ofn.lpstrFilter = UiText(TextId::ui_0077);
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.lpstrTitle = UiText(TextId::ui_0078);
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
        ShowModernNotice(UiText(TextId::ui_0079), UiText(TextId::ui_0080),
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
                out += csv(column >= 18 ? GeneratedText(metricCsvCellText(m, column)) : metricCsvCellText(m, column));
                out += (column + 1 == kMetricColumnCount) ? "\r\n" : ",";
            }
        }
    } catch (const std::bad_alloc&) {
        MessageBoxW(App().hMain, UiText(TextId::ui_0081), UiText(TextId::ui_0043), MB_ICONERROR);
        return;
    }
    std::wstring writeErr;
    if (!WriteFileBytesAtomic(file, out, writeErr)) {
        MessageBoxW(App().hMain, writeErr.c_str(), UiText(TextId::ui_0082), MB_ICONERROR);
        return;
    }
    SetWindowTextW(App().hStatus, (std::wstring(UiText(TextId::ui_0083)) + file).c_str());
    ShowModernNotice(UiText(TextId::ui_0084), file, ModernNoticeKind::Success, 6000);
}

void DoExportReport() {
    if (App().document.lines.empty()) {
        ShowModernNotice(UiText(TextId::ui_0085), UiText(TextId::ui_0086), ModernNoticeKind::Info);
        return;
    }
    wchar_t file[MAX_PATH] = L"diallog_report.md";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = App().hMain;
    ofn.lpstrFilter = UiText(TextId::ui_0087);
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
        add(UiText(TextId::ui_0088)); add();
        add(L"- dialLog v" DL_VER_WSTR);
        add(FmtW(UiText(TextId::ui_0089),
                 now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond));
        add(); add(UiText(TextId::ui_0090)); add();
        add(FmtW(UiText(TextId::ui_0091), U8ToW(GeneratedText(App().document.platform.name)).c_str()));
        add(L"- " + AnalysisTimeRangeText());
        add(L"- " + AnalysisSourceText());
        if (HasAnalysisTimeFilter())
            add(UiText(TextId::ui_0092));
        add(FmtW(UiText(TextId::ui_0093),
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
            add(FmtW(UiText(TextId::ui_0094), U8ToW(fmtTime(firstTime, "FULL")).c_str(),
                     U8ToW(fmtTime(lastTime, "FULL")).c_str()));
            const ObservationStats observation = observationStats(App().document.lines);
            add(FmtW(UiText(TextId::ui_0475),
                     U8ToW(fmtDur(observation.observedSpan)).c_str(),
                     U8ToW(fmtDur(observation.calendarSpan)).c_str(), observation.coveragePercent,
                     static_cast<int>(observation.clockDiscontinuities)));
        }
        add(FmtW(UiText(TextId::ui_0095),
                 static_cast<int>(App().document.outages.size()), static_cast<int>(App().document.audit.unparsed),
                 App().document.audit.unparsedRatio() * 100.0,
                 static_cast<int>(App().document.audit.nulBytes),
                 static_cast<int>(App().document.audit.nulLines)));

        const AvailabilityStats availability = availabilityStats(App().document.filtered, App().document.outages);
        const DataCallStats calls = collectDataCallStats(App().document.filtered);
        if (availability.evidenceLimited()) {
            add(UiText(TextId::ui_0096));
            add(L"- " + U8ToW(GeneratedText(availabilityEvidenceNote(availability))));
        }
        if (availability.runtimeValid())
            add(FmtW(UiText(TextId::ui_0097),
                     availability.evidenceLimited() ? UiText(TextId::ui_0098) : L"",
                     availability.runtimePercent()));
        if (availability.fullValid())
            add(FmtW(UiText(TextId::ui_0099),
                     availability.evidenceLimited() ? UiText(TextId::ui_0098) : L"",
                     availability.fullPercent()));
        add(FmtW(UiText(TextId::ui_0100),
                 (int)calls.disconnected, (int)calls.appStop, (int)calls.sdkUrc, (int)calls.legacy));

        if (App().document.sources.size() > 1) {
            add(); add(UiText(TextId::ui_0101)); add();
            add(UiText(TextId::ui_0102));
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
        add(); add(UiText(TextId::ui_0103)); add();
        add(FmtW(UiText(TextId::ui_0104),
                 static_cast<int>(App().document.metrics.size()), static_cast<int>(cells.size())));
        add(L"- " + U8ToW(rssiSummaryText(App().document.rssi)));
        add(L"- " + std::wstring(UiText(TextId::rssi_note)));
        add(UiText(TextId::ui_0105));
        add(UiText(TextId::ui_0106));
        add(UiText(TextId::ui_0107));
        add(UiText(TextId::ui_0108));
        add(UiText(TextId::ui_0109));
        add(UiText(TextId::ui_0110));
        if (snrCount)
            add(FmtW(UiText(TextId::ui_0111),
                     snrMin / 10.0, snrTotal / (10.0 * snrCount), snrMax / 10.0, snrCount));
        if (!cells.empty()) {
            add(); add(UiText(TextId::ui_0112)); add(L"|---|---:|---|---:|");
            std::map<std::string, const CellSummary*> observedCells;
            for (const auto& cell : App().document.cellAnalysis.cells) observedCells.emplace(cell.cellId, &cell);
            std::vector<std::pair<std::string, int>> ranked(cells.begin(), cells.end());
            std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
                return left.second > right.second;
            });
            for (const auto& cell : ranked) {
                const auto found = observedCells.find(cell.first);
                const RssiObservation rssi = found == observedCells.end() ? RssiObservation{} : found->second->rssi;
                add(L"| " + U8ToW(cell.first) + L" | " + std::to_wstring(cell.second) + L" | " +
                    U8ToW(rssiRangeText(rssi)) + L" | " + std::to_wstring(rssi.samples) + L" |");
            }
        }

        add(); add(UiText(TextId::ui_0113)); add();
        if (App().document.findings.empty()) add(UiText(TextId::ui_0114));
        for (size_t index = 0; index < App().document.findings.size(); ++index) {
            const auto& finding = App().document.findings[index];
            const wchar_t* level = finding.severity == 2 ? UiText(TextId::ui_0115) : finding.severity == 1 ? UiText(TextId::ui_0116) : UiText(TextId::ui_0117);
            add(FmtW(L"### %d. [%s] %s", static_cast<int>(index + 1), level,
                     AnalysisScopedText(finding.title).c_str())); add();
            add(UiText(TextId::ui_0118) + AnalysisScopedText(finding.detail));
            add(UiText(TextId::ui_0119) + U8ToW(GeneratedText(finding.advice)));
            for (const auto& evidence : finding.ev)
                add(FmtW(UiText(TextId::ui_0120), static_cast<int>(evidence.lineNo),
                         U8ToW(evidence.ts).c_str(), U8ToW(evidence.text).c_str()));
            add();
        }

        if (!EvidenceBookmarks().empty()) {
            add(UiText(TextId::ui_0121)); add();
            for (const auto& bookmark : EvidenceBookmarks())
                add(FmtW(UiText(TextId::ui_0122), static_cast<int>(bookmark.lineNo), bookmark.text.c_str()));
        }
    } catch (const std::bad_alloc&) {
        MessageBoxW(App().hMain, UiText(TextId::ui_0123), UiText(TextId::ui_0043), MB_ICONERROR);
        return;
    }

    std::wstring writeErr;
    if (!WriteFileBytesAtomic(file, out, writeErr)) {
        MessageBoxW(App().hMain, writeErr.c_str(), UiText(TextId::ui_0082), MB_ICONERROR);
        return;
    }
    SetWindowTextW(App().hStatus, (std::wstring(UiText(TextId::ui_0124)) + file).c_str());
    ShowModernNotice(UiText(TextId::ui_0125), file, ModernNoticeKind::Success, 6000);
}

void DoExportHtml() {
    if (App().document.lines.empty()) {
        ShowModernNotice(UiText(TextId::ui_0085), UiText(TextId::ui_0086), ModernNoticeKind::Info);
        return;
    }
    wchar_t file[MAX_PATH] = L"diallog_visual_report.html";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = App().hMain;
    ofn.lpstrFilter = L"HTML (*.html)\0*.html\0\0"; ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH; ofn.lpstrDefExt = L"html";
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return;

    BusyScope busy(UiText(TextId::ui_0126));
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

        html = UiText8(TextId::ui_0489) +
               std::string(generated) + " · dialLog v" DL_VER_STR + UiText8(TextId::ui_0490);

        html += UiText8(TextId::ui_0491);
        auto kpi = [&](const char* label, const std::string& value) {
            html += "<div class=\"kpi\"><span class=\"muted\">" + std::string(label) +
                    "</span><b>" + escape(value) + "</b></div>";
        };
        kpi(UiText8(TextId::ui_0492), GeneratedText(App().document.platform.name));
        kpi(UiText8(TextId::ui_0493), std::to_string(App().document.lines.size()));
        kpi(UiText8(TextId::ui_0494), std::to_string(App().document.filtered.size()));
        kpi(UiText8(TextId::ui_0485), std::to_string(App().document.outages.size()) + UiText8(TextId::ui_0495));
        kpi(UiText8(TextId::ui_0496), std::to_string(App().document.cellAnalysis.cells.size()) + UiText8(TextId::ui_0497));
        kpi(UiText8(TextId::ui_0498), std::to_string(App().document.audit.unparsed) + UiText8(TextId::ui_0499));
        kpi(UiText8(TextId::ui_0500), std::to_string(App().document.audit.nulBytes) + UiText8(TextId::ui_0501));
        kpi(UiText8(TextId::ui_0502), WToU8(AnalysisTimeRangeText()));
        kpi(UiText8(TextId::ui_0244), WToU8(AnalysisSourceText()));
        const AvailabilityStats availability = availabilityStats(App().document.filtered, App().document.outages);
        const DataCallStats calls = collectDataCallStats(App().document.filtered);
        if (availability.evidenceLimited()) kpi(UiText8(TextId::ui_0503), UiText8(TextId::ui_0380));
        if (availability.runtimeValid())
            kpi(availability.evidenceLimited() ? UiText8(TextId::ui_0504) : UiText8(TextId::ui_0253),
                WToU8(FmtW(L"%.3f%%", availability.runtimePercent())));
        if (availability.fullValid())
            kpi(availability.evidenceLimited() ? UiText8(TextId::ui_0505) : UiText8(TextId::ui_0506),
                WToU8(FmtW(L"%.3f%%", availability.fullPercent())));
        kpi(UiText8(TextId::ui_0507), std::to_string(calls.disconnected) + UiText8(TextId::ui_0508) +
            std::to_string(calls.appStop) + " / SDK_URC " + std::to_string(calls.sdkUrc) +
            UiText8(TextId::ui_0509) + std::to_string(calls.legacy) + "）");
        const ObservationStats observation = observationStats(App().document.lines);
        html += UiText8(TextId::ui_0510) + escape(fmtTime(firstTime, "FULL")) + " → " +
                escape(fmtTime(lastTime, "FULL")) + UiText8(TextId::ui_0511) +
                escape(fmtDur(observation.observedSpan)) + UiText8(TextId::ui_0512) +
                escape(fmtDur(observation.calendarSpan)) + UiText8(TextId::ui_0513) +
                oneDecimal(static_cast<int>(observation.coveragePercent * 10.0)) +
                UiText8(TextId::ui_0514) + std::to_string(observation.clockDiscontinuities) +
                UiText8(TextId::ui_0515) + (availability.evidenceLimited()
                    ? "<p>" + escape(GeneratedText(availabilityEvidenceNote(availability))) + "</p>" : "") +
                (HasAnalysisTimeFilter() ? UiText8(TextId::ui_0516) : "") + "</section>";

        const auto signals=reportedSignalSeries(App().document.metricView);
        long long signalStart=0,signalEnd=0;bool haveSignalRange=false;
        for(const auto* series:{&signals.csq,&signals.rsrp,&signals.rsrq,&signals.snr10,&signals.rssi}) {
            if(series->empty())continue;
            if(!haveSignalRange){signalStart=series->front().first;signalEnd=series->back().first;haveSignalRange=true;}
            else{signalStart=std::min(signalStart,series->front().first);signalEnd=std::max(signalEnd,series->back().first);}
        }
        if(App().document.timeRange.active){signalStart=App().document.timeRange.start;signalEnd=App().document.timeRange.end;}
        auto chart=[&](const char* title,const char* unit,const ChartSeries& input,int low,int high,
                       const char* color,bool scaled10,std::vector<ReportChartGuide> guides) {
            ReportChartOptions options;options.title=title;options.unit=unit;options.color=color;
            options.low=low;options.high=high;options.scaled10=scaled10;options.english=IsEnglish();
            options.start=signalStart;options.end=signalEnd;options.guides=std::move(guides);
            return renderReportChart(input,App().document.outages,options).html;
        };
        const auto rssiBounds=rssiDisplayBounds(signals.rssi);
        html += UiText8(TextId::ui_0520) +
                chart(UiText8(TextId::ui_0521),"",signals.csq,0,31,"#2a78d6",false,
                    {{kCsqFair,UiText8(TextId::ui_0341),"#9a6700"},{kCsqGood,UiText8(TextId::ui_0342),"#1769d2"},
                     {kCsqExcellent,UiText8(TextId::ui_0343),"#14805e"}}) +
                chart(UiText8(TextId::ui_0522),"dBm",signals.rsrp,-140,-40,"#8876df",false,
                    {{kRsrpFair,UiText8(TextId::ui_0344),"#9a6700"},{kRsrpGood,UiText8(TextId::ui_0345),"#1769d2"},
                     {kRsrpExcellent,UiText8(TextId::ui_0346),"#14805e"}}) +
                chart(UiText8(TextId::ui_0523),"dB",signals.rsrq,-25,0,"#8876df",false,
                    {{kRsrqFair,UiText8(TextId::ui_0347),"#9a6700"},{kRsrqGood,UiText8(TextId::ui_0348),"#1769d2"},
                     {kRsrqExcellent,UiText8(TextId::ui_0349),"#14805e"}}) +
                chart(UiText8(TextId::ui_0524),"dB",signals.snr10,-200,300,"#1baf7a",true,
                    {{kSnrFair10,UiText8(TextId::ui_0350),"#9a6700"},{kSnrGood10,UiText8(TextId::ui_0351),"#1769d2"},
                     {kSnrExcellent10,UiText8(TextId::ui_0343),"#14805e"}}) +
                chart(UiText8(TextId::ui_0540),"dBm",signals.rssi,rssiBounds.first,rssiBounds.second,"#eb6834",false,{}) +
                "</div></section>";

        html += "<p>" + escape(rssiSummaryText(observedRssi(App().document.metricView))) + "</p>";
        html += UiText8(TextId::ui_0525);
        for (const CellSummary& cell : App().document.cellAnalysis.cells) {
            html += "<tr>";
            for (std::size_t column = 0; column <= 10; ++column)
                html += "<td>" + escape(cellSummaryCellText(cell, column)) + "</td>";
            html += "<td>" + std::to_string(cell.switchesIn) + "/" + std::to_string(cell.switchesOut) +
                    "</td><td>" + std::to_string(cell.outageStarts) + "</td><td>" +
                    escape(GeneratedText(cellSummaryCellText(cell, 14))) + "</td><td>" +
                    escape(cellSummaryCellText(cell, 15)) + "</td><td>" +
                    cellSummaryCellText(cell, 16) + "</td></tr>";
        }
        html += UiText8(TextId::ui_0526);

        html += UiText8(TextId::ui_0527);
        for (std::size_t i = 0; i < App().document.outages.size(); ++i) {
            const Outage& outage = App().document.outages[i];
            html += "<tr><td>" + std::to_string(i + 1) + "</td><td>" + escape(fmtTime(outage.start, "FULL")) +
                    "</td><td>" + (outage.recovered ? escape(fmtTime(outage.end, "FULL")) :
                                  HasAnalysisTimeFilter() ? UiText8(TextId::ui_0251) : UiText8(TextId::ui_0279)) +
                    "</td><td>" + (outage.recovered ? escape(fmtDur(outage.dur)) : "-") +
                    "</td><td>" + std::to_string(outage.startLine) + " → " +
                    (outage.endLine ? std::to_string(outage.endLine) : "-") + "</td></tr>";
        }
        html += UiText8(TextId::ui_0528);
        if (App().document.findings.empty()) html += UiText8(TextId::ui_0529);
        for (const Finding& finding : App().document.findings) {
            html += "<article class=\"finding severity" + std::to_string(finding.severity) + "\"><h3>" +
                    escape(WToU8(AnalysisScopedText(finding.title))) + UiText8(TextId::ui_0530) + escape(WToU8(AnalysisScopedText(finding.detail))) +
                    UiText8(TextId::ui_0531) + escape(GeneratedText(finding.advice)) + "</p>";
            for (const Evidence& evidence : finding.ev)
                html += UiText8(TextId::ui_0532) + std::to_string(evidence.lineNo) + UiText8(TextId::ui_0533) +
                        escape(evidence.ts) + " · " + escape(evidence.text) + "</p>";
            html += "</article>";
        }
        if (!EvidenceBookmarks().empty()) {
            html += UiText8(TextId::ui_0534);
            for (const EvidenceBookmark& bookmark : EvidenceBookmarks())
                html += UiText8(TextId::ui_0535) + std::to_string(bookmark.lineNo) + UiText8(TextId::ui_0533) +
                        escape(WToU8(bookmark.text)) + "</li>";
            html += "</ul>";
        }
        html += UiText8(TextId::ui_0536);
    } catch (const std::bad_alloc&) {
        MessageBoxW(App().hMain, UiText(TextId::ui_0127), UiText(TextId::ui_0043), MB_ICONERROR); return;
    }
    if (IsEnglish()) {
        const auto language=html.find("lang=\"zh-CN\"");
        if (language!=std::string::npos) html.replace(language,12,"lang=\"en-US\"");
    }
    std::wstring writeError;
    if (!WriteFileBytesAtomic(file, html, writeError)) {
        MessageBoxW(App().hMain, writeError.c_str(), UiText(TextId::ui_0082), MB_ICONERROR); return;
    }
    SetWindowTextW(App().hStatus, (std::wstring(UiText(TextId::ui_0128)) + file).c_str());
    ShowModernNotice(UiText(TextId::ui_0129), file, ModernNoticeKind::Success, 6000);
}


} // namespace dl
