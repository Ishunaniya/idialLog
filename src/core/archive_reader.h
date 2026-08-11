// archive_reader.h — 文本切分与压缩包读取 API
#pragma once

#include <string>
#include <vector>

namespace dl {

// ---- BOM 剥离 ----
// 剥掉缓冲区开头的 UTF-8 BOM(EF BB BF)。为什么必须做:用户用记事本打开日志另存后,
// 记事本会在文件头写入 3 字节 BOM,粘在首行时间戳前 → firstTimestamp/parseSd 认不出
// 首行 → 该份日志首时间戳判定失败 → 波及定序与时基判定(实测 TB_NONE)。
// 不处理 GBK/UTF-16:dial 日志由设备端 C printf 产出,结构上只有 ASCII(全部真机夹具
// 实测为 us-ascii,无一带 BOM 或非 ASCII),GBK 支持无真机依据,不加。
void stripBom(std::string& buf);

// UTF-8 文本缓冲区切行:统一支持 LF / CRLF / CR,并剥离文件开头的 UTF-8 BOM。
// 不保留“文本末尾换行”产生的额外空行,但保留正文中真实存在的空行。
// 文件读取与剪贴板粘贴必须共用此实现,避免 Windows CRLF 被重复计为空行。
void splitTextLines(std::string buf, std::vector<std::string>& out);

// ---- 压缩包直读 ----
// 现场日志多为打包回传(.zip/.tar.gz)。这里在内存中解压,免去手工先解压再拖入。
// 只解压、不落地临时文件;逻辑纯 buffer→buffer,可单元测试。
enum ArchiveKind {
    ARC_NONE = 0,   // 不是已知压缩格式(按普通日志处理)
    ARC_GZIP,       // .gz / .tar.gz(gzip 魔数 1F 8B)
    ARC_ZIP         // .zip(魔数 PK\x03\x04)
};

// 按内容魔数(不是扩展名)判定压缩格式。扩展名可能被改过,魔数是事实。
ArchiveKind archiveKindOf(const std::string& buf);

// 解压结果:一个压缩包里可能有多个日志文件(尤其 .zip / .tar)。
struct ArchiveEntry {
    std::string name;    // 包内文件名(用于提示/排序)
    std::string data;    // 解压后的原始字节
};

// 解压缓冲区。成功返回 true 并填入 entries(至少一个);失败返回 false 并把原因写入 err。
// 为防止损坏包/压缩炸弹耗尽内存,限制单条目 256MiB、总解压 512MiB、最多 1000 个条目。
//   ARC_GZIP: 先 gunzip;若解出来是 tar(512 块魔数)再拆成多个条目,否则整体作单条目。
//   ARC_ZIP : 遍历中央目录,解压每个非目录条目。
// 非压缩内容(ARC_NONE)返回 false —— 调用方应把它当普通日志直接读。
bool extractArchive(const std::string& buf, std::vector<ArchiveEntry>& entries, std::string& err);

} // namespace dl
