/* driver.c — 跑 artery 的**真实 dial_task**(src/dial/dial.c:753)。
 * 照 main.c:186 的真实流程:dial_task 要 dial_mng_new() 构造的 dial_mng_t。 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "dial.h"
extern void *dial_task(void *arg);
extern dial_mng_t *dial_mng_new(void);
int main(int argc, char** argv) {
    int logical = (argc > 1) ? atoi(argv[1]) : 3000;
    const char* sc = getenv("SIM_TIME_SCALE");
    fprintf(stderr, "[driver] artery 逻辑运行 %ds(%.0f 分钟),加速 ×%s\n", logical, logical/60.0, sc?sc:"1");
    dial_mng_t *m = dial_mng_new();
    if (!m) { fprintf(stderr, "[driver] dial_mng_new 失败\n"); return 1; }
    fprintf(stderr, "[driver] dial_mng_new OK, smd_fd=%d\n", m->smd_fd);
    pthread_t t; pthread_create(&t, NULL, dial_task, (void*)m);
    sleep((unsigned)logical);
    fprintf(stderr, "[driver] 时间到,退出\n"); fflush(stdout); _exit(0);
}
