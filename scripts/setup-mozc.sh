#!/usr/bin/env bash
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ $(uname -m) != x86_64 ]]; then
    echo 'This pinned helper is x86_64. Build native/mozc from Meltype v1.3.0 and set MONAKIME_MOZC_HELPER.' >&2
    exit 1
fi
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
archive=${1:-"$work/Meltype-1.3.0-linux.zip"}
if [[ $# == 0 ]]; then
    curl -fL --retry 3 https://github.com/yksr-melt/Meltype/releases/download/v1.3.0/Meltype-1.3.0-linux.zip -o "$archive"
fi
python3 - "$archive" "$root/third_party/mozc" <<'PY'
import hashlib, json, pathlib, sys, zipfile
archive, destination = map(pathlib.Path, sys.argv[1:])
expected = '284ee5729975df2de9ed4fd7d5f5b22b0982db7c5a4f2186bb01d5bac835e736'
digest = hashlib.sha256()
with archive.open('rb') as stream:
    while block := stream.read(1024 * 1024):
        digest.update(block)
if digest.hexdigest() != expected:
    raise SystemExit('Mozc archive SHA-256 mismatch')
files = {'meltype_mozc_helper': '28e060a0cadc1405045cb8ae1516e938de143cb8347307be459f6052e7c9fbd9',
         'MOZC-LICENSE.txt': '44cdd923b91ea9199293abecc2762c70c87dbf1e581c027a94c416368d1a648c',
         'MOZC-CREDITS.html': 'dd7c566382204f448040095e5d3c2f2b5199a1b85d44f427ceb94277dc04af50'}
destination.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(archive) as bundle:
    for name, digest in files.items():
        data = bundle.read('Meltype-linux/mozc/' + name)
        if hashlib.sha256(data).hexdigest() != digest:
            raise SystemExit('Mozc member SHA-256 mismatch: ' + name)
        path = destination / name
        path.write_bytes(data)
        path.chmod(0o755 if name == 'meltype_mozc_helper' else 0o644)
(destination / 'provenance.json').write_text(json.dumps({
    'release': 'https://github.com/yksr-melt/Meltype/releases/tag/v1.3.0',
    'archive_sha256': expected, 'files': files,
    'mozc_revision': 'a069a88d4cb5c011de0f9aebb6c149a1c808d904',
    'dictionaries': 'official OSS; no UT additions', 'architecture': 'x86_64'
}, indent=2) + '\n')
print('Installed pinned native Mozc helper:', destination)
PY
