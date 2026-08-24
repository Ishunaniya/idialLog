/* driver.c — 跑 EG25 的**真实 dial_task**(eg25/dial/dial.c:540)。
 *
 * 初始化照 dialer_eg25.c 的真实流程来:dial_task 要一个由 **dial_mng_new()**
 * (真代码自己的构造函数)建好的 dial_mng_t —— 早先传 NULL 直接段错误。
 * monitor/cpa/nano_handler 三个是对外设施句柄,置空即可(它们是 IPC/流量统计,
 * 不是拨号逻辑;真代码对空句柄有自己的处理,那也是真代码的行为)。 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "dial.h"

extern void *dial_task(void *arg);
extern dial_mng_t *dial_mng_new(void);
extern bool dail_start_data_call(dial_mng_t *p_dial_mng);

int main(int argc, char** argv) {
    int logical = (argc > 1) ? atoi(argv[1]) : 3000;   /* 逻辑秒(sleep 已被 fastclock 加速) */
    const char* sc = getenv("SIM_TIME_SCALE");
    fprintf(stderr, "[driver] EG25 逻辑运行 %ds(%.0f 分钟),加速 ×%s\n",
            logical, logical/60.0, sc?sc:"1");

    dial_mng_t *m = dial_mng_new();                    /* ← 真代码的构造 */
    if (!m) { fprintf(stderr, "[driver] dial_mng_new 失败\n"); return 1; }
    fprintf(stderr, "[driver] dial_mng_new OK, smd_fd=%d(<0 表示 AT 口没打开)\n", m->smd_fd);

    /* Init 致命分支无需让 SIM/AT 状态机先跑一圈：直接调用产品真实函数，
     * 只由 SDK 桩注入返回码。成功场景仍继续走完整 dial_task。 */
    if (getenv("SIM_DIRECT_DATACALL_INIT")) {
        fprintf(stderr, "[driver] 直测真实 dail_start_data_call\n");
        return dail_start_data_call(m) ? 0 : 2;
    }

    pthread_t t;
    pthread_create(&t, NULL, dial_task, (void*)m);     /* ← 真任务 */
    sleep((unsigned)logical);
    fprintf(stderr, "[driver] 时间到,退出\n");
    fflush(stdout);
    _exit(0);
}
