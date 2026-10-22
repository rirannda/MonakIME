// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
// Run under the real launcher environment; never reset paths using the testing helper.
#include <cstdlib>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/instance.h>
#include <filesystem>
#include <iostream>

int main() {
    using namespace fcitx;
    const auto &paths = StandardPaths::global();
    if (const char *custom = std::getenv("MONAKIME_LAUNCHER_TEST_CUSTOM_DIR")) {
        if (paths.locate(StandardPathsType::Addon, "monakime-launcher-test-marker") !=
            std::filesystem::path(custom) / "monakime-launcher-test-marker") {
            std::cerr << "Launcher discarded caller addon directories\n";
            return 1;
        }
    }
    const auto installed = StandardPaths::fcitxPath("addondir");
    // Check each installed library without opening a display or a DBus session.
    for (const char *library : {"libdbus.so", "libwayland.so", "libxcb.so", "libclassicui.so",
                                "libclipboard.so", "libquickphrase.so", "libimselector.so"}) {
        if (std::filesystem::exists(installed / library) &&
            paths.locate(StandardPathsType::Addon, library).empty()) {
            std::cerr << "Launcher hid the installed addon: " << library << '\n';
            return 1;
        }
    }
    for (const char *metadata : {"addon/monakime.conf", "inputmethod/monakime.conf"}) {
        if (paths.locate(StandardPathsType::PkgData, metadata).empty()) {
            std::cerr << "Launcher cannot find: " << metadata << '\n';
            return 1;
        }
    }
    if (paths.locate(StandardPathsType::Addon, "monakime.so").empty()) {
        std::cerr << "Launcher cannot find monakime.so\n";
        return 1;
    }
    char name[] = "monakime-launcher-test";
    char disable[] = "--disable=all";
    char enable[] = "--enable=keyboard,monakime";
    char *argv[] = {name, disable, enable};
    Log::setLogRule("default=3");
    Instance instance(3, argv);
    instance.addonManager().registerDefaultLoader(nullptr);
    EventDispatcher dispatcher;
    dispatcher.attach(&instance.eventLoop());
    int result = 0;
    dispatcher.schedule([&] {
        const auto *entry = instance.inputMethodManager().entry("monakime");
        const auto *legacy = instance.inputMethodManager().entry("newime");
        if (!instance.addonManager().addon("monakime", true) || !entry || !legacy ||
            entry->name().find("MonakIME") != 0 || legacy->addon() != "monakime") {
            std::cerr << "MonakIME is missing from the normal input method enumeration\n";
            result = 1;
        } else {
            std::cout << "Launcher: system addons found; MonakIME loaded and listed\n";
        }
        dispatcher.detach();
        instance.exit();
    });
    const int eventResult = instance.exec();
    return result ? result : eventResult;
}
