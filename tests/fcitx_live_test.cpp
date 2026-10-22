// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <fcitx-utils/event.h>
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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <testfrontend_public.h>
#include <unistd.h>

using namespace fcitx;
namespace {
void key(AddonInstance *frontend, ICUUID uuid, const std::string &name) {
    frontend->call<ITestFrontend::keyEvent>(uuid, Key(name), false);
    frontend->call<ITestFrontend::keyEvent>(uuid, Key(name), true);
}
void type(AddonInstance *frontend, ICUUID uuid, const std::string &raw) {
    for (char c : raw)
        key(frontend, uuid, std::string(1, c));
}
std::string preedit(InputContext *ic) {
    return ic->capabilityFlags().test(CapabilityFlag::Preedit)
               ? ic->inputPanel().clientPreedit().toString()
               : ic->inputPanel().preedit().toString();
}
bool modelLoaded() {
    std::ifstream file("/proc/self/maps");
    std::string line;
    while (std::getline(file, line))
        if (line.find("zenz-v3.2-xsmall-Q5_K_M.gguf") != line.npos)
            return true;
    return false;
}
} // namespace
int main() {
    char path[] = "/tmp/monakime-live-test-XXXXXX";
    FCITX_ASSERT(mkdtemp(path));
    setupTestingEnvironment(MONAKIME_BINARY_DIR,
                            {std::string(MONAKIME_BINARY_DIR) + "/addon", MONAKIME_SYSTEM_ADDON_DIR},
                            {"test"});
    setenv("MONAKIME_LEARNING", "0", 1);
    // Disable the fallback dictionaries so only a real Mozc reply can pass live assertions.
    setenv("MONAKIME_DICTIONARY", "/nonexistent-monakime-live-dictionary", 1);
    setenv("MONAKIME_SYSTEM_DICTIONARY", "/nonexistent-monakime-live-system", 1);
    setenv("MONAKIME_ENGLISH_DICTIONARY", MONAKIME_SOURCE_DIR "/data/english.txt", 1);
    setenv("MONAKIME_LIVE_CONVERSION", "1", 1);
    setenv("MONAKIME_MOZC_HELPER", MONAKIME_SOURCE_DIR "/third_party/mozc/meltype_mozc_helper", 1);
    setenv("MONAKIME_MOZC_PROFILE", path, 1);
    setenv("MONAKIME_MODEL", MONAKIME_SOURCE_DIR "/models/zenz-v3.2-xsmall-Q5_K_M.gguf", 1);
#ifdef MONAKIME_TEST_NEURAL
    setenv("MONAKIME_ZENZ", "1", 1);
#else
    setenv("MONAKIME_ZENZ", "0", 1);
#endif
    char arg0[] = "monakime-live-test", arg1[] = "--disable=all";
    char arg2[] = "--enable=testfrontend,testui,testim,keyboard,monakime";
    char *argv[] = {arg0, arg1, arg2};
    Log::setLogRule("default=3");
    Instance instance(3, argv);
    instance.addonManager().registerDefaultLoader(nullptr);
    EventDispatcher dispatcher;
    dispatcher.attach(&instance.eventLoop());
    AddonInstance *frontend = nullptr;
    InputContext *ic = nullptr;
    ICUUID uuid{};
    int stage = 0;
    uint64_t started = 0;
    std::string selected;
    std::unique_ptr<EventSourceTime> timer;
    dispatcher.schedule([&] {
        frontend = instance.addonManager().addon("testfrontend");
        FCITX_ASSERT(frontend && instance.addonManager().addon("monakime", true));
        auto group = instance.inputMethodManager().currentGroup();
        group.inputMethodList().clear();
        group.inputMethodList().push_back(InputMethodGroupItem("keyboard-us"));
        group.inputMethodList().push_back(InputMethodGroupItem("monakime"));
        group.setDefaultInputMethod("");
        instance.inputMethodManager().setGroup(group);
        uuid = frontend->call<ITestFrontend::createInputContext>("monakime-live");
        ic = instance.inputContextManager().findByUUID(uuid);
        ic->focusIn();
        instance.setCurrentInputMethod(ic, "monakime", true);
        type(frontend, uuid, "nihongo");
        started = now(CLOCK_MONOTONIC);
        timer = instance.eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, started + 10000, 0, [&](EventSourceTime *source, uint64_t current) {
                FCITX_ASSERT(current - started < 5000000)
                    << "live stage " << stage << ": " << preedit(ic);
                auto next = [&](int value) {
                    stage = value;
                    started = current;
                };
                if (stage == 0 && preedit(ic) == "日本語") {
                    FCITX_ASSERT(!modelLoaded()) << "typing loaded the neural model";
                    FCITX_ASSERT(!ic->inputPanel().candidateList());
                    key(frontend, uuid, "k");
                    next(1);
                } else if (stage == 1 && preedit(ic) == "日本語k" && current - started > 60000) {
                    FCITX_ASSERT(!modelLoaded());
                    key(frontend, uuid, "BackSpace");
                    next(2);
                } else if (stage == 2 && preedit(ic) == "日本語" && current - started > 60000) {
                    key(frontend, uuid, "BackSpace");
                    FCITX_ASSERT(preedit(ic) == "にほん") << preedit(ic);
                    next(11);
                } else if (stage == 11 && preedit(ic) == "日本" && current - started > 60000) {
                    type(frontend, uuid, "go");
                    next(12);
                } else if (stage == 12 && preedit(ic) == "日本語" && current - started > 60000) {
                    key(frontend, uuid, "Escape");
                    FCITX_ASSERT(preedit(ic) == "にほんご");
                    next(3);
                } else if (stage == 3 && current - started > 100000) {
                    FCITX_ASSERT(preedit(ic) == "にほんご") << "cancelled live result returned";
                    ic->reset();
                    type(frontend, uuid, "hashidegohanwotaberu");
                    next(4);
                } else if (stage == 4 && preedit(ic) == "橋でご飯を食べる") {
                    FCITX_ASSERT(!modelLoaded());
                    const auto before = now(CLOCK_MONOTONIC);
                    key(frontend, uuid, "space");
                    FCITX_ASSERT(now(CLOCK_MONOTONIC) - before < 100000) << "Space blocked UI";
                    FCITX_ASSERT(ic->inputPanel().candidateList());
                    next(5);
#ifdef MONAKIME_TEST_NEURAL
                } else if (stage == 5 && preedit(ic) == "箸でご飯を食べる") {
                    FCITX_ASSERT(modelLoaded());
#else
                } else if (stage == 5 && preedit(ic) == "橋でご飯を食べる" && current - started > 100000) {
                    FCITX_ASSERT(!modelLoaded());
#endif
                    std::cout << "live Mozc and explicit neural conversion passed\n";
                    ic->reset();
                    type(frontend, uuid, "Googledeshirabete,GitHubnoko-dowosyuuseishimashita.");
                    next(6);
                } else if (stage == 6 &&
                           preedit(ic) == "Googleで調べて、GitHubのコードを修正しました。") {
                    frontend->call<ITestFrontend::pushCommitExpectation>(preedit(ic));
                    key(frontend, uuid, "Return");
                    FCITX_ASSERT(ic->inputPanel().empty());
                    type(frontend, uuid, "CPUsiyourituha12.5%,memorishiyouryouha256MBdesita.");
                    next(7);
                } else if (stage == 7 &&
                           preedit(ic) == "CPU使用率は12.5%、メモリ使用量は256MBでした。") {
                    key(frontend, uuid, "space");
                    key(frontend, uuid, "Down");
                    selected = preedit(ic);
                    next(8);
                } else if (stage == 8 && current - started > 200000) {
                    FCITX_ASSERT(preedit(ic) == selected) << "manual selection overwritten";
                    ic->reset();
                    type(frontend, uuid, "toukyou");
                    ic->focusOut();
                    next(9);
                } else if (stage == 9 && current - started > 100000) {
                    FCITX_ASSERT(ic->inputPanel().empty());
                    ic->focusIn();
                    const auto capabilities = ic->capabilityFlags();
                    ic->setCapabilityFlags(capabilities | CapabilityFlag::Sensitive);
                    type(frontend, uuid, "nihongo");
                    FCITX_ASSERT(ic->inputPanel().empty());
                    ic->setCapabilityFlags(capabilities);
                    type(frontend, uuid, "hashidegohanwotaberu");
                    key(frontend, uuid, "space");
                    const auto oldUuid = uuid;
                    uuid = frontend->call<ITestFrontend::createInputContext>(
                        "monakime-live-replacement");
                    ic = instance.inputContextManager().findByUUID(uuid);
                    ic->focusIn();
                    instance.setCurrentInputMethod(ic, "monakime", true);
                    type(frontend, uuid, "GitHubnoko-do");
                    // The old field closes after the new field has queued live conversion.
                    frontend->call<ITestFrontend::destroyInputContext>(oldUuid);
                    next(10);
                } else if (stage == 10 && preedit(ic) == "GitHubのコード" &&
                           current - started > 200000) {
                    key(frontend, uuid, "x");
                    FCITX_ASSERT(preedit(ic) == "GitHubのコードx");
                    frontend->call<ITestFrontend::pushCommitExpectation>("GitHubのコードx");
                    key(frontend, uuid, "Return");
                    frontend->call<ITestFrontend::destroyInputContext>(uuid);
                    std::cout << "mixed English, editing, selection, focus, sensitivity and stale "
                                 "jobs passed\n";
                    dispatcher.schedule([&] {
                        dispatcher.detach();
                        instance.exit();
                    });
                    return false;
                }
                source->setTime(current + 10000);
                source->setOneShot();
                return true;
            });
        timer->setOneShot();
    });
    const auto result = instance.exec();
    std::filesystem::remove_all(path);
    return result;
}
