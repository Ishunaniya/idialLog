/* stubs.c — EG25 的假外设层。只打桩外设,不替代业务逻辑。
 * EG25 的 AT 走真串口(open /dev/smd8 + select/read/write),故不在这里打桩 ——
 * 用 fakemodem.so(LD_PRELOAD 拦 open)把它接到 PTY 上的假模组,
 * 这样 Ql_SendAT 的收发时序与应答解析全跑真代码。 */
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

/* dialer_eg25.c 里的全局 AT 口互斥锁(dial.c/nw.c 用 extern 引它)。
 * 这是**真代码的并发不变量**(见 CLAUDE.md:EG25 AT 口序列化),不能省。 */
pthread_mutex_t g_at_port_mutex = PTHREAD_MUTEX_INITIALIZER;

int  __android_log_print(int p, const char* t, const char* f, ...) { (void)p;(void)t;(void)f; return 0; }
int  ql_sys_log_print(int p, const char* f, ...) { (void)p;(void)f; return 0; }
int  cpactive_upt_atime(void* h) { (void)h; return 0; }
void* cpactive_init(const char* n) { (void)n; static int d; return &d; }
void cpactive_deinit(void* h) { (void)h; }

void* LEDControl_create(void) { static int d; return &d; }
void  LEDControl_destroy(void* h) { (void)h; }
int   LEDControl_controlLight(void* h, int id, int st) { (void)h; fprintf(stderr,"[stub] LED id=%d st=%d\n",id,st); return 0; }
int   LEDControl_blinkLight(void* h, int a, int b, int c) { (void)h;(void)a;(void)b;(void)c; return 0; }
int   LEDControl_getLedIdType(void* h, const char* n) { (void)h;(void)n; return 0; }

/* iniparser 不打桩 —— eg25/opt_iniparser/ 里有真实现,已编进来。
 * 原则:真代码存在就编真的,只有外部设备/库才打桩。 */
