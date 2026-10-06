#pragma once
#include <string>
#include <vector>
#include "text_catalog.h"

namespace dl {
// Use complete original sentences, retaining every inference/source qualifier.
// No paraphrasing, new verdict or mid-sentence character cut is introduced.
inline std::string findingBrief(const std::string& text) {
    std::vector<std::string> sentences;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::size_t end = 0;
        if (text.compare(i, 3, "。") == 0)
            end = i + 3;
        else if (text[i] == '\n')
            end = i + 1;
        else if (text[i] == '.' && (i + 1 == text.size() || text[i + 1] == ' '))
            end = i + 1;
        if (end) {
            sentences.push_back(text.substr(start, end - start));
            start = end;
            i = end - 1;
        }
    }
    if (start < text.size())
        sentences.push_back(text.substr(start));
    if (sentences.empty())
        return text;
    std::string out = sentences.front();
    for (std::size_t i = 1; i < sentences.size(); ++i) {
        const auto& sentence = sentences[i];
        bool qualifier = false;
        std::string normalized = sentence;
        for (char& c : normalized)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        for (const char* word : {"【推断】",
                                 "【源码直证】",
                                 "【样本实证】",
                                 "【日志直证】",
                                 "不能",
                                 "不等同",
                                 "不代表",
                                 "无法",
                                 "尚待",
                                 "尚未",
                                 "未证明",
                                 "不证明",
                                 "未见",
                                 "不足",
                                 "可能",
                                 "疑似",
                                 "未知",
                                 "缺少",
                                 "待复核",
                                 "仅",
                                 "[inference]",
                                 "[source-code evidence]",
                                 "[sample evidence]",
                                 "[log evidence]",
                                 "cannot",
                                 "does not",
                                 "do not",
                                 "not prove",
                                 "not establish",
                                 "not yet",
                                 "no evidence",
                                 "insufficient",
                                 "only",
                                 "unconfirmed",
                                 "uncertain",
                                 "unknown",
                                 "suspected",
                                 "may ",
                                 "might "})
            if (normalized.find(word) != std::string::npos)
                qualifier = true;
        if (qualifier) {
            if (!out.empty() && out.back() != ' ')
                out += ' ';
            out += sentence;
        }
    }
    return out;
}

// A literal preview, not a generated summary. Full copy/export use the original.
inline std::string findingPreview(const std::string& text, std::size_t characters) {
    std::size_t end = 0, count = 0;
    while (end < text.size() && count < characters) {
        ++end;
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
            ++end;
        ++count;
    }
    if (end == text.size())
        return text;
    // Keep every explicit evidence qualification visible, including any past the cut.
    std::string labels;
    for (const char* label : {"【源码直证】", "【日志直证】", "【样本实证】", "【推断】", "[Source-code evidence]",
                              "[Log evidence]", "[Sample evidence]", "[Inference]"})
        if (text.find(label) != std::string::npos && text.substr(0, end).find(label) == std::string::npos)
            labels += label;
    return labels + text.substr(0, end) + " …";
}

inline std::string findingDisplayTitle(const std::string& text) {
    for (const std::string prefix :
         {"全量日志历史汇总:", "当前区间汇总:", "当前来源历史汇总:", "Full-input history summary:",
          "Selected-range summary:", "Current-source history summary:"}) {
        if (text.compare(0, prefix.size(), prefix) == 0) {
            auto start = prefix.size();
            while (start < text.size() && text[start] == ' ')
                ++start;
            return text.substr(start);
        }
    }
    return text;
}

inline std::string findingScopedText(const std::string& text, TextId scope) {
    // Translate intact phrases first; changing the Chinese prefix first would
    // break long catalog phrases and leave mixed-language classification text.
    std::string result = GeneratedText(text);
    const std::string original = UiText8(TextId::ui_0014), replacement = UiText8(scope);
    for (std::size_t position = 0; (position = result.find(original, position)) != std::string::npos;
         position += replacement.size())
        result.replace(position, original.size(), replacement);
    return result;
}
}  // namespace dl
