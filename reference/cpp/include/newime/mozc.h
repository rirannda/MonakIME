// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "newime/converter.h"
#include <chrono>
#include <functional>
#include <optional>

namespace newime {
// Single-owner, persistent native Mozc subprocess. Never called on the UI thread.
class MozcEngine {
  public:
    MozcEngine(std::string helper, std::string profile);
    ~MozcEngine();
    MozcEngine(const MozcEngine &) = delete;
    MozcEngine &operator=(const MozcEngine &) = delete;
    std::optional<Conversion> convert(std::string_view reading, std::string_view context = {},
                                      const std::function<bool()> &cancelled = {});
    const std::string &lastError() const {
        return error_;
    }
    static std::optional<Conversion> parse(std::string_view reading, std::string_view reply);

  private:
    bool start();
    void stop();
    bool send(std::string_view request, std::chrono::steady_clock::time_point deadline);
    std::optional<std::string> receive(std::chrono::steady_clock::time_point deadline);
    std::string helper_, profile_, error_, buffer_;
    int input_ = -1, output_ = -1, pid_ = -1;
    bool failed_ = false;
};
} // namespace newime
