// log_time.cpp — 与时区无关的日志时间转换与展示
#include "log_time.h"

#include <cstdio>
#include <cstring>

namespace dl {

static long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long long)era * 146097 + (long long)doe - 719468;
}

// 与 days_from_civil 互逆
static void civil_from_days(long long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    const long long yy = (long long)yoe + era * 400;
    const unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    const unsigned mp = (5*doy + 2)/153;
    d = doy - (153*mp+2)/5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = (int)(yy + (m <= 2));
}

long long mkEpoch(int Y, int Mo, int D, int h, int mi, int s) {
    return days_from_civil(Y, (unsigned)Mo, (unsigned)D) * 86400LL + h*3600LL + mi*60LL + s;
}

std::string fmtDur(long long sec) {
    char buf[64];
    if (sec < 60)        std::snprintf(buf, sizeof buf, "%llds", (long long)sec);
    else if (sec < 3600) std::snprintf(buf, sizeof buf, "%lldm%02llds", sec/60, sec%60);
    else                 std::snprintf(buf, sizeof buf, "%lldh%02lldm", sec/3600, (sec%3600)/60);
    return buf;
}

std::string fmtTime(long long epoch, const char* fmt) {
    long long days = epoch / 86400;
    long long rem  = epoch % 86400;
    if (rem < 0) { rem += 86400; days -= 1; }
    int Y; unsigned M, D;
    civil_from_days(days, Y, M, D);
    int h = (int)(rem / 3600), mi = (int)((rem % 3600) / 60), s = (int)(rem % 60);
    char buf[64];
    if (std::strcmp(fmt, "HM") == 0)
        std::snprintf(buf, sizeof buf, "%02d:%02d", h, mi);
    else if (std::strcmp(fmt, "FULL") == 0)
        std::snprintf(buf, sizeof buf, "%04d-%02u-%02u %02d:%02d:%02d", Y, M, D, h, mi, s);
    else // "MD"
        std::snprintf(buf, sizeof buf, "%02u-%02u %02d:%02d:%02d", M, D, h, mi, s);
    return buf;
}

} // namespace dl
