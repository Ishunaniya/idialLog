// fastclock.c — LD_PRELOAD 时间加速器。
//
// 为什么要它:真代码的恢复阶梯按**真实时长**判级(EC200A: L1>5min, L2>10min, L3>35min)。
// 要跑出 L3 就得等 35 分钟。加速时钟后,35 分钟的**逻辑时间**在几十秒真实时间内走完。
//
// 为什么用 LD_PRELOAD 而不是改源码里的阈值:
//   改阈值 = 改了被测代码 → 跑的就不是真代码了,失去意义。
//   劫持时钟 = 源码一行不改,**真代码的真实分支逻辑照常跑**,只是它感知的时间变快。
//
// 只加速 CLOCK_MONOTONIC / time():那是阶梯计时用的。usleep 不动 —— 循环节奏保持真实,
// 每次 50ms 真实睡眠对应 SIM_TIME_SCALE 倍的逻辑时间推进。
//
// 用法: LD_PRELOAD=./fastclock.so SIM_TIME_SCALE=100 ./driver
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <time.h>

static int (*real_clock_gettime)(clockid_t, struct timespec*) = NULL;
static time_t (*real_time)(time_t*) = NULL;

static double scale(void) {
    static double s = -1;
    if (s < 0) {
        const char* v = getenv("SIM_TIME_SCALE");
        s = v ? atof(v) : 1.0;
        if (s < 1.0) s = 1.0;
    }
    return s;
}

/* 进程启动时刻作为基准:加速只作用于"流逝的时间",不改绝对起点 ——
   否则日志时间戳会跳到很久以后,看着别扭且干扰按天分文件的逻辑。 */
static struct timespec t0;
static int t0_init = 0;

static void init_once(void) {
    if (!real_clock_gettime)
        real_clock_gettime = (int(*)(clockid_t, struct timespec*))dlsym(RTLD_NEXT, "clock_gettime");
    if (!real_time)
        real_time = (time_t(*)(time_t*))dlsym(RTLD_NEXT, "time");
    if (!t0_init && real_clock_gettime) {
        real_clock_gettime(CLOCK_MONOTONIC, &t0);
        t0_init = 1;
    }
}

int clock_gettime(clockid_t clk, struct timespec* ts) {
    init_once();
    int r = real_clock_gettime(clk, ts);
    if (r != 0 || scale() == 1.0) return r;
    if (clk == CLOCK_MONOTONIC || clk == CLOCK_MONOTONIC_RAW || clk == CLOCK_BOOTTIME) {
        double el = (ts->tv_sec - t0.tv_sec) + (ts->tv_nsec - t0.tv_nsec) / 1e9;
        double fast = el * scale();
        ts->tv_sec  = t0.tv_sec + (time_t)fast;
        ts->tv_nsec = t0.tv_nsec + (long)((fast - (long)fast) * 1e9);
        if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
    }
    return r;
}

time_t time(time_t* tloc) {
    init_once();
    time_t now = real_time(NULL);
    if (scale() != 1.0) {
        static time_t wall0 = 0;
        if (!wall0) wall0 = now;
        now = wall0 + (time_t)((now - wall0) * scale());
    }
    if (tloc) *tloc = now;
    return now;
}

/* ============================================================================
 * sleep / usleep / nanosleep 也必须按比例缩短 —— 否则时序失真。
 *
 * 【实证】不缩短会怎样:代码里 CFUN=0 与 CFUN=1 之间有 sleep(5)。
 *   时钟 ×150 但 sleep 真睡 5 秒 → 那 5 秒被放大成 750 秒**逻辑**时间,
 *   于是日志里 L2 的 CFUN=0→CFUN=1 跨了 32 分钟。
 *   而真机 EG25 1.31.15 日志里这两行只差 **3 秒**。完全失真。
 *
 * 正确做法:sleep(N) 实际只睡 N/scale 真实秒;这段真实时间被加速时钟放大回
 * N 逻辑秒 —— 两边一致,日志时序就和真机一个量纲了。
 * ========================================================================== */
static unsigned (*real_sleep)(unsigned) = NULL;
static int (*real_usleep)(useconds_t) = NULL;
static int (*real_nanosleep)(const struct timespec*, struct timespec*) = NULL;

unsigned int sleep(unsigned int sec) {
    if (!real_sleep) real_sleep = (unsigned(*)(unsigned))dlsym(RTLD_NEXT, "sleep");
    double s = scale();
    if (s <= 1.0) return real_sleep(sec);
    double want = sec / s;                       /* 缩短后的真实秒 */
    struct timespec ts;
    ts.tv_sec  = (time_t)want;
    ts.tv_nsec = (long)((want - ts.tv_sec) * 1e9);
    if (!real_nanosleep) real_nanosleep = (int(*)(const struct timespec*, struct timespec*))dlsym(RTLD_NEXT, "nanosleep");
    real_nanosleep(&ts, NULL);
    return 0;
}

int usleep(useconds_t us) {
    if (!real_usleep) real_usleep = (int(*)(useconds_t))dlsym(RTLD_NEXT, "usleep");
    double s = scale();
    if (s <= 1.0) return real_usleep(us);
    useconds_t u = (useconds_t)(us / s);
    return real_usleep(u ? u : 1);
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    if (!real_nanosleep) real_nanosleep = (int(*)(const struct timespec*, struct timespec*))dlsym(RTLD_NEXT, "nanosleep");
    double s = scale();
    if (s <= 1.0 || !req) return real_nanosleep(req, rem);
    double want = (req->tv_sec + req->tv_nsec / 1e9) / s;
    struct timespec ts;
    ts.tv_sec  = (time_t)want;
    ts.tv_nsec = (long)((want - ts.tv_sec) * 1e9);
    return real_nanosleep(&ts, rem);
}
