// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <string_view>
namespace newime {
class Romaji {
  public:
    static std::string convert(std::string_view raw, bool final = false);
};
} // namespace newime
