// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "newime/mozc.h"
#include "newime/text.h"
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
namespace newime {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t maxReply = 1024 * 1024;
bool safe(std::string_view value) {
    return std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 0x20; });
}
std::string ascii(std::string_view value) {
    std::string out;
    for (unsigned char c : value)
        if (c < 128)
            out += static_cast<char>(c);
    return out;
}
bool ready(int fd, short events, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        pollfd p{fd, events, 0};
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        const auto result = poll(&p, 1, static_cast<int>(std::max<int64_t>(1, ms)));
        if (result > 0)
            return (p.revents & events) != 0;
        if (result == 0 || errno != EINTR)
            return false;
    }
    return false;
}
ssize_t writePipe(int fd, const char *data, size_t size) {
    // A crashed subprocess must not deliver SIGPIPE to the hosting fcitx5 process.
    sigset_t blocked, old, pending;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &old);
    sigpending(&pending);
    const auto count = write(fd, data, size);
    const auto error = errno;
    if (count < 0 && error == EPIPE && !sigismember(&pending, SIGPIPE) &&
        !sigismember(&old, SIGPIPE)) {
        const timespec zero{};
        sigtimedwait(&blocked, nullptr, &zero);
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    errno = error;
    return count;
}
} // namespace
MozcEngine::MozcEngine(std::string helper, std::string profile)
    : helper_(std::move(helper)), profile_(std::move(profile)) {}
MozcEngine::~MozcEngine() {
    stop();
}
void MozcEngine::stop() {
    if (input_ >= 0) {
        close(input_);
        input_ = -1;
    }
    if (output_ >= 0) {
        close(output_);
        output_ = -1;
    }
    if (pid_ > 0) {
        // The protocol owns no learning writes. Bound shutdown even if a helper hangs.
        if (waitpid(pid_, nullptr, WNOHANG) == 0) {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
        pid_ = -1;
    }
    buffer_.clear();
}
bool MozcEngine::send(std::string_view request, Clock::time_point deadline) {
    while (!request.empty()) {
        if (!ready(input_, POLLOUT, deadline))
            return false;
        const auto n = writePipe(input_, request.data(), request.size());
        if (n > 0)
            request.remove_prefix(static_cast<size_t>(n));
        else if (n == 0 || (errno != EAGAIN && errno != EINTR))
            return false;
    }
    return true;
}
std::optional<std::string> MozcEngine::receive(Clock::time_point deadline) {
    for (;;) {
        const auto newline = buffer_.find('\n');
        if (newline != buffer_.npos) {
            auto line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            return line;
        }
        if (buffer_.size() > maxReply || !ready(output_, POLLIN, deadline))
            return {};
        char bytes[8192];
        const auto n = read(output_, bytes, sizeof(bytes));
        if (n > 0)
            buffer_.append(bytes, static_cast<size_t>(n));
        else if (n == 0 || (errno != EAGAIN && errno != EINTR))
            return {};
    }
}
bool MozcEngine::start() {
    if (input_ >= 0)
        return true;
    if (failed_ || helper_.empty())
        return false;
    failed_ = true;
    std::error_code ec;
    std::filesystem::create_directories(profile_, ec);
    if (ec || chmod(profile_.c_str(), 0700) != 0) {
        error_ = "could not create private Mozc profile";
        return false;
    }
    int toChild[2], fromChild[2];
    if (pipe2(toChild, O_CLOEXEC | O_NONBLOCK) != 0) {
        error_ = "could not create Mozc input pipe";
        return false;
    }
    if (pipe2(fromChild, O_CLOEXEC | O_NONBLOCK) != 0) {
        close(toChild[0]);
        close(toChild[1]);
        error_ = "could not create Mozc output pipe";
        return false;
    }
    // Child uses blocking stdio; the parent uses bounded, nonblocking pipes.
    fcntl(toChild[0], F_SETFL, fcntl(toChild[0], F_GETFL) & ~O_NONBLOCK);
    fcntl(fromChild[1], F_SETFL, fcntl(fromChild[1], F_GETFL) & ~O_NONBLOCK);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, toChild[1]);
    posix_spawn_file_actions_addclose(&actions, fromChild[0]);
    posix_spawn_file_actions_adddup2(&actions, toChild[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fromChild[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, toChild[0]);
    posix_spawn_file_actions_addclose(&actions, fromChild[1]);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    char *argv[] = {helper_.data(), profile_.data(), nullptr};
    pid_t child = -1;
    const auto status = posix_spawn(&child, helper_.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(toChild[0]);
    close(fromChild[1]);
    if (status != 0) {
        close(toChild[1]);
        close(fromChild[0]);
        error_ = "could not launch Mozc helper";
        return false;
    }
    pid_ = child;
    input_ = toChild[1];
    output_ = fromChild[0];
    const auto greeting = receive(Clock::now() + std::chrono::milliseconds(1500));
    if (!greeting || *greeting != "READY") {
        error_ = "Mozc startup failed or timed out";
        stop();
        return false;
    }
    failed_ = false;
    return true;
}
std::optional<Conversion> MozcEngine::parse(std::string_view reading, std::string_view reply) {
    if (reply.empty() || reply.size() > maxReply || reading.size() > 1024 || !safe(reading))
        return {};
    Conversion out;
    out.reading = reading;
    std::string reconstructed;
    size_t candidateBytes = 0;
    while (!reply.empty()) {
        auto end = reply.find('\x1e');
        auto record = reply.substr(0, end);
        auto split = record.find('\x1f');
        if (split == record.npos || split == 0)
            return {};
        Segment segment;
        segment.reading = record.substr(0, split);
        if (!safe(segment.reading))
            return {};
        reconstructed += segment.reading;
        record.remove_prefix(split + 1);
        const auto signature = ascii(segment.reading);
        for (size_t count = 0; !record.empty() && count < 256; ++count) {
            split = record.find('\x1f');
            const auto value = record.substr(0, split);
            if (!safe(value))
                return {};
            if (!value.empty() && value.size() <= 1024 && candidateBytes + value.size() <= 65536 &&
                ascii(value) == signature && segment.candidates.size() < 9 &&
                std::none_of(segment.candidates.begin(), segment.candidates.end(),
                             [&](const auto &c) { return c.text == value; })) {
                candidateBytes += value.size();
                segment.candidates.push_back(
                    {std::string(value), -static_cast<double>(segment.candidates.size())});
            }
            if (split == record.npos)
                break;
            record.remove_prefix(split + 1);
        }
        if (segment.candidates.empty())
            segment.candidates.push_back({segment.reading, 0});
        const auto chars = text::utf8ToU32(segment.reading);
        segment.literal = std::none_of(chars.begin(), chars.end(), text::isKana);
        out.segments.push_back(std::move(segment));
        if (out.segments.size() > 256)
            return {};
        if (end == reply.npos)
            break;
        reply.remove_prefix(end + 1);
    }
    if (reconstructed != reading)
        return {};
    out.rebuildCandidates();
    return out;
}
std::optional<Conversion> MozcEngine::convert(std::string_view reading, std::string_view context,
                                              const std::function<bool()> &cancelled) {
    if ((cancelled && cancelled()) || reading.empty() || reading.size() > 1024 ||
        text::codepoints(reading) > 256 || !safe(reading) || !safe(context))
        return {};
    const auto chars = text::utf8ToU32(reading);
    if (std::none_of(chars.begin(), chars.end(), text::isKana)) {
        Conversion literal{std::string(reading),
                           {{std::string(reading), {{std::string(reading), 0}}, 0, true}},
                           {{std::string(reading), 0}}};
        return literal;
    }
    if (!start())
        return {};
    const auto deadline = Clock::now() + std::chrono::milliseconds(300);
    if (!send("C\t" + text::suffix(context, 64) + "\t" + std::string(reading) + "\n", deadline)) {
        error_ = "Mozc request failed or timed out: " + std::to_string(errno);
        failed_ = true;
        stop();
        return {};
    }
    // Always drain a cancelled reply to keep the persistent stream synchronized.
    auto reply = receive(deadline);
    if (!reply) {
        error_ = "Mozc response failed or timed out";
        failed_ = true;
        stop();
        return {};
    }
    if (cancelled && cancelled())
        return {};
    auto result = parse(reading, *reply);
    error_ = result ? "" : "invalid Mozc response";
    return result;
}
} // namespace newime
