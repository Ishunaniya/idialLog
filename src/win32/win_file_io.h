// win_file_io.h — dialLog 的 Win32 文件读写与日志来源展开
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "archive_reader.h"

namespace dl {

inline constexpr std::size_t kMaxInputBytes = 512ULL * 1024 * 1024;
inline constexpr std::size_t kMaxBatchTextBytes = 512ULL * 1024 * 1024;

// 一次加载中的单个来源。普通文件保留路径并流式读取；压缩包条目持有已展开的行。
struct LoadSource {
    bool streamPlain = false;
    std::wstring path;
    std::wstring label;
    std::size_t textBytes = 0;
    std::vector<std::string> lines;
    std::vector<std::string> probe;
};

using PlainLineSink = void (*)(void*, std::string&&);

// ReadPlainLines 的非模板实现。公开模板只负责把调用方的 lambda 转成无分配回调，
// 避免百万行日志逐行经过 std::function。
bool ReadPlainLinesImpl(const std::wstring& path, std::size_t maxLines,
                        void* sinkContext, PlainLineSink sink,
                        std::size_t* fileBytes, std::wstring& err);

template <class Sink>
bool ReadPlainLines(const std::wstring& path, std::size_t maxLines, Sink&& sink,
                    std::size_t* fileBytes, std::wstring& err) {
    using SinkType = typename std::remove_reference<Sink>::type;
    return ReadPlainLinesImpl(
        path, maxLines, std::addressof(sink),
        [](void* context, std::string&& line) {
            (*static_cast<SinkType*>(context))(std::move(line));
        },
        fileBytes, err);
}

bool InspectFile(const std::wstring& path, ArchiveKind& kind,
                 std::size_t& fileBytes, std::wstring& err);

bool WriteFileBytesAtomic(const std::wstring& path, const std::string& data,
                          std::wstring& err);

bool ReadPathExpand(const std::wstring& path,
                    std::vector<std::vector<std::string>>& chunks,
                    std::vector<std::wstring>& labels,
                    std::size_t& textBytes,
                    std::wstring& err);

std::wstring FileNameOf(const std::wstring& path);

} // namespace dl
