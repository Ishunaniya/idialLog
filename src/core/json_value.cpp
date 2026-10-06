#include "json_value.h"
#include <stdexcept>
#include <cctype>
#include <limits>

namespace dl {
namespace {
[[noreturn]] void bad() {
    throw std::invalid_argument("Invalid JSON metadata");
}

struct Parser {
    const std::string& s;
    std::size_t p = 0, nodes = 0;

    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\r' || s[p] == '\n' || s[p] == '\t'))
            ++p;
    }

    bool take(char c) {
        ws();
        if (p < s.size() && s[p] == c) {
            ++p;
            return true;
        }

        return false;
    }

    unsigned hex4() {
        unsigned x = 0;
        for (int i = 0; i < 4; ++i) {
            if (p == s.size())
                bad();
            char c = s[p++];
            x *= 16;
            if (c >= '0' && c <= '9')
                x += c - '0';
            else if (c >= 'a' && c <= 'f')
                x += c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                x += c - 'A' + 10;
            else
                bad();
        }

        return x;
    }

    std::string string() {
        if (!take('"'))
            bad();
        std::string out;
        while (p < s.size()) {
            unsigned char c = s[p++];
            if (c == '"')
                return out;
            if (c < 32)
                bad();
            if (c != '\\') {
                out += c;
                continue;
            }

            if (p == s.size())
                bad();
            c = s[p++];
            switch (c) {

            case '"':

            case '\\':

            case '/':
                out += c;
                break;

            case 'n':
                out += '\n';
                break;

            case 'r':
                out += '\r';
                break;

            case 't':
                out += '\t';
                break;

            case 'b':
                out += '\b';
                break;

            case 'f':
                out += '\f';
                break;

            case 'u': {
                unsigned u = hex4();
                if (u >= 0xd800 && u <= 0xdbff) {
                    if (p + 2 > s.size() || s[p++] != '\\' || s[p++] != 'u')
                        bad();
                    unsigned lo = hex4();
                    if (lo < 0xdc00 || lo > 0xdfff)
                        bad();
                    u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                } else if (u >= 0xdc00 && u <= 0xdfff)
                    bad();
                if (u < 0x80)
                    out += char(u);
                else if (u < 0x800) {
                    out += char(0xc0 | (u >> 6));
                    out += char(0x80 | (u & 63));
                } else if (u < 0x10000) {
                    out += char(0xe0 | (u >> 12));
                    out += char(0x80 | ((u >> 6) & 63));
                    out += char(0x80 | (u & 63));
                } else {
                    out += char(0xf0 | (u >> 18));
                    out += char(0x80 | ((u >> 12) & 63));
                    out += char(0x80 | ((u >> 6) & 63));
                    out += char(0x80 | (u & 63));
                }

                break;
            }

            default:
                bad();
            }
        }

        bad();
    }

    Json value(int depth = 0) {
        ws();
        if (depth > 24 || ++nodes > 100000 || p == s.size())
            bad();
        if (s[p] == '"')
            return Json(string());
        if (take('{')) {
            Json j = Json::dict();
            if (take('}'))
                return j;
            do {
                auto k = string();
                if (!take(':'))
                    bad();
                auto v = value(depth + 1);
                if (!j.object.emplace(k, std::move(v)).second)
                    bad();
            } while (take(','));
            if (!take('}'))
                bad();
            return j;
        }

        if (take('[')) {
            Json j = Json::list();
            if (take(']'))
                return j;
            do {
                j.array.push_back(value(depth + 1));
            } while (take(','));
            if (!take(']'))
                bad();
            return j;
        }

        for (auto pair : {std::pair<const char*, int>{"null", 0}, {"true", 1}, {"false", 2}}) {
            const std::string token = pair.first;
            if (s.compare(p, token.size(), token) == 0) {
                p += token.size();
                return pair.second ? Json::flag(pair.second == 1) : Json();
            }
        }

        const auto start = p;
        bool negative = p < s.size() && s[p] == '-';
        if (negative)
            ++p;
        if (p == s.size() || !std::isdigit(static_cast<unsigned char>(s[p])))
            bad();
        if (s[p] == '0')
            ++p;
        else
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p])))
                ++p;
        if (p < s.size() && (s[p] == '.' || s[p] == 'e' || s[p] == 'E'))
            bad();
        try {
            std::size_t used = 0;
            auto n = std::stoll(s.substr(start, p - start), &used);
            if (used != p - start)
                bad();
            return Json(n);
        } catch (...) {
            bad();
        }
    }
};

std::string quote(const std::string& s) {
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += c;
    }

    return out + '"';
}
}  // namespace

const Json& Json::at(const std::string& key) const {
    if (kind != Object)
        bad();
    auto it = object.find(key);
    if (it == object.end())
        bad();
    return it->second;
}

std::string Json::str() const {
    if (kind != String)
        bad();
    return text;
}

long long Json::num() const {
    if (kind != Integer)
        bad();
    return integer;
}

bool Json::flag() const {
    if (kind != Boolean)
        bad();
    return boolean;
}

Json parseJson(const std::string& s) {
    if (s.size() > 16 * 1024 * 1024)
        bad();
    Parser p{s};
    auto j = p.value();
    p.ws();
    if (p.p != s.size())
        bad();
    return j;
}

std::string writeJson(const Json& j) {
    switch (j.kind) {

    case Json::Null:
        return "null";

    case Json::String:
        return quote(j.text);

    case Json::Integer:
        return std::to_string(j.integer);

    case Json::Boolean:
        return j.boolean ? "true" : "false";

    default:
        break;
    }

    std::string out = j.kind == Json::Array ? "[" : "{";
    bool comma = false;
    if (j.kind == Json::Array) {
        for (const auto& v : j.array) {
            if (comma)
                out += ',';
            comma = true;
            out += writeJson(v);
        }
    } else
        for (const auto& v : j.object) {
            if (comma)
                out += ',';
            comma = true;
            out += quote(v.first) + ":" + writeJson(v.second);
        }
    return out + (j.kind == Json::Array ? "]" : "}");
}
}  // namespace dl
