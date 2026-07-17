// driver.cpp — 把**真实的 EC200ADialer::dial_loop** 在本机跑起来。
//
// 它做的事只有三件:构造真 dialer、调用真 dial_loop、到点让它退出。
// **不代替任何业务逻辑** —— 日志由真代码打,分支由真代码选。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#define USE_EC200A_DIAL 1
#include "dialer.hpp"

int main(int argc, char** argv) {
    /* 参数 = **逻辑秒**(不是真实秒)。
     * 因为 fastclock.so 连 sleep/nanosleep 一起加速了,下面的 sleep_for(run_sec)
     * 实际只睡 run_sec/SIM_TIME_SCALE 真实秒 —— 正好等于我们想要的逻辑时长。
     * (踩过的坑:早先按"真实秒"理解,sleep 被加速后 driver 0.2 秒就退了。) */
    int run_sec = (argc > 1) ? atoi(argv[1]) : 3000;

    const char* scale = getenv("SIM_TIME_SCALE");
    double sc = scale ? atof(scale) : 1.0;
    fprintf(stderr, "[driver] 逻辑运行 %ds(%.0f 分钟),加速 ×%.0f → 真实约 %.1fs\n",
            run_sec, run_sec / 60.0, sc, run_sec / sc);

    EC200ADialer dial;                       // ← 真类

    // 到点置 isExist,让真 dial_loop 自己走正常退出路径(而非 kill),收尾日志也是真的。
    // set_isExist 是类成员(dialer.hpp:301),不是全局函数。
    std::thread([&dial, run_sec]() {
        std::this_thread::sleep_for(std::chrono::seconds(run_sec));
        fprintf(stderr, "[driver] 时间到,置 isExist 让 dial_loop 正常退出\n");
        dial.set_isExist(true);
    }).detach();

    std::string ifname = "ccinet1";
    dial.dial_loop(nullptr, ifname, [](){}); // ← 真循环

    fprintf(stderr, "[driver] dial_loop 已返回\n");
    return 0;
}
