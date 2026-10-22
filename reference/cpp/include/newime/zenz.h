// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace newime {

struct ZenzConfig {
    std::string model_path;
    int threads = 2;
    int context_size = 512;
    int max_tokens = 128;
    int budget_ms = 500;
};

// A single-worker synchronous engine. Construction does not load the model.
// All methods (including unload/destruction) must run on the owning worker.
class ZenzEngine {
  public:
    explicit ZenzEngine(ZenzConfig config);
    ~ZenzEngine();
    ZenzEngine(const ZenzEngine &) = delete;
    ZenzEngine &operator=(const ZenzEngine &) = delete;

    std::optional<std::string> convert(std::string_view reading, std::string_view left_context = {},
                                       const std::function<bool()> &cancelled = {});
    bool loaded() const noexcept;
    const std::string &last_error() const noexcept;
    static bool compiled_with_llama() noexcept;
    void unload();

    // Public to allow checking the model protocol without downloading weights.
    static std::string make_prompt(std::string_view reading, std::string_view left_context = {});
    static bool valid_output(std::string_view reading, std::string_view output);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace newime
