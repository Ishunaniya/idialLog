// archivetest.cpp — 压缩包直读(extractArchive)+ BOM 剥离(stripBom)断言测试。
//
// ============================ 它能证明什么 ============================
// ✅ 用 Python 造的**真实压缩包**(.gz/.tar.gz/.zip,载荷是真机日志前若干行),
//    断言 miniz 解压出的字节能被 parseLines 正常解析、行数/首戳对得上。
//    压缩包由标准库(gzip/tarfile/zipfile)产出,不是我手搓的字节 —— 解得开就证明
//    我们的 gzip 头跳过 / tar 512 块拆分 / zip 中央目录遍历都对。
// ✅ stripBom:断言 BOM 被剥掉后首行恢复可解析(否则首戳判定失败,波及定序/时基)。
// ✅ archiveKindOf 按魔数(非扩展名)判格式;非压缩内容返回 ARC_NONE。
// ⚠️ 不覆盖损坏包 / 加密 zip / >4GB。现场日志包不会是这些形态,留待有样本再加。
//
// 构建运行:make archivetest && ./archivetest      (rc=0 全过)
#include "logmodel.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace dl;

static int g_fail = 0;
static void ok(bool c, const char* what) {
    std::printf("  %s %s\n", c ? "[通过]" : "[失败]", what);
    if (!c) g_fail++;
}

static std::string readFile(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// 从原始字节切成行(复刻 ui.cpp 的 SplitLines,含 stripBom),用于比对
static std::vector<std::string> splitLines(std::string buf) {
    stripBom(buf);
    std::vector<std::string> out;
    size_t a = 0;
    while (a <= buf.size()) {
        size_t b = buf.find('\n', a);
        if (b == std::string::npos) { if (a < buf.size()) out.push_back(buf.substr(a)); break; }
        out.push_back(buf.substr(a, b - a));
        a = b + 1;
    }
    return out;
}

// ============================ T1:魔数判定 ============================
static void t1_kind() {
    std::printf("== T1 archiveKindOf 按魔数判格式 ==\n");
    ok(archiveKindOf(std::string("\x1F\x8B\x08\x00", 4)) == ARC_GZIP, "1F 8B → ARC_GZIP");
    ok(archiveKindOf(std::string("PK\x03\x04", 4)) == ARC_ZIP, "PK\\x03\\x04 → ARC_ZIP");
    ok(archiveKindOf("[2026-06-30 00:00:00] normal log") == ARC_NONE, "普通日志 → ARC_NONE");
    ok(archiveKindOf("") == ARC_NONE, "空串 → ARC_NONE");
    // 扩展名像压缩包但内容不是 → 仍按内容判 NONE(魔数是事实,扩展名会骗人)
    ok(archiveKindOf("this is plain text not gzip") == ARC_NONE, "伪装内容按魔数仍判 NONE");
}

// ============================ T2:BOM 剥离 ============================
static void t2_bom() {
    std::printf("== T2 stripBom ==\n");
    std::string withBom = "\xEF\xBB\xBF[2026-06-30 00:00:26] head";
    std::string noBom    = "[2026-06-30 00:00:26] head";
    std::string a = withBom; stripBom(a);
    ok(a == noBom, "EF BB BF 被剥掉,内容不变");
    std::string b = noBom; stripBom(b);
    ok(b == noBom, "无 BOM 时不误删");
    std::string tiny = "\xEF\xBB"; stripBom(tiny);
    ok(tiny == "\xEF\xBB", "不足 3 字节不误动");

    // 真机夹具:带 BOM 的日志,剥离后首行必须能被 firstTimestamp 认出
    std::string bomFile = readFile("samples/archive/with_bom.log");
    ok(!bomFile.empty(), "with_bom.log 读到");
    auto lines = splitLines(bomFile);   // splitLines 内部已 stripBom
    long long t = 0;
    ok(!lines.empty() && firstTimestamp(lines, &t), "剥 BOM 后首行时间戳恢复可识别");
    ok(timeBaseOf(lines) == TB_WALL, "剥 BOM 后时基正确判为 TB_WALL(而非 BOM 导致的 NONE)");
}

// ============================ T3:单文件 .gz ============================
static void t3_gz() {
    std::printf("== T3 单文件 .gz 解压 ==\n");
    std::string buf = readFile("samples/archive/single.log.gz");
    ok(!buf.empty() && archiveKindOf(buf) == ARC_GZIP, "single.log.gz 判为 ARC_GZIP");
    std::vector<ArchiveEntry> es; std::string err;
    bool r = extractArchive(buf, es, err);
    ok(r, r ? "解压成功" : ("解压失败: " + err).c_str());
    ok(es.size() == 1, ("单文件 gz → 1 个条目(实得 " + std::to_string(es.size()) + ")").c_str());
    if (es.empty()) return;
    auto lines = splitLines(es[0].data);
    ok(lines.size() == 200, ("解压出 200 行(实得 " + std::to_string(lines.size()) + ")").c_str());
    long long t = 0;
    ok(firstTimestamp(lines, &t) && fmtTime(t, "FULL") == "2026-06-30 00:00:26",
       "解压内容首戳 == 2026-06-30 00:00:26(与真机源一致)");
}

// ============================ T4:.tar.gz 多文件 ============================
static void t4_targz() {
    std::printf("== T4 .tar.gz 拆成多条目 ==\n");
    std::string buf = readFile("samples/archive/two_logs.tar.gz");
    ok(!buf.empty() && archiveKindOf(buf) == ARC_GZIP, "two_logs.tar.gz 判为 ARC_GZIP(gzip 外层)");
    std::vector<ArchiveEntry> es; std::string err;
    bool r = extractArchive(buf, es, err);
    ok(r, r ? "解压成功" : ("解压失败: " + err).c_str());
    ok(es.size() == 2, ("tar 内 2 个文件 → 2 条目(实得 " + std::to_string(es.size()) + ")").c_str());
    if (es.size() < 2) return;
    // tar 保序:a_early 在前
    ok(es[0].name == "a_early.log" && es[1].name == "b_late.log",
       ("条目名与顺序正确(实得 " + es[0].name + ", " + es[1].name + ")").c_str());
    ok(splitLines(es[0].data).size() == 200, "a_early 200 行");
    ok(splitLines(es[1].data).size() == 200, "b_late 200 行");
}

// ============================ T5:.zip 多文件 ============================
static void t5_zip() {
    std::printf("== T5 .zip 解压 ==\n");
    std::string buf = readFile("samples/archive/two_logs.zip");
    ok(!buf.empty() && archiveKindOf(buf) == ARC_ZIP, "two_logs.zip 判为 ARC_ZIP");
    std::vector<ArchiveEntry> es; std::string err;
    bool r = extractArchive(buf, es, err);
    ok(r, r ? "解压成功" : ("解压失败: " + err).c_str());
    ok(es.size() == 2, ("zip 内 2 文件 → 2 条目(实得 " + std::to_string(es.size()) + ")").c_str());
    if (es.size() < 2) return;
    // 两个条目都要能解析出 200 行(不依赖顺序,zip 目录顺序不保证)
    size_t total = splitLines(es[0].data).size() + splitLines(es[1].data).size();
    ok(total == 400, ("两条目合计 400 行(实得 " + std::to_string(total) + ")").c_str());
}

// ============================ T6:非压缩内容拒绝 ============================
static void t6_reject() {
    std::printf("== T6 非压缩内容 extractArchive 返回 false ==\n");
    std::vector<ArchiveEntry> es; std::string err;
    std::string plain = "[2026-06-30 00:00:00] this is a plain log line\n";
    ok(!extractArchive(plain, es, err), "普通日志 → false(调用方应按普通日志读)");
    ok(es.empty(), "失败时不产出条目");
}

int main() {
    t1_kind();
    t2_bom();
    t3_gz();
    t4_targz();
    t5_zip();
    t6_reject();
    std::printf("\n%s 失败 %d 项\n", g_fail ? "**" : "==", g_fail);
    return g_fail ? 1 : 0;
}
