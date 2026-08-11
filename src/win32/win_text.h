// win_text.h — Win32 UTF-8/UTF-16、控件文本与格式化辅助
#pragma once

#include <windows.h>

#include <string>

namespace dl {

std::wstring U8ToW(const std::string& text);
std::string WToU8(const std::wstring& text);
std::wstring GetText(HWND window);
std::wstring FmtW(const wchar_t* format, ...);

} // namespace dl
