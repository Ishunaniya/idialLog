/* fakeinc/android/log.h — 最小假头。
 * EG25 是 OpenLinux(带 Android 日志层),3rdparty/tbox-common/src/logger.h 的
 * LOG_I/LOG_E 宏用 __android_log_print(ANDROID_LOG_*, ...)。主机没有这个头,
 * 故给一份最小定义 —— 只为让**真代码**编过,不改真代码一行。
 * __android_log_print 的实现是空桩(见 stubs.c):EG25 的诊断日志走 dial_log,
 * 不走 Android log,故空桩不影响我们要观察的日志。 */
#ifndef _FAKE_ANDROID_LOG_H_
#define _FAKE_ANDROID_LOG_H_
typedef enum {
    ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE,
    ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR, ANDROID_LOG_FATAL, ANDROID_LOG_SILENT
} android_LogPriority;
#ifdef __cplusplus
extern "C" {
#endif
int __android_log_print(int prio, const char* tag, const char* fmt, ...);
#ifdef __cplusplus
}
#endif
#endif
