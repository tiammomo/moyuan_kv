#pragma once

#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mokv {
namespace codec {

inline std::string Escape(std::string_view raw) {
    std::string encoded;
    encoded.reserve(raw.size());
    for (const char ch : raw) {
        switch (ch) {
            case '\\':
                encoded += "\\\\";
                break;
            case '\n':
                encoded += "\\n";
                break;
            case '\t':
                encoded += "\\t";
                break;
            default:
                encoded += ch;
                break;
        }
    }
    return encoded;
}

inline std::string EscapeKeyPart(std::string_view raw) {
    std::string encoded;
    encoded.reserve(raw.size());
    for (const char ch : raw) {
        switch (ch) {
            case '\\':
                encoded += "\\\\";
                break;
            case '\n':
                encoded += "\\n";
                break;
            case '\t':
                encoded += "\\t";
                break;
            case ':':
                encoded += "\\:";
                break;
            default:
                encoded += ch;
                break;
        }
    }
    return encoded;
}

inline std::string Unescape(std::string_view raw) {
    std::string decoded;
    decoded.reserve(raw.size());
    bool escaped = false;
    for (const char ch : raw) {
        if (!escaped) {
            if (ch == '\\') {
                escaped = true;
            } else {
                decoded += ch;
            }
            continue;
        }

        switch (ch) {
            case 'n':
                decoded += '\n';
                break;
            case 't':
                decoded += '\t';
                break;
            default:
                decoded += ch;
                break;
        }
        escaped = false;
    }
    if (escaped) {
        decoded += '\\';
    }
    return decoded;
}

inline std::vector<std::string> Split(std::string_view raw, char delimiter) {
    std::vector<std::string> tokens;
    size_t start = 0;
    while (start <= raw.size()) {
        const size_t pos = raw.find(delimiter, start);
        if (pos == std::string_view::npos) {
            tokens.emplace_back(raw.substr(start));
            break;
        }
        tokens.emplace_back(raw.substr(start, pos - start));
        start = pos + 1;
    }
    return tokens;
}

template <typename Integer>
inline std::optional<Integer> ParseInteger(std::string_view raw) {
    Integer value{};
    const char* begin = raw.data();
    const char* end = begin + raw.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc() || result.ptr != end) {
        return std::nullopt;
    }
    return value;
}

inline std::optional<int64_t> ParseInt64(std::string_view raw) {
    return ParseInteger<int64_t>(raw);
}

inline std::optional<uint32_t> ParseUInt32(std::string_view raw) {
    return ParseInteger<uint32_t>(raw);
}

inline std::string JoinKeyParts(std::initializer_list<std::string_view> parts) {
    std::string key;
    bool first = true;
    for (const auto part : parts) {
        if (!first) {
            key += ':';
        }
        key += EscapeKeyPart(part);
        first = false;
    }
    return key;
}

}  // namespace codec
}  // namespace mokv
