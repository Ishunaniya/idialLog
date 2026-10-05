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

# 本地可清理输出目录，不是发布程序的运行依赖；删除后构建/测试会重新生成。
# 文档引用的历史日志、截图及 Wine 测试配置不会由编译原样恢复，留档前请勿清理。
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
LIBS     := -lcomctl32 -lgdiplus -lgdi32 -lcomdlg32 -lshell32 -ldwmapi -luxtheme -ladvapi32 -luser32 -lkernel32

MINIZ_DEF := -DDL_HAVE_MINIZ
MINIZ_CFLAGS := -std=c11 -O2 -DMINIZ_NO_STDIO -DMINIZ_NO_TIME

CORE_NAMES := json_value log_time log_parser log_analysis log_filter archive_reader
APP_NAMES := workspace_state document_state app_context
PRESENTATION_NAMES := tablemodel chartmodel report_chart report_interaction text_catalog incidentmodel incident_export
WIN32_NAMES := workspace_window ui modern_shell ui_pages overview_page chart_page load_controller app_settings win_file_io win_text source_workspace incident_review text_view

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

HOST_CORE_BASE_NAMES := json_value log_time log_parser log_analysis log_filter
HOST_CORE_BASE_OBJS := $(addprefix $(HOST_BUILD_DIR)/,$(addsuffix .o,$(HOST_CORE_BASE_NAMES)))
HOST_ARCHIVE_STUB_OBJ := $(HOST_BUILD_DIR)/archive_reader.o
HOST_ARCHIVE_FULL_OBJ := $(HOST_BUILD_DIR)/archive_reader_miniz.o
HOST_CORE_OBJS := $(HOST_CORE_BASE_OBJS) $(HOST_ARCHIVE_STUB_OBJ)
HOST_ARCHIVE_OBJS := $(HOST_CORE_BASE_OBJS) $(HOST_ARCHIVE_FULL_OBJ)
HOST_TABLE_OBJ := $(HOST_BUILD_DIR)/tablemodel.o
HOST_LOCALE_OBJ := $(HOST_BUILD_DIR)/text_catalog.o
HOST_CHART_OBJ := $(HOST_BUILD_DIR)/chartmodel.o
HOST_REPORT_CHART_OBJ := $(HOST_BUILD_DIR)/report_chart.o $(HOST_BUILD_DIR)/report_interaction.o
HOST_WORKSPACE_OBJ := $(HOST_BUILD_DIR)/workspace_state.o
HOST_DOCUMENT_OBJ := $(HOST_BUILD_DIR)/document_state.o
HOST_INCIDENT_OBJS := $(HOST_BUILD_DIR)/incidentmodel.o $(HOST_BUILD_DIR)/incident_export.o
HOST_MINIZ_OBJ := $(HOST_BUILD_DIR)/miniz.o
HOST_DEPS := $(HOST_LOCALE_OBJ:.o=.d) $(HOST_CORE_OBJS:.o=.d) $(HOST_ARCHIVE_FULL_OBJ:.o=.d) \
             $(HOST_INCIDENT_OBJS:.o=.d) $(HOST_TABLE_OBJ:.o=.d) $(HOST_CHART_OBJ:.o=.d) $(HOST_REPORT_CHART_OBJ:.o=.d) $(HOST_DOCUMENT_OBJ:.o=.d) $(HOST_WORKSPACE_OBJ:.o=.d) $(HOST_MINIZ_OBJ:.o=.d)

UNIT_BIN_DIR := $(HOST_TEST_DIR)/unit
REGRESSION_BIN_DIR := $(HOST_TEST_DIR)/regression
PERF_BIN_DIR := $(HOST_TEST_DIR)/performance

INCIDENTTEST_BIN := $(UNIT_BIN_DIR)/incidenttest
SELFTEST_BIN := $(UNIT_BIN_DIR)/selftest
ARCHIVETEST_BIN := $(UNIT_BIN_DIR)/archivetest
BOUNDARYTEST_BIN := $(UNIT_BIN_DIR)/boundarytest
TABLETEST_BIN := $(UNIT_BIN_DIR)/tabletest
CHARTTEST_BIN := $(UNIT_BIN_DIR)/charttest
REPORTCHARTTEST_BIN := $(UNIT_BIN_DIR)/reportcharttest
LOCALETEST_BIN := $(UNIT_BIN_DIR)/localetest
DOCUMENTTEST_BIN := $(UNIT_BIN_DIR)/documenttest
MODEMV2PARSERTEST_BIN := $(UNIT_BIN_DIR)/modem_v2_parser_test
SIMTEST_BIN := $(REGRESSION_BIN_DIR)/simtest
HOSTRUNTEST_BIN := $(REGRESSION_BIN_DIR)/hostruntest
BASELINETEST_BIN := $(REGRESSION_BIN_DIR)/baselinetest
MERGETEST_BIN := $(REGRESSION_BIN_DIR)/mergetest
MODEMV2TEST_BIN := $(REGRESSION_BIN_DIR)/modemv2test
ARTERYTEST_BIN := $(REGRESSION_BIN_DIR)/arterytest
RK3506JTEST_BIN := $(REGRESSION_BIN_DIR)/rk3506jtest
PERF_BIN := $(PERF_BIN_DIR)/perftest
UI_SMOKE_BIN := $(HOST_TEST_DIR)/ui/smoke.exe
UI_WINEPREFIX ?= $(abspath $(BUILD_ROOT)/wine-smoke)
WORKSPACETEST_BIN := $(UNIT_BIN_DIR)/workspacetest
TEST_BINS := $(WORKSPACETEST_BIN) $(INCIDENTTEST_BIN) $(SELFTEST_BIN) $(SIMTEST_BIN) $(HOSTRUNTEST_BIN) $(BASELINETEST_BIN) \
             $(MERGETEST_BIN) $(MODEMV2TEST_BIN) $(ARCHIVETEST_BIN) $(BOUNDARYTEST_BIN) \
             $(TABLETEST_BIN) $(CHARTTEST_BIN) $(REPORTCHARTTEST_BIN) $(DOCUMENTTEST_BIN) $(LOCALETEST_BIN) $(MODEMV2PARSERTEST_BIN) $(RK3506JTEST_BIN) $(ARTERYTEST_BIN)
TEST_TARGETS := incidenttest  selftest simtest hostruntest baselinetest mergetest \
                modemv2test modemv2parsertest archivetest boundarytest tabletest charttest reportcharttest \
                documenttest perftest rk3506jtest arterytest

$(HOST_BUILD_DIR) $(UNIT_BIN_DIR) $(REGRESSION_BIN_DIR) $(PERF_BIN_DIR) $(HOST_TEST_DIR)/ui:
	mkdir -p $@

$(UI_SMOKE_BIN): $(TEST_UI_DIR)/smoke.cpp version.h | $(HOST_TEST_DIR)/ui
	x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -municode -mwindows -static \
		-static-libgcc -static-libstdc++ -o $@ $< -lcomctl32 -lshell32 -luser32 -lkernel32 -lgdi32

$(HOST_BUILD_DIR)/archive_reader_miniz.o: $(CORE_DIR)/archive_reader.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) $(MINIZ_DEF) -c $< -o $@

$(HOST_BUILD_DIR)/%.o: $(CORE_DIR)/%.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(HOST_INCIDENT_OBJS): $(HOST_BUILD_DIR)/%.o: $(PRESENTATION_DIR)/%.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(HOST_TABLE_OBJ): $(PRESENTATION_DIR)/tablemodel.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(HOST_CHART_OBJ): $(PRESENTATION_DIR)/chartmodel.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(HOST_REPORT_CHART_OBJ): $(HOST_BUILD_DIR)/%.o: $(PRESENTATION_DIR)/%.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(HOST_LOCALE_OBJ): $(PRESENTATION_DIR)/text_catalog.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(HOST_DOCUMENT_OBJ): $(APP_DIR)/document_state.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(HOST_MINIZ_OBJ): $(THIRD_PARTY_DIR)/miniz.c $(THIRD_PARTY_DIR)/miniz.h | $(HOST_BUILD_DIR)
	$(HOST_CC) -std=c11 -O2 -DMINIZ_NO_STDIO -DMINIZ_NO_TIME $(DEPFLAGS) -c $< -o $@

-include $(HOST_DEPS)

$(HOST_WORKSPACE_OBJ): $(APP_DIR)/workspace_state.cpp | $(HOST_BUILD_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(WORKSPACETEST_BIN): $(TEST_UNIT_DIR)/workspacetest.cpp version.h $(HOST_WORKSPACE_OBJ) $(HOST_DOCUMENT_OBJ) $(HOST_INCIDENT_OBJS) $(HOST_REPORT_CHART_OBJ) $(HOST_CHART_OBJ) $(HOST_TABLE_OBJ) $(HOST_LOCALE_OBJ) $(HOST_CORE_OBJS) $(HOST_MINIZ_OBJ) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $(filter-out version.h,$^) $(HOST_LDFLAGS)

$(INCIDENTTEST_BIN): $(TEST_UNIT_DIR)/incidenttest.cpp version.h $(HOST_WORKSPACE_OBJ) $(HOST_CHART_OBJ) $(HOST_INCIDENT_OBJS) $(HOST_TABLE_OBJ) $(HOST_LOCALE_OBJ) $(HOST_CORE_OBJS) $(HOST_MINIZ_OBJ) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $(filter-out version.h,$^) $(HOST_LDFLAGS)

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

$(MODEMV2TEST_BIN): $(TEST_REGRESSION_DIR)/modemv2test.cpp $(HOST_CORE_OBJS) | $(REGRESSION_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(ARTERYTEST_BIN): $(TEST_REGRESSION_DIR)/arterytest.cpp $(HOST_CORE_OBJS) | $(REGRESSION_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(RK3506JTEST_BIN): $(TEST_REGRESSION_DIR)/rk3506jtest.cpp $(HOST_CORE_OBJS) $(HOST_TABLE_OBJ) | $(REGRESSION_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(ARCHIVETEST_BIN): $(TEST_UNIT_DIR)/archivetest.cpp $(HOST_ARCHIVE_OBJS) $(HOST_MINIZ_OBJ) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) $(MINIZ_DEF) -o $@ $^ $(HOST_LDFLAGS)

$(BOUNDARYTEST_BIN): $(TEST_UNIT_DIR)/boundarytest.cpp $(HOST_CORE_OBJS) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(TABLETEST_BIN): $(TEST_UNIT_DIR)/tabletest.cpp $(HOST_TABLE_OBJ) $(HOST_CORE_OBJS) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(CHARTTEST_BIN): $(TEST_UNIT_DIR)/charttest.cpp $(HOST_CHART_OBJ) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(REPORTCHARTTEST_BIN): $(TEST_UNIT_DIR)/reportcharttest.cpp $(HOST_REPORT_CHART_OBJ) $(HOST_CHART_OBJ) $(HOST_CORE_OBJS) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(LOCALETEST_BIN): $(TEST_UNIT_DIR)/localetest.cpp $(HOST_LOCALE_OBJ) $(HOST_CORE_OBJS) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(DOCUMENTTEST_BIN): $(TEST_UNIT_DIR)/documenttest.cpp $(HOST_DOCUMENT_OBJ) $(HOST_CORE_OBJS) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(MODEMV2PARSERTEST_BIN): $(TEST_UNIT_DIR)/modem_v2_parser_test.cpp $(HOST_CORE_OBJS) | $(UNIT_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

$(PERF_BIN): $(TEST_PERF_DIR)/perftest.cpp $(HOST_CHART_OBJ) $(HOST_TABLE_OBJ) $(HOST_CORE_OBJS) | $(PERF_BIN_DIR)
	$(HOST_CXX) $(HOST_CPPFLAGS) $(HOST_CXXFLAGS) -o $@ $^ $(HOST_LDFLAGS)

incidenttest: $(INCIDENTTEST_BIN)
selftest: $(SELFTEST_BIN)
simtest: $(SIMTEST_BIN)
hostruntest: $(HOSTRUNTEST_BIN)
baselinetest: $(BASELINETEST_BIN)
mergetest: $(MERGETEST_BIN)
modemv2test: $(MODEMV2TEST_BIN)
arterytest: $(ARTERYTEST_BIN)
rk3506jtest: $(RK3506JTEST_BIN)
modemv2parsertest: $(MODEMV2PARSERTEST_BIN)
archivetest: $(ARCHIVETEST_BIN)
boundarytest: $(BOUNDARYTEST_BIN)
tabletest: $(TABLETEST_BIN)
charttest: $(CHARTTEST_BIN)
reportcharttest: $(REPORTCHARTTEST_BIN)
documenttest: $(DOCUMENTTEST_BIN)
perftest: $(PERF_BIN)

perf: $(PERF_BIN)
	$(PERF_BIN)

check: $(TEST_BINS)
	python3 tools/build_text_catalog.py --check
	python3 tests/unit/sourceaudit_test.py
	$(SELFTEST_BIN) samples/rtms_eg25/dial_20260630_000026.log
	$(SIMTEST_BIN)
	$(HOSTRUNTEST_BIN)
	$(BASELINETEST_BIN)
	$(MERGETEST_BIN)
	$(MODEMV2TEST_BIN)
	$(MODEMV2PARSERTEST_BIN)
	$(RK3506JTEST_BIN)
	$(ARTERYTEST_BIN)
	$(ARCHIVETEST_BIN)
	$(BOUNDARYTEST_BIN)
	$(TABLETEST_BIN)
	$(CHARTTEST_BIN)
	$(REPORTCHARTTEST_BIN)
	python3 tests/unit/report_chart_test.py
	$(DOCUMENTTEST_BIN)
	$(LOCALETEST_BIN)
	$(INCIDENTTEST_BIN)
	$(WORKSPACETEST_BIN)
	python3 tests/unit/workspace_package_test.py
	python3 tests/unit/incident_export_test.py

check-full: check
	python3 sim/mutate.py

ui-smoke: windows-x64 $(UI_SMOKE_BIN)
	WINEPREFIX=$(UI_WINEPREFIX) xvfb-run -a wine $(UI_SMOKE_BIN) \
		$(abspath $(BUILD_ROOT)/x64/$(EXE_NAME)) $(abspath samples/rtms_eg25/dial_20260630_000026.log)

# 默认清理 build/；包含其中的日志、截图、Wine 配置和备份，根目录发布 exe 不受影响。
clean:
	rm -rf $(BUILD_ROOT)

version:
	@echo $(VER)

.PHONY: all clean version check check-full perf ui-smoke windows-x64 windows-x86 windows-all release $(TEST_TARGETS)
