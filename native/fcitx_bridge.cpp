// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
// fcitx API translation only. State transitions and workers are implemented in Rust.
#include "fcitx_bridge.h"
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/textformatflags.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

namespace monakime::fcitxaddon {
namespace {
using OwnedView = std::unique_ptr<MonakimeView, decltype(&monakime_view_free)>;
class Word final : public fcitx::CandidateWord {
  public:
    Word(Engine *engine, size_t index, std::string text)
        : CandidateWord(fcitx::Text(std::move(text))), engine_(engine), index_(index) {}
    void select(fcitx::InputContext *ic) const override {
        engine_->choose(ic, index_);
    }

  private:
    Engine *engine_;
    size_t index_;
};
} // namespace
State::~State() {
    if (auto owner = registry.lock()) {
        if (owner->core)
            monakime_remove(owner->core, id);
        owner->contexts.erase(id);
    }
}
Engine::Engine(fcitx::Instance *instance) : instance_(instance) {
    instance_->inputContextManager().registerProperty("monakime-state", &stateFactory_);
    dispatcher_.attach(&instance_->eventLoop());
    registry_->core = monakime_core_new(wake, this);
    if (!registry_->core)
        FCITX_ERROR() << "MonakIME: Rust core initialization failed";
}
Engine::~Engine() {
    saveTimer_.reset();
    monakime_core_free(registry_->core); // Joins workers before the callback/user expires.
    registry_->core = nullptr;
    dispatcher_.detach();
    registry_.reset();
}
State *Engine::state(fcitx::InputContext *ic) {
    auto *value = ic->propertyFor(&stateFactory_);
    if (!value->id) {
        value->id = registry_->next++;
        value->registry = registry_;
        registry_->contexts.emplace(value->id, ic->uuid());
    }
    return value;
}
bool Engine::sensitive(fcitx::InputContext *ic) const {
    return ic->capabilityFlags().testAny(fcitx::CapabilityFlag::PasswordOrSensitive) ||
           ic->capabilityFlags().test(fcitx::CapabilityFlag::Disable);
}
void Engine::wake(void *user) {
    auto *engine = static_cast<Engine *>(user);
    engine->dispatcher_.schedule([engine] { engine->drain(); });
}
void Engine::drain() {
    uint64_t ids[4];
    const auto count = monakime_poll(registry_->core, ids, 4);
    for (size_t i = 0; i < count; ++i) {
        const auto entry = registry_->contexts.find(ids[i]);
        if (entry == registry_->contexts.end())
            continue;
        auto *ic = instance_->inputContextManager().findByUUID(entry->second);
        if (!ic || !ic->hasFocus() || sensitive(ic)) {
            monakime_remove(registry_->core, ids[i]);
            continue;
        }
        render(ic, monakime_view(registry_->core, ids[i]));
    }
}
void Engine::render(fcitx::InputContext *ic, MonakimeView *raw) {
    OwnedView view(raw, monakime_view_free);
    if (!view)
        return;
    if (*view->commit)
        ic->commitString(view->commit);
    auto &panel = ic->inputPanel();
    panel.reset();
    const std::string value(view->preedit);
    if (!value.empty()) {
        fcitx::Text preedit;
        if (view->highlight_end > view->highlight_start && view->highlight_end <= value.size()) {
            preedit.append(value.substr(0, view->highlight_start),
                           fcitx::TextFormatFlag::Underline);
            preedit.append(
                value.substr(view->highlight_start, view->highlight_end - view->highlight_start),
                fcitx::TextFormatFlags(fcitx::TextFormatFlag::Underline) |
                    fcitx::TextFormatFlag::HighLight);
            preedit.append(value.substr(view->highlight_end), fcitx::TextFormatFlag::Underline);
        } else
            preedit.append(value, fcitx::TextFormatFlag::Underline);
        preedit.setCursor(static_cast<int>(view->cursor));
        if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit))
            panel.setClientPreedit(preedit);
        else
            panel.setPreedit(preedit);
    }
    if (view->count) {
        auto list = std::make_unique<fcitx::CommonCandidateList>();
        list->setPageSize(9);
        list->setLayoutHint(fcitx::CandidateLayoutHint::Vertical);
        list->setSelectionKey({fcitx::Key("1"), fcitx::Key("2"), fcitx::Key("3"), fcitx::Key("4"),
                               fcitx::Key("5"), fcitx::Key("6"), fcitx::Key("7"), fcitx::Key("8"),
                               fcitx::Key("9")});
        for (size_t i = 0; i < view->count; ++i)
            list->append<Word>(this, i, view->candidates[i]);
        list->setGlobalCursorIndex(static_cast<int>(view->selection));
        panel.setCandidateList(std::move(list));
        panel.setAuxDown(fcitx::Text(view->auxiliary));
    }
    if (view->flags & 4) {
        const auto deadline = fcitx::now(CLOCK_MONOTONIC) + 2000000;
        if (!saveTimer_) {
            saveTimer_ = instance_->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, deadline, 100000, [this](fcitx::EventSourceTime *, uint64_t) {
                    if (!monakime_save(registry_->core))
                        FCITX_WARN() << "MonakIME: could not save learning";
                    return true;
                });
        } else
            saveTimer_->setTime(deadline);
        saveTimer_->setOneShot();
    }
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}
void Engine::reset(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    render(ic, monakime_reset(registry_->core, state(ic)->id));
}
void Engine::activate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) {
    auto *ic = event.inputContext();
    const auto id = state(ic)->id;
    render(ic,
           sensitive(ic) ? monakime_reset(registry_->core, id) : monakime_view(registry_->core, id));
}
std::string Engine::subModeLabelImpl(const fcitx::InputMethodEntry &, fcitx::InputContext &ic) {
    OwnedView view(monakime_view(registry_->core, state(&ic)->id), monakime_view_free);
    return view && (view->flags & 2) ? "A" : "あ";
}
void Engine::choose(fcitx::InputContext *ic, size_t index) {
    if (ic)
        render(ic, monakime_key(registry_->core, state(ic)->id, 16, "", false, sensitive(ic), index));
}
void Engine::keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &event) {
    if (event.isRelease())
        return;
    auto *ic = event.inputContext();
    const auto key = event.key();
    const auto sym = key.sym();
    const auto modifiers = fcitx::KeyStates(fcitx::KeyState::Ctrl) | fcitx::KeyState::Alt |
                           fcitx::KeyState::Super | fcitx::KeyState::Super2 |
                           fcitx::KeyState::Hyper | fcitx::KeyState::Meta;
    uint32_t action = 17;
    std::string text;
    if (key.states().testAny(modifiers))
        action = key.isModifier() ? 17 : 14;
    else if (sym == FcitxKey_space)
        action = 1;
    else if (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter)
        action = 2;
    else if (sym == FcitxKey_Escape)
        action = 3;
    else if (sym == FcitxKey_BackSpace)
        action = 4;
    else if (sym == FcitxKey_Left)
        action = 5;
    else if (sym == FcitxKey_Right)
        action = 6;
    else if (sym == FcitxKey_Up || sym == FcitxKey_ISO_Left_Tab)
        action = 7;
    else if (sym == FcitxKey_Down || sym == FcitxKey_Tab)
        action = 8;
    else if (sym == FcitxKey_F6)
        action = 9;
    else if (sym == FcitxKey_F7)
        action = 10;
    else if (sym == FcitxKey_F10)
        action = 11;
    else if (sym == FcitxKey_Muhenkan)
        action = 12;
    else if (sym == FcitxKey_Henkan || sym == FcitxKey_Hiragana_Katakana)
        action = 13;
    else {
        const auto character = fcitx::Key::keySymToUnicode(sym);
        if (character >= 0x21 && character != 0x7f) {
            action = 0;
            text = fcitx::Key::keySymToUTF8(sym);
        } else if (key.isCursorMove() || sym == FcitxKey_Delete)
            action = 15;
    }
    auto *view = monakime_key(registry_->core, state(ic)->id, action, text.c_str(),
                            key.states().test(fcitx::KeyState::Shift), sensitive(ic), 0);
    if (view && (view->flags & 1))
        event.filterAndAccept();
    render(ic, view);
}
} // namespace monakime::fcitxaddon
