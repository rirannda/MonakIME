// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
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
#include <iostream>
#include <memory>
#include <testfrontend_public.h>

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
} // namespace
int main() {
    setupTestingEnvironment(MONAKIME_BINARY_DIR,
                            {std::string(MONAKIME_BINARY_DIR) + "/addon", MONAKIME_SYSTEM_ADDON_DIR},
                            {"test"});
    setenv("MONAKIME_DICTIONARY", MONAKIME_SOURCE_DIR "/data/base.tsv", 1);
    setenv("MONAKIME_SYSTEM_DICTIONARY", "/nonexistent-monakime-test-system", 1);
    setenv("MONAKIME_MODEL", MONAKIME_SOURCE_DIR "/models/zenz-v3.2-xsmall-Q5_K_M.gguf", 1);
    setenv("MONAKIME_LEARNING", "0", 1);
    setenv("MONAKIME_ZENZ", "1", 1);
    setenv("MONAKIME_LIVE_CONVERSION", "0", 1);
    char arg0[] = "monakime-model-test", arg1[] = "--disable=all";
    char arg2[] = "--enable=testfrontend,testui,testim,keyboard,monakime";
    char *argv[] = {arg0, arg1, arg2};
    Log::setLogRule("default=3");
    Instance instance(3, argv);
    instance.addonManager().registerDefaultLoader(nullptr);
    EventDispatcher dispatcher;
    dispatcher.attach(&instance.eventLoop());
    AddonInstance *frontend = nullptr;
    ICUUID uuid{};
    InputContext *ic = nullptr;
    int stage = 0;
    uint64_t stageStarted = 0;
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
        uuid = frontend->call<ITestFrontend::createInputContext>("monakime-model-test");
        ic = instance.inputContextManager().findByUUID(uuid);
        ic->focusIn();
        instance.setCurrentInputMethod(ic, "monakime", true);
        type(frontend, uuid, "kyouhatennkigaiidesu");
        const auto start = std::chrono::steady_clock::now();
        key(frontend, uuid, "space");
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        FCITX_ASSERT(elapsed < 100) << "Space blocked UI: " << elapsed << "ms";
        FCITX_ASSERT(ic->inputPanel().candidateList());
        stageStarted = now(CLOCK_MONOTONIC);
        timer = instance.eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, stageStarted + 10000, 0,
            [&](EventSourceTime *source, uint64_t current) {
                FCITX_ASSERT(current - stageStarted < 5000000)
                    << "async model timed out at stage " << stage;
                if (stage == 0 && preedit(ic) == "今日は天気がいいです") {
                    std::cout << "fcitx5: real CPU neural candidate delivered asynchronously\n";
                    ic->reset();
                    type(frontend, uuid, "Googledeshirabete,GitHubnoko-dowosyuuseishimashita.");
                    key(frontend, uuid, "space");
                    stage = 3;
                    stageStarted = current;
                } else if (stage == 3 &&
                           preedit(ic) == "Googleで調べて、GitHubのコードを修正しました。") {
                    ic->reset();
                    type(frontend, uuid, "CPUsiyourituha12.5%,memorishiyouryouha256MBdesita.");
                    key(frontend, uuid, "space");
                    stage = 4;
                    stageStarted = current;
                } else if (stage == 4 &&
                           preedit(ic) == "CPU使用率は12.5%、メモリ使用量は256MBでした。") {
                    std::cout << "fcitx5: mixed-case English prefixes and numeric units passed\n";
                    ic->reset();
                    type(frontend, uuid, "nihongonyuuryoku");
                    key(frontend, uuid, "space");
                    key(frontend, uuid, "Escape");
                    key(frontend, uuid, "Escape");
                    type(frontend, uuid, "ka");
                    stage = 1;
                    stageStarted = current;
                } else if (stage == 1 && current - stageStarted > 150000) {
                    FCITX_ASSERT(preedit(ic) == "か") << "stale conversion replaced new typing";
                    ic->reset();
                    type(frontend, uuid, "kyouhatennkigaiidesu");
                    key(frontend, uuid, "space");
                    frontend->call<ITestFrontend::destroyInputContext>(uuid);
                    uuid = frontend->call<ITestFrontend::createInputContext>("monakime-replacement");
                    ic = instance.inputContextManager().findByUUID(uuid);
                    ic->focusIn();
                    instance.setCurrentInputMethod(ic, "monakime", true);
                    type(frontend, uuid, "ni");
                    stage = 2;
                    stageStarted = current;
                } else if (stage == 2 && current - stageStarted > 150000) {
                    FCITX_ASSERT(preedit(ic) == "に")
                        << "destroyed context result reached another field";
                    frontend->call<ITestFrontend::destroyInputContext>(uuid);
                    std::cout << "fcitx5: cancelled/stale/destroyed context requests passed\n";
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
    return instance.exec();
}
