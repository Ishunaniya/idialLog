# Makefile — dialLog (Win32 原生 GUI, MinGW)
#
# 交叉编译(Linux 上生成 Windows exe,本仓库默认):
#     make                         # build/x64/dialLog_vX.Y.Z.exe
#     make windows-all             # 同时构建 build/x64 与 build/x86
#     make release                 # 正式 x64 产物复制到仓库根目录
# Windows 本机 MinGW 编译:
#     mingw32-make CROSS=
# 32 位:
#     make windows-x86
#
# 产物为静态链接,不依赖任何 MinGW/MSVC 运行时 DLL,拷到 Windows 双击即用。
# 文件名自带版本号 —— 发给别人/存档时不会搞混是哪个 build。

# 注意:用 := 而非 ?=。本机环境常导出 CC/CXX(RK3576/buildroot 交叉链),
# ?= 对“已由环境定义”的变量不生效,会误用 aarch64 编译器。命令行 `make CXX=g++` 仍可覆盖。
CROSS   ?= x86_64-w64-mingw32-
CXX     := $(CROSS)g++
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres

# 每种工具链使用独立目录。切换 CROSS 后不能复用上一架构的 .o 或 exe。
# 未知交叉前缀可由调用方显式传 BUILD_FLAVOR=<名称>。
ifeq ($(strip $(CROSS)),)
BUILD_FLAVOR ?= native
else ifneq ($(findstring i686,$(CROSS)),)
BUILD_FLAVOR ?= x86
else ifneq ($(findstring x86_64,$(CROSS)),)
BUILD_FLAVOR ?= x64
else
BUILD_FLAVOR ?= cross
endif
BUILD_ROOT ?= build
BUILD_DIR  ?= $(BUILD_ROOT)/$(BUILD_FLAVOR)

# 解析层测试始终用本机编译器。单独命名,避免环境里的嵌入式 CC/CXX 污染。
HOST_CXX      := g++
HOST_CC       := gcc
HOST_CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
HOST_LDFLAGS  :=

# 版本号从 version.h 解析,保持单一来源:改 version.h 即同时改变
# exe 文件名、exe 版本资源(右键属性)、标题栏,三者永远一致。
VER_MAJOR := $(shell sed -n 's/^#define[ \t]\+DL_VER_MAJOR[ \t]\+\([0-9]\+\).*/\1/p' version.h)
VER_MINOR := $(shell sed -n 's/^#define[ \t]\+DL_VER_MINOR[ \t]\+\([0-9]\+\).*/\1/p' version.h)
VER_PATCH := $(shell sed -n 's/^#define[ \t]\+DL_VER_PATCH[ \t]\+\([0-9]\+\).*/\1/p' version.h)
VER       := $(VER_MAJOR).$(VER_MINOR).$(VER_PATCH)
ifeq ($(VER),..)
$(error 无法从 version.h 解析版本号 —— 检查 DL_VER_MAJOR/MINOR/PATCH 的写法)
endif
EXE_NAME  := dialLog_v$(VER).exe
TARGET    ?= $(BUILD_DIR)/$(EXE_NAME)

# -municode      : 使用 wWinMain 入口
# -mwindows      : GUI 子系统(不弹控制台)
# 字符集三件套   : 源码 UTF-8;窄串按 UTF-8 存;宽串按 UTF-16LE 存(Windows wchar_t 为 2 字节)
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -municode \
            -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE

LDFLAGS  := -mwindows -municode -static -static-libgcc -static-libstdc++ -s
LIBS     := -lcomctl32 -lgdi32 -lcomdlg32 -lshell32 -luser32 -lkernel32

# 压缩包直读(.zip/.tar.gz)靠内嵌 miniz(MIT,纯 C 单文件,静态编入,零运行时依赖)。
# 定义 DL_HAVE_MINIZ 后 logmodel 才编入解压实现;不定义则解压函数返回"未编入"。
# MINIZ_NO_STDIO/NO_TIME:只用内存解压,砍掉文件 IO 与时间戳依赖,减小体积。
MINIZ_DEF := -DDL_HAVE_MINIZ
MINIZ_CFLAGS := -std=c11 -O2 -DMINIZ_NO_STDIO -DMINIZ_NO_TIME

OBJS := $(BUILD_DIR)/ui.o $(BUILD_DIR)/logmodel.o \
        $(BUILD_DIR)/miniz.o $(BUILD_DIR)/resource.o
TEST_BINS := selftest simtest hostruntest baselinetest mergetest archivetest boundarytest

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)
	@echo "==> 生成 $@ (静态链接,无运行时依赖)"

$(BUILD_DIR)/ui.o: ui.cpp logmodel.h version.h theme.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(MINIZ_DEF) -c $< -o $@

$(BUILD_DIR)/logmodel.o: logmodel.cpp logmodel.h miniz.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(MINIZ_DEF) -c $< -o $@

$(BUILD_DIR)/miniz.o: miniz.c miniz.h | $(BUILD_DIR)
	$(CC) $(MINIZ_CFLAGS) -c $< -o $@

$(BUILD_DIR)/resource.o: resource.rc app.manifest version.h dialLog.ico | $(BUILD_DIR)
	$(WINDRES) -c 65001 $< -O coff -o $@

# 双架构便捷入口。即使连续执行也只会复用各自目录里的正确对象。
windows-x64:
	$(MAKE) CROSS=x86_64-w64-mingw32- BUILD_FLAVOR=x64 \
		TARGET=$(BUILD_ROOT)/x64/$(EXE_NAME) all

windows-x86:
	$(MAKE) CROSS=i686-w64-mingw32- BUILD_FLAVOR=x86 \
		TARGET=$(BUILD_ROOT)/x86/$(EXE_NAME) all

windows-all: windows-x64 windows-x86

# 仓库根目录只保留一个供直接取用的正式 x64 exe。日常构建不碰它。
release: windows-x64
	cp $(BUILD_ROOT)/x64/$(EXE_NAME) $(EXE_NAME)
	@echo "==> 发布产物 $(EXE_NAME)"

# 解析层自测:logmodel 不含 Win32 依赖。普通测试共享同一个 host 对象,
# 避免七个测试各自重复编译 1476 行的 logmodel.cpp。
logmodel_host.o: logmodel.cpp logmodel.h
	$(HOST_CXX) $(HOST_CXXFLAGS) -c logmodel.cpp -o $@

logmodel_archive_host.o: logmodel.cpp logmodel.h miniz.h
	$(HOST_CXX) $(HOST_CXXFLAGS) -DDL_HAVE_MINIZ -c logmodel.cpp -o $@

selftest: selftest.cpp logmodel_host.o
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

# 场景模拟器 + 结论引擎断言测试(只验结论引擎,验不了解析器 —— 见 simtest.cpp 顶部说明)
simtest: simtest.cpp logmodel_host.o
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

# 对**真代码产出**的日志(sim/hostrun*/)做结论断言 —— 证据等级比 simtest 的手写日志高一档
hostruntest: hostruntest.cpp logmodel_host.o
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

# 真机日志基线断言:把真机上的**具体数字**钉死 —— 变异测试证明"只验结论出现"没牙齿
baselinetest: baselinetest.cpp logmodel_host.o
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

# 多文件合并定序断言测试:真机日志切分打乱→定序→逐行还原(最有牙齿的一层在这)
mergetest: mergetest.cpp logmodel_host.o
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

# 压缩包直读 + BOM 剥离断言测试:host 侧也编入 miniz(它是可移植 C,Linux 能编),
# 因此解压逻辑完全可单元测试,不依赖 Windows。miniz_host.o 与交叉编译的 miniz.o 分开。
miniz_host.o: miniz.c miniz.h
	$(HOST_CC) -std=c11 -O2 -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -c miniz.c -o miniz_host.o

archivetest: archivetest.cpp logmodel_archive_host.o miniz_host.o
	$(HOST_CXX) $(HOST_CXXFLAGS) -DDL_HAVE_MINIZ -o $@ $^ $(HOST_LDFLAGS)

# 跨文件续行防御 + 时钟跳变检测断言测试(问题②机制实证/问题①无真机样本,见文件头声明)
boundarytest: boundarytest.cpp logmodel_host.o
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

# 日常唯一回归入口。变异测试耗时较长,单独放在 check-full。
check: $(TEST_BINS)
	./selftest samples/rtms_eg25/dial_20260630_000026.log
	./simtest
	./hostruntest
	./baselinetest
	./mergetest
	./archivetest
	./boundarytest

check-full: check
	python3 sim/mutate.py

clean:
	rm -rf build
	rm -f logmodel_host.o logmodel_archive_host.o miniz_host.o $(TEST_BINS)

version:
	@echo $(VER)

.PHONY: all clean version check check-full windows-x64 windows-x86 windows-all release
