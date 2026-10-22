// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime/converter.h"
#include "newime/romaji.h"
#include "newime/text.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
void check(bool value, const char *message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
void expectEqual(std::string_view actual, std::string_view expected, const char *message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << " actual=" << actual << " expected=" << expected
                  << '\n';
        std::exit(1);
    }
}
} // namespace
int main(int argc, char **argv) {
    using namespace newime;
    expectEqual(Romaji::convert("konnichiha", true), "こんにちは", "basic romaji");
    expectEqual(Romaji::convert("gakkou", true), "がっこう", "double consonant");
    expectEqual(Romaji::convert("kinnyuu", true), "きんにゅう", "nn before yoon");
    expectEqual(Romaji::convert("kan'i", true), "かんい", "explicit n boundary");
    expectEqual(Romaji::convert("shin", false), "しn", "pending n");
    expectEqual(Romaji::convert("shin", true), "しん", "final n");
    expectEqual(Romaji::convert("matcha", true), "まっちゃ", "tch");
    expectEqual(text::hiragana("カタカナ"), "かたかな", "katakana normalization");
    expectEqual(text::katakana("ゔぁかな"), "ヴァカナ", "katakana presentation");
    expectEqual(text::u32ToUtf8(text::utf8ToU32("日本🌸")), "日本🌸",
                "UTF-8 supplementary roundtrip");
    check(text::codepoints("日本🌸") == 3, "unicode codepoint count");
    Dictionary dictionary;
    check(dictionary.load(argc > 1 ? argv[1] : "data/base.tsv"), "load bootstrap dictionary");
    dictionary.add("とうきょうだいがく", "東京大学", 1);
    Converter converter(dictionary);
    expectEqual(converter.compose("konnichiha", true), "こんにちは", "compose nn");
    expectEqual(converter.compose("gakkou", true), "がっこう", "compose sokuon");
    expectEqual(converter.compose("kinnyuu", true), "きんにゅう", "compose nn yoon");
    expectEqual(converter.convert("わたしはにほんごをにゅうりょくします").text(),
                "私は日本語を入力します", "bunsetsu conversion");
    auto longWord = converter.convert("とうきょうだいがく");
    check(longWord.segments.size() == 1, "preserve long recognized word");
    expectEqual(longWord.text(), "東京大学", "recognized long word");
    auto unknown = converter.convert("ぬほぴゃふょ");
    check(unknown.segments.size() == 1, "merge unknown kana run");
    expectEqual(unknown.text(), "ぬほぴゃふょ", "unknown is lossless");
    auto mixed = converter.convert("GitHubでかいはつ");
    expectEqual(mixed.text(), "GitHubで開発", "literal latin is lossless");
    auto temp =
        std::filesystem::temp_directory_path() / ("newime-core-" + std::to_string(getpid()));
    std::filesystem::create_directories(temp);
    {
        std::ofstream stream(temp / "english.txt");
        stream << "google\ngithub\nstring\nalcoholic\nmost\nsushi\nuse\ncode\n";
    }
    check(dictionary.loadEnglish((temp / "english.txt").string()), "load English words");
    {
        std::ofstream stream(temp / "developer-english.txt");
        stream << "hash\ntab\n";
    }
    dictionary.loadEnglish((temp / "developer-english.txt").string());
    expectEqual(converter.compose("hashiwowataru", true), "はしをわたる",
                "English cannot split hashi");
    expectEqual(converter.compose("gohanwotaberu", true), "ごはんをたべる",
                "English cannot split taberu");
    expectEqual(converter.compose("kyouhagoogle", true), "きょうはgoogle",
                "mixed language boundary");
    expectEqual(converter.compose("githubdekanji", true), "githubでかんじ", "latin plus Japanese");
    expectEqual(converter.compose("sushi", true), "すし", "ambiguous Japanese word is protected");
    expectEqual(converter.compose("https://google.com/a", true), "https://google.com/a",
                "opaque URL");
    expectEqual(converter.compose("foo_bar", true), "foo_bar", "opaque identifier");
    expectEqual(converter.compose("IME", true), "IME", "uppercase acronym");
    expectEqual(converter.compose("Googledeshirabete", true), "Googleでしらべて",
                "capitalized English prefix must not capture Japanese suffix");
    expectEqual(converter.compose("GitHubnoko-dowosyuuseishimashita.", true),
                "GitHubのこーどをしゅうせいしました。", "mixed-case name and long vowel");
    expectEqual(converter.compose("CPUsiyourituha12.5%", true), "CPUしようりつは12.5%",
                "uppercase acronym followed by Japanese and decimal");
    expectEqual(converter.compose("256MBdesita.", true), "256MBでした。",
                "two-letter unit must not capture Japanese suffix");
    expectEqual(converter.compose("AIwotsukaimasu", true), "AIをつかいます",
                "two-letter acronym followed by Japanese");
    expectEqual(converter.compose("syuuseishimashita", true), "しゅうせいしました",
                "short English word inside Japanese");
    expectEqual(converter.compose("usedesuka", true), "useですか",
                "short English word at the start remains recognized");
    expectEqual(converter.compose("githubdecodewokaku", true), "githubでcodeをかく",
                "longer English word after Japanese remains recognized");
    expectEqual(converter.compose("Googledeshirabete,GitHubnoko-dowosyuuseishimashita.", true),
                "Googleでしらべて、GitHubのこーどをしゅうせいしました。",
                "reported mixed-case sentence");
    expectEqual(converter.compose("CPUsiyourituha12.5%,memorishiyouryouha256MBdesita.", true),
                "CPUしようりつは12.5%、めもりしようりょうは256MBでした。",
                "reported resource usage sentence");
    expectEqual(converter.compose("ko-hi-", true), "こーひー", "Japanese long vowel hyphens");
    expectEqual(converter.compose("nihon.", false), "にほん。", "punctuation finalizes pending n");
    expectEqual(converter.compose("google.com", true), "google.com", "English punctuation");
    expectEqual(converter.compose("10.5", true), "10.5", "decimal punctuation");
    expectEqual(converter.compose("2026-10-22", true), "2026-10-22", "numeric hyphens");
    dictionary.learn("はし", "箸");
    expectEqual(converter.convert("はし").text(), "箸", "learning changes ranking");
    check(dictionary.saveLearning((temp / "learn.tsv").string()), "atomic learning save");
    Dictionary loaded;
    check(loaded.loadLearning((temp / "learn.tsv").string()), "learning reload");
    expectEqual(loaded.lookup("はし").front().text, "箸", "persisted learning");
    for (size_t i = 0; i < 4200; ++i)
        dictionary.learn("よみ" + std::to_string(i), "語");
    check(dictionary.learningSize() == 4096, "bounded learning");
    auto resized = converter.convert("わたしはにほんご");
    auto original = resized.reading;
    check(converter.resize(resized, 0, -1), "shrink segment");
    std::string all;
    for (const auto &s : resized.segments)
        all += s.reading;
    expectEqual(all, original, "boundary edit preserves reading");
    check(converter.resize(resized, 0, 1), "grow segment");
    all.clear();
    for (const auto &s : resized.segments)
        all += s.reading;
    expectEqual(all, original, "grow preserves reading");
    {
        std::ofstream stream(temp / "bad.nimedic", std::ios::binary);
        stream << "NIMEDIC1";
    }
    check(!dictionary.load((temp / "bad.nimedic").string()), "reject truncated binary dictionary");
    check(converter.checkReading("にほんご", "日本語") == ReadingCheck::Verified,
          "dictionary verifies neural reading");
    check(converter.checkReading("githubでかく", "githubで書く") != ReadingCheck::Mismatch,
          "mixed literals preserved");
    check(converter.checkReading("githubでかく", "GitHubで書く") == ReadingCheck::Mismatch,
          "neural cannot alter Latin");
    check(converter.checkReading("かな", "かに") == ReadingCheck::Mismatch, "neural kana mismatch");
    std::filesystem::remove_all(temp);
    std::cout << "core tests passed\n";
}
