# Makefile — 把 **modem_mng EG25 的真实拨号代码**编译成本机可跑的 driver。
# 与 ../hostrun(EC200A)同法,但 EG25 差异极大:第三套 SDK、AT 走真串口、有 roamlink。
M    := /home/tronlong/lyp/code/rtms_sdk/apps/modem_mng
CC   := gcc

# EG25 的 include 链是逐个试出来的(ql_oe.h→comdef.h→qmi_port_defs.h→...)。
# ARM sysroot 用 -idirafter:它里面有自己的 stdint.h,会和主机 x86 的打架,必须让主机头优先。
# -include fakeinc/android/log.h:tbox-common/logger.h 的 LOG_I 要 Android 日志头,
#   主机没有 → 造最小假头强制插入(**不改真代码一行**)。
INC := -include /home/tronlong/lyp/code/tools/dialLog/sim/hostrun_eg25/fakeinc/android/log.h -I/home/tronlong/lyp/code/tools/dialLog/sim/hostrun_eg25/fakeinc -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/eg25 -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/eg25/dial -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/eg25/at -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/eg25/nw -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/eg25/apn -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/eg25/sim -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/eg25/tz -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/eg25/diag -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/eg25/json_old -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/eg25/opt_iniparser -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/roamlink -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/status -I/home/tronlong/lyp/code/rtms_sdk/apps/modem_mng/cc_deque -I/home/tronlong/lyp/code/rtms_sdk/3rdparty/ledctl -I/home/tronlong/lyp/code/rtms_sdk/3rdparty/libappmng -I/home/tronlong/lyp/code/rtms_sdk/3rdparty/tbox-common/src -I/home/tronlong/lyp/code/skes/build/install/cjson-1.7.15/include -I/home/tronlong/lyp/code/skes/build/install/nanomsg-1.1.5/include -I/home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-extsdk/include -idirafter /home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-crosstool/sysroots/armv7a-vfp-neon-oe-linux-gnueabi/usr/include/qmi -idirafter /home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-crosstool/sysroots/armv7a-vfp-neon-oe-linux-gnueabi/usr/include/qmi-framework -idirafter /home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-crosstool/sysroots/armv7a-vfp-neon-oe-linux-gnueabi/usr/include/data -idirafter /home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-crosstool/sysroots/armv7a-vfp-neon-oe-linux-gnueabi/usr/include/dsutils -idirafter /home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-crosstool/sysroots/armv7a-vfp-neon-oe-linux-gnueabi/usr/include
CFLAGS := -std=gnu99 -g -O0 -DUSE_EG25_DIAL $(INC)

# 真代码(直接编 modem_mng 原文件,不拷贝)
REAL_C := $(M)/eg25/dial/dial.c $(M)/eg25/at/at.c $(M)/eg25/nw/nw.c $(M)/eg25/apn/apn.c \
          $(M)/eg25/sim/sim.c $(M)/eg25/tz/tz.c $(M)/eg25/diag/diag.c \
          $(M)/roamlink/roamlink.c $(M)/status/dial_status.c $(M)/logger_sd.c \
          $(M)/cc_deque/cc_common.c $(M)/cc_deque/cc_deque.c \
          $(M)/eg25/opt_iniparser/dictionary.c $(M)/eg25/opt_iniparser/iniparser.c
CJSON  := /home/tronlong/lyp/code/skes/skes-3rd-party/cjson-git/cJSON.c
REAL_OBJ := $(notdir $(REAL_C:.c=.o)) cJSON_full.o

all: driver fakemodem.so

driver: driver.o stubs.o sdk_stubs.o $(REAL_OBJ)
	$(CC) -o $@ $^ -lpthread -lm
	@echo "==> EG25 driver 就绪(内含真实 eg25/dial/dial.c $(shell wc -l < $(M)/eg25/dial/dial.c) 行)"

vpath %.c $(M) $(M)/eg25/dial $(M)/eg25/at $(M)/eg25/nw $(M)/eg25/apn $(M)/eg25/sim \
          $(M)/eg25/tz $(M)/eg25/diag $(M)/roamlink $(M)/status $(M)/cc_deque \
          $(M)/eg25/opt_iniparser .

cJSON_full.o: $(CJSON)
	$(CC) -c -std=gnu99 -I$(M)/eg25/json_old $< -o $@

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

# 假模组:LD_PRELOAD 拦 open(/dev/smd8) 接到 PTY;并拦 reboot(roamlink 会真调它)
fakemodem.so: fakemodem.c
	$(CC) -shared -fPIC -o $@ $< -ldl -lutil

clean:
	rm -f *.o driver fakemodem.so

.PHONY: all clean
