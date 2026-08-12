# Makefile — dialLog (Win32 原生 GUI, MinGW)
#
#     make               # build/x64/dialLog_vX.Y.Z.exe
#     make windows-all   # 同时构建 x64 / x86
#     make check         # 本机完整日常回归
#     make check-full    # 日常回归 + 变异测试
#     make perf          # 百万行性能基准

CROSS   ?= x86_64-w64-mingw32-
CXX     := $(CROSS)g++
CC      := $(CROSS)gcc
WINDRES := $(CROSS)windres

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
HOST_BUILD_DIR := $(BUILD_ROOT)/host
HOST_TEST_DIR := $(BUILD_ROOT)/tests

CORE_DIR := src/core
APP_DIR := src/app
PRESENTATION_DIR := src/presentation
WIN32_DIR := src/win32
THIRD_PARTY_DIR := third_party/miniz
WINDOWS_RESOURCE_DIR := resources/windows
TEST_UNIT_DIR := tests/unit
TEST_REGRESSION_DIR := tests/regression
TEST_PERF_DIR := tests/performance
TEST_UI_DIR := tests/ui

INCLUDE_DIRS := -I$(CORE_DIR) -I$(APP_DIR) -I$(PRESENTATION_DIR) -I$(WIN32_DIR) \
                -I$(THIRD_PARTY_DIR) -I.
CPPFLAGS := $(INCLUDE_DIRS)
DEPFLAGS := -MMD -MP

HOST_CXX      := g++
HOST_CC       := gcc
HOST_CPPFLAGS := $(INCLUDE_DIRS)
HOST_CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
HOST_LDFLAGS  :=

VER_MAJOR := $(shell sed -n 's/^#define[ \t]\+DL_VER_MAJOR[ \t]\+\([0-9]\+\).*/\1/p' version.h)
VER_MINOR := $(shell sed -n 's/^#define[ \t]\+DL_VER_MINOR[ \t]\+\([0-9]\+\).*/\1/p' version.h)
VER_PATCH := $(shell sed -n 's/^#define[ \t]\+DL_VER_PATCH[ \t]\+\([0-9]\+\).*/\1/p' version.h)
VER       := $(VER_MAJOR).$(VER_MINOR).$(VER_PATCH)
ifeq ($(VER),..)
$(error 无法从 version.h 解析版本号 —— 检查 DL_VER_MAJOR/MINOR/PATCH 的写法)
endif

EXE_NAME := dialLog_v$(VER).exe
TARGET   ?= $(BUILD_DIR)/$(EXE_NAME)

CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -municode \
            -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE
LDFLAGS  := -mwindows -municode -static -static-libgcc -static-libstdc++ -s
LIBS     := -lcomctl32 -lgdi32 -lcomdlg32 -lshell32 -ldwmapi -luxtheme -ladvapi32 -luser32 -lkernel32

MINIZ_DEF := -DDL_HAVE_MINIZ
MINIZ_CFLAGS := -std=c11 -O2 -DMINIZ_NO_STDIO -DMINIZ_NO_TIME

CORE_NAMES := log_time log_parser log_analysis log_filter archive_reader
APP_NAMES := document_state app_context
PRESENTATION_NAMES := tablemodel chartmodel
WIN32_NAMES := ui modern_shell ui_pages overview_page chart_page load_controller app_settings win_file_io win_text

CORE_OBJS := $(addprefix $(BUILD_DIR)/core/,$(addsuffix .o,$(CORE_NAMES)))
APP_OBJS := $(addprefix $(BUILD_DIR)/app/,$(addsuffix .o,$(APP_NAMES)))
PRESENTATION_OBJS := $(addprefix $(BUILD_DIR)/presentation/,$(addsuffix .o,$(PRESENTATION_NAMES)))
WIN32_OBJS := $(addprefix $(BUILD_DIR)/win32/,$(addsuffix .o,$(WIN32_NAMES)))
MINIZ_OBJ := $(BUILD_DIR)/third_party/miniz.o
RESOURCE_OBJ := $(BUILD_DIR)/resource.o
OBJS := $(WIN32_OBJS) $(APP_OBJS) $(CORE_OBJS) $(PRESENTATION_OBJS) $(MINIZ_OBJ) $(RESOURCE_OBJ)
DEPS := $(filter %.d,$(OBJS:.o=.d))

all: $(TARGET)

$(BUILD_DIR)/core $(BUILD_DIR)/app $(BUILD_DIR)/presentation $(BUILD_DIR)/win32 $(BUILD_DIR)/third_party:
	mkdir -p $@

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)
	@echo "==> 生成 $@ (静态链接,无运行时依赖)"

$(BUILD_DIR)/core/archive_reader.o: $(CORE_DIR)/archive_reader.cpp | $(BUILD_DIR)/core
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) $(MINIZ_DEF) -c $< -o $@

$(BUILD_DIR)/core/%.o: $(CORE_DIR)/%.cpp | $(BUILD_DIR)/core
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/app/%.o: $(APP_DIR)/%.cpp | $(BUILD_DIR)/app
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/presentation/%.o: $(PRESENTATION_DIR)/%.cpp | $(BUILD_DIR)/presentation
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILD_DIR)/win32/%.o: $(WIN32_DIR)/%.cpp | $(BUILD_DIR)/win32
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(MINIZ_OBJ): $(THIRD_PARTY_DIR)/miniz.c $(THIRD_PARTY_DIR)/miniz.h | $(BUILD_DIR)/third_party
	$(CC) $(MINIZ_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(RESOURCE_OBJ): $(WINDOWS_RESOURCE_DIR)/resource.rc \
                 $(WINDOWS_RESOURCE_DIR)/app.manifest \
                 $(WINDOWS_RESOURCE_DIR)/dialLog.ico version.h
	mkdir -p $(dir $@)
	$(WINDRES) -I. -c 65001 $< -O coff -o $@

-include $(DEPS)

windows-x64:
	$(MAKE) CROSS=x86_64-w64-mingw32- BUILD_FLAVOR=x64 \
		TARGET=$(BUILD_ROOT)/x64/$(EXE_NAME) all

windows-x86:
	$(MAKE) CROSS=i686-w64-mingw32- BUILD_FLAVOR=x86 \
		TARGET=$(BUILD_ROOT)/x86/$(EXE_NAME) all

windows-all: windows-x64 windows-x86

release: windows-x64
	cp $(BUILD_ROOT)/x64/$(EXE_NAME) $(EXE_NAME)
	@echo "==> 发布产物 $(EXE_NAME)"

HOST_CORE_BASE_NAMES := log_time log_parser log_analysis log_filter
HOST_CORE_BASE_OBJS := $(addprefix $(HOST_BUILD_DIR)/,$(addsuffix .o,$(HOST_CORE_BASE_NAMES)))
HOST_ARCHIVE_STUB_OBJ := $(HOST_BUILD_DIR)/archive_reader.o
HOST_ARCHIVE_FULL_OBJ := $(HOST_BUILD_DIR)/archive_reader_miniz.o
HOST_CORE_OBJS := $(HOST_CORE_BASE_OBJS) $(HOST_ARCHIVE_STUB_OBJ)
HOST_ARCHIVE_OBJS := $(HOST_CORE_BASE_OBJS) $(HOST_ARCHIVE_FULL_OBJ)
HOST_TABLE_OBJ := $(HOST_BUILD_DIR)/tablemodel.o
HOST_CHART_OBJ := $(HOST_BUILD_DIR)/chartmodel.o
HOST_DOCUMENT_OBJ := $(HOST_BUILD_DIR)/document_state.o
HOST_MINIZ_OBJ := $(HOST_BUILD_DIR)/miniz.o
HOST_DEPS := $(HOST_CORE_OBJS:.o=.d) $(HOST_ARCHIVE_FULL_OBJ:.o=.d) \
             $(HOST_TABLE_OBJ:.o=.d) $(HOST_CHART_OBJ:.o=.d) $(HOST_DOCUMENT_OBJ:.o=.d) $(HOST_MINIZ_OBJ:.o=.d)

UNIT_BIN_DIR := $(HOST_TEST_DIR)/unit
REGRESSION_BIN_DIR := $(HOST_TEST_DIR)/regression
PERF_BIN_DIR := $(HOST_TEST_DIR)/performance

SELFTEST_BIN := $(UNIT_BIN_DIR)/selftest
ARCHIVETEST_BIN := $(UNIT_BIN_DIR)/archivetest
BOUNDARYTEST_BIN := $(UNIT_BIN_DIR)/boundarytest
TABLETEST_BIN := $(UNIT_BIN_DIR)/tabletest
CHARTTEST_BIN := $(UNIT_BIN_DIR)/charttest
DOCUMENTTEST_BIN := $(UNIT_BIN_DIR)/documenttest
SIMTEST_BIN := $(REGRESSION_BIN_DIR)/simtest
HOSTRUNTEST_BIN := $(REGRESSION_BIN_DIR)/hostruntest
BASELINETEST_BIN := $(REGRESSION_BIN_DIR)/baselinetest
MERGETEST_BIN := $(REGRESSION_BIN_DIR)/mergetest
PERF_BIN := $(PERF_BIN_DIR)/perftest
UI_SMOKE_BIN := $(HOST_TEST_DIR)/ui/smoke.exe
TEST_BINS := $(SELFTEST_BIN) $(SIMTEST_BIN) $(HOSTRUNTEST_BIN) $(BASELINETEST_BIN) \
             $(MERGETEST_BIN) $(ARCHIVETEST_BIN) $(BOUNDARYTEST_BIN) \
             $(TABLETEST_BIN) $(CHARTTEST_BIN) $(DOCUMENTTEST_BIN)
TEST_TARGETS := selftest simtest hostruntest baselinetest mergetest \
                archivetest boundarytest tabletest charttest documenttest perftest

$(HOST_BUILD_DIR) $(UNIT_BIN_DIR) $(REGRESSION_BIN_DIR) $(PERF_BIN_DIR) $(HOST_TEST_DIR)/ui:
	mkdir -p $@

$(UI_SMOKE_BIN): $(TEST_UI_DIR)/smoke.cpp | $(HOST_TEST_DIR)/ui
	x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -municode -mwindows -static \
		-static-libgcc -static-libstdc++ -o $@ $< -lcomctl32 -lshell32 -luser32 -lkernel32

$(HOST_BUILD_DIR)/archive_reader_miniz.o: $(CORE_DIR)/archive_reader.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) $(MINIZ_DEF) -c $< -o $@

$(HOST_BUILD_DIR)/%.o: $(CORE_DIR)/%.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(HOST_TABLE_OBJ): $(PRESENTATION_DIR)/tablemodel.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(HOST_CHART_OBJ): $(PRESENTATION_DIR)/chartmodel.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(HOST_DOCUMENT_OBJ): $(APP_DIR)/document_state.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(HOST_MINIZ_OBJ): $(THIRD_PARTY_DIR)/miniz.c $(THIRD_PARTY_DIR)/miniz.h | $(HOST_BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -DMINIZ_NO_STDIO -DMINIZ_NO_TIME $(DEPFLAGS) -c $< -o $@

-include $(HOST_DEPS)

$(SELFTEST_BIN): $(TEST_UNIT_DIR)/selftest.cpp $(HOST_CORE_OBJS) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(SIMTEST_BIN): $(TEST_REGRESSION_DIR)/simtest.cpp $(HOST_CORE_OBJS) | $(REGRESSION_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(HOSTRUNTEST_BIN): $(TEST_REGRESSION_DIR)/hostruntest.cpp $(HOST_CORE_OBJS) | $(REGRESSION_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(BASELINETEST_BIN): $(TEST_REGRESSION_DIR)/baselinetest.cpp $(HOST_CORE_OBJS) | $(REGRESSION_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(MERGETEST_BIN): $(TEST_REGRESSION_DIR)/mergetest.cpp $(HOST_CORE_OBJS) | $(REGRESSION_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(ARCHIVETEST_BIN): $(TEST_UNIT_DIR)/archivetest.cpp $(HOST_ARCHIVE_OBJS) $(HOST_MINIZ_OBJ) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(MINIZ_DEF) -o $@ $^ $(HOST_LDFLAGS)

$(BOUNDARYTEST_BIN): $(TEST_UNIT_DIR)/boundarytest.cpp $(HOST_CORE_OBJS) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(TABLETEST_BIN): $(TEST_UNIT_DIR)/tabletest.cpp $(HOST_TABLE_OBJ) $(HOST_CORE_OBJS) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(CHARTTEST_BIN): $(TEST_UNIT_DIR)/charttest.cpp $(HOST_CHART_OBJ) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(DOCUMENTTEST_BIN): $(TEST_UNIT_DIR)/documenttest.cpp $(HOST_DOCUMENT_OBJ) $(HOST_CORE_OBJS) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(PERF_BIN): $(TEST_PERF_DIR)/perftest.cpp $(HOST_CHART_OBJ) $(HOST_TABLE_OBJ) $(HOST_CORE_OBJS) | $(PERF_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

selftest: $(SELFTEST_BIN)
simtest: $(SIMTEST_BIN)
hostruntest: $(HOSTRUNTEST_BIN)
baselinetest: $(BASELINETEST_BIN)
mergetest: $(MERGETEST_BIN)
archivetest: $(ARCHIVETEST_BIN)
boundarytest: $(BOUNDARYTEST_BIN)
tabletest: $(TABLETEST_BIN)
charttest: $(CHARTTEST_BIN)
documenttest: $(DOCUMENTTEST_BIN)
perftest: $(PERF_BIN)

perf: $(PERF_BIN)
	$(PERF_BIN)

check: $(TEST_BINS)
	$(SELFTEST_BIN) samples/rtms_eg25/dial_20260630_000026.log
	$(SIMTEST_BIN)
	$(HOSTRUNTEST_BIN)
	$(BASELINETEST_BIN)
	$(MERGETEST_BIN)
	$(ARCHIVETEST_BIN)
	$(BOUNDARYTEST_BIN)
	$(TABLETEST_BIN)
	$(CHARTTEST_BIN)
	$(DOCUMENTTEST_BIN)

check-full: check
	python3 sim/mutate.py

ui-smoke: windows-x64 $(UI_SMOKE_BIN)
	WINEPREFIX=$(abspath $(BUILD_ROOT)/wine-smoke) xvfb-run -a wine $(UI_SMOKE_BIN) \
		$(abspath $(BUILD_ROOT)/x64/$(EXE_NAME)) $(abspath samples/rtms_eg25/dial_20260630_000026.log)

clean:
	rm -rf $(BUILD_ROOT)

version:
	@echo $(VER)

.PHONY: all clean version check check-full perf ui-smoke windows-x64 windows-x86 windows-all release $(TEST_TARGETS)
