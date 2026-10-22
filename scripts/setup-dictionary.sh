#!/usr/bin/env bash
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
revision=a069a88d4cb5c011de0f9aebb6c149a1c808d904
# Optional local google/mozc git checkout. Read committed sources so local UT
# dictionary additions are not accidentally redistributed under Mozc's license.
local_source=${1:-}
temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT
mkdir -p "$root/data/licenses"
fetch() {
    local relative=$1 destination=$2
    if [[ -n "$local_source" ]]; then
        git -C "$local_source" show "$revision:$relative" > "$destination"
    else
        curl --fail --location --proto '=https' --proto-redir '=https' --retry 3 \
            --connect-timeout 20 --max-time 180 \
            "https://raw.githubusercontent.com/google/mozc/$revision/$relative" -o "$destination"
    fi
}
files=()
for index in {0..9}; do
    name="dictionary0${index}.txt"
    fetch "src/data/dictionary_oss/$name" "$temporary/$name"
    files+=("$temporary/$name")
done
fetch LICENSE "$root/data/licenses/Mozc-LICENSE"
fetch src/data/dictionary_oss/README.txt "$root/data/licenses/Mozc-dictionary-LICENSE"
python3 "$root/scripts/import-dictionary.py" --format mozc \
    --output "$root/data/system.nimedic" --max-entries "${MONAKIME_DICTIONARY_ENTRIES:-${NEWIME_DICTIONARY_ENTRIES:-300000}}" "${files[@]}"
printf 'Mozc dictionary source revision: %s\n' "$revision"
