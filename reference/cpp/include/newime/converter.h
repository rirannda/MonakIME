// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "newime/dictionary.h"
#include <string>
#include <string_view>
#include <vector>
namespace newime {
enum class ReadingCheck { Verified, Unverified, Mismatch };
struct Segment {
    std::string reading;
    std::vector<Candidate> candidates;
    size_t selected = 0;
    bool literal = false;
    std::string text() const;
};
struct Conversion {
    std::string reading;
    std::vector<Segment> segments;
    std::vector<Candidate> candidates;
    std::string text() const;
    void rebuildCandidates(size_t limit = 9);
};
class Converter {
  public:
    explicit Converter(Dictionary &dictionary) : dictionary_(dictionary) {}
    Conversion convert(std::string_view reading, size_t limit = 9) const;
    std::string compose(std::string_view raw, bool final = false) const;
    std::vector<Candidate> candidates(std::string_view reading, size_t limit = 9) const;
    bool resize(Conversion &conversion, size_t segment, int delta) const;
    ReadingCheck checkReading(std::string_view reading, std::string_view surface) const;
    void rank(Conversion &conversion, size_t limit = 9) const;

  private:
    Dictionary &dictionary_;
};
} // namespace newime
