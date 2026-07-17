# Makefile — dialLog (Win32 原生 GUI, MinGW)
#
# 交叉编译(Linux 上生成 Windows exe,本仓库默认):
#     make
# Windows 本机 MinGW 编译:
#     mingw32-make CXX=g++ WINDRES=windres
# 32 位:
#     make CXX=i686-w64-mingw32-g++ WINDRES=i686-w64-mingw32-windres
#
# 产物 dialLog.exe 为静态链接,不依赖任何 MinGW/MSVC 运行时 DLL,拷到 Windows 双击即用。

# 注意:用 := 而非 ?=。本机环境常导出 CC/CXX(RK3576/buildroot 交叉链),
# ?= 对“已由环境定义”的变量不生效,会误用 aarch64 编译器。命令行 `make CXX=g++` 仍可覆盖。
CXX     := x86_64-w64-mingw32-g++
WINDRES := x86_64-w64-mingw32-windres
TARGET  := dialLog.exe

# -municode      : 使用 wWinMain 入口
# -mwindows      : GUI 子系统(不弹控制台)
# 字符集三件套   : 源码 UTF-8;窄串按 UTF-8 存;宽串按 UTF-16LE 存(Windows wchar_t 为 2 字节)
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -municode \
            -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE

LDFLAGS  := -mwindows -municode -static -static-libgcc -static-libstdc++ -s
LIBS     := -lcomctl32 -lgdi32 -lcomdlg32 -lshell32 -luser32 -lkernel32

OBJS := ui.o logmodel.o resource.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)
	@echo "==> 生成 $@ (静态链接,无运行时依赖)"

ui.o: ui.cpp logmodel.h version.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

logmodel.o: logmodel.cpp logmodel.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

resource.o: resource.rc app.manifest version.h
	$(WINDRES) -c 65001 $< -O coff -o $@

# 解析层自测(用本机 g++ 编译运行,与 tools/diallog.py 对拍)
selftest: selftest.cpp logmodel.cpp logmodel.h
	g++ -std=c++17 -O2 -Wall -Wextra -o selftest selftest.cpp logmodel.cpp

clean:
	rm -f $(OBJS) $(TARGET) selftest

.PHONY: all clean
