# Makefile — 把 modem_mng 的**真实拨号代码**编译成能在本机 x86 跑的 driver。
#
# 这不是"我照源码写的模拟器",是**真代码本身**:
#   真代码(编进来): ec200a/dial/dial.cpp(1756行)+ at.c/sim.c/apn.c/data_call.c/
#                    nw.c/diag.c/misc.c/slot_mgr.c/oper.cpp + logger_sd.c
#   假的(只有外设): SDK 桩(签名自动取自 SDK 真头)、LED/json-c/iniparser 桩,
#                    以及 PATH 上的假 ping / serial_atcmd(fakebin/)
#
# 故障注入靠环境变量,**源码一行不改**。时间靠 LD_PRELOAD 加速(fastclock.so)。
#
# 用法:
#   make                     # 构建 driver + fastclock.so
#   ./run_scenario.sh        # 跑全部场景,日志落到 ../../samples/sim/hostrun/

M       := /home/tronlong/lyp/code/rtms_sdk/apps/modem_mng
SDK     := /home/tronlong/lyp/SDK/EC200A/ql-ol-extsdk-ec200acntar03a02m2g_ocpu/ql-sysroots/usr/include
SDKQ    := $(SDK)/ql-sdk
TP      := /home/tronlong/lyp/code/rtms_sdk/3rdparty
SK      := /home/tronlong/lyp/code/skes/build/install

# 注意:用 := 且显式指定,别让环境里的 RK3576/buildroot CC/CXX 污染(本机老毛病)
CXX     := g++
CC      := gcc

INC := -I$(M) -I$(M)/ec200a -I$(M)/ec200a/dial -I$(M)/ec200a/at -I$(M)/ec200a/misc \
       -I$(M)/ec200a/diag -I$(M)/ec200a/slot -I$(M)/ec200a/oper -I$(M)/ec200a/sim \
       -I$(M)/ec200a/apn -I$(M)/ec200a/data_call -I$(M)/ec200a/nw -I$(M)/ec200a/test_utils \
       -I$(M)/fault_report -I$(M)/traffic_sql -I$(M)/cc_deque -I$(M)/status \
       -I$(TP)/ledctl -I$(TP)/libappmng -I$(TP)/tbox-common/src \
       -I$(SK)/cjson-1.7.15/include -I$(SK)/nanomsg-1.1.5/include \
       -I$(SDK) -I$(SDKQ)

DEF      := -DUSE_EC200A_DIAL
CXXFLAGS := -std=c++17 -g -O0 $(DEF) $(INC)
CFLAGS   := -std=gnu99 -g -O0 $(DEF) $(INC)

# —— 真代码(来自 modem_mng,不是副本)——
# fault_report/NetworkMonitor.cpp 才是真正执行 ping 的地方(dial_loop 通过它判连通),
# 早先漏编 → 真代码恒判"ping 不通",所有场景都塌缩成 never-connected。
REAL_CXX := $(M)/ec200a/dial/dial.cpp $(M)/ec200a/oper/oper.cpp \
            $(M)/fault_report/NetworkMonitor.cpp
# cJSON 是单文件库且本机有源码 → 编真的,不打桩(打桩会让 NetworkMonitor 的上报逻辑走样)
CJSON_C  := /home/tronlong/lyp/code/skes/skes-3rd-party/cjson-git/cJSON.c
REAL_C   := $(CJSON_C) $(M)/logger_sd.c $(M)/ec200a/at/at.c $(M)/ec200a/misc/misc.c \
            $(M)/ec200a/diag/diag.c $(M)/ec200a/sim/sim.c $(M)/ec200a/apn/apn.c \
            $(M)/ec200a/data_call/data_call.c $(M)/ec200a/nw/nw.c $(M)/ec200a/slot/slot_mgr.c

REAL_OBJ := $(notdir $(REAL_CXX:.cpp=.o)) $(notdir $(REAL_C:.c=.o))
OWN_OBJ  := driver.o stubs.o sdk_stubs.o

all: driver fastclock.so

driver: $(OWN_OBJ) $(REAL_OBJ)
	$(CXX) -o $@ $^ -lpthread
	@echo "==> driver 就绪(内含真实 dial.cpp $(shell wc -l < $(M)/ec200a/dial/dial.cpp) 行)"

# 真代码 —— 用 vpath 直接编 modem_mng 里的原文件,不拷贝(拷贝会走样)
vpath %.cpp $(M)/ec200a/dial $(M)/ec200a/oper $(M)/fault_report .
vpath %.c   /home/tronlong/lyp/code/skes/skes-3rd-party/cjson-git $(M) $(M)/ec200a/at $(M)/ec200a/misc $(M)/ec200a/diag $(M)/ec200a/sim \
            $(M)/ec200a/apn $(M)/ec200a/data_call $(M)/ec200a/nw $(M)/ec200a/slot

%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) $< -o $@

%.o: %.c
	$(CC) -c $(CFLAGS) $< -o $@

# 时间加速器(LD_PRELOAD):让 35 分钟的 L3 在几十秒内跑到,源码不改
fastclock.so: fastclock.c
	$(CC) -shared -fPIC -o $@ $< -ldl

clean:
	rm -f *.o driver fastclock.so

.PHONY: all clean
