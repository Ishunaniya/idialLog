#pragma once
#include <string>
#include "text_ids.h"
namespace dl {
void SetEnglish(bool enabled);
bool IsEnglish();
const wchar_t* UiText(TextId id);
const char* UiText8(TextId id);
// For application-generated prose only. Never pass log messages, evidence, paths or user input.
std::wstring RelocalizeUiText(const std::wstring& text);
std::string GeneratedText(const std::string& text);
}
