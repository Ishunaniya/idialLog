// fakemodem.c — LD_PRELOAD:把 EG25 的 AT 串口 /dev/smd8 换成一个假模组(PTY)。
//
// 为什么不像 EC200A 那样用假 serial_atcmd 脚本:
//   EG25 的 AT 不走 popen,而是 at_init() → open("/dev/smd8") + Ql_SendAT 里
//   select/read/write 真串口(eg25/at/at.c:179)。所以要在 open() 这一层拦。
//
// 拦了之后:Ql_SendAT 的**收发时序与应答解析全部跑真代码** —— 我只负责在 PTY 另一端
// 扮演模组吐 AT 应答。应答内容由环境变量控制(SIM_CSQ / SIM_CEREG / ...),
// **真代码怎么解析、怎么据此决策,是真代码的事**。
//
// 另:EG25 的 ping 是 system("ping -c 1 8.8.8.8 > /dev/null") **只看返回码**
// (dial.c:319),与 EC200A(看输出文本)不同 —— 那个仍由 PATH 上的假 ping 处理。
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <pty.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>

static int (*real_open)(const char*, int, ...) = NULL;
static int g_master = -1;          /* 假模组这一端 */
static pthread_t g_th;

static const char* envs(const char* k, const char* d) {
    const char* v = getenv(k);
    return (v && *v) ? v : d;
}

/* 假模组主循环:读真代码发来的 AT,按环境变量吐应答。
   只做"扮演模组",不替真代码做任何解析。 */
static void* modem_thread(void* arg) {
    int fd = *(int*)arg;
    char buf[1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n <= 0) { usleep(20000); continue; }
        buf[n] = 0;
        char low[1024];
        size_t i;
        for (i = 0; i < (size_t)n && i < sizeof low - 1; ++i) low[i] = (char)tolower((unsigned char)buf[i]);
        low[i] = 0;

        char out[1024] = "";
        if      (strstr(low, "csq"))     snprintf(out, sizeof out, "\r\n+CSQ: %s,99\r\n\r\nOK\r\n", envs("SIM_CSQ", "20"));
        else if (strstr(low, "cereg"))   snprintf(out, sizeof out, "\r\n+CEREG: 0,%s\r\n\r\nOK\r\n", envs("SIM_CEREG", "1"));
        else if (strstr(low, "creg"))    snprintf(out, sizeof out, "\r\n+CREG: 0,%s\r\n\r\nOK\r\n", envs("SIM_CREG", "1"));
        else if (strstr(low, "cpin"))    snprintf(out, sizeof out, atoi(envs("SIM_CARD_ABSENT","0"))
                                                  ? "\r\n+CME ERROR: 10\r\n" : "\r\n+CPIN: READY\r\n\r\nOK\r\n");
        else if (strstr(low, "qtemp"))   snprintf(out, sizeof out, "\r\n+QTEMP: %s,%s,%s\r\n\r\nOK\r\n",
                                                  envs("SIM_TEMP","40"), envs("SIM_TEMP","35"), envs("SIM_TEMP","35"));
        else if (strstr(low, "ceer"))    snprintf(out, sizeof out, "\r\n+CEER: %s\r\n\r\nOK\r\n", envs("SIM_CEER","0,-1"));
        else if (strstr(low, "cgact"))   snprintf(out, sizeof out, "\r\n+CGACT: 1,%s\r\n\r\nOK\r\n", envs("SIM_PDP_ACT","1"));
        else if (strstr(low, "cgpaddr")) snprintf(out, sizeof out, "\r\n+CGPADDR: 1,\"%s\"\r\n\r\nOK\r\n", envs("SIM_IP","10.1.2.3"));
        else if (strstr(low, "cgdcont")) snprintf(out, sizeof out, "\r\n+CGDCONT: 1,\"IP\",\"%s\",\"0.0.0.0\",0,0,0,0\r\n\r\nOK\r\n", envs("SIM_APN","internet"));
        else if (strstr(low, "cops"))    snprintf(out, sizeof out, "\r\n+COPS: 0,2,\"%s\",7\r\n\r\nOK\r\n", envs("SIM_PLMN","46000"));
        else if (strstr(low, "qeng"))    snprintf(out, sizeof out, "\r\n+QENG: \"servingcell\",\"NOCONN\",\"LTE\",\"FDD\",460,00,D17C148,496,1850,3,5,5,272D,%s,-6,-75,23,18\r\n\r\nOK\r\n", envs("SIM_RSRP","-101"));
        else if (strstr(low, "cfun?"))   snprintf(out, sizeof out, "\r\n+CFUN: %s\r\n\r\nOK\r\n", envs("SIM_CFUN","1"));
        else if (strstr(low, "qgmr") || strstr(low, "ati") || strstr(low, "cgmr"))
                                         snprintf(out, sizeof out, "\r\nEG25GGBR07A08M2G_OCPU_30.200.30.200\r\n\r\nOK\r\n");
        else if (strstr(low, "cimi"))    snprintf(out, sizeof out, "\r\n460089087110299\r\n\r\nOK\r\n");
        else if (strstr(low, "cgsn") || strstr(low, "gsn"))
                                         snprintf(out, sizeof out, "\r\n867929069681908\r\n\r\nOK\r\n");
        else if (strstr(low, "qccid") || strstr(low, "iccid"))
                                         snprintf(out, sizeof out, "\r\n+QCCID: 89860000000000000000\r\n\r\nOK\r\n");
        else                             snprintf(out, sizeof out, "\r\nOK\r\n");
        (void)write(fd, out, strlen(out));
    }
    return NULL;
}

int open(const char* path, int flags, ...) {
    if (!real_open) real_open = (int(*)(const char*, int, ...))dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }

    /* 只拦 AT 口(eg25/at/at.h:60 QUEC_AT_PORT "/dev/smd8"),其余照常 */
    if (path && strstr(path, "/dev/smd")) {
        if (g_master >= 0) return real_open("/dev/null", O_RDWR);   /* 已开过 */
        int m, s;
        if (openpty(&m, &s, NULL, NULL, NULL) != 0) return -1;
        g_master = m;
        static int marg;
        marg = m;
        pthread_create(&g_th, NULL, modem_thread, &marg);           /* 假模组住在 master 端 */
        fprintf(stderr, "[fakemodem] %s -> pty(假模组已就位)\n", path);
        return s;                                                   /* 真代码拿到 slave 端 */
    }
    return real_open(path, flags, mode);
}
