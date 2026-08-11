// win_text.cpp — Win32 文本边界实现
#include "win_text.h"

#include <cstdarg>
#include <cwchar>

namespace dl {

std::wstring U8ToW(const std::string& text) {
    if (text.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &wide[0], n);
    return wide;
}

std::string WToU8(const std::wstring& text) {
    if (text.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        &utf8[0], n, nullptr, nullptr);
    return utf8;
}

std::wstring GetText(HWND window) {
    int n = GetWindowTextLengthW(window);
    if (n <= 0) return L"";
    std::wstring text(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(window, &text[0], n + 1);
    text.resize(static_cast<std::size_t>(n));
    return text;
}

std::wstring FmtW(const wchar_t* format, ...) {
    wchar_t buf[2048];
    va_list args;
    va_start(args, format);
    _vsnwprintf(buf, 2047, format, args);
    va_end(args);
    buf[2047] = 0;
    return buf;
}

} // namespace dl
