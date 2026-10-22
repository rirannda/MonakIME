// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime/config.h"
#include "newime/converter.h"
#include "newime/mozc.h"
#include "newime/text.h"
#include "newime/zenz.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <sys/resource.h>
#include <vector>

namespace {
std::string json(std::string_view value) {
    std::string result = "\"";
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 0x20) {
            result += "\\u00";
            result += digits[c >> 4];
            result += digits[c & 15];
        } else
            result += static_cast<char>(c);
    }
    return result + "\"";
}
void help() {
    std::cout << "NewIME 0.1.0 — CPU Japanese IME prototype\n"
                 "Usage: newime-cli [options] [romaji-or-kana]\n"
                 "  --json                 JSON candidates and segmentation\n"
                 "  --stdin                Read one conversion per line (model stays loaded)\n"
                 "  --no-model             Dictionary conversion only\n"
                 "  --live                 Mozc preview only (never runs Zenz)\n"
                 "  --mozc                 Mozc candidates followed by explicit Zenz\n"
                 "  --model PATH           Use a Zenz v3 GGUF model\n"
                 "  --context TEXT         Left context for Zenz\n"
                 "  --threads N            CPU inference threads (1..8)\n"
                 "  --budget-ms N          Inference deadline (10..5000 ms)\n"
                 "  --benchmark N          Core latency over N requests, one model sample\n"
                 "  --learn READING TEXT   Record a local preferred candidate\n"
                 "  --diagnose             Report paths and compiled capabilities\n";
}
int number(const std::string &text, int minimum, int maximum) {
    size_t end = 0;
    int value = std::stoi(text, &end);
    if (end != text.size() || value < minimum || value > maximum)
        throw std::runtime_error("numeric option out of range");
    return value;
}
struct Result {
    newime::Conversion conversion;
    std::string reading;
    bool neural = false;
    bool reading_verified = false;
    double model_ms = 0;
    std::string error;
    bool mozc = false;
    std::string mozc_error;
};
void printResult(const Result &result, bool as_json) {
    if (!as_json) {
        for (size_t i = 0; i < result.conversion.candidates.size(); ++i)
            std::cout << i + 1 << "\t" << result.conversion.candidates[i].text << '\n';
        return;
    }
    std::cout << "{\"reading\":" << json(result.reading)
              << ",\"neural\":" << (result.neural ? "true" : "false")
              << ",\"reading_verified\":" << (result.reading_verified ? "true" : "false")
              << ",\"model_ms\":" << result.model_ms << ",\"model_error\":" << json(result.error)
              << ",\"mozc\":" << (result.mozc ? "true" : "false")
              << ",\"mozc_error\":" << json(result.mozc_error) << ",\"candidates\":[";
    bool comma = false;
    for (const auto &candidate : result.conversion.candidates) {
        if (comma)
            std::cout << ',';
        std::cout << json(candidate.text);
        comma = true;
    }
    std::cout << "],\"segments\":[";
    comma = false;
    for (const auto &segment : result.conversion.segments) {
        if (comma)
            std::cout << ',';
        std::cout << "{\"reading\":" << json(segment.reading)
                  << ",\"text\":" << json(segment.text()) << '}';
        comma = true;
    }
    std::cout << "]}\n";
}
} // namespace

int main(int argc, char **argv) {
    try {
        auto config = newime::RuntimeConfig::fromEnvironment();
        std::string input, context, learnReading, learnSurface;
        bool asJson = false, stdinMode = false, diagnose = false;
        bool live = false, useMozc = false;
        int benchmark = 0;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto next = [&]() -> std::string {
                if (++i >= argc)
                    throw std::runtime_error("missing argument after " + arg);
                return argv[i];
            };
            if (arg == "--help" || arg == "-h") {
                help();
                return 0;
            }
            if (arg == "--json")
                asJson = true;
            else if (arg == "--stdin")
                stdinMode = true;
            else if (arg == "--diagnose")
                diagnose = true;
            else if (arg == "--no-model")
                config.model_path.clear();
            else if (arg == "--live")
                live = useMozc = true;
            else if (arg == "--mozc")
                useMozc = true;
            else if (arg == "--model")
                config.model_path = next();
            else if (arg == "--context")
                context = next();
            else if (arg == "--threads")
                config.threads = number(next(), 1, 8);
            else if (arg == "--budget-ms")
                config.inference_budget_ms = number(next(), 10, 5000);
            else if (arg == "--benchmark")
                benchmark = number(next(), 1, 1000000);
            else if (arg == "--learn") {
                learnReading = next();
                learnSurface = next();
            } else if (arg.starts_with("--"))
                throw std::runtime_error("unknown option: " + arg);
            else if (!input.empty())
                throw std::runtime_error("provide a single input argument (quote spaces)");
            else
                input = std::move(arg);
        }
        if (live)
            config.model_path.clear();
        newime::Dictionary dictionary;
        dictionary.load(config.system_dictionary_path);
        const bool baseLoaded = dictionary.load(config.dictionary_path);
        dictionary.loadEnglish(config.english_dictionary_path);
        dictionary.load(config.user_dictionary_path, true);
        if (config.enable_learning)
            dictionary.loadLearning(config.learning_path);
        if (!baseLoaded && dictionary.size() == 0)
            throw std::runtime_error("no dictionary found; set NEWIME_DATA_DIR");
        newime::Converter converter(dictionary);
        if (!learnReading.empty()) {
            if (!config.enable_learning)
                throw std::runtime_error("learning is disabled");
            dictionary.learn(learnReading, learnSurface);
            if (!dictionary.saveLearning(config.learning_path))
                throw std::runtime_error("could not save learning");
            std::cout << "Saved " << learnReading << " -> " << learnSurface << '\n';
            return 0;
        }
        if (diagnose) {
            std::cout << "{\"version\":\"0.1.0\",\"llama\":"
                      << (newime::ZenzEngine::compiled_with_llama() ? "true" : "false")
                      << ",\"cpu_only\":true,\"dictionary_entries\":" << dictionary.size()
                      << ",\"learning_entries\":" << dictionary.learningSize()
                      << ",\"system_dictionary\":" << json(config.system_dictionary_path)
                      << ",\"model\":" << json(config.model_path) << ",\"model_exists\":"
                      << (std::filesystem::is_regular_file(config.model_path) ? "true" : "false")
                      << ",\"threads\":" << config.threads
                      << ",\"budget_ms\":" << config.inference_budget_ms
                      << ",\"learning_path\":" << json(config.learning_path)
                      << ",\"mozc_helper\":" << json(config.mozc_helper_path) << ",\"mozc_exists\":"
                      << (std::filesystem::is_regular_file(config.mozc_helper_path) ? "true"
                                                                                    : "false")
                      << ",\"mozc_profile\":" << json(config.mozc_profile_path)
                      << ",\"live_delay_ms\":" << config.live_delay_ms << "}\n";
            return 0;
        }
        newime::ZenzConfig modelConfig;
        modelConfig.model_path = config.model_path;
        modelConfig.threads = config.threads;
        modelConfig.budget_ms = config.inference_budget_ms;
        newime::ZenzEngine engine(modelConfig);
        newime::MozcEngine mozc(config.mozc_helper_path, config.mozc_profile_path);
        auto convert = [&](std::string_view raw) {
            if (raw.size() > config.max_input_chars * 4 ||
                newime::text::codepoints(raw) > config.max_input_chars)
                throw std::runtime_error("input exceeds configured max_input_chars");
            Result result;
            result.reading = converter.compose(raw, !live);
            result.conversion = converter.convert(result.reading);
            if (useMozc) {
                auto converted = mozc.convert(result.reading, context);
                if (converted) {
                    result.conversion = std::move(*converted);
                    converter.rank(result.conversion);
                    result.mozc = true;
                }
                result.mozc_error = mozc.lastError();
            }
            const auto readingChars = newime::text::utf8ToU32(result.reading);
            if (!config.model_path.empty() &&
                std::any_of(readingChars.begin(), readingChars.end(), newime::text::isKana)) {
                auto start = std::chrono::steady_clock::now();
                auto neural = engine.convert(result.reading, context);
                result.model_ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
                result.error = engine.last_error();
                const auto check = neural ? converter.checkReading(result.reading, *neural)
                                          : newime::ReadingCheck::Mismatch;
                if (neural && check != newime::ReadingCheck::Mismatch) {
                    auto &candidates = result.conversion.candidates;
                    bool preferLearning = false;
                    auto learned = dictionary.lookup(result.reading, 1);
                    if (!learned.empty() && learned.front().score >= 4)
                        preferLearning = true;
                    for (const auto &segment : result.conversion.segments) {
                        learned = dictionary.lookup(segment.reading, 1);
                        if (!learned.empty() && learned.front().score >= 4)
                            preferLearning = true;
                    }
                    const bool alreadyPreferred =
                        preferLearning && !candidates.empty() && candidates.front().text == *neural;
                    if (!alreadyPreferred) {
                        candidates.erase(
                            std::remove_if(candidates.begin(), candidates.end(),
                                           [&](const auto &c) { return c.text == *neural; }),
                            candidates.end());
                        candidates.insert(candidates.begin() +
                                              (preferLearning && !candidates.empty() ? 1 : 0),
                                          {*neural, -100});
                    }
                    if (candidates.size() > 9)
                        candidates.resize(9);
                    result.neural = true;
                    result.reading_verified = check == newime::ReadingCheck::Verified;
                } else if (neural)
                    result.error = "zenz output changed literal characters or kana reading";
            }
            return result;
        };
        std::cout << std::fixed << std::setprecision(3);
        if (benchmark) {
            const std::vector<std::string> corpus =
                input.empty() ? std::vector<std::string>{"kyouhatennkigaii", "nihongonyuuryoku",
                                                         "toukyou",          "hashi",
                                                         "googlede検索",     "konnichiha",
                                                         "shinkansen",       "jinkouchinou"}
                              : std::vector<std::string>{input};
            for (const auto &raw : corpus)
                (void)converter.convert(converter.compose(raw, true));
            std::vector<double> times;
            times.reserve(benchmark);
            const auto cpuStart = std::clock();
            const auto wallStart = std::chrono::steady_clock::now();
            for (int i = 0; i < benchmark; ++i) {
                auto start = std::chrono::steady_clock::now();
                auto result = converter.convert(converter.compose(corpus[i % corpus.size()], true));
                if (result.reading.empty())
                    throw std::runtime_error("empty benchmark result");
                times.push_back(std::chrono::duration<double, std::micro>(
                                    std::chrono::steady_clock::now() - start)
                                    .count());
            }
            auto wall =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
            const double cpu = static_cast<double>(std::clock() - cpuStart) / CLOCKS_PER_SEC;
            rusage usage{};
            getrusage(RUSAGE_SELF, &usage);
            std::sort(times.begin(), times.end());
            auto percentile = [&](double p) {
                return times[static_cast<size_t>(std::floor((times.size() - 1) * p))];
            };
            std::cout << "{\"requests\":" << benchmark << ",\"core_p50_us\":" << percentile(0.50)
                      << ",\"core_p95_us\":" << percentile(0.95)
                      << ",\"core_p99_us\":" << percentile(0.99) << ",\"core_wall_s\":" << wall
                      << ",\"core_cpu_s\":" << cpu << ",\"core_peak_rss_kib\":" << usage.ru_maxrss
                      << ",\"dictionary_entries\":" << dictionary.size() << "}\n";
            if (!config.model_path.empty())
                printResult(convert(corpus.front()), true);
            return 0;
        }
        if (stdinMode) {
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line.empty())
                    continue;
                printResult(convert(line), asJson);
                std::cout.flush();
            }
            return 0;
        }
        if (input.empty()) {
            help();
            return 0;
        }
        printResult(convert(input), asJson);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "newime-cli: " << error.what() << '\n';
        return 1;
    }
}
