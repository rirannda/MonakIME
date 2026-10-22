#!/usr/bin/env bash
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ "${1:-}" == --help || "${1:-}" == -h ]]; then
    printf 'Usage: %s [xsmall|small] [model-output-directory]\n' "$0"
    exit 0
fi
revision=d88040c704c8855c497d46c20675a3d492f70f94
llama_dir="$root/third_party/llama.cpp"
if [[ ! -f "$llama_dir/CMakeLists.txt" ]]; then
    mkdir -p "$root/third_party"
    git init "$llama_dir"
    git -C "$llama_dir" remote add origin https://github.com/ggml-org/llama.cpp.git
    git -C "$llama_dir" fetch --depth 1 origin "$revision"
    git -C "$llama_dir" checkout --detach FETCH_HEAD
fi
actual=$(git -C "$llama_dir" rev-parse HEAD)
if [[ "$actual" != "$revision" ]]; then
    printf 'llama.cpp revision mismatch: %s (expected %s)\n' "$actual" "$revision" >&2
    exit 1
fi
cargo fetch --locked --manifest-path "$root/Cargo.toml"
bash "$root/scripts/setup-model.sh" "$@"
printf '\nReady: cmake -S "%s" -B "%s/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr\n' "$root" "$root"
