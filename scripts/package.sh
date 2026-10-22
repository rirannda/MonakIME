#!/usr/bin/env bash
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${MONAKIME_BUILD_DIR:-${NEWIME_BUILD_DIR:-"$root/build"}}
stage_dir=$(mktemp -d "$build_dir/package-stage.XXXXXX")
trap 'rm -rf -- "$stage_dir"' EXIT
DESTDIR="$stage_dir" cmake --install "$build_dir" --strip
archive="$build_dir/monakime-0.2.0-linux-$(uname -m).tar.gz"
tar -czf "$archive" -C "$stage_dir" .
sha256sum "$archive" > "$archive.sha256"
printf '\nPackage: %s\n' "$archive"
