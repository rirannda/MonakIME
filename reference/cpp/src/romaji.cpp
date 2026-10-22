// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime/romaji.h"
#include <algorithm>
#include <array>
#include <utility>

namespace newime {
namespace {
constexpr std::pair<std::string_view, std::string_view> rules[] = {
    {"a", "あ"},     {"i", "い"},     {"u", "う"},     {"e", "え"},     {"o", "お"},
    {"ka", "か"},    {"ki", "き"},    {"ku", "く"},    {"ke", "け"},    {"ko", "こ"},
    {"ga", "が"},    {"gi", "ぎ"},    {"gu", "ぐ"},    {"ge", "げ"},    {"go", "ご"},
    {"sa", "さ"},    {"si", "し"},    {"su", "す"},    {"se", "せ"},    {"so", "そ"},
    {"shi", "し"},   {"za", "ざ"},    {"zi", "じ"},    {"zu", "ず"},    {"ze", "ぜ"},
    {"zo", "ぞ"},    {"ji", "じ"},    {"ta", "た"},    {"ti", "ち"},    {"tu", "つ"},
    {"te", "て"},    {"to", "と"},    {"chi", "ち"},   {"tsu", "つ"},   {"da", "だ"},
    {"di", "ぢ"},    {"du", "づ"},    {"de", "で"},    {"do", "ど"},    {"na", "な"},
    {"ni", "に"},    {"nu", "ぬ"},    {"ne", "ね"},    {"no", "の"},    {"ha", "は"},
    {"hi", "ひ"},    {"hu", "ふ"},    {"he", "へ"},    {"ho", "ほ"},    {"fu", "ふ"},
    {"ba", "ば"},    {"bi", "び"},    {"bu", "ぶ"},    {"be", "べ"},    {"bo", "ぼ"},
    {"pa", "ぱ"},    {"pi", "ぴ"},    {"pu", "ぷ"},    {"pe", "ぺ"},    {"po", "ぽ"},
    {"ma", "ま"},    {"mi", "み"},    {"mu", "む"},    {"me", "め"},    {"mo", "も"},
    {"ya", "や"},    {"yu", "ゆ"},    {"yo", "よ"},    {"yi", "い"},    {"ye", "いぇ"},
    {"ra", "ら"},    {"ri", "り"},    {"ru", "る"},    {"re", "れ"},    {"ro", "ろ"},
    {"wa", "わ"},    {"wi", "うぃ"},  {"we", "うぇ"},  {"wo", "を"},    {"wu", "う"},
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"}, {"kyi", "きぃ"}, {"kye", "きぇ"},
    {"gya", "ぎゃ"}, {"gyu", "ぎゅ"}, {"gyo", "ぎょ"}, {"gye", "ぎぇ"}, {"sha", "しゃ"},
    {"shu", "しゅ"}, {"sho", "しょ"}, {"she", "しぇ"}, {"sya", "しゃ"}, {"syu", "しゅ"},
    {"syo", "しょ"}, {"sye", "しぇ"}, {"ja", "じゃ"},  {"ju", "じゅ"},  {"jo", "じょ"},
    {"je", "じぇ"},  {"jya", "じゃ"}, {"jyu", "じゅ"}, {"jyo", "じょ"}, {"jye", "じぇ"},
    {"zya", "じゃ"}, {"zyu", "じゅ"}, {"zyo", "じょ"}, {"zye", "じぇ"}, {"cha", "ちゃ"},
    {"chu", "ちゅ"}, {"cho", "ちょ"}, {"che", "ちぇ"}, {"tya", "ちゃ"}, {"tyu", "ちゅ"},
    {"tyo", "ちょ"}, {"tye", "ちぇ"}, {"cya", "ちゃ"}, {"cyu", "ちゅ"}, {"cyo", "ちょ"},
    {"cye", "ちぇ"}, {"dya", "ぢゃ"}, {"dyu", "ぢゅ"}, {"dyo", "ぢょ"}, {"nya", "にゃ"},
    {"nyu", "にゅ"}, {"nyo", "にょ"}, {"nye", "にぇ"}, {"hya", "ひゃ"}, {"hyu", "ひゅ"},
    {"hyo", "ひょ"}, {"hye", "ひぇ"}, {"bya", "びゃ"}, {"byu", "びゅ"}, {"byo", "びょ"},
    {"bye", "びぇ"}, {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"}, {"pye", "ぴぇ"},
    {"mya", "みゃ"}, {"myu", "みゅ"}, {"myo", "みょ"}, {"mye", "みぇ"}, {"rya", "りゃ"},
    {"ryu", "りゅ"}, {"ryo", "りょ"}, {"rye", "りぇ"}, {"fa", "ふぁ"},  {"fi", "ふぃ"},
    {"fe", "ふぇ"},  {"fo", "ふぉ"},  {"fyu", "ふゅ"}, {"va", "ゔぁ"},  {"vi", "ゔぃ"},
    {"vu", "ゔ"},    {"ve", "ゔぇ"},  {"vo", "ゔぉ"},  {"vyu", "ゔゅ"}, {"tsa", "つぁ"},
    {"tsi", "つぃ"}, {"tse", "つぇ"}, {"tso", "つぉ"}, {"thi", "てぃ"}, {"thu", "てゅ"},
    {"dhi", "でぃ"}, {"dhu", "でゅ"}, {"twa", "とぁ"}, {"twi", "とぃ"}, {"twu", "とぅ"},
    {"twe", "とぇ"}, {"two", "とぉ"}, {"dwa", "どぁ"}, {"dwi", "どぃ"}, {"dwu", "どぅ"},
    {"dwe", "どぇ"}, {"dwo", "どぉ"}, {"qa", "くぁ"},  {"qi", "くぃ"},  {"qu", "く"},
    {"qe", "くぇ"},  {"qo", "くぉ"},  {"kwa", "くぁ"}, {"kwi", "くぃ"}, {"kwe", "くぇ"},
    {"kwo", "くぉ"}, {"xa", "ぁ"},    {"xi", "ぃ"},    {"xu", "ぅ"},    {"xe", "ぇ"},
    {"xo", "ぉ"},    {"la", "ぁ"},    {"li", "ぃ"},    {"lu", "ぅ"},    {"le", "ぇ"},
    {"lo", "ぉ"},    {"xya", "ゃ"},   {"xyu", "ゅ"},   {"xyo", "ょ"},   {"lya", "ゃ"},
    {"lyu", "ゅ"},   {"lyo", "ょ"},   {"xtu", "っ"},   {"xtsu", "っ"},  {"ltu", "っ"},
    {"ltsu", "っ"},  {"xwa", "ゎ"},   {"lwa", "ゎ"},   {"xka", "ゕ"},   {"xke", "ゖ"},
    {"lka", "ゕ"},   {"lke", "ゖ"}};
bool vowel(char c) {
    return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o';
}
} // namespace
std::string Romaji::convert(std::string_view raw, bool final) {
    std::string result;
    for (size_t i = 0; i < raw.size();) {
        const char c = raw[i];
        if (c == 'n') {
            if (i + 1 == raw.size()) {
                result += final ? "ん" : "n";
                ++i;
                continue;
            }
            if (raw[i + 1] == '\'') {
                result += "ん";
                i += 2;
                continue;
            }
            if (raw[i + 1] == 'n') {
                result += "ん";
                i += i + 2 < raw.size() && (vowel(raw[i + 2]) || raw[i + 2] == 'y') ? 1 : 2;
                continue;
            }
            if (!vowel(raw[i + 1]) && raw[i + 1] != 'y') {
                result += "ん";
                ++i;
                continue;
            }
        }
        if (i + 1 < raw.size() && c == raw[i + 1] && c >= 'b' && c <= 'z' && !vowel(c) &&
            c != 'n' && c != 'x' && c != 'l') {
            result += "っ";
            ++i;
            continue;
        }
        if (i + 2 < raw.size() && c == 't' && raw[i + 1] == 'c' && raw[i + 2] == 'h') {
            result += "っ";
            ++i;
            continue;
        }
        size_t matched = 0;
        std::string_view kana;
        bool pending = false;
        for (const auto &[latin, value] : rules) {
            if (latin.size() > matched && raw.substr(i).starts_with(latin)) {
                matched = latin.size();
                kana = value;
            }
            if (latin.starts_with(raw.substr(i)))
                pending = true;
        }
        if (matched) {
            result += kana;
            i += matched;
            continue;
        }
        if (pending && !final) {
            result += raw.substr(i);
            break;
        }
        switch (c) {
        case ',':
            result += "、";
            break;
        case '.':
            result += "。";
            break;
        case '-':
            result += "ー";
            break;
        case '[':
            result += "「";
            break;
        case ']':
            result += "」";
            break;
        default:
            result += c;
        }
        ++i;
    }
    return result;
}
} // namespace newime
