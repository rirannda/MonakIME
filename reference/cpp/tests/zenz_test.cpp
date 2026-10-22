// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime/zenz.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}
} // namespace

int main(int argc, char **argv) {
    using newime::ZenzEngine;
    try {
        require(ZenzEngine::make_prompt("きょうはいいてんき") == "\uEE00キョウハイイテンキ\uEE01",
                "v3 katakana protocol");
        require(ZenzEngine::make_prompt("みわ", "私の名前は") == "\uEE02私の名前は\uEE00ミワ\uEE01",
                "v3 context order");
        require(ZenzEngine::make_prompt("", "context").empty(), "empty reading rejected");
        require(ZenzEngine::make_prompt("かな\uEE01").empty(), "input marker injection rejected");
        require(ZenzEngine::make_prompt(std::string("\xff", 1)).empty(), "invalid UTF-8 rejected");
        require(ZenzEngine::make_prompt(std::string(129, 'a')).empty(), "reading bound");
        require(ZenzEngine::make_prompt("かな", "左\uEE01\n文脈") ==
                    "\uEE02左　文脈\uEE00カナ\uEE01",
                "context markers removed");
        require(ZenzEngine::make_prompt("かな", "abc def") == "\uEE02abc　def\uEE00カナ\uEE01",
                "zenz tokenizer space normalization");
        require(ZenzEngine::valid_output("きょう", "今日"), "ordinary conversion accepted");
        require(!ZenzEngine::valid_output("きょう", "今日\n"), "multiline completion rejected");
        require(!ZenzEngine::valid_output("きょう", "今日\uEE00"), "output marker rejected");
        require(!ZenzEngine::valid_output("きょう", "今日<s>"), "literal EOS rejected");
        require(!ZenzEngine::valid_output("きょう", std::string("\xc0\x80", 2)),
                "overlong UTF-8 rejected");
        require(!ZenzEngine::valid_output("あ", std::string(60, 'a')), "runaway output rejected");

        newime::ZenzConfig missing_config;
        missing_config.model_path = "/nonexistent/newime-model.gguf";
        ZenzEngine missing(missing_config);
        require(!missing.loaded(), "model loading is lazy");
        require(!missing.convert("きょう", {}, [] { return true; }), "cancelled request rejected");
        require(!missing.loaded(), "cancel before model loading");
        require(!missing.convert("きょう"), "missing model falls back");
        require(!missing.last_error().empty(), "model error is observable");
        require(!missing.convert("きょう"), "repeated missing model falls back");
        require(!missing.last_error().empty(), "cached load error remains observable");
        missing.unload();
        require(missing.last_error().empty(), "unload resets failure state");

        const char *model = argc > 1 ? argv[1] : std::getenv("NEWIME_TEST_MODEL");
        if (model && *model && ZenzEngine::compiled_with_llama()) {
            newime::ZenzConfig config;
            config.model_path = model;
            config.budget_ms = 3000;
            config.threads = 2;
            ZenzEngine engine(config);
            for (const std::string reading :
                 {"きょうはいいてんき", "にほんごにゅうりょく", "とうきょう"}) {
                const auto started = std::chrono::steady_clock::now();
                const auto output = engine.convert(reading);
                require(output.has_value(), "real model conversion failed: " + engine.last_error());
                require(ZenzEngine::valid_output(reading, *output), "real model output invalid");
                std::cout << reading << " -> " << *output << " ("
                          << std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - started)
                                 .count()
                          << " ms)\n";
            }
            require(engine.loaded(), "model remains warm for next conversion");
            require(!engine.convert("きょう", {}, [] { return true; }), "warm cancellation");
            engine.unload();
            require(!engine.loaded(), "model unload frees state");
        }
        std::cout << "zenz tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "zenz test failed: " << error.what() << '\n';
        return 1;
    }
}
