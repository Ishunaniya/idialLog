/* driver.c — 把 open_dial 的**真实 dial_loop** 在本机跑起来。
 * open_dial 的 dial_loop 是 while(1) 无退出开关(与 modem_mng 的 isExist 不同),
 * 故放到线程里跑,主线程到点退出进程 —— 这不影响日志真实性(日志已经打出来了)。 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
extern void dial_loop(void (*on_connected)(void *user), void *user_data);
static void on_conn(void* u) { (void)u; }
static void* run(void* a) { (void)a; dial_loop(on_conn, NULL); return NULL; }
int main(int argc, char** argv) {
    int logical = (argc > 1) ? atoi(argv[1]) : 3000;   /* 逻辑秒(sleep 已被 fastclock 加速) */
    const char* sc = getenv("SIM_TIME_SCALE");
    fprintf(stderr, "[driver] open_dial 逻辑运行 %ds(%.0f 分钟),加速 ×%s\n",
            logical, logical/60.0, sc ? sc : "1");
    pthread_t t; pthread_create(&t, NULL, run, NULL);
    sleep((unsigned)logical);          /* 被 fastclock 缩短成 logical/scale 真实秒 */
    fprintf(stderr, "[driver] 时间到,退出\n");
    fflush(stdout);
    _exit(0);
}
