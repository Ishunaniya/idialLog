# Makefile — 把 **open_dial_for_artery 的真实拨号代码**编译成本机可跑的 driver。
# 第四份(前三:../hostrun=EC200A/AG35、../hostrun_open_dial=open_dial、../hostrun_eg25=EG25)。
# artery 与 modem_mng EG25 **同一套 Quectel SDK**,故 include 链与假模组可复用。
A    := /home/tronlong/lyp/code/open_dial_for_artery
CC   := gcc
INC := -I/home/tronlong/lyp/code/open_dial_for_artery -I/home/tronlong/lyp/code/open_dial_for_artery/src -I/home/tronlong/lyp/code/open_dial_for_artery/src/dial -I/home/tronlong/lyp/code/open_dial_for_artery/src/at -I/home/tronlong/lyp/code/open_dial_for_artery/src/nw -I/home/tronlong/lyp/code/open_dial_for_artery/src/apn -I/home/tronlong/lyp/code/open_dial_for_artery/src/sim -I/home/tronlong/lyp/code/open_dial_for_artery/src/tz -I/home/tronlong/lyp/code/open_dial_for_artery/src/json -I/home/tronlong/lyp/code/open_dial_for_artery/src/opt_iniparser -I/home/tronlong/lyp/code/open_dial_for_artery/src/roamlink -I/home/tronlong/lyp/code/open_dial_for_artery/src/seas_log -I/home/tronlong/lyp/code/open_dial_for_artery/src/status -I/home/tronlong/lyp/code/open_dial_for_artery/src/cc_deque -I/home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-extsdk/include -idirafter /home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-crosstool/sysroots/armv7a-vfp-neon-oe-linux-gnueabi/usr/include/qmi -idirafter /home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-crosstool/sysroots/armv7a-vfp-neon-oe-linux-gnueabi/usr/include/qmi-framework -idirafter /home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-crosstool/sysroots/armv7a-vfp-neon-oe-linux-gnueabi/usr/include/data -idirafter /home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-crosstool/sysroots/armv7a-vfp-neon-oe-linux-gnueabi/usr/include/dsutils -idirafter /home/tronlong/lyp/SDK/EG25/ql-ol-sdk/ql-ol-crosstool/sysroots/armv7a-vfp-neon-oe-linux-gnueabi/usr/include
CFLAGS := -std=gnu99 -g -O0 $(INC)

# 真代码(直接编 open_dial_for_artery 原文件,不拷贝)
REAL_C := $(A)/src/dial/dial.c $(A)/src/at/at.c $(A)/src/nw/nw.c $(A)/src/apn/apn.c \
          $(A)/src/sim/sim.c $(A)/src/tz/tz.c $(A)/src/roamlink/roamlink.c \
          $(A)/src/seas_log/seas_log.c $(A)/src/json/cJSON.c \
          $(wildcard $(A)/src/status/*.c) $(wildcard $(A)/src/opt_iniparser/*.c) \
          $(wildcard $(A)/src/cc_deque/*.c)
REAL_OBJ := artery_main.o $(notdir $(REAL_C:.c=.o))

all: driver

driver: driver.o stubs.o sdk_stubs.o $(REAL_OBJ)
	$(CC) -o $@ $^ -lpthread -lm
	@echo "==> artery driver 就绪(内含真实 src/dial/dial.c $(shell wc -l < $(A)/src/dial/dial.c) 行)"

vpath %.c $(A) $(A)/src/dial $(A)/src/at $(A)/src/nw $(A)/src/apn $(A)/src/sim $(A)/src/tz \
          $(A)/src/roamlink $(A)/src/seas_log $(A)/src/json $(A)/src/status \
          $(A)/src/opt_iniparser $(A)/src/cc_deque .

# 产品 main 与 host-run 驱动都叫 main；仅在编译产物里重命名入口，源码不改。
artery_main.o: $(A)/main.c
	$(CC) -c $(CFLAGS) -Dmain=artery_product_main $< -o $@

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f *.o driver

.PHONY: all clean
