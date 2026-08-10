// win_file_io.cpp — Win32 文件读取、原子写入与压缩日志来源展开
#include "win_file_io.h"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <new>

namespace dl {
namespace {

std::wstring FormatW(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = 0;
    return buf;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

bool ReadFileBytes(const std::wstring& path, std::string& buf, std::wstring& err) {
    err.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = L"无法打开文件"; return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h); err = L"无法取得文件大小"; return false;
    }
    if (static_cast<unsigned long long>(sz.QuadPart) > kMaxInputBytes) {
        CloseHandle(h); err = L"文件超过 512 MiB 输入限制"; return false;
    }
    try {
        buf.assign(static_cast<std::size_t>(sz.QuadPart), '\0');
    } catch (const std::bad_alloc&) {
        CloseHandle(h); err = L"内存不足,无法读取文件"; return false;
    }
    DWORD got = 0;
    std::size_t total = 0;
    while (total < static_cast<std::size_t>(sz.QuadPart)) {
        std::size_t remain = static_cast<std::size_t>(sz.QuadPart) - total;
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(remain, 1024 * 1024));
        if (!ReadFile(h, &buf[total], want, &got, nullptr) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    if (total != static_cast<std::size_t>(sz.QuadPart)) {
        buf.clear(); err = L"文件读取不完整"; return false;
    }
    return true;
}

} // namespace

// 普通日志按 1 MiB 分块切行。carry 只保留跨块的半行；典型日志的输入峰值由
// “整文件字节串 + 全部 string 行”降为一个块和一条未完成行。
bool ReadPlainLinesImpl(const std::wstring& path, std::size_t maxLines,
                        void* sinkContext, PlainLineSink sink,
                        std::size_t* fileBytes, std::wstring& err) {
    err.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = L"无法打开文件"; return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h); err = L"无法取得文件大小"; return false;
    }
    if (static_cast<unsigned long long>(sz.QuadPart) > kMaxInputBytes) {
        CloseHandle(h); err = L"文件超过 512 MiB 输入限制"; return false;
    }
    if (fileBytes) *fileBytes = static_cast<std::size_t>(sz.QuadPart);

    std::vector<char> block;
    std::string carry;
    try {
        block.resize(1024 * 1024);
    } catch (const std::bad_alloc&) {
        CloseHandle(h); err = L"内存不足,无法建立读取缓冲"; return false;
    }

    std::size_t total = 0, emitted = 0;
    bool first = true;
    try {
        while (total < static_cast<std::size_t>(sz.QuadPart)) {
            DWORD want = static_cast<DWORD>(std::min<std::size_t>(
                block.size(), static_cast<std::size_t>(sz.QuadPart) - total));
            DWORD got = 0;
            if (!ReadFile(h, block.data(), want, &got, nullptr) || got == 0) break;
            total += got;
            carry.append(block.data(), got);
            if (first) { stripBom(carry); first = false; }

            const bool eof = total == static_cast<std::size_t>(sz.QuadPart);
            std::size_t start = 0, i = 0;
            while (i < carry.size()) {
                if (carry[i] != '\n' && carry[i] != '\r') { ++i; continue; }
                // CRLF 被块边界切开时先留下 CR，等下一块一起判定。
                if (carry[i] == '\r' && i + 1 == carry.size() && !eof) break;
                sink(sinkContext, carry.substr(start, i - start));
                ++emitted;
                if (carry[i] == '\r' && i + 1 < carry.size() && carry[i + 1] == '\n') ++i;
                start = ++i;
                if (emitted >= maxLines) {
                    CloseHandle(h);
                    return true;
                }
            }
            if (start) carry.erase(0, start);
        }
    } catch (...) {
        CloseHandle(h);
        throw;
    }
    CloseHandle(h);
    if (total != static_cast<std::size_t>(sz.QuadPart)) {
        err = L"文件读取不完整";
        return false;
    }
    if (!carry.empty() && emitted < maxLines) sink(sinkContext, std::move(carry));
    return true;
}

bool InspectFile(const std::wstring& path, ArchiveKind& kind,
                 std::size_t& fileBytes, std::wstring& err) {
    err.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = L"无法打开文件"; return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h); err = L"无法取得文件大小"; return false;
    }
    if (static_cast<unsigned long long>(sz.QuadPart) > kMaxInputBytes) {
        CloseHandle(h); err = L"文件超过 512 MiB 输入限制"; return false;
    }
    char magic[4]{};
    DWORD got = 0;
    DWORD want = static_cast<DWORD>(std::min<long long>(4, sz.QuadPart));
    if (want && (!ReadFile(h, magic, want, &got, nullptr) || got != want)) {
        CloseHandle(h); err = L"文件读取不完整"; return false;
    }
    CloseHandle(h);
    fileBytes = static_cast<std::size_t>(sz.QuadPart);
    kind = archiveKindOf(std::string(magic, magic + got));
    return true;
}

// CSV 先完整写到目标目录中的临时文件，Flush 成功后再原子替换目标。
bool WriteFileBytesAtomic(const std::wstring& path, const std::string& data,
                          std::wstring& err) {
    err.clear();
    std::size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"." : path.substr(0, slash + 1);
    wchar_t tempPath[MAX_PATH]{};
    if (dir.size() >= MAX_PATH) {
        err = L"CSV 目标目录路径过长";
        return false;
    }
    if (!GetTempFileNameW(dir.c_str(), L"dlg", 0, tempPath)) {
        err = FormatW(L"无法在目标目录创建临时文件 (Windows 错误 %lu)", GetLastError());
        return false;
    }

    HANDLE h = CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        DeleteFileW(tempPath);
        err = FormatW(L"无法打开 CSV 临时文件 (Windows 错误 %lu)", code);
        return false;
    }

    bool ok = true;
    DWORD code = ERROR_SUCCESS;
    std::size_t total = 0;
    while (total < data.size()) {
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - total, 1024 * 1024));
        DWORD wrote = 0;
        if (!WriteFile(h, data.data() + total, want, &wrote, nullptr) || wrote != want) {
            code = GetLastError();
            if (code == ERROR_SUCCESS) code = ERROR_WRITE_FAULT;
            ok = false;
            break;
        }
        total += wrote;
    }
    if (ok && !FlushFileBuffers(h)) { code = GetLastError(); ok = false; }
    if (!CloseHandle(h) && ok) { code = GetLastError(); ok = false; }

    if (!ok) {
        DeleteFileW(tempPath);
        err = FormatW(L"CSV 写入未完成 (Windows 错误 %lu),原文件未修改", code);
        return false;
    }
    if (!MoveFileExW(tempPath, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        code = GetLastError();
        DeleteFileW(tempPath);
        err = FormatW(L"无法用完整 CSV 替换目标文件 (Windows 错误 %lu),原文件未修改", code);
        return false;
    }
    return true;
}

bool ReadPathExpand(const std::wstring& path,
                    std::vector<std::vector<std::string>>& chunks,
                    std::vector<std::wstring>& labels,
                    std::size_t& textBytes,
                    std::wstring& err) {
    chunks.clear();
    labels.clear();
    textBytes = 0;
    std::string buf;
    if (!ReadFileBytes(path, buf, err)) return false;

    const std::wstring base = FileNameOf(path);
    if (archiveKindOf(buf) != ARC_NONE) {
        std::vector<ArchiveEntry> entries;
        std::string archiveErr;
        if (extractArchive(buf, entries, archiveErr)) {
            for (auto& entry : entries) {
                std::size_t entryBytes = entry.data.size();
                std::vector<std::string> lines;
                splitTextLines(std::move(entry.data), lines);
                if (lines.empty()) continue;
                if (entryBytes > kMaxBatchTextBytes - textBytes) {
                    chunks.clear(); labels.clear(); textBytes = 0;
                    err = L"压缩包展开后的日志文本超过 512 MiB";
                    return false;
                }
                textBytes += entryBytes;
                chunks.push_back(std::move(lines));
                std::wstring inner = Utf8ToWide(entry.name);
                labels.push_back(inner.empty() ? base : (base + L"!" + inner));
            }
            if (!chunks.empty()) return true;
            err = L"压缩包内没有非空日志";
            return false;
        }
        err = L"压缩包读取失败: " + Utf8ToWide(archiveErr);
        return false;
    }

    std::vector<std::string> lines;
    textBytes = buf.size();
    splitTextLines(std::move(buf), lines);
    if (lines.empty()) { textBytes = 0; err = L"文件为空"; return false; }
    chunks.push_back(std::move(lines));
    labels.push_back(base);
    return true;
}

std::wstring FileNameOf(const std::wstring& path) {
    std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

} // namespace dl
