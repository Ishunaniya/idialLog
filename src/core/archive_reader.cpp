// archive_reader.cpp — 文本切行与 gzip/zip/tar 内存解压
#include "archive_reader.h"

#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#ifdef DL_HAVE_MINIZ
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
#endif

namespace dl {

// ============================ BOM 剥离 / 压缩包直读 ============================
void stripBom(std::string& buf) {
    if (buf.size() >= 3 &&
        (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        buf.erase(0, 3);
    }
}

void splitTextLines(std::string buf, std::vector<std::string>& out) {
    stripBom(buf);
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] != '\n' && buf[i] != '\r') continue;
        out.push_back(buf.substr(start, i - start));
        if (buf[i] == '\r' && i + 1 < buf.size() && buf[i + 1] == '\n') ++i;
        start = i + 1;
    }
    if (start < buf.size()) out.push_back(buf.substr(start));
}

ArchiveKind archiveKindOf(const std::string& buf) {
    if (buf.size() >= 2 &&
        (unsigned char)buf[0] == 0x1F && (unsigned char)buf[1] == 0x8B) return ARC_GZIP;
    if (buf.size() >= 4 &&
        buf[0] == 'P' && buf[1] == 'K' &&
        (unsigned char)buf[2] == 0x03 && (unsigned char)buf[3] == 0x04) return ARC_ZIP;
    return ARC_NONE;
}

// ---- tar 拆分(gzip 解出来若是 tar,再拆成多条目)----
// tar 是 512 字节块:每个文件一个 512 头(name[0..99], size 在 124..135 八进制),
// 紧跟 ceil(size/512) 个数据块。两个全零块表示结束。只取普通文件(typeflag '0' 或 '\0')。
// 仅在编入 miniz 时才需要(只被 gzip 分支调用);不定义 DL_HAVE_MINIZ 时不编译,
// 避免 -Wunused-function(本项目 -Wall -Wextra 零告警)。
#ifdef DL_HAVE_MINIZ
static constexpr size_t kMaxArchiveEntries   = 1000;
static constexpr size_t kMaxArchiveEntrySize = 256ULL * 1024 * 1024;
static constexpr size_t kMaxArchiveTotalSize = 512ULL * 1024 * 1024;

static bool looksLikeTar(const std::string& d) {
    // POSIX tar 在偏移 257 有 "ustar" 魔数;GNU tar 也是。老式 v7 tar 无魔数,
    // 这里用魔数做主判据(可靠),避免把普通文本误判成 tar。
    return d.size() >= 262 && std::memcmp(d.data() + 257, "ustar", 5) == 0;
}

static bool splitTar(const std::string& d, std::vector<ArchiveEntry>& out) {
    size_t off = 0;
    size_t total = 0;
    while (off + 512 <= d.size()) {
        const char* h = d.data() + off;
        // 全零块 = 结束
        bool allZero = true;
        for (int i = 0; i < 512; ++i) if (h[i]) { allZero = false; break; }
        if (allZero) break;

        // 文件名(可能不足 100 字节,以 NUL 结尾)
        size_t nameLen = 0;
        while (nameLen < 100 && h[nameLen]) nameLen++;
        std::string name(h, nameLen);

        // 大小:偏移 124,11 位八进制 + 可能的空格/NUL
        char szbuf[13] = {0};
        std::memcpy(szbuf, h + 124, 12);
        char* sizeEnd = nullptr;
        long long fsize = std::strtoll(szbuf, &sizeEnd, 8);
        if (sizeEnd == szbuf || fsize < 0 || (unsigned long long)fsize > kMaxArchiveEntrySize)
            return false;

        char typeflag = h[156];
        off += 512;   // 跳过头
        if ((size_t)fsize > d.size() - off) return false;   // 数据不完整

        // 普通文件('0' 或 '\0');目录('5')/其它类型跳过数据
        if (typeflag == '0' || typeflag == '\0') {
            if (out.size() >= kMaxArchiveEntries || (size_t)fsize > kMaxArchiveTotalSize - total)
                return false;
            if (fsize > 0) {
                out.push_back(ArchiveEntry{ name, d.substr(off, (size_t)fsize) });
                total += (size_t)fsize;
            }
        }
        // 跳到下一个 512 对齐
        off += ((size_t)fsize + 511) & ~((size_t)511);
    }
    return true;
}
#endif  // DL_HAVE_MINIZ (tar helpers)

bool extractArchive(const std::string& buf, std::vector<ArchiveEntry>& entries, std::string& err) try {
    entries.clear();
    err.clear();
    ArchiveKind k = archiveKindOf(buf);
    if (k == ARC_NONE) { err = "不是已知压缩格式"; return false; }

#ifdef DL_HAVE_MINIZ
    if (k == ARC_GZIP) {
        // gzip 解压:miniz 的 tinfl 只做裸 DEFLATE,gzip 需先跳过头、末尾无 adler。
        // 用 mz_inflate 走 raw deflate,gzip 头(10字节固定 + 可选字段)手工跳过。
        if (buf.size() < 18) { err = "gzip 数据过短"; return false; }
        const size_t trailer = buf.size() - 8;
        size_t p = 10;
        unsigned char flg = (unsigned char)buf[3];
        if (flg & 0xE0) { err = "gzip 标志位非法"; return false; }
        if (flg & 0x04) {   // FEXTRA
            if (p > trailer || trailer - p < 2) { err = "gzip FEXTRA 越界"; return false; }
            unsigned xlen = (unsigned char)buf[p] | ((unsigned char)buf[p+1] << 8);
            p += 2;
            if (xlen > trailer - p) { err = "gzip FEXTRA 越界"; return false; }
            p += xlen;
        }
        auto skipZeroTerminated = [&](const char* field) -> bool {
            size_t z = p;
            while (z < trailer && buf[z]) ++z;
            if (z >= trailer) { err = std::string("gzip ") + field + " 越界"; return false; }
            p = z + 1;
            return true;
        };
        if ((flg & 0x08) && !skipZeroTerminated("FNAME")) return false;
        if ((flg & 0x10) && !skipZeroTerminated("FCOMMENT")) return false;
        if (flg & 0x02) {
            if (p > trailer || trailer - p < 2) { err = "gzip FHCRC 越界"; return false; }
            p += 2;
        }
        if (p >= trailer) { err = "gzip 头后没有 DEFLATE 数据"; return false; }
        const size_t compressedSize = trailer - p;
        if (compressedSize > std::numeric_limits<unsigned>::max()) {
            err = "gzip 压缩数据过大"; return false;
        }

        // gzip 末 8 字节是 CRC32 + ISIZE;ISIZE 给出原始大小,预分配。
        uint32_t expectedCrc = (unsigned char)buf[buf.size()-8] |
                               ((unsigned char)buf[buf.size()-7] << 8) |
                               ((unsigned char)buf[buf.size()-6] << 16) |
                               ((uint32_t)(unsigned char)buf[buf.size()-5] << 24);
        uint32_t isize = (unsigned char)buf[buf.size()-4] |
                         ((unsigned char)buf[buf.size()-3] << 8) |
                         ((unsigned char)buf[buf.size()-2] << 16) |
                         ((uint32_t)(unsigned char)buf[buf.size()-1] << 24);
        if ((size_t)isize > kMaxArchiveEntrySize) { err = "gzip 解压后超过 256MiB 限制"; return false; }
        std::string out;
        size_t fallbackCap = buf.size() > (kMaxArchiveEntrySize - 1024) / 4
            ? kMaxArchiveEntrySize : buf.size() * 4 + 1024;
        size_t cap = isize ? (size_t)isize : fallbackCap;
        if (cap == 0) cap = 1;
        out.resize(cap);

        mz_stream s; std::memset(&s, 0, sizeof(s));
        if (mz_inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) { err = "inflate 初始化失败"; return false; }
        s.next_in = (const unsigned char*)buf.data() + p;
        s.avail_in = (unsigned)compressedSize;
        s.next_out = (unsigned char*)&out[0];
        s.avail_out = (unsigned)out.size();
        int r = mz_inflate(&s, MZ_FINISH);
        if (r != MZ_STREAM_END) {
            mz_inflateEnd(&s); err = "gzip 解压失败"; return false;
        }
        out.resize(s.total_out);
        mz_inflateEnd(&s);
        if ((uint32_t)out.size() != isize) { err = "gzip ISIZE 校验失败"; return false; }
        mz_ulong actualCrc = mz_crc32(MZ_CRC32_INIT,
            out.empty() ? nullptr : (const unsigned char*)out.data(), out.size());
        if ((uint32_t)actualCrc != expectedCrc) { err = "gzip CRC32 校验失败"; return false; }

        if (looksLikeTar(out)) {
            if (!splitTar(out, entries) || entries.empty()) {
                entries.clear(); err = "tar 损坏、为空或超过解压限制"; return false;
            }
        } else {
            entries.push_back(ArchiveEntry{ "", std::move(out) });
        }
        return true;
    }

    if (k == ARC_ZIP) {
        mz_zip_archive z; mz_zip_zero_struct(&z);
        if (!mz_zip_reader_init_mem(&z, buf.data(), buf.size(), 0)) { err = "zip 打开失败"; return false; }
        mz_uint n = mz_zip_reader_get_num_files(&z);
        if (n > kMaxArchiveEntries) {
            mz_zip_reader_end(&z); err = "zip 条目数超过 1000 限制"; return false;
        }
        size_t total = 0;
        for (mz_uint i = 0; i < n; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&z, i, &st)) {
                mz_zip_reader_end(&z); entries.clear(); err = "zip 目录项读取失败"; return false;
            }
            if (mz_zip_reader_is_file_a_directory(&z, i)) continue;
            if (st.m_uncomp_size > kMaxArchiveEntrySize ||
                st.m_uncomp_size > kMaxArchiveTotalSize - total) {
                mz_zip_reader_end(&z); entries.clear(); err = "zip 解压后超过大小限制"; return false;
            }
            size_t outSz = 0;
            void* p = mz_zip_reader_extract_to_heap(&z, i, &outSz, 0);
            if (!p || outSz != (size_t)st.m_uncomp_size) {
                if (p) mz_free(p);
                mz_zip_reader_end(&z); entries.clear(); err = "zip 条目解压失败"; return false;
            }
            entries.push_back(ArchiveEntry{ st.m_filename, std::string((char*)p, outSz) });
            total += outSz;
            mz_free(p);
        }
        mz_zip_reader_end(&z);
        if (entries.empty()) { err = "zip 内无可读文件"; return false; }
        return true;
    }
    err = "未支持的压缩格式";
    return false;
#else
    (void)entries;
    err = "本次构建未编入解压支持(DL_HAVE_MINIZ 未定义)";
    return false;
#endif
} catch (const std::bad_alloc&) {
    entries.clear();
    err = "内存不足,压缩包未展开";
    return false;
}


} // namespace dl
