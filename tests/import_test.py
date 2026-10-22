#!/usr/bin/env python3
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check offline dictionary compilation and the actual native mmap reader."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="monakime-import-test-") as directory:
    temp = Path(directory)
    source = temp / "mozc.txt"
    source.write_text("うちゅうきつね\t1\t1\t6000\t宇宙狐\n"
                      "うちゅうきつね\t2\t2\t3000\t宇宙狐\n"
                      "ウチュウキツネ\t1\t1\t9000\t宇宙きつね\n"
                      "こわれた\t1\t1\tnan\t不正\n"
                      "bad line\n", encoding="utf-8")
    dictionary = temp / "test.nimedic"
    subprocess.run([sys.executable, str(root / "scripts/import-dictionary.py"),
                    "--format", "mozc", "-o", str(dictionary), str(source)], check=True,
                   capture_output=True, text=True)
    raw = dictionary.read_bytes()
    magic, count, blob_size = struct.unpack_from("<8sII", raw)
    assert magic == b"NIMEDIC1" and count == 2
    assert len(raw) == 16 + count * 12 + blob_size
    assert struct.unpack_from("<f", raw, 24)[0] == 3.0  # deduplicate, keep lowest cost
    env = {**os.environ, "MONAKIME_SYSTEM_DICTIONARY": str(dictionary),
           "MONAKIME_LEARNING": "0", "MONAKIME_DATA_DIR": str(root / "data")}
    result = subprocess.run([str(binary), "--no-model", "--json", "うちゅうきつね"],
                            env=env, text=True, capture_output=True, check=True)
    assert json.loads(result.stdout)["candidates"][0] == "宇宙狐"
    # Learning must outrank both dictionaries and a later neural suggestion.
    env.update(MONAKIME_LEARNING="1", MONAKIME_LEARNING_PATH=str(temp / "learning.tsv"))
    subprocess.run([str(binary), "--no-model", "--learn", "はし", "箸"], env=env,
                   check=True, capture_output=True)
    result = subprocess.run([str(binary), "--no-model", "--json", "hashi"], env=env,
                            check=True, text=True, capture_output=True)
    assert json.loads(result.stdout)["candidates"][0] == "箸"
    model = root / "models/zenz-v3.2-xsmall-Q5_K_M.gguf"
    capabilities = subprocess.run([str(binary), "--diagnose"], env=env, check=True,
                                  text=True, capture_output=True)
    if model.exists() and json.loads(capabilities.stdout)["llama"]:
        result = subprocess.run([str(binary), "--model", str(model), "--json", "hashi"],
                                env=env, check=True, text=True, capture_output=True)
        assert json.loads(result.stdout)["candidates"][0] == "箸"
    assert (temp / "learning.tsv").stat().st_mode & 0o777 == 0o600
print("dictionary import, mmap lookup and isolated learning passed")
