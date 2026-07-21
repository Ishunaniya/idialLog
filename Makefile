# Makefile — dialLog (Win32 原生 GUI, MinGW)
#
# 交叉编译(Linux 上生成 Windows exe,本仓库默认):
#     make
# Windows 本机 MinGW 编译:
#     mingw32-make CXX=g++ WINDRES=windres
# 32 位:
#     make CXX=i686-w64-mingw32-g++ WINDRES=i686-w64-mingw32-windres
#
# 产物 dialLog_vX.Y.Z.exe 为静态链接,不依赖任何 MinGW/MSVC 运行时 DLL,拷到 Windows 双击即用。
# 文件名自带版本号 —— 发给别人/存档时不会搞混是哪个 build。

# 注意:用 := 而非 ?=。本机环境常导出 CC/CXX(RK3576/buildroot 交叉链),
# ?= 对“已由环境定义”的变量不生效,会误用 aarch64 编译器。命令行 `make CXX=g++` 仍可覆盖。
CXX     := x86_64-w64-mingw32-g++
WINDRES := x86_64-w64-mingw32-windres

# 版本号从 version.h 解析,保持单一来源:改 version.h 即同时改变
# exe 文件名、exe 版本资源(右键属性)、标题栏,三者永远一致。
VER_MAJOR := $(shell sed -n 's/^#define[ \t]\+DL_VER_MAJOR[ \t]\+\([0-9]\+\).*/\1/p' version.h)
VER_MINOR := $(shell sed -n 's/^#define[ \t]\+DL_VER_MINOR[ \t]\+\([0-9]\+\).*/\1/p' version.h)
VER_PATCH := $(shell sed -n 's/^#define[ \t]\+DL_VER_PATCH[ \t]\+\([0-9]\+\).*/\1/p' version.h)
VER       := $(VER_MAJOR).$(VER_MINOR).$(VER_PATCH)
ifeq ($(VER),..)
$(error 无法从 version.h 解析版本号 —— 检查 DL_VER_MAJOR/MINOR/PATCH 的写法)
endif
TARGET    := dialLog_v$(VER).exe

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
CC       := x86_64-w64-mingw32-gcc

OBJS := ui.o logmodel.o miniz.o resource.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)
	@echo "==> 生成 $@ (静态链接,无运行时依赖)"

ui.o: ui.cpp logmodel.h version.h
	$(CXX) $(CXXFLAGS) $(MINIZ_DEF) -c $< -o $@

logmodel.o: logmodel.cpp logmodel.h miniz.h
	$(CXX) $(CXXFLAGS) $(MINIZ_DEF) -c $< -o $@

miniz.o: miniz.c miniz.h
	$(CC) $(MINIZ_CFLAGS) -c $< -o $@

resource.o: resource.rc app.manifest version.h dialLog.ico
	$(WINDRES) -c 65001 $< -O coff -o $@

# 解析层自测:logmodel 不含 Win32 依赖,用本机 g++ 直接编译运行
selftest: selftest.cpp logmodel.cpp logmodel.h
	g++ -std=c++17 -O2 -Wall -Wextra -o selftest selftest.cpp logmodel.cpp

# 用 dialLog_v*.exe 通配:升版本后旧版本的 exe 也一并清掉,不留残留
# 场景模拟器 + 结论引擎断言测试(只验结论引擎,验不了解析器 —— 见 simtest.cpp 顶部说明)
simtest: simtest.cpp logmodel.cpp logmodel.h
	g++ -std=c++17 -O2 -Wall -Wextra -o simtest simtest.cpp logmodel.cpp

# 对**真代码产出**的日志(sim/hostrun*/)做结论断言 —— 证据等级比 simtest 的手写日志高一档
hostruntest: hostruntest.cpp logmodel.cpp logmodel.h
	g++ -std=c++17 -O2 -Wall -Wextra -o hostruntest hostruntest.cpp logmodel.cpp

# 真机日志基线断言:把真机上的**具体数字**钉死 —— 变异测试证明"只验结论出现"没牙齿
baselinetest: baselinetest.cpp logmodel.cpp logmodel.h
	g++ -std=c++17 -O2 -Wall -Wextra -o baselinetest baselinetest.cpp logmodel.cpp

# 多文件合并定序断言测试:真机日志切分打乱→定序→逐行还原(最有牙齿的一层在这)
mergetest: mergetest.cpp logmodel.cpp logmodel.h
	g++ -std=c++17 -O2 -Wall -Wextra -o mergetest mergetest.cpp logmodel.cpp

# 压缩包直读 + BOM 剥离断言测试:host 侧也编入 miniz(它是可移植 C,Linux 能编),
# 因此解压逻辑完全可单元测试,不依赖 Windows。miniz_host.o 与交叉编译的 miniz.o 分开。
miniz_host.o: miniz.c miniz.h
	gcc -std=c11 -O2 -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -c miniz.c -o miniz_host.o

archivetest: archivetest.cpp logmodel.cpp logmodel.h miniz_host.o
	g++ -std=c++17 -O2 -Wall -Wextra -DDL_HAVE_MINIZ -o archivetest archivetest.cpp logmodel.cpp miniz_host.o

# 跨文件续行防御 + 时钟跳变检测断言测试(问题②机制实证/问题①无真机样本,见文件头声明)
boundarytest: boundarytest.cpp logmodel.cpp logmodel.h
	g++ -std=c++17 -O2 -Wall -Wextra -o boundarytest boundarytest.cpp logmodel.cpp

clean:
	rm -f $(OBJS) miniz_host.o dialLog_v*.exe selftest simtest hostruntest baselinetest mergetest archivetest boundarytest

version:
	@echo $(VER)

.PHONY: all clean version
