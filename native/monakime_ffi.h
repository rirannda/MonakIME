// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
extern "C" {
struct MonakimeCore;
// Read-only prefix of the Rust-owned view. Always release with view_free.
struct MonakimeView {
    const char *preedit;
    const char *commit;
    const char *auxiliary;
    const char *const *candidates;
    size_t count, selection, cursor, highlight_start, highlight_end;
    uint32_t flags; // 1: accepted key, 2: Latin mode, 4: save learning after debounce
};
MonakimeCore *monakime_core_new(void (*wake)(void *), void *user);
void monakime_core_free(MonakimeCore *);
MonakimeView *monakime_key(MonakimeCore *, uint64_t id, uint32_t action, const char *text, bool shift,
                       bool sensitive, size_t index);
MonakimeView *monakime_reset(MonakimeCore *, uint64_t id);
MonakimeView *monakime_view(const MonakimeCore *, uint64_t id);
void monakime_view_free(MonakimeView *);
void monakime_remove(MonakimeCore *, uint64_t id);
size_t monakime_poll(MonakimeCore *, uint64_t *ids, size_t capacity);
bool monakime_save(MonakimeCore *);
}
