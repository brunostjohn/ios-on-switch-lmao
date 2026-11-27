// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/seed/cpp/json_min.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace switch_seed {
namespace {

void writeString(const std::string &s, std::string &out)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(c));
                out += buf;
            } else {
                out += char(c);
            }
            break;
        }
    }
    out += '"';
}

void writeValue(const JsonValue &v, std::string &out)
{
    switch (v.kind) {
    case JsonValue::Kind::Null:
        out += "null";
        break;
    case JsonValue::Kind::Bool:
        out += v.boolean ? "true" : "false";
        break;
    case JsonValue::Kind::Int:
        out += std::to_string(v.integer);
        break;
    case JsonValue::Kind::Uint:
        out += std::to_string(v.uinteger);
        break;
    case JsonValue::Kind::String:
        writeString(v.text, out);
        break;
    case JsonValue::Kind::Array:
        out += '[';
        for (size_t i = 0; i < v.array.size(); i++) {
            if (i)
                out += ',';
            writeValue(v.array[i], out);
        }
        out += ']';
        break;
    case JsonValue::Kind::Object:
        out += '{';
        bool first = true;
        for (const auto &[k, val] : v.object) {
            if (!first)
                out += ',';
            first = false;
            writeString(k, out);
            out += ':';
            writeValue(val, out);
        }
        out += '}';
        break;
    }
}

class Parser {
public:
    explicit Parser(const std::string &s) : s_(s)
    {}

    bool parse(JsonValue &out)
    {
        skipWs();
        if (!parseValue(out))
            return false;
        skipWs();
        return at_ == s_.size();
    }

private:
    const std::string &s_;
    size_t at_ = 0;

    void skipWs()
    {
        while (at_ < s_.size() && (s_[at_] == ' ' || s_[at_] == '\t' || s_[at_] == '\n' || s_[at_] == '\r'))
            at_++;
    }

    bool consume(char expected)
    {
        skipWs();
        if (at_ >= s_.size() || s_[at_] != expected)
            return false;
        at_++;
        return true;
    }

    bool parseHex4(uint32_t &cp)
    {
        if (at_ + 4 > s_.size())
            return false;
        cp = 0;
        for (int i = 0; i < 4; i++) {
            const char c = s_[at_++];
            uint32_t d = 0;
            if (c >= '0' && c <= '9')
                d = uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f')
                d = uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                d = uint32_t(c - 'A' + 10);
            else
                return false;
            cp = (cp << 4) | d;
        }
        return true;
    }

    static void appendUtf8(uint32_t cp, std::string &out)
    {
        if (cp <= 0x7F) {
            out += char(cp);
        } else if (cp <= 0x7FF) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool parseString(std::string &out)
    {
        if (!consume('"'))
            return false;
        out.clear();
        while (at_ < s_.size()) {
            const char c = s_[at_++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= s_.size())
                return false;
            const char esc = s_[at_++];
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                out += esc;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
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
            case 'u': {
                uint32_t cp = 0;
                if (!parseHex4(cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && at_ + 2 <= s_.size() && s_[at_] == '\\' && s_[at_ + 1] == 'u') {
                    at_ += 2;
                    uint32_t low = 0;
                    if (!parseHex4(low) || low < 0xDC00 || low > 0xDFFF)
                        return false;
                    cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                }
                appendUtf8(cp, out);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool parseNumber(JsonValue &out)
    {
        const size_t start = at_;
        bool neg = false;
        if (at_ < s_.size() && s_[at_] == '-') {
            neg = true;
            at_++;
        }
        if (at_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[at_])))
            return false;
        while (at_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[at_])))
            at_++;
        if (at_ < s_.size() && (s_[at_] == '.' || s_[at_] == 'e' || s_[at_] == 'E'))
            return false;
        const std::string token = s_.substr(start, at_ - start);
        char *end = nullptr;
        if (neg) {
            const long long v = std::strtoll(token.c_str(), &end, 10);
            if (!end || *end != '\0')
                return false;
            out = JsonValue::makeInt(int64_t(v));
        } else {
            const unsigned long long v = std::strtoull(token.c_str(), &end, 10);
            if (!end || *end != '\0')
                return false;
            out = JsonValue::makeUint(uint64_t(v));
        }
        return true;
    }

    bool parseArray(JsonValue &out)
    {
        if (!consume('['))
            return false;
        out = JsonValue::makeArray();
        skipWs();
        if (at_ < s_.size() && s_[at_] == ']') {
            at_++;
            return true;
        }
        while (true) {
            JsonValue elem;
            if (!parseValue(elem))
                return false;
            out.array.push_back(std::move(elem));
            skipWs();
            if (at_ < s_.size() && s_[at_] == ',') {
                at_++;
                continue;
            }
            return consume(']');
        }
    }

    bool parseObject(JsonValue &out)
    {
        if (!consume('{'))
            return false;
        out = JsonValue::makeObject();
        skipWs();
        if (at_ < s_.size() && s_[at_] == '}') {
            at_++;
            return true;
        }
        while (true) {
            std::string key;
            if (!parseString(key) || !consume(':'))
                return false;
            JsonValue val;
            if (!parseValue(val))
                return false;
            out.object[std::move(key)] = std::move(val);
            skipWs();
            if (at_ < s_.size() && s_[at_] == ',') {
                at_++;
                continue;
            }
            return consume('}');
        }
    }

    bool parseValue(JsonValue &out)
    {
        skipWs();
        if (at_ >= s_.size())
            return false;
        const char c = s_[at_];
        if (c == '"') {
            std::string text;
            if (!parseString(text))
                return false;
            out = JsonValue::makeString(std::move(text));
            return true;
        }
        if (c == '[')
            return parseArray(out);
        if (c == '{')
            return parseObject(out);
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
            return parseNumber(out);
        if (s_.compare(at_, 4, "true") == 0) {
            at_ += 4;
            out = JsonValue::makeBool(true);
            return true;
        }
        if (s_.compare(at_, 5, "false") == 0) {
            at_ += 5;
            out = JsonValue::makeBool(false);
            return true;
        }
        if (s_.compare(at_, 4, "null") == 0) {
            at_ += 4;
            out = JsonValue::makeNull();
            return true;
        }
        return false;
    }
};

}  // namespace

std::string jsonSerialize(const JsonValue &value)
{
    std::string out;
    writeValue(value, out);
    return out;
}

bool jsonParse(const std::string &text, JsonValue &out)
{
    Parser parser(text);
    return parser.parse(out);
}

}  // namespace switch_seed
