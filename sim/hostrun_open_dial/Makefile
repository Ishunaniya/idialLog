# Makefile — 把 **open_dial 的真实拨号代码**编译成能在本机 x86 跑的 driver。
# 与 ../hostrun(modem_mng)同法,但 open_dial 是独立仓库、纯 C、dial_loop 是全局函数。
O    := /home/tronlong/lyp/code/open_dial
ESDK := /home/tronlong/lyp/SDK/EC200A/ql-ol-extsdk-ec200acntar03a02m2g_ocpu/ql-sysroots/usr/include
CC   := gcc

INC := -I$(O) -I$(O)/apn -I$(O)/at -I$(O)/data_call -I$(O)/nw -I$(O)/sim -I$(O)/diag \
       -I$(O)/test_utils -I$(O)/cc_deque -I$(O)/reboot_conf -I$(ESDK) -I$(ESDK)/ql-sdk
CFLAGS := -std=gnu99 -g -O0 $(INC)

# 真代码(直接编 open_dial 原文件,不拷贝)
REAL_C := $(O)/dial.c $(O)/misc.c $(O)/logger_sd.c $(O)/apn/apn.c $(O)/at/at.c \
          $(O)/data_call/data_call.c $(O)/nw/nw.c $(O)/sim/sim.c $(O)/diag/diag.c \
          $(O)/reboot_conf/dial_reboot_conf.c
REAL_OBJ := $(notdir $(REAL_C:.c=.o))

all: driver

driver: driver.o sdk_stubs.o $(REAL_OBJ)
	$(CC) -o $@ $^ -lpthread
	@echo "==> open_dial driver 就绪(内含真实 dial.c $(shell wc -l < $(O)/dial.c) 行)"

vpath %.c $(O) $(O)/apn $(O)/at $(O)/data_call $(O)/nw $(O)/sim $(O)/diag $(O)/reboot_conf .

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f *.o driver

.PHONY: all clean
