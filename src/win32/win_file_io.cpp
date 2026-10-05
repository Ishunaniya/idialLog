// win_file_io.cpp — Win32 文件读取、原子写入与压缩日志来源展开
#include "win_file_io.h"
#include "text_catalog.h"
#include "sha256.h"
#include "incident_export.h"

#include "log_parser.h"

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

bool ReadFileBytes(const std::wstring& path, std::string& buf, std::wstring& err,
                   void* observerContext, ReadObserver observer) {
    err.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = UiText(TextId::ui_0130); return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h); err = UiText(TextId::ui_0131); return false;
    }
    if (static_cast<unsigned long long>(sz.QuadPart) > kMaxInputBytes) {
        CloseHandle(h); err = UiText(TextId::ui_0132); return false;
    }
    try {
        buf.assign(static_cast<std::size_t>(sz.QuadPart), '\0');
    } catch (const std::bad_alloc&) {
        CloseHandle(h); err = UiText(TextId::ui_0133); return false;
    }
    DWORD got = 0;
    std::size_t total = 0;
    while (total < static_cast<std::size_t>(sz.QuadPart)) {
        std::size_t remain = static_cast<std::size_t>(sz.QuadPart) - total;
        DWORD want = static_cast<DWORD>(std::min<std::size_t>(remain, 1024 * 1024));
        if (!ReadFile(h, &buf[total], want, &got, nullptr) || got == 0) break;
        total += got;
        if (observer && !observer(observerContext, total, static_cast<std::size_t>(sz.QuadPart))) {
            CloseHandle(h); buf.clear(); err = UiText(TextId::ui_0032); return false;
        }
    }
    CloseHandle(h);
    if (total != static_cast<std::size_t>(sz.QuadPart)) {
        buf.clear(); err = UiText(TextId::ui_0134); return false;
    }
    return true;
}

} // namespace

// 普通日志按 1 MiB 分块切行。carry 只保留跨块的半行；典型日志的输入峰值由
// “整文件字节串 + 全部 string 行”降为一个块和一条未完成行。
bool ReadPlainLinesImpl(const std::wstring& path, std::size_t maxLines,
                        void* sinkContext, PlainLineSink sink,
                        std::size_t* fileBytes, std::wstring& err,
                        void* observerContext, ReadObserver observer, std::string* hash) {
    Sha256 digest;
    err.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = UiText(TextId::ui_0130); return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h); err = UiText(TextId::ui_0131); return false;
    }
    if (static_cast<unsigned long long>(sz.QuadPart) > kMaxInputBytes) {
        CloseHandle(h); err = UiText(TextId::ui_0132); return false;
    }
    if (fileBytes) *fileBytes = static_cast<std::size_t>(sz.QuadPart);

    std::vector<char> block;
    std::string carry;
    try {
        block.resize(1024 * 1024);
    } catch (const std::bad_alloc&) {
        CloseHandle(h); err = UiText(TextId::ui_0135); return false;
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
            if(hash)digest.update(std::string_view(block.data(),got));
            if (observer && !observer(observerContext, total, static_cast<std::size_t>(sz.QuadPart))) {
                CloseHandle(h); err = UiText(TextId::ui_0032); return false;
            }
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
        err = UiText(TextId::ui_0134);
        return false;
    }
    if(hash)*hash=digest.finish();
    if (!carry.empty() && emitted < maxLines) sink(sinkContext, std::move(carry));
    return true;
}

bool InspectFile(const std::wstring& path, ArchiveKind& kind,
                 std::size_t& fileBytes, std::wstring& err) {
    err.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = UiText(TextId::ui_0130); return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h); err = UiText(TextId::ui_0131); return false;
    }
    if (static_cast<unsigned long long>(sz.QuadPart) > kMaxInputBytes) {
        CloseHandle(h); err = UiText(TextId::ui_0132); return false;
    }
    char magic[4]{};
    DWORD got = 0;
    DWORD want = static_cast<DWORD>(std::min<long long>(4, sz.QuadPart));
    if (want && (!ReadFile(h, magic, want, &got, nullptr) || got != want)) {
        CloseHandle(h); err = UiText(TextId::ui_0134); return false;
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
        err = UiText(TextId::ui_0136);
        return false;
    }
    if (!GetTempFileNameW(dir.c_str(), L"dlg", 0, tempPath)) {
        err = FormatW(UiText(TextId::ui_0137), GetLastError());
        return false;
    }

    HANDLE h = CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        DeleteFileW(tempPath);
        err = FormatW(UiText(TextId::ui_0138), code);
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
        err = FormatW(UiText(TextId::ui_0139), code);
        return false;
    }
    if (!MoveFileExW(tempPath, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        code = GetLastError();
        DeleteFileW(tempPath);
        err = FormatW(UiText(TextId::ui_0140), code);
        return false;
    }
    return true;
}

bool ReadPathExpand(const std::wstring& path,
                    std::vector<std::vector<std::string>>& chunks,
                    std::vector<std::wstring>& labels,
                    std::size_t& textBytes,
                    std::wstring& err,
                    void* observerContext, ReadObserver observer, std::vector<OriginalSource>* originals, ImportedEvidencePackage* package) {
    if(originals)originals->clear();
    chunks.clear();
    labels.clear();
    textBytes = 0;
    std::string buf;
    if (!ReadFileBytes(path, buf, err, observerContext, observer)) return false;

    const std::wstring base = FileNameOf(path);
    if (archiveKindOf(buf) != ARC_NONE) {
        std::vector<ArchiveEntry> entries;
        std::string archiveErr;
        if (extractArchive(buf, entries, archiveErr)) {
            const bool evidencePackage=std::any_of(entries.begin(),entries.end(),[](const ArchiveEntry& e){return e.name=="package-index.json";});
            if(evidencePackage){
                try{auto imported=readEvidencePackage(buf);entries.clear();for(auto& o:imported.originals)entries.push_back({o.name,std::move(o.bytes)});if(package)*package=std::move(imported);}
                catch(const std::exception& e){err=UiText(TextId::package_invalid)+Utf8ToWide(e.what());return false;}
            }
            std::size_t entryIndex=0;
            for (auto& entry : entries) {
                if (observer && !observer(observerContext, textBytes, std::max<std::size_t>(1, buf.size()))) {
                    chunks.clear(); labels.clear(); textBytes = 0; err = UiText(TextId::ui_0032); return false;
                }
                std::size_t entryBytes = entry.data.size();
                OriginalSource origin{path,entry.name,sha256(entry.data)};
                std::vector<std::string> lines;
                splitTextLines(std::move(entry.data), lines);
                if (lines.empty() && !evidencePackage) continue;
                if (entryBytes > kMaxBatchTextBytes - textBytes) {
                    chunks.clear(); labels.clear(); textBytes = 0;
                    err = UiText(TextId::ui_0141);
                    return false;
                }
                textBytes += entryBytes;
                chunks.push_back(std::move(lines));
                if(originals)originals->push_back(std::move(origin));
                std::wstring inner = Utf8ToWide(entry.name);
                if(evidencePackage&&package&&entryIndex<package->originals.size())labels.push_back(Utf8ToWide(package->originals[entryIndex].label));
                else labels.push_back(inner.empty() ? base : (base + L"!" + inner));
                ++entryIndex;
            }
            if (!chunks.empty()) return true;
            err = UiText(TextId::ui_0142);
            return false;
        }
        err = UiText(TextId::ui_0143) + Utf8ToWide(GeneratedText(archiveErr));
        return false;
    }

    std::vector<std::string> lines;
    textBytes = buf.size();
    splitTextLines(std::move(buf), lines);
    if (lines.empty()) { textBytes = 0; err = UiText(TextId::ui_0020); return false; }
    chunks.push_back(std::move(lines));
    labels.push_back(base);
    return true;
}

bool ReadBoundedFile(const std::wstring& path,std::string& bytes,std::wstring& error,std::size_t limit) {
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE){error=UiText(TextId::ui_0130);return false;}
    LARGE_INTEGER size{};bool ok=GetFileSizeEx(h,&size)&&size.QuadPart>=0&&static_cast<unsigned long long>(size.QuadPart)<=limit;
    if(ok){try{bytes.resize(static_cast<std::size_t>(size.QuadPart));}catch(...){CloseHandle(h);throw;}
        std::size_t done=0;while(done<bytes.size()){DWORD got=0;DWORD n=static_cast<DWORD>(std::min<std::size_t>(1024*1024,bytes.size()-done));if(!ReadFile(h,&bytes[done],n,&got,nullptr)||got!=n){ok=false;break;}done+=got;}}
    CloseHandle(h);if(!ok){bytes.clear();error=UiText(TextId::ui_0134);}return ok;
}
bool ReadOriginalBytes(const OriginalSource& source,std::string& bytes,std::wstring& error) {
    if(!ReadBoundedFile(source.path,bytes,error,kMaxInputBytes))return false;
    if(!source.entry.empty() || archiveKindOf(bytes)!=ARC_NONE){std::vector<ArchiveEntry> entries;std::string why;
        if(!extractArchive(bytes,entries,why)){error=Utf8ToWide(why);return false;}
        bool found=false;for(auto& e:entries)if(e.name==source.entry){if(found){error=UiText(TextId::package_invalid);return false;}bytes=std::move(e.data);found=true;}
        if(!found){error=UiText(TextId::package_hash_error);return false;}}
    if(sha256(bytes)!=source.hash){error=UiText(TextId::package_hash_error);bytes.clear();return false;}return true;
}

std::wstring FileNameOf(const std::wstring& path) {
    std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

} // namespace dl
