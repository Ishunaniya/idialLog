#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace dl {
struct TextEvidence {
    std::size_t line = 0;
    std::wstring text;
};

void ShowSelectableText(const std::wstring& title, const std::wstring& text,
                        const std::vector<TextEvidence>& evidence = {}, const std::string& reviewKey = {},
                        const std::string& reviewTitle = {});
void CloseSelectableText();
bool RouteSelectableTextMessage(MSG& message);
bool CopyWindowText(HWND owner, const std::wstring& text);
void ShowSignalGuide();
}  // namespace dl
