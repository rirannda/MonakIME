// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace newime {
struct Candidate {
    std::string text;
    double score = 0;
};
class Dictionary {
  public:
    bool load(const std::string &path, bool preferred = false);
    bool loadEnglish(const std::string &path);
    void add(std::string reading, std::string surface, double cost = 1);
    std::vector<Candidate> lookup(std::string_view reading, size_t limit = 16) const;
    bool contains(std::string_view reading) const;
    bool english(std::string_view word) const;
    void learn(std::string_view reading, std::string_view surface);
    bool loadLearning(const std::string &path);
    bool saveLearning(const std::string &path) const;
    size_t size() const;
    size_t learningSize() const {
        return learned_.size();
    }

  private:
    struct Entry {
        std::string reading;
        std::string surface;
        double cost;
        bool preferred = false;
    };
    struct Learned {
        std::string reading;
        std::string surface;
        uint32_t count;
        uint64_t age;
    };
    struct Mapped;
    std::shared_ptr<Mapped> mapped_;
    std::vector<Entry> entries_;
    std::vector<std::string> english_;
    std::vector<Learned> learned_;
    uint64_t clock_ = 0;
};
} // namespace newime
