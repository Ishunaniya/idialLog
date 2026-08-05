/* driver.c — 在受控时长内跑 artery 的**真实产品 main**。
 * 新版心跳位于 main.c，不再只启动 dial_task 而漏掉主线程的打印代码。 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
extern int artery_product_main(int argc, char **argv);

static void *run_product_main(void *unused) {
    (void)unused;
    char arg0[] = "artery-hostrun";
    char *argv[] = { arg0, NULL };
    artery_product_main(1, argv);
    return NULL;
}

int main(int argc, char** argv) {
    int logical = (argc > 1) ? atoi(argv[1]) : 3000;
    const char* sc = getenv("SIM_TIME_SCALE");
    fprintf(stderr, "[driver] artery 逻辑运行 %ds(%.0f 分钟),加速 ×%s\n", logical, logical/60.0, sc?sc:"1");
    pthread_t t;
    if (pthread_create(&t, NULL, run_product_main, NULL) != 0) return 1;
    sleep((unsigned)logical);
    fprintf(stderr, "[driver] 时间到,退出\n"); fflush(stdout); _exit(0);
}
