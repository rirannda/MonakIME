// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "monakime_ffi.h"
#include <fcitx-utils/event.h>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <memory>
#include <unordered_map>

namespace fcitx {
class Instance;
}
namespace monakime::fcitxaddon {
struct Registry {
    MonakimeCore *core = nullptr;
    uint64_t next = 1;
    std::unordered_map<uint64_t, fcitx::ICUUID> contexts;
};
struct State : fcitx::InputContextProperty {
    uint64_t id = 0;
    std::weak_ptr<Registry> registry;
    ~State() override;
};
class Engine final : public fcitx::InputMethodEngineV2 {
  public:
    explicit Engine(fcitx::Instance *);
    ~Engine() override;
    void keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &) override;
    void reset(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &) override;
    void activate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &) override;
    std::string subModeLabelImpl(const fcitx::InputMethodEntry &, fcitx::InputContext &) override;
    void choose(fcitx::InputContext *, size_t index);

  private:
    static void wake(void *);
    void drain();
    State *state(fcitx::InputContext *);
    bool sensitive(fcitx::InputContext *) const;
    void render(fcitx::InputContext *, MonakimeView *);
    fcitx::Instance *instance_;
    std::shared_ptr<Registry> registry_ = std::make_shared<Registry>();
    fcitx::SimpleInputContextPropertyFactory<State> stateFactory_;
    fcitx::EventDispatcher dispatcher_;
    std::unique_ptr<fcitx::EventSourceTime> saveTimer_;
};
} // namespace monakime::fcitxaddon
