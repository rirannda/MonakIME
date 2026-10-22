// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>
#include <vector>

namespace newime::text {
std::u32string utf8ToU32(std::string_view text);
std::string u32ToUtf8(std::u32string_view text);
std::string hiragana(std::string_view text);
std::string katakana(std::string_view text);
size_t codepoints(std::string_view text);
std::string prefix(std::string_view text, size_t length);
std::string suffix(std::string_view text, size_t length);
bool isKana(char32_t c);
bool isAsciiLetter(char c);
} // namespace newime::text
