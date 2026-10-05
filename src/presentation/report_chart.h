#pragma once
#include "chartmodel.h"
#include "log_types.h"
#include <string>
#include <vector>

namespace dl {
struct ReportChartGuide { int value; std::string label; std::string color; };
struct ReportChartOptions {
    std::string title, unit, color;
    int low = 0, high = 31;
    bool scaled10 = false, english = false;
    long long start = 0, end = 0;
    std::vector<ReportChartGuide> guides;
    ChartSeries comparison;
    bool relative=false, comparisonMode=false;
    long long originalStart=0,comparisonOriginalStart=0;
};
std::string reportChartScript(bool english);
struct ReportChartResult {
    std::string html;
    std::size_t samples = 0, drawn = 0, segments = 0;
};
// Offline SVG: keep all small series, otherwise keep first/last and bucket
// extrema. Original gaps break the path even if their edges were downsampled.
ReportChartResult renderReportChart(const ChartSeries& sortedInput,
                                   const std::vector<Outage>& outages,
                                   const ReportChartOptions& options);
}
