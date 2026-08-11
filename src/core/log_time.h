// log_time.h — 日志时间转换与格式化 API
#pragma once

#include <string>

namespace dl {

std::string fmtDur(long long sec);                  // 42s / 3m20s / 1h05m
std::string fmtTime(long long epoch, const char* fmt); // fmt: "MD" -> MM-DD HH:MM:SS, "HM" -> HH:MM, "FULL"
long long   mkEpoch(int Y, int Mo, int D, int h, int mi, int s);

} // namespace dl
