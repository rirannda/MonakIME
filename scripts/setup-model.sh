#!/usr/bin/env bash
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

size="${1:-xsmall}"
case "$size" in
  xsmall)
    revision=4f5423f0fad41a73b1242eb96fe5c12ae4fdca83
    checksum=00c64b3d318045a708d0cad5434faccab10f5481a49e6362864551fd0995fa58
    bytes=20970304
    ;;
  small)
    revision=c67e03e07d215c869f591b274c1631170d3e11fe
    checksum=29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673
    bytes=73871936
    ;;
  -h|--help)
    printf 'Usage: %s [xsmall|small] [output-directory]\n' "$0"
    exit 0
    ;;
  *) printf 'Unknown model size: %s\n' "$size" >&2; exit 2 ;;
esac

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir="${2:-$project_dir/models}"
model="zenz-v3.2-${size}-Q5_K_M.gguf"
url="https://huggingface.co/Miwa-Keita/zenz-v3.2-${size}-gguf/resolve/${revision}/ggml-model-Q5_K_M.gguf"
mkdir -p -- "$output_dir"
destination="$output_dir/$model"

verify() {
  [[ -f "$1" ]] && [[ "$(wc -c < "$1")" -eq "$bytes" ]] &&
    printf '%s  %s\n' "$checksum" "$1" | sha256sum --check --status
}

if verify "$destination"; then
  printf 'Verified: %s\n' "$destination"
  exit 0
fi
temporary="$(mktemp "$output_dir/.${model}.XXXXXX")"
trap 'rm -f -- "$temporary"' EXIT
printf 'Downloading %s (%s bytes, Apache-2.0)\n' "$model" "$bytes"
curl --fail --location --proto '=https' --proto-redir '=https' \
  --tlsv1.2 --retry 3 --connect-timeout 20 --max-time 1800 \
  --output "$temporary" "$url"
if ! verify "$temporary"; then
  printf 'Model checksum/size mismatch; downloaded file was discarded.\n' >&2
  exit 1
fi
chmod 644 "$temporary"
mv -f -- "$temporary" "$destination"
printf 'Verified: %s\n' "$destination"
