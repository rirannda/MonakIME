// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime/text.h"

namespace newime::text {
std::u32string utf8ToU32(std::string_view text) {
    std::u32string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const auto a = static_cast<unsigned char>(text[i]);
        if (a < 0x80) {
            result.push_back(a);
            ++i;
            continue;
        }
        unsigned count = a >= 0xc2 && a <= 0xdf   ? 2
                         : a >= 0xe0 && a <= 0xef ? 3
                         : a >= 0xf0 && a <= 0xf4 ? 4
                                                  : 0;
        char32_t cp = count ? a & ((1u << (7 - count)) - 1u) : 0;
        bool valid = count && i + count <= text.size();
        for (unsigned j = 1; valid && j < count; ++j) {
            auto b = static_cast<unsigned char>(text[i + j]);
            if ((b & 0xc0) != 0x80)
                valid = false;
            else
                cp = (cp << 6) | (b & 0x3f);
        }
        valid = valid && !(count == 2 && cp < 0x80) && !(count == 3 && cp < 0x800) &&
                !(count == 4 && cp < 0x10000) && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
        result.push_back(valid ? cp : 0xfffd);
        i += valid ? count : 1;
    }
    return result;
}
std::string u32ToUtf8(std::u32string_view text) {
    std::string result;
    for (char32_t cp : text) {
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            cp = 0xfffd;
        if (cp < 0x80)
            result.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 63)));
        } else if (cp < 0x10000) {
            result.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
            result.push_back(static_cast<char>(0x80 | (cp & 63)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
            result.push_back(static_cast<char>(0x80 | (cp & 63)));
        }
    }
    return result;
}
std::string hiragana(std::string_view input) {
    auto s = utf8ToU32(input);
    for (auto &c : s)
        if (c >= U'ァ' && c <= U'ヶ')
            c -= 0x60;
    return u32ToUtf8(s);
}
std::string katakana(std::string_view input) {
    auto s = utf8ToU32(input);
    for (auto &c : s)
        if (c >= U'ぁ' && c <= U'ゖ')
            c += 0x60;
    return u32ToUtf8(s);
}
size_t codepoints(std::string_view input) {
    return utf8ToU32(input).size();
}
std::string prefix(std::string_view input, size_t length) {
    auto s = utf8ToU32(input);
    return u32ToUtf8(s.substr(0, length));
}
std::string suffix(std::string_view input, size_t length) {
    auto s = utf8ToU32(input);
    return u32ToUtf8(s.substr(s.size() > length ? s.size() - length : 0));
}
bool isKana(char32_t c) {
    return (c >= U'ぁ' && c <= U'ゖ') || (c >= U'ァ' && c <= U'ヺ') || c == U'ー' || c == U'ゔ';
}
bool isAsciiLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
} // namespace newime::text
