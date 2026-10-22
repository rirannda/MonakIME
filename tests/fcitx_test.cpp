// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end checks through Fcitx's public test frontend and the real addon loader.
#include <cstdlib>
#include <fcitx-utils/eventdispatcher.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/testing.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodgroup.h>
#include <fcitx/inputmethodmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/globalconfig.h>
#include <iostream>
#include <string>
#include <testfrontend_public.h>

namespace {
using namespace fcitx;
void key(AddonInstance *frontend, ICUUID uuid, const char *name) {
    frontend->call<ITestFrontend::keyEvent>(uuid, Key(name), false);
    frontend->call<ITestFrontend::keyEvent>(uuid, Key(name), true);
}
void type(AddonInstance *frontend, ICUUID uuid, const std::string &raw) {
    for (char c : raw)
        key(frontend, uuid, std::string(1, c).c_str());
}
std::string preedit(InputContext *ic) {
    return ic->capabilityFlags().test(CapabilityFlag::Preedit)
               ? ic->inputPanel().clientPreedit().toString()
               : ic->inputPanel().preedit().toString();
}
void expectCommit(AddonInstance *frontend, ICUUID uuid, const std::string &text) {
    frontend->call<ITestFrontend::pushCommitExpectation>(text);
    key(frontend, uuid, "Return");
}
} // namespace

int main() {
    using namespace fcitx;
    setupTestingEnvironment(MONAKIME_BINARY_DIR,
                            {std::string(MONAKIME_BINARY_DIR) + "/addon", MONAKIME_SYSTEM_ADDON_DIR},
                            {"test"});
    setenv("MONAKIME_DICTIONARY", MONAKIME_SOURCE_DIR "/data/base.tsv", 1);
    setenv("MONAKIME_SYSTEM_DICTIONARY", "/nonexistent-monakime-test-system", 1);
    setenv("MONAKIME_LEARNING", "0", 1);
    setenv("MONAKIME_ZENZ", "0", 1);
    setenv("MONAKIME_LIVE_CONVERSION", "0", 1);
    char arg0[] = "monakime-fcitx-test";
    char arg1[] = "--disable=all";
    char arg2[] = "--enable=testfrontend,testui,testim,keyboard,monakime";
    char *argv[] = {arg0, arg1, arg2};
    Log::setLogRule("default=3");
    Instance instance(FCITX_ARRAY_SIZE(argv), argv);
    instance.addonManager().registerDefaultLoader(nullptr);
    EventDispatcher dispatcher;
    dispatcher.attach(&instance.eventLoop());
    dispatcher.schedule([&] {
        // Instance initialization loads the config; override it afterwards.
        RawConfig hotkeys;
        hotkeys["Hotkey/AltTriggerKeys"];
        instance.globalConfig().load(hotkeys);
        auto *frontend = instance.addonManager().addon("testfrontend");
        FCITX_ASSERT(frontend && instance.addonManager().addon("monakime", true));
        auto group = instance.inputMethodManager().currentGroup();
        group.inputMethodList().clear();
        group.inputMethodList().push_back(InputMethodGroupItem("keyboard-us"));
        group.inputMethodList().push_back(InputMethodGroupItem("monakime"));
        group.setDefaultInputMethod("");
        instance.inputMethodManager().setGroup(group);

        auto uuid = frontend->call<ITestFrontend::createInputContext>("monakime-integration");
        auto *ic = instance.inputContextManager().findByUUID(uuid);
        FCITX_ASSERT(ic);
        instance.setCurrentInputMethod(ic, "monakime", true);
        FCITX_ASSERT(instance.inputMethod(ic) == "monakime");
        type(frontend, uuid, "nihongo");
        FCITX_ASSERT(preedit(ic) == "にほんご") << preedit(ic);
        key(frontend, uuid, "space");
        auto list = ic->inputPanel().candidateList();
        FCITX_ASSERT(list && list->toBulk() && list->toBulk()->totalSize() > 0);
        const auto first = preedit(ic);
        FCITX_ASSERT(!first.empty());
        key(frontend, uuid, "Down");
        key(frontend, uuid, "Up");
        FCITX_ASSERT(preedit(ic) == first);
        list = ic->inputPanel().candidateList();
        const auto lastIndex = list->toBulk()->totalSize() - 1;
        const auto chosen = list->toBulk()->candidateFromAll(lastIndex).text().toString();
        list->toBulk()->candidateFromAll(lastIndex).select(ic);
        FCITX_ASSERT(preedit(ic) == chosen);
        expectCommit(frontend, uuid, chosen);
        FCITX_ASSERT(ic->inputPanel().empty());

        // Function keys and cancellation must preserve the editable raw reading.
        type(frontend, uuid, "nihongo");
        key(frontend, uuid, "F7");
        FCITX_ASSERT(preedit(ic) == "ニホンゴ");
        key(frontend, uuid, "F6");
        FCITX_ASSERT(preedit(ic) == "にほんご");
        key(frontend, uuid, "F10");
        FCITX_ASSERT(preedit(ic) == "nihongo");
        key(frontend, uuid, "Escape");
        FCITX_ASSERT(preedit(ic) == "にほんご");
        key(frontend, uuid, "Escape");
        FCITX_ASSERT(ic->inputPanel().empty());
        type(frontend, uuid, "ka");
        key(frontend, uuid, "BackSpace");
        FCITX_ASSERT(ic->inputPanel().empty());

        type(frontend, uuid, "kyaku");
        key(frontend, uuid, "BackSpace");
        FCITX_ASSERT(preedit(ic) == "きゃ");
        key(frontend, uuid, "BackSpace");
        FCITX_ASSERT(preedit(ic) == "き");
        key(frontend, uuid, "Shift_L");
        key(frontend, uuid, "Shift_R");
        FCITX_ASSERT(instance.inputMethod(ic) == "monakime");
        FCITX_ASSERT(preedit(ic) == "き");
        key(frontend, uuid, "BackSpace");
        FCITX_ASSERT(ic->inputPanel().empty());

        type(frontend, uuid, "google");
        key(frontend, uuid, "BackSpace");
        FCITX_ASSERT(preedit(ic) == "googl");
        key(frontend, uuid, "BackSpace");
        FCITX_ASSERT(preedit(ic) == "goog");
        type(frontend, uuid, "le");
        FCITX_ASSERT(preedit(ic) == "google");
        key(frontend, uuid, "space");
        expectCommit(frontend, uuid, "google");

        // Segment navigation and Unicode boundary edits remain usable end to end.
        type(frontend, uuid, "watashihanihongowohanashimasu");
        key(frontend, uuid, "space");
        key(frontend, uuid, "Right");
        key(frontend, uuid, "Shift+Right");
        key(frontend, uuid, "Shift+Left");
        key(frontend, uuid, "Left");
        key(frontend, uuid, "2");
        FCITX_ASSERT(!preedit(ic).empty());
        expectCommit(frontend, uuid, preedit(ic));
        FCITX_ASSERT(ic->inputPanel().empty());

        // A literal acronym can absorb a kana syllable using its original keys.
        type(frontend, uuid, "CPUsiyou");
        key(frontend, uuid, "space");
        key(frontend, uuid, "Shift+Right");
        FCITX_ASSERT(preedit(ic).rfind("CPUsi", 0) == 0) << preedit(ic);
        expectCommit(frontend, uuid, preedit(ic));

        // Return before conversion commits final n as ん.
        type(frontend, uuid, "nihon");
        expectCommit(frontend, uuid, "にほん");

        // Capitalization must protect only the English span, including byte units.
        type(frontend, uuid, "Googledeshirabete,GitHubnoko-dowosyuuseishimashita.");
        FCITX_ASSERT(preedit(ic) == "Googleでしらべて、GitHubのこーどをしゅうせいしました。")
            << preedit(ic);
        expectCommit(frontend, uuid, "Googleでしらべて、GitHubのこーどをしゅうせいしました。");
        type(frontend, uuid, "CPUsiyourituha12.5%,memorishiyouryouha256MBdesita.");
        FCITX_ASSERT(preedit(ic) == "CPUしようりつは12.5%、めもりしようりょうは256MBでした。")
            << preedit(ic);
        expectCommit(frontend, uuid, "CPUしようりつは12.5%、めもりしようりょうは256MBでした。");

        // Reset, focus changes, and sensitive fields discard all composition.
        type(frontend, uuid, "toukyou");
        ic->reset();
        FCITX_ASSERT(ic->inputPanel().empty());
        type(frontend, uuid, "toukyou");
        ic->focusOut();
        FCITX_ASSERT(ic->inputPanel().empty());
        ic->focusIn();
        const auto originalCapabilities = ic->capabilityFlags();
        ic->setCapabilityFlags(originalCapabilities | CapabilityFlag::Password);
        FCITX_ASSERT(!frontend->call<ITestFrontend::sendKeyEvent>(uuid, Key("a"), false));
        FCITX_ASSERT(ic->inputPanel().empty());
        ic->setCapabilityFlags(originalCapabilities | CapabilityFlag::Sensitive);
        FCITX_ASSERT(!frontend->call<ITestFrontend::sendKeyEvent>(uuid, Key("a"), false));
        FCITX_ASSERT(ic->inputPanel().empty());
        ic->setCapabilityFlags(originalCapabilities);

        // Composition stays attached to its input context.
        type(frontend, uuid, "kana");
        const auto uuid2 = frontend->call<ITestFrontend::createInputContext>("monakime-second-field");
        auto *ic2 = instance.inputContextManager().findByUUID(uuid2);
        instance.setCurrentInputMethod(ic2, "monakime", true);
        FCITX_ASSERT(ic2->inputPanel().empty());
        type(frontend, uuid2, "ai");
        FCITX_ASSERT(preedit(ic2) == "あい");
        FCITX_ASSERT(preedit(ic) != "あい");
        ic2->reset();
        frontend->call<ITestFrontend::destroyInputContext>(uuid2);
        ic->focusIn();
        ic->reset();

        // Consecutive convert/cancel/commit must not leave a candidate panel behind.
        for (int i = 0; i < 8; ++i) {
            type(frontend, uuid, "nihongo");
            key(frontend, uuid, "space");
            key(frontend, uuid, "Escape");
            expectCommit(frontend, uuid, "にほんご");
        }
        FCITX_ASSERT(ic->inputPanel().empty());
        frontend->call<ITestFrontend::destroyInputContext>(uuid);
        std::cout << "fcitx5: real addon loading, typing, candidates, segments, reset, focus and "
                     "sensitive fields passed\n";
        dispatcher.schedule([&] {
            dispatcher.detach();
            instance.exit();
        });
    });
    return instance.exec();
}
