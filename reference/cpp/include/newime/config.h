// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <string>

namespace newime {
struct RuntimeConfig {
    std::string dictionary_path;
    std::string system_dictionary_path;
    std::string english_dictionary_path;
    std::string user_dictionary_path;
    std::string learning_path;
    std::string model_path;
    std::string mozc_helper_path;
    std::string mozc_profile_path;
    int live_delay_ms = 25;
    int threads = 2;
    int inference_budget_ms = 500;
    size_t max_input_chars = 128;
    bool enable_learning = true;
    static RuntimeConfig fromEnvironment();
};
} // namespace newime
