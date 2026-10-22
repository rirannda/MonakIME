// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
// Read the generated configuration through fcitx5's real configuration parser.
#include <fcitx-config/iniparser.h>
#include <fcitx/globalconfig.h>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
    fcitx::GlobalConfig config;
    if (argc > 1) {
        std::ifstream stream(argv[1]);
        if (!stream)
            return 1;
        fcitx::RawConfig raw;
        fcitx::readFromIni(raw, stream);
        config.load(raw);
    }
    const auto print = [](const char *name, const fcitx::KeyList &keys) {
        std::cout << name;
        for (const auto &key : keys)
            std::cout << '\t' << key.toString();
        std::cout << '\n';
    };
    print("alt", config.altTriggerKeys());
    print("trigger", config.triggerKeys());
    print("backward", config.enumerateBackwardKeys());
}
