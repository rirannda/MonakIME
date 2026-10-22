// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime/mozc.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
int main(int argc, char **argv) {
    // A native fake peer exercises timeout and cancelled-stream handling, without a runtime
    // dependency.
    if (argc == 2 && std::string(argv[1]).find("fake-") != std::string::npos) {
        const std::string greeting = "READY\n", reply = "にほんご\x1f日本語\n";
        (void)write(STDOUT_FILENO, greeting.data(), greeting.size());
        if (std::string(argv[1]).ends_with("fake-exit"))
            return 0;
        char request[4096];
        while (read(STDIN_FILENO, request, sizeof(request)) > 0) {
            if (std::string(argv[1]).ends_with("fake-timeout"))
                usleep(500000);
            (void)write(STDOUT_FILENO, reply.data(), reply.size());
        }
        return 0;
    }
    char pattern[] = "/tmp/newime-mozc-test-XXXXXX";
    const char *temp = mkdtemp(pattern);
    if (!temp)
        return 1;
    const std::filesystem::path root(temp);
    try {
        using newime::MozcEngine;
        auto numeric =
            MozcEngine::parse("CPUしようりつは12.5%", "CPU\x1fＣＰＵ\x1f"
                                                      "CPU\x1eしようりつは\x1f使用率は\x1e"
                                                      "12.5%\x1f１２．５％\x1f"
                                                      "12.5%");
        require(numeric && numeric->text() == "CPU使用率は12.5%", "ASCII preservation");
        require(!MozcEngine::parse("かな", "べつ\x1f別"), "mismatched segment reading rejected");
        require(!MozcEngine::parse("かな", "かな\x1f仮\n名"), "control characters rejected");
        require(!MozcEngine::parse("かな", "かな"), "malformed response rejected");
        newime::Dictionary dictionary;
        newime::Converter converter(dictionary);
        auto learned = MozcEngine::parse("はし", "はし\x1f橋\x1f箸");
        dictionary.learn("はし", "箸");
        converter.rank(*learned);
        require(learned->candidates.front().text == "箸", "learning ranks above Mozc");
        std::ofstream(root / "user.tsv") << "にほんご\t二本語\t1\n";
        dictionary.load((root / "user.tsv").string(), true);
        auto custom = MozcEngine::parse("にほんご", "にほんご\x1f日本語");
        converter.rank(*custom);
        require(custom->candidates.front().text == "二本語", "user dictionary ranks above Mozc");
        MozcEngine missing("/nonexistent-newime-helper", (root / "missing").string());
        require(!missing.convert("かな"), "missing helper is recoverable");
        require(!missing.lastError().empty(), "failure is observable");
        require(missing.convert("GitHub12.5%")->text() == "GitHub12.5%", "literal bypass");
        require(!missing.convert("かな\nQ"), "protocol injection rejected");
        require(!missing.convert(std::string(1025, 'a')), "input bound");
        MozcEngine peer("/proc/self/exe", (root / "fake-peer").string());
        int calls = 0;
        require(!peer.convert("にほんご", {}, [&] { return ++calls >= 2; }), "cancel after reply");
        auto afterCancel = peer.convert("にほんご");
        if (!afterCancel)
            std::cerr << "peer error: " << peer.lastError() << '\n';
        require(afterCancel && afterCancel->text() == "日本語", "cancelled reply drained");
        MozcEngine timeout("/proc/self/exe", (root / "fake-timeout").string());
        const auto start = std::chrono::steady_clock::now();
        require(!timeout.convert("にほんご"), "hung helper falls back");
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
                "timeout bounded");
        require(!timeout.convert("にほんご"), "failed helper is not respawned each keystroke");
        MozcEngine exited("/proc/self/exe", (root / "fake-exit").string());
        require(!exited.convert("にほんご"), "broken pipe does not terminate the host");
        if (argc == 2) {
            MozcEngine real(argv[1], (root / "real").string());
            auto result = real.convert("はしでごはんをたべる");
            require(result && result->text() == "橋でご飯を食べる", "real Mozc conversion");
            require(result->segments.size() == 3, "real bunsetsu boundaries");
            require(result->segments[0].candidates[1].text == "箸で", "real segment alternatives");
            result = real.convert("Googleでしらべて、GitHubのこーどをしゅうせいしました。");
            require(result && result->text() == "Googleで調べて、GitHubのコードを修正しました。",
                    "mixed English");
            result = real.convert("CPUしようりつは12.5%、めもりしようりょうは256MBでした。");
            require(result && result->text() == "CPU使用率は12.5%、メモリ使用量は256MBでした。",
                    "numbers and units");
            result = real.convert("にほんごk");
            require(result && result->text() == "日本語k", "incomplete romaji suffix");
        }
        std::cout
            << "Mozc protocol, bounds, learning, cancellation, timeout and conversion passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
    std::filesystem::remove_all(root);
}
