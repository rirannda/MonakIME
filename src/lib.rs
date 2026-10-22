// Copyright (C) 2026 rirannda
// SPDX-License-Identifier: GPL-3.0-or-later
//! MonakIME's implementation. Native API adapters live in `native/`.
//! Start with `romaji`, then `compose`, `converter`, `session`, and `worker`.
pub mod compose;
pub mod config;
pub mod converter;
pub mod dictionary;
pub mod ffi;
pub mod mozc;
pub mod romaji;
pub mod session;
pub mod text;
pub mod worker;
pub mod zenz;
