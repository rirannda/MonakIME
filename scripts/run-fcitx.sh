#!/usr/bin/env bash
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${MONAKIME_BUILD_DIR:-${NEWIME_BUILD_DIR:-"$root/build"}}
if [[ ! -f "$build_dir/addon/monakime.so" ]]; then
    printf 'Build the addon first: cmake --build "%s" -j2\n' "$build_dir" >&2
    exit 1
fi
# FCITX_ADDON_DIRS replaces fcitx5's compiled-in addon directories. Include the
# installed libraries as well as the development addon and any caller paths.
if [[ -s "$build_dir/fcitx5-addon-dir" ]]; then
    IFS= read -r system_addon_dir < "$build_dir/fcitx5-addon-dir"
elif command -v pkg-config >/dev/null && pkg-config --exists Fcitx5Core; then
    system_addon_dir="$(pkg-config --variable=libdir Fcitx5Core)/fcitx5"
else
    printf 'Reconfigure this build to locate the installed fcitx5 addons: cmake -S "%s" -B "%s"\n' "$root" "$build_dir" >&2
    exit 1
fi
if [[ "$system_addon_dir" != /* || ! -d "$system_addon_dir" ]]; then
    printf 'Installed fcitx5 addon directory is unavailable: %s\n' "$system_addon_dir" >&2
    exit 1
fi
export MONAKIME_DATA_DIR=${MONAKIME_DATA_DIR:-${NEWIME_DATA_DIR:-"$root/data"}}
export FCITX_ADDON_DIRS="$build_dir/addon${FCITX_ADDON_DIRS:+:$FCITX_ADDON_DIRS}:$system_addon_dir"
export XDG_DATA_DIRS="$build_dir/run-share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
mkdir -p "$build_dir/run-share/fcitx5/addon" "$build_dir/run-share/fcitx5/inputmethod"
mkdir -p "$build_dir/run-share/icons/hicolor/scalable/apps"
cp "$root/data/monakime.svg" "$build_dir/run-share/icons/hicolor/scalable/apps/monakime.svg"
cp "$build_dir/test/addon/monakime.conf" "$build_dir/run-share/fcitx5/addon/monakime.conf"
cp "$build_dir/test/inputmethod/monakime.conf" "$build_dir/run-share/fcitx5/inputmethod/monakime.conf"
cp "$build_dir/test/inputmethod/newime.conf" "$build_dir/run-share/fcitx5/inputmethod/newime.conf"
# Remove metadata from development builds before the addon was renamed.
rm -f "$build_dir/run-share/fcitx5/addon/newime.conf"
python3 "$root/scripts/disable-shift-toggle.py"
# Explicit launch command: existing fcitx5 is replaced only when this script is run.
exec fcitx5 -r "$@"
