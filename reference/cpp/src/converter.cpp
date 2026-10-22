// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime/converter.h"
#include "newime/romaji.h"
#include "newime/text.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace newime {
namespace {
constexpr size_t maxReading = 256;
constexpr size_t maxWord = 48;
bool allAscii(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c < 128; });
}
bool hasAsciiLetter(std::string_view s) {
    return std::any_of(s.begin(), s.end(), text::isAsciiLetter);
}
bool uppercase(char c) {
    return c >= 'A' && c <= 'Z';
}
bool acronym(std::string_view s) {
    return s.size() >= 2 && std::all_of(s.begin(), s.end(), uppercase);
}
bool suffixWord(std::string_view s) {
    constexpr std::string_view suffixes[] = {
        "は",   "が",   "を",   "に",     "で",     "と",   "も",     "の",     "へ",
        "から", "まで", "より", "だけ",   "です",   "ます", "でした", "ました", "します",
        "した", "する", "して", "される", "された", "では", "には",   "とは",   "ので",
        "ても", "って", "ない", "たい",   "た",     "て"};
    return std::find(std::begin(suffixes), std::end(suffixes), s) != std::end(suffixes);
}
void uniqueCandidates(std::vector<Candidate> &candidates, size_t limit) {
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const auto &a, const auto &b) { return a.score > b.score; });
    std::unordered_set<std::string> seen;
    std::erase_if(candidates, [&](const auto &v) { return !seen.insert(v.text).second; });
    if (candidates.size() > limit)
        candidates.resize(limit);
}
std::vector<Candidate> combine(const std::vector<Segment> &segments, size_t limit) {
    std::vector<Candidate> beam{{"", 0}};
    for (const auto &segment : segments) {
        std::vector<Candidate> next;
        for (const auto &prefix : beam)
            for (const auto &candidate : segment.candidates)
                next.push_back({prefix.text + candidate.text, prefix.score + candidate.score});
        uniqueCandidates(next, limit);
        beam = std::move(next);
    }
    return beam;
}
} // namespace
std::string Segment::text() const {
    return candidates.empty() ? reading
                              : candidates[std::min(selected, candidates.size() - 1)].text;
}
std::string Conversion::text() const {
    std::string result;
    for (const auto &segment : segments)
        result += segment.text();
    return result;
}
void Conversion::rebuildCandidates(size_t limit) {
    candidates = combine(segments, std::clamp<size_t>(limit, 1, 16));
}
void Converter::rank(Conversion &conversion, size_t limit) const {
    for (auto &segment : conversion.segments) {
        if (segment.literal)
            continue;
        for (const auto &learned : dictionary_.lookup(segment.reading, limit))
            if (learned.score >= 4) {
                std::erase_if(segment.candidates,
                              [&](const auto &c) { return c.text == learned.text; });
                segment.candidates.push_back(learned);
            }
        uniqueCandidates(segment.candidates, limit);
        segment.selected = 0;
    }
    conversion.rebuildCandidates(limit);
    for (const auto &learned : dictionary_.lookup(conversion.reading, limit))
        if (learned.score >= 4)
            conversion.candidates.push_back(learned);
    uniqueCandidates(conversion.candidates, limit);
}
std::vector<Candidate> Converter::candidates(std::string_view reading, size_t limit) const {
    limit = std::clamp<size_t>(limit, 1, 16);
    auto result = dictionary_.lookup(reading, limit);
    auto add = [&](std::string value, double score) {
        if (std::none_of(result.begin(), result.end(),
                         [&](const auto &x) { return x.text == value; }))
            result.push_back({std::move(value), score});
    };
    add(std::string(reading), -30);
    add(text::katakana(reading), -32);
    uniqueCandidates(result, limit);
    return result;
}
Conversion Converter::convert(std::string_view source, size_t limit) const {
    Conversion out;
    limit = std::clamp<size_t>(limit, 1, 16);
    if (source.size() > maxReading * 4) {
        out.reading = std::string(source);
        out.segments.push_back({out.reading, {{out.reading, 0}}, 0, true});
        out.candidates = {{out.reading, 0}};
        return out;
    }
    auto chars = text::utf8ToU32(text::hiragana(source));
    // Bound all lattice and beam allocations. A too-long composition remains lossless.
    if (chars.size() > maxReading) {
        out.reading = std::string(source);
        out.segments.push_back({out.reading, {{out.reading, 0}}, 0, true});
        out.candidates = {{out.reading, 0}};
        return out;
    }
    out.reading = text::u32ToUtf8(chars);
    if (chars.empty())
        return out;
    struct Path {
        double cost = std::numeric_limits<double>::infinity();
        size_t previous = 0;
        bool known = false;
    };
    std::vector<Path> best(chars.size() + 1);
    best[0].cost = 0;
    for (size_t i = 0; i < chars.size(); ++i) {
        auto offer = [&](size_t end, double cost, bool known) {
            if (best[i].cost + cost < best[end].cost)
                best[end] = {best[i].cost + cost, i, known};
        };
        if (!text::isKana(chars[i])) {
            size_t end = i + 1;
            while (end < chars.size() && !text::isKana(chars[end]))
                ++end;
            offer(end, 0, false);
            continue;
        }
        offer(i + 1, 5, false);
        std::u32string word;
        for (size_t end = i; end < chars.size() && end < i + maxWord && text::isKana(chars[end]);
             ++end) {
            word.push_back(chars[end]);
            auto reading = text::u32ToUtf8(word);
            auto matches = dictionary_.lookup(reading, 1);
            if (i > 0 && suffixWord(reading))
                offer(end + 1, 0.2, true);
            if (matches.empty())
                continue;
            double len = static_cast<double>(word.size());
            double lexicalCost =
                3 + std::max(-8.0, -matches.front().score) * 0.35 - std::min(3.0, len * len * 0.1);
            // A frequent stem followed by a particle is stronger evidence than
            // an unrelated same-reading word (きょうは -> 今日 + は, 教派).
            for (size_t split = 3; split < word.size(); ++split) {
                auto stem = text::u32ToUtf8(std::u32string_view(word).substr(0, split));
                auto tail = text::u32ToUtf8(std::u32string_view(word).substr(split));
                if (!suffixWord(tail))
                    continue;
                auto stemMatches = dictionary_.lookup(stem, 1);
                if (!stemMatches.empty() && stemMatches.front().score >= -2 &&
                    !matches.front().text.starts_with(stemMatches.front().text)) {
                    lexicalCost += 2;
                    break;
                }
            }
            offer(end + 1, lexicalCost, true);
        }
    }
    struct Span {
        size_t begin, end;
        bool known;
    };
    std::vector<Span> spans;
    for (size_t end = chars.size(); end > 0;) {
        auto &step = best[end];
        spans.push_back({step.previous, end, step.known});
        end = step.previous;
    }
    std::reverse(spans.begin(), spans.end());
    for (size_t i = 0; i < spans.size();) {
        auto span = spans[i++];
        if (!span.known && text::isKana(chars[span.begin]))
            while (i < spans.size() && !spans[i].known && text::isKana(chars[spans[i].begin]))
                span.end = spans[i++].end;
        auto reading =
            text::u32ToUtf8(std::u32string_view(chars).substr(span.begin, span.end - span.begin));
        bool literal = !text::isKana(chars[span.begin]);
        std::vector<Candidate> choices;
        if (literal)
            choices.push_back({reading, 0});
        else
            choices = candidates(reading, limit);
        // Bunsetsu = lexical stem plus grammatical suffixes. Keep the user's word intact.
        if (!literal && suffixWord(reading) && !out.segments.empty() &&
            !out.segments.back().literal) {
            auto &previous = out.segments.back();
            previous.reading += reading;
            for (auto &candidate : previous.candidates)
                candidate.text += reading;
            auto exact = dictionary_.lookup(previous.reading, limit);
            previous.candidates.insert(previous.candidates.end(), exact.begin(), exact.end());
            uniqueCandidates(previous.candidates, limit);
        } else
            out.segments.push_back({reading, std::move(choices), 0, literal});
    }
    out.candidates = combine(out.segments, limit);
    auto exact = dictionary_.lookup(out.reading, limit);
    out.candidates.insert(out.candidates.end(), exact.begin(), exact.end());
    uniqueCandidates(out.candidates, limit);
    return out;
}
std::string Converter::compose(std::string_view raw, bool final) const {
    // Opaque URLs, addresses and identifiers are never reparsed as Japanese.
    constexpr std::string_view domainSuffixes[] = {".com", ".org", ".net", ".dev",  ".io",
                                                   ".jp",  ".edu", ".gov", ".local"};
    const bool domain = std::any_of(std::begin(domainSuffixes), std::end(domainSuffixes),
                                    [&](std::string_view suffix) {
                                        return raw.size() > suffix.size() && raw.ends_with(suffix);
                                    });
    if (allAscii(raw) &&
        (domain || raw.find("://") != raw.npos || raw.find('@') != raw.npos ||
         raw.find('_') != raw.npos || raw.find('/') != raw.npos || raw.find('\\') != raw.npos))
        return std::string(raw);
    if (raw.size() > 1024)
        return std::string(raw);
    std::string output;
    for (size_t start = 0; start < raw.size();) {
        if (!text::isAsciiLetter(raw[start])) {
            char c = raw[start++];
            const bool literalBefore =
                !output.empty() && (text::isAsciiLetter(output.back()) ||
                                    (output.back() >= '0' && output.back() <= '9'));
            if ((c == '.' || c == '-') && literalBefore)
                output += c;
            else
                output += Romaji::convert(std::string_view(&c, 1), final);
            continue;
        }
        size_t end = start;
        while (end < raw.size() && (text::isAsciiLetter(raw[end]) || raw[end] == '\''))
            ++end;
        auto token = raw.substr(start, end - start);
        const bool tokenFinal = final || end < raw.size();
        if (acronym(token)) {
            output += token;
            start = end;
            continue;
        }
        // Keep unknown names lossless, but a known word/acronym is only a prefix:
        // Google+desu, GitHub+noko-do, CPU+shiyouritsu, MB+deshita.
        if (token.size() >= 3 && uppercase(token.front())) {
            bool knownPrefix = false;
            for (size_t len = 2; len <= maxWord && len <= token.size(); ++len) {
                const auto prefix = token.substr(0, len);
                if (acronym(prefix) || (len >= 3 && dictionary_.english(prefix))) {
                    knownPrefix = true;
                    break;
                }
            }
            if (!knownPrefix) {
                output += token;
                start = end;
                continue;
            }
        }
        size_t n = token.size();
        struct State {
            double score = -std::numeric_limits<double>::infinity();
            size_t prev = 0;
            std::string value;
            bool english = false;
        };
        std::vector<std::array<State, 2>> best(n + 1);
        best[0][0].score = 0;
        for (size_t i = 0; i < n; ++i)
            for (size_t state = 0; state < 2; ++state) {
                if (!std::isfinite(best[i][state].score))
                    continue;
                auto offer = [&](size_t to, bool english, double gain, std::string value) {
                    double score = best[i][state].score + gain;
                    if (score > best[to][english].score)
                        best[to][english] = {score, i, std::move(value), state != 0};
                };
                // Emit one complete romaji phoneme; an unfinished phoneme stays as typed.
                size_t consume = 1;
                std::string kana;
                if (token[i] == 'n' && i + 1 < n && token[i + 1] == 'n' && i + 2 < n &&
                    (std::string_view("aiueoy").find(token[i + 2]) != std::string_view::npos))
                    kana = "ん";
                else if (token[i] == 'n' && i + 1 < n && token[i + 1] != 'n' &&
                         token[i + 1] != 'y' &&
                         std::string_view("aiueo").find(token[i + 1]) == std::string_view::npos &&
                         token[i + 1] != '\'')
                    kana = "ん";
                else if (i + 1 < n && token[i] == token[i + 1] && token[i] >= 'b' &&
                         token[i] <= 'z' &&
                         std::string_view("aiueonxl").find(token[i]) == std::string_view::npos)
                    kana = "っ";
                else if (i + 2 < n && token.substr(i, 3) == "tch")
                    kana = "っ";
                for (size_t len = 1; kana.empty() && len <= 4 && i + len <= n; ++len) {
                    auto part = token.substr(i, len);
                    auto value = Romaji::convert(part, tokenFinal && i + len == n);
                    if (!hasAsciiLetter(value)) {
                        consume = len;
                        kana = std::move(value);
                        break;
                    }
                }
                if (kana.empty())
                    kana = Romaji::convert(token.substr(i, 1), tokenFinal && i + 1 == n);
                offer(i + consume, false, hasAsciiLetter(kana) ? -0.7 : 0.0, std::move(kana));
                for (size_t len = 2; len <= maxWord && i + len <= n; ++len) {
                    auto word = token.substr(i, len);
                    const bool capitalAcronym = uppercase(word.front()) && acronym(word);
                    if (!capitalAcronym && (len < 3 || !dictionary_.english(word)))
                        continue;
                    const bool markedLatin = std::any_of(word.begin(), word.end(), uppercase);
                    // Do not take 'hash' from 'hashi' or 'tab' from 'taberu':
                    // the Latin boundary would split a complete romaji syllable.
                    if (!markedLatin && i + len < n &&
                        std::string_view("aiueoy").find(token[i + len]) != std::string_view::npos &&
                        std::string_view("bcdfghjklmpqrstvwxyz").find(word.back()) !=
                            std::string_view::npos)
                        continue;
                    auto japanese = Romaji::convert(word, true);
                    // A short, kana-compatible English word inside Japanese is
                    // weak evidence (syu+use+i -> しゅうせい, not しゅuseい).
                    if (!markedLatin && len == 3 && i > 0 && state == 0 &&
                        !hasAsciiLetter(japanese))
                        continue;
                    // Ambiguous Latin/Japanese words (sushi, tomato) favor a known Japanese
                    // reading.
                    if (!markedLatin && !hasAsciiLetter(japanese) && dictionary_.contains(japanese))
                        continue;
                    double l = static_cast<double>(len);
                    offer(i + len, true, 0.12 * l * l - (state ? 2.5 : 0.5), std::string(word));
                }
            }
        size_t state = best[n][1].score > best[n][0].score ? 1 : 0;
        std::vector<std::string> reversed;
        for (size_t i = n; i > 0;) {
            auto &p = best[i][state];
            reversed.push_back(p.value);
            i = p.prev;
            state = p.english ? 1 : 0;
        }
        for (auto it = reversed.rbegin(); it != reversed.rend(); ++it)
            output += *it;
        start = end;
    }
    return output;
}
bool Converter::resize(Conversion &conversion, size_t segment, int delta) const {
    if (segment >= conversion.segments.size() || delta == 0)
        return false;
    auto &current = conversion.segments[segment];
    if (current.literal)
        return false;
    auto chars = text::utf8ToU32(current.reading);
    if (delta < 0) {
        if (chars.size() <= 1)
            return false;
        auto tail = text::u32ToUtf8(std::u32string_view(chars).substr(chars.size() - 1));
        chars.pop_back();
        current.reading = text::u32ToUtf8(chars);
        current.candidates = candidates(current.reading);
        current.selected = 0;
        std::string remainder = tail;
        for (size_t i = segment + 1; i < conversion.segments.size(); ++i)
            remainder += conversion.segments[i].reading;
        conversion.segments.resize(segment + 1);
        auto suffix = convert(remainder);
        conversion.segments.insert(conversion.segments.end(), suffix.segments.begin(),
                                   suffix.segments.end());
    } else {
        if (segment + 1 >= conversion.segments.size() || conversion.segments[segment + 1].literal)
            return false;
        auto next = text::utf8ToU32(conversion.segments[segment + 1].reading);
        if (next.empty())
            return false;
        chars.push_back(next.front());
        next.erase(next.begin());
        current.reading = text::u32ToUtf8(chars);
        current.candidates = candidates(current.reading);
        current.selected = 0;
        std::string remainder = text::u32ToUtf8(next);
        for (size_t i = segment + 2; i < conversion.segments.size(); ++i)
            remainder += conversion.segments[i].reading;
        conversion.segments.resize(segment + 1);
        auto suffix = convert(remainder);
        conversion.segments.insert(conversion.segments.end(), suffix.segments.begin(),
                                   suffix.segments.end());
    }
    conversion.candidates = combine(conversion.segments, 9);
    return true;
}

ReadingCheck Converter::checkReading(std::string_view reading, std::string_view surface) const {
    auto input = text::utf8ToU32(text::hiragana(reading));
    auto output = text::utf8ToU32(text::hiragana(surface));
    if (input.empty() || input.size() > 128 || output.empty() || output.size() > 256)
        return ReadingCheck::Mismatch;
    auto latinRuns = [](const std::u32string &s) {
        std::vector<std::u32string> runs;
        for (size_t i = 0; i < s.size();) {
            auto ascii = [](char32_t c) {
                return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') ||
                       (c >= U'0' && c <= U'9');
            };
            if (!ascii(s[i])) {
                ++i;
                continue;
            }
            size_t start = i;
            while (i < s.size() && ascii(s[i]))
                ++i;
            runs.push_back(s.substr(start, i - start));
        }
        return runs;
    };
    if (latinRuns(input) != latinRuns(output))
        return ReadingCheck::Mismatch;
    const bool kanaOnly =
        std::all_of(output.begin(), output.end(), [](char32_t c) { return text::isKana(c); });
    if (kanaOnly)
        return input == output ? ReadingCheck::Verified : ReadingCheck::Mismatch;
    const size_t width = output.size() + 1;
    std::vector<unsigned char> reachable((input.size() + 1) * width, 0);
    reachable[0] = 1;
    for (size_t i = 0; i < input.size(); ++i) {
        std::vector<std::pair<size_t, std::u32string>> words;
        std::u32string part;
        for (size_t end = i; end < input.size() && end < i + 48; ++end) {
            part.push_back(input[end]);
            for (const auto &candidate : dictionary_.lookup(text::u32ToUtf8(part), 32))
                words.emplace_back(end + 1, text::utf8ToU32(text::hiragana(candidate.text)));
        }
        for (size_t j = 0; j < output.size(); ++j) {
            if (!reachable[i * width + j])
                continue;
            if (input[i] == output[j] || (input[i] == U' ' && output[j] == U'　'))
                reachable[(i + 1) * width + j + 1] = 1;
            for (const auto &[end, value] : words) {
                if (value.size() <= output.size() - j &&
                    std::equal(value.begin(), value.end(), output.begin() + j))
                    reachable[end * width + j + value.size()] = 1;
            }
        }
    }
    return reachable[input.size() * width + output.size()] ? ReadingCheck::Verified
                                                           : ReadingCheck::Unverified;
}
} // namespace newime
