// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime/dictionary.h"
#include "newime/text.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace newime {
namespace {
uint32_t u32(const unsigned char *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
bool validField(std::string_view s) {
    return !s.empty() && s.size() <= 1024 &&
           s.find_first_of("\t\r\n\0", 0, 4) == std::string_view::npos;
}
void insertCandidate(std::vector<Candidate> &out, std::string_view surface, double score) {
    auto it =
        std::find_if(out.begin(), out.end(), [&](const auto &v) { return v.text == surface; });
    if (it == out.end())
        out.push_back({std::string(surface), score});
    else
        it->score = std::max(it->score, score);
}
} // namespace
struct Dictionary::Mapped {
    void *memory = MAP_FAILED;
    size_t length = 0;
    uint32_t count = 0;
    uint32_t blobSize = 0;
    const unsigned char *records = nullptr;
    const char *blob = nullptr;
    ~Mapped() {
        if (memory != MAP_FAILED)
            munmap(memory, length);
    }
    std::string_view field(uint32_t offset) const {
        return {blob + offset};
    }
    std::string_view reading(size_t index) const {
        return field(u32(records + index * 12));
    }
};
size_t Dictionary::size() const {
    return entries_.size() + (mapped_ ? mapped_->count : 0);
}
bool Dictionary::load(const std::string &path, bool preferred) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < 0) {
        close(fd);
        return false;
    }
    unsigned char header[16]{};
    ssize_t n = pread(fd, header, sizeof header, 0);
    if (n >= 8 && std::memcmp(header, "NIMEDIC1", 8) == 0) {
        if (n != 16) {
            close(fd);
            return false;
        }
        const uint32_t count = u32(header + 8), blobSize = u32(header + 12);
        const uint64_t expected = 16ull + 12ull * count + blobSize;
        if (count > 2000000 || expected != static_cast<uint64_t>(st.st_size)) {
            close(fd);
            return false;
        }
        auto mapped = std::make_shared<Mapped>();
        mapped->length = static_cast<size_t>(st.st_size);
        mapped->memory = mmap(nullptr, mapped->length, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (mapped->memory == MAP_FAILED)
            return false;
        mapped->count = count;
        mapped->blobSize = blobSize;
        mapped->records = static_cast<const unsigned char *>(mapped->memory) + 16;
        mapped->blob = reinterpret_cast<const char *>(mapped->records + 12ull * count);
        std::string_view previous;
        for (size_t i = 0; i < count; ++i) {
            const auto *record = mapped->records + i * 12;
            auto ro = u32(record), so = u32(record + 4), bits = u32(record + 8);
            float cost;
            std::memcpy(&cost, &bits, sizeof cost);
            if (ro >= blobSize || so >= blobSize || !std::isfinite(cost))
                return false;
            auto *re = static_cast<const char *>(
                std::memchr(mapped->blob + ro, 0, std::min<uint32_t>(blobSize - ro, 1025)));
            auto *se = static_cast<const char *>(
                std::memchr(mapped->blob + so, 0, std::min<uint32_t>(blobSize - so, 1025)));
            if (!re || !se || re == mapped->blob + ro || se == mapped->blob + so)
                return false;
            auto reading = mapped->reading(i);
            if (i && reading < previous)
                return false;
            previous = reading;
        }
        // Validation touches every record once. Release those pages afterward;
        // later binary searches fault in only the pages actually used.
        madvise(mapped->memory, mapped->length, MADV_DONTNEED);
        madvise(mapped->memory, mapped->length, MADV_RANDOM);
        mapped_ = std::move(mapped);
        return true;
    }
    close(fd);
    std::ifstream stream(path);
    if (!stream)
        return false;
    std::string line;
    std::vector<Entry> added;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#' || line.size() > 4096)
            continue;
        auto first = line.find('\t');
        if (first == line.npos)
            continue;
        auto second = line.find('\t', first + 1);
        auto reading = text::hiragana(line.substr(0, first));
        auto surface = line.substr(first + 1, second == line.npos ? second : second - first - 1);
        if (!surface.empty() && surface.back() == '\r')
            surface.pop_back();
        if (!validField(reading) || !validField(surface))
            continue;
        double cost = 1;
        if (second != line.npos)
            try {
                cost = std::stod(line.substr(second + 1));
            } catch (...) {
                continue;
            }
        if (!std::isfinite(cost))
            continue;
        added.push_back(
            {std::move(reading), std::move(surface), std::clamp(cost, 0.0, 100.0), preferred});
        if (added.size() >= 2000000)
            break;
    }
    entries_.insert(entries_.end(), std::make_move_iterator(added.begin()),
                    std::make_move_iterator(added.end()));
    std::sort(entries_.begin(), entries_.end(), [](const auto &a, const auto &b) {
        return a.reading < b.reading ||
               (a.reading == b.reading &&
                (a.cost < b.cost || (a.cost == b.cost && a.surface < b.surface)));
    });
    return true;
}
void Dictionary::add(std::string reading, std::string surface, double cost) {
    reading = text::hiragana(reading);
    if (!validField(reading) || !validField(surface) || !std::isfinite(cost))
        return;
    auto it = std::lower_bound(entries_.begin(), entries_.end(), reading,
                               [](const auto &a, std::string_view b) { return a.reading < b; });
    entries_.insert(it, {std::move(reading), std::move(surface), std::clamp(cost, 0.0, 100.0)});
}
std::vector<Candidate> Dictionary::lookup(std::string_view reading, size_t limit) const {
    std::vector<Candidate> out;
    limit = std::min<size_t>(limit, 32);
    if (!limit)
        return out;
    auto it = std::lower_bound(entries_.begin(), entries_.end(), reading,
                               [](const auto &a, std::string_view b) { return a.reading < b; });
    for (size_t count = 0; it != entries_.end() && it->reading == reading && count < 64;
         ++it, ++count)
        insertCandidate(out, it->surface, it->preferred ? 4.0 + 1.0 / (1.0 + it->cost) : -it->cost);
    if (mapped_) {
        size_t lo = 0, hi = mapped_->count;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (mapped_->reading(mid) < reading)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (size_t count = 0; lo < mapped_->count && mapped_->reading(lo) == reading && count < 32;
             ++lo, ++count) {
            const auto *rec = mapped_->records + lo * 12;
            auto bits = u32(rec + 8);
            float cost;
            std::memcpy(&cost, &bits, sizeof cost);
            insertCandidate(out, mapped_->field(u32(rec + 4)), -cost);
        }
    }
    auto learned =
        std::lower_bound(learned_.begin(), learned_.end(), reading,
                         [](const auto &a, std::string_view b) { return a.reading < b; });
    for (; learned != learned_.end() && learned->reading == reading; ++learned)
        insertCandidate(out, learned->surface, 4.0 + std::log1p(learned->count) * 2.0);
    std::stable_sort(out.begin(), out.end(),
                     [](const auto &a, const auto &b) { return a.score > b.score; });
    if (out.size() > limit)
        out.resize(limit);
    return out;
}
bool Dictionary::contains(std::string_view reading) const {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), reading,
                               [](const auto &a, std::string_view b) { return a.reading < b; });
    if (it != entries_.end() && it->reading == reading)
        return true;
    if (mapped_) {
        size_t lo = 0, hi = mapped_->count;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (mapped_->reading(mid) < reading)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < mapped_->count && mapped_->reading(lo) == reading)
            return true;
    }
    auto learned =
        std::lower_bound(learned_.begin(), learned_.end(), reading,
                         [](const auto &a, std::string_view b) { return a.reading < b; });
    return learned != learned_.end() && learned->reading == reading;
}
bool Dictionary::loadEnglish(const std::string &path) {
    std::ifstream file(path);
    if (!file)
        return false;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line.front() == '#' || line.size() > 64)
            continue;
        if (line.back() == '\r')
            line.pop_back();
        if (!std::all_of(line.begin(), line.end(),
                         [](char c) { return (c >= 'a' && c <= 'z') || c == '\'' || c == '-'; }))
            continue;
        english_.push_back(std::move(line));
        if (english_.size() >= 100000)
            break;
    }
    std::sort(english_.begin(), english_.end());
    english_.erase(std::unique(english_.begin(), english_.end()), english_.end());
    return true;
}
bool Dictionary::english(std::string_view word) const {
    std::string lower(word);
    for (auto &c : lower)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return std::binary_search(english_.begin(), english_.end(), lower);
}
void Dictionary::learn(std::string_view reading, std::string_view surface) {
    if (!validField(reading) || !validField(surface) || text::codepoints(reading) > 256)
        return;
    auto start = std::lower_bound(learned_.begin(), learned_.end(), reading,
                                  [](const auto &a, std::string_view b) { return a.reading < b; });
    auto found = start;
    while (found != learned_.end() && found->reading == reading && found->surface != surface)
        ++found;
    if (found != learned_.end() && found->reading != reading)
        found = learned_.end();
    if (found != learned_.end()) {
        found->count = std::min<uint32_t>(1000000, found->count + 1);
        found->age = ++clock_;
        return;
    }
    if (learned_.size() >= 4096)
        learned_.erase(
            std::min_element(learned_.begin(), learned_.end(),
                             [](const auto &a, const auto &b) { return a.age < b.age; }));
    auto position =
        std::lower_bound(learned_.begin(), learned_.end(), reading,
                         [](const auto &a, std::string_view b) { return a.reading < b; });
    learned_.insert(position, {std::string(reading), std::string(surface), 1, ++clock_});
}
bool Dictionary::loadLearning(const std::string &path) {
    std::ifstream stream(path);
    if (!stream)
        return false;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.size() > 4096)
            continue;
        auto a = line.find('\t'), b = line.find('\t', a == line.npos ? line.size() : a + 1);
        if (a == line.npos)
            continue;
        auto reading = line.substr(0, a),
             surface = line.substr(a + 1, b == line.npos ? b : b - a - 1);
        learn(reading, surface);
        if (b != line.npos)
            try {
                auto item = std::lower_bound(
                    learned_.begin(), learned_.end(), reading,
                    [](const auto &a, std::string_view v) { return a.reading < v; });
                for (; item != learned_.end() && item->reading == reading; ++item)
                    if (item->surface == surface) {
                        item->count = std::clamp<uint32_t>(
                            static_cast<uint32_t>(std::stoul(line.substr(b + 1))), 1, 1000000);
                        break;
                    }
            } catch (...) {
            }
    }
    return true;
}
bool Dictionary::saveLearning(const std::string &path) const {
    std::error_code error;
    auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, error);
    if (error)
        return false;
    std::string temp = path + ".tmp.XXXXXX";
    int fd = mkstemp(temp.data());
    if (fd < 0)
        return false;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    std::string data;
    for (const auto &item : learned_)
        data += item.reading + '\t' + item.surface + '\t' + std::to_string(item.count) + '\n';
    size_t offset = 0;
    while (offset < data.size()) {
        ssize_t n = write(fd, data.data() + offset, data.size() - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            unlink(temp.c_str());
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    bool ok = fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    if (ok && rename(temp.c_str(), path.c_str()) == 0)
        return true;
    unlink(temp.c_str());
    return false;
}
} // namespace newime
