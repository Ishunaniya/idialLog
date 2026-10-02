#include "text_catalog.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <vector>
#include <unordered_map>
namespace dl {
namespace {
std::atomic<bool> g_english{false};
struct Entry { const char* zh; const char* en; const wchar_t* wideZh; const wchar_t* wideEn; };
const Entry kEntries[] = {
#include "text_catalog.inc"
};
}
void SetEnglish(bool enabled) { g_english.store(enabled); }
bool IsEnglish() { return g_english.load(); }
const wchar_t* UiText(TextId id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= sizeof(kEntries)/sizeof(kEntries[0])) return L"";
    return IsEnglish() ? kEntries[index].wideEn : kEntries[index].wideZh;
}
const char* UiText8(TextId id) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= sizeof(kEntries)/sizeof(kEntries[0])) return "";
    return IsEnglish() ? kEntries[index].en : kEntries[index].zh;
}
std::wstring RelocalizeUiText(const std::wstring& text) {
    for (const auto& entry : kEntries)
        if (text == entry.wideZh || text == entry.wideEn) return IsEnglish() ? entry.wideEn : entry.wideZh;
    return text;
}
std::string GeneratedText(const std::string& text) {
    if (!IsEnglish()) return text;
    static const auto exact = [] {
        std::unordered_map<std::string,std::string> map;
        for (const auto& entry : kEntries) map.emplace(entry.zh,entry.en);
        return map;
    }();
    auto found = exact.find(text);
    if (found != exact.end()) return found->second;
    // Resolve application-owned fragments before joining log evidence into the output.
    static const auto ordered = [] {
        std::vector<const Entry*> entries;
        for (const auto& entry : kEntries)
            if (*entry.zh) entries.push_back(&entry);
        std::stable_sort(entries.begin(),entries.end(),[](const Entry* a,const Entry* b) {
            return std::strlen(a->zh)>std::strlen(b->zh);
        });
        return entries;
    }();
    std::string output;
    for (std::size_t position=0;position<text.size();) {
        const Entry* match=nullptr;
        for (const Entry* entry : ordered) {
            const auto size=std::strlen(entry->zh);
            if (size<=text.size()-position && text.compare(position,size,entry->zh)==0) {match=entry;break;}
        }
        if (match) {output+=match->en;position+=std::strlen(match->zh);}
        else output+=text[position++];
    }
    return output;
}
}
