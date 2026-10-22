// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fcitx_bridge.h"
#include <fcitx/addonfactory.h>
#include <fcitx/addonmanager.h>

namespace monakime::fcitxaddon {
class Factory final : public fcitx::AddonFactory {
  public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new Engine(manager->instance());
    }
};
} // namespace monakime::fcitxaddon
FCITX_ADDON_FACTORY(monakime::fcitxaddon::Factory)
