#pragma once
#include <string>
#include <vector>
#include "text_catalog.h"

namespace dl {
// A literal preview, not a generated summary. Full copy/export use the original.
inline std::string findingPreview(const std::string& text, std::size_t characters) {
    std::size_t end = 0, count = 0;
    while (end < text.size() && count < characters) {
        ++end;
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) ++end;
        ++count;
    }
    if (end == text.size()) return text;
    // Keep every explicit evidence qualification visible, including any past the cut.
    std::string labels;
    for (const char* label : {"【源码直证】", "【日志直证】", "【样本实证】", "【推断】",
                             "[Source-code evidence]", "[Log evidence]", "[Sample evidence]", "[Inference]"})
        if (text.find(label) != std::string::npos && text.substr(0,end).find(label) == std::string::npos)
            labels += label;
    return labels + text.substr(0,end) + " …";
}
inline std::string findingDisplayTitle(const std::string& text) {
    for(const std::string prefix : {"全量日志历史汇总:","当前区间汇总:","当前来源历史汇总:",
        "Full-input history summary:","Selected-range summary:","Current-source history summary:"}) {
        if(text.compare(0,prefix.size(),prefix)==0) {
            auto start=prefix.size();while(start<text.size() && text[start]==' ') ++start;
            return text.substr(start);
        }
    }
    return text;
}
inline std::string findingScopedText(const std::string& text, TextId scope) {
    // Translate intact phrases first; changing the Chinese prefix first would
    // break long catalog phrases and leave mixed-language classification text.
    std::string result=GeneratedText(text);
    const std::string original=UiText8(TextId::ui_0014), replacement=UiText8(scope);
    for(std::size_t position=0;(position=result.find(original,position))!=std::string::npos;
        position+=replacement.size()) result.replace(position,original.size(),replacement);
    return result;
}
}
