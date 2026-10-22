// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime/config.h"
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>

#ifndef NEWIME_DATA_DIR
#define NEWIME_DATA_DIR "/usr/local/share/newime"
#endif
#ifndef NEWIME_LIBEXEC_DIR
#define NEWIME_LIBEXEC_DIR "/usr/libexec/newime"
#endif
#ifndef NEWIME_SOURCE_DATA_DIR
#define NEWIME_SOURCE_DATA_DIR "data"
#endif
#ifndef NEWIME_DEFAULT_MODEL_PATH
#define NEWIME_DEFAULT_MODEL_PATH "models/zenz-v3.2-xsmall-Q5_K_M.gguf"
#endif

namespace newime {
namespace {
std::string env(const char *key, std::string fallback = {}) {
    const char *value = std::getenv(key);
    return value && *value ? std::string(value) : std::move(fallback);
}
std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
int integer(const std::string &s, int fallback, int minimum, int maximum) {
    int number = fallback;
    auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), number);
    return error == std::errc() && end == s.data() + s.size() ? std::clamp(number, minimum, maximum)
                                                              : fallback;
}
} // namespace
RuntimeConfig RuntimeConfig::fromEnvironment() {
    namespace fs = std::filesystem;
    const auto home = env("HOME", "/tmp");
    const auto configHome = env("XDG_CONFIG_HOME", home + "/.config") + "/newime";
    const auto dataHome = env("XDG_DATA_HOME", home + "/.local/share") + "/newime";
    std::map<std::string, std::string> values;
    std::ifstream file(configHome + "/config");
    std::string line;
    while (std::getline(file, line)) {
        if (line.size() > 4096)
            continue;
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        auto pos = line.find('=');
        if (pos != std::string::npos)
            values[trim(line.substr(0, pos))] = trim(line.substr(pos + 1));
        if (values.size() > 32)
            break;
    }
    auto value = [&](const char *key, const char *variable, std::string fallback) {
        auto it = values.find(key);
        return env(variable, it == values.end() ? std::move(fallback) : it->second);
    };
    auto dataDir = env("NEWIME_DATA_DIR", NEWIME_DATA_DIR);
    if (!fs::exists(dataDir + "/base.tsv"))
        dataDir = NEWIME_SOURCE_DATA_DIR;
    RuntimeConfig config;
    config.dictionary_path = value("dictionary", "NEWIME_DICTIONARY", dataDir + "/base.tsv");
    config.system_dictionary_path =
        value("system_dictionary", "NEWIME_SYSTEM_DICTIONARY", dataDir + "/system.nimedic");
    config.english_dictionary_path =
        value("english_dictionary", "NEWIME_ENGLISH_DICTIONARY", dataDir + "/english.txt");
    config.user_dictionary_path =
        value("user_dictionary", "NEWIME_USER_DICTIONARY", configHome + "/user.tsv");
    config.learning_path =
        value("learning_path", "NEWIME_LEARNING_PATH", dataHome + "/learning.tsv");
    std::string model = dataHome + "/models/zenz-v3.2-xsmall-Q5_K_M.gguf";
    if (!fs::exists(model))
        model = dataDir + "/models/zenz-v3.2-xsmall-Q5_K_M.gguf";
    if (!fs::exists(model))
        model = NEWIME_DEFAULT_MODEL_PATH;
    config.model_path = value("model", "NEWIME_MODEL", model);
    std::string helper = NEWIME_LIBEXEC_DIR "/meltype_mozc_helper";
    if (!fs::exists(helper))
        helper = std::string(NEWIME_SOURCE_DATA_DIR) + "/../third_party/mozc/meltype_mozc_helper";
    if (!fs::exists(helper))
        helper = "/usr/lib/meltype/mozc/meltype_mozc_helper";
    config.mozc_helper_path = value("mozc_helper", "NEWIME_MOZC_HELPER", helper);
    config.mozc_profile_path = value("mozc_profile", "NEWIME_MOZC_PROFILE", dataHome + "/mozc");
    config.live_delay_ms =
        integer(value("live_delay_ms", "NEWIME_LIVE_DELAY_MS", "25"), 25, 0, 200);
    auto live = value("live_conversion", "NEWIME_LIVE_CONVERSION", "true");
    if (live == "false" || live == "0")
        config.mozc_helper_path.clear();
    config.threads = integer(value("threads", "NEWIME_THREADS", "2"), 2, 1, 8);
    config.inference_budget_ms =
        integer(value("inference_budget_ms", "NEWIME_BUDGET_MS", "500"), 500, 10, 5000);
    config.max_input_chars = static_cast<size_t>(
        integer(value("max_input_chars", "NEWIME_MAX_INPUT_CHARS", "128"), 128, 8, 256));
    config.enable_learning = value("learning", "NEWIME_LEARNING", "true") != "false" &&
                             value("learning", "NEWIME_LEARNING", "true") != "0";
    auto enabled = value("zenz", "NEWIME_ZENZ", "true");
    if (enabled == "false" || enabled == "0")
        config.model_path.clear();
    return config;
}
} // namespace newime
