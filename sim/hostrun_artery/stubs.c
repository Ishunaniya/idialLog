/* stubs.c — artery 的假外设层。只打桩外设。
 * AT 走真串口 → 复用 ../hostrun_eg25/fakemodem.so(同样 /dev/smd8 + O_NONBLOCK)。 */
#include <stdio.h>
#include <pthread.h>
pthread_mutex_t g_at_port_mutex = PTHREAD_MUTEX_INITIALIZER;
int  __android_log_print(int p, const char* t, const char* f, ...) { (void)p;(void)t;(void)f; return 0; }
int  cpactive_upt_atime(void* h) { (void)h; return 0; }
void* LEDControl_create(void) { static int d; return &d; }
void  LEDControl_destroy(void* h) { (void)h; }
int   LEDControl_controlLight(void* h, int id, int st) { (void)h; fprintf(stderr,"[stub] LED %d=%d\n",id,st); return 0; }
int  ql_sys_log_print(int p, const char* f, ...) { (void)p;(void)f; return 0; }
