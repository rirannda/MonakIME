#!/usr/bin/env python3
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep old settings and learning usable while preferring the MonakIME names."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary = Path(sys.argv[1]).resolve()
root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="monakime-rename-test-") as directory:
    temporary = Path(directory)
    config = temporary / "config"
    data = temporary / "data"
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("MONAKIME_", "NEWIME_"))}
    env.update(HOME=str(temporary), XDG_CONFIG_HOME=str(config), XDG_DATA_HOME=str(data),
               MONAKIME_DATA_DIR=str(root / "data"),
               MONAKIME_SYSTEM_DICTIONARY=str(temporary / "no-system"))

    def diagnose(**changes):
        case = {**env, **changes}
        result = subprocess.run([str(binary), "--diagnose"], env=case, capture_output=True,
                                text=True, check=True, timeout=10)
        return json.loads(result.stdout)

    fresh = diagnose()
    assert fresh["name"] == "MonakIME"
    assert fresh["learning_path"] == str(data / "monakime/learning.tsv")
    assert fresh["user_dictionary"] == str(config / "monakime/user.tsv")
    assert fresh["mozc_profile"] == str(data / "monakime/mozc")

    old_config, old_data = config / "newime", data / "newime"
    old_config.mkdir(parents=True)
    old_data.mkdir(parents=True)
    (old_config / "config").write_text("threads=3\nmodel=/legacy/model.gguf\n")
    (old_config / "user.tsv").write_text("ひみつ\t秘密\t1000\n")
    (old_data / "learning.tsv").write_text("")
    (old_data / "mozc").mkdir()
    old = diagnose()
    assert old["threads"] == 3 and old["model"] == "/legacy/model.gguf"
    assert old["user_dictionary"] == str(old_config / "user.tsv")
    assert old["learning_path"] == str(old_data / "learning.tsv")
    assert old["mozc_profile"] == str(old_data / "mozc")

    new_config, new_data = config / "monakime", data / "monakime"
    new_config.mkdir()
    new_data.mkdir()
    (new_config / "config").write_text("threads=4\n")
    (new_config / "user.tsv").touch()
    (new_data / "learning.tsv").touch()
    (new_data / "mozc").mkdir()
    new = diagnose()
    assert new["threads"] == 4 and new["model"] == "/legacy/model.gguf"
    assert new["user_dictionary"] == str(new_config / "user.tsv")
    assert new["learning_path"] == str(new_data / "learning.tsv")
    assert new["mozc_profile"] == str(new_data / "mozc")

    assert diagnose(NEWIME_THREADS="6")["threads"] == 6
    assert diagnose(NEWIME_THREADS="6", MONAKIME_THREADS="7")["threads"] == 7
    assert diagnose(NEWIME_ZENZ="0")["model"] == ""
    assert diagnose(NEWIME_LIVE_CONVERSION="0")["mozc_helper"] == ""
    assert diagnose(NEWIME_LEARNING_PATH="/legacy/learning.tsv")["learning_path"] == "/legacy/learning.tsv"
    assert diagnose(NEWIME_LEARNING_PATH="/legacy/learning.tsv",
                    MONAKIME_LEARNING_PATH="/new/learning.tsv")["learning_path"] == "/new/learning.tsv"
print("MonakIME names, legacy environment, settings and learning paths: passed")
