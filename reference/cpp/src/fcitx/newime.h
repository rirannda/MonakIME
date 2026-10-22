// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "newime/config.h"
#include "newime/converter.h"
#include "newime/mozc.h"
#include "newime/zenz.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace fcitx {
class Instance;
}

namespace newime::fcitxaddon {

struct State : fcitx::InputContextProperty {
    std::string raw;
    std::string forced;
    std::string leftContext;
    Conversion conversion;
    Conversion live;
    std::vector<Candidate> sentences;
    size_t sentence = 0;
    size_t segment = 0;
    uint64_t generation = 0;
    bool converting = false;
    bool sentenceMode = true;
    bool pending = false;
    bool latin = false;
    bool neuralApplied = false;
};

class Engine final : public fcitx::InputMethodEngineV2 {
  public:
    explicit Engine(fcitx::Instance *instance);
    ~Engine() override;
    void keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &) override;
    void reset(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &) override;
    void activate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &) override;
    std::string subModeLabelImpl(const fcitx::InputMethodEntry &, fcitx::InputContext &) override;
    void choose(fcitx::InputContext *ic, size_t index);

  private:
    struct Job {
        fcitx::ICUUID context;
        uint64_t generation;
        std::string reading;
        std::string leftContext;
        bool explicitConversion = false;
        std::chrono::steady_clock::time_point ready{};
    };
    bool sensitive(fcitx::InputContext *ic) const;
    void invalidate(State &state);
    void clear(fcitx::InputContext *ic, bool context = false);
    void update(fcitx::InputContext *ic);
    void begin(fcitx::InputContext *ic);
    void cycle(fcitx::InputContext *ic, int direction);
    void commit(fcitx::InputContext *ic);
    std::string current(const State &state) const;
    void request(fcitx::InputContext *ic);
    void requestMozc(fcitx::InputContext *ic, bool explicitConversion = false);
    void scheduleLearningSave();
    void saveLearning();
    void run();
    void runMozc();

    fcitx::Instance *instance_;
    RuntimeConfig config_;
    Dictionary dictionary_;
    Converter converter_;
    fcitx::SimpleInputContextPropertyFactory<State> stateFactory_;
    fcitx::EventDispatcher dispatcher_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<Job> nextJob_;
    std::condition_variable mozcCondition_;
    std::optional<Job> nextMozcJob_;
    std::atomic<uint64_t> latest_{0};
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::thread mozcWorker_;
    std::unique_ptr<fcitx::EventSourceTime> saveTimer_;
    bool learningDirty_ = false;
    bool modelAvailable_ = false;
    bool mozcAvailable_ = false;
};

} // namespace newime::fcitxaddon
