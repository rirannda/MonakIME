#!/usr/bin/env python3
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise Shift-toggle migration using fcitx5's real GlobalConfig parser."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/disable-shift-toggle.py"
PROBE = Path(sys.argv[1]).resolve()


def read_keys(path=None):
    command = [str(PROBE)] + ([str(path)] if path is not None else [])
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    return {parts[0]: parts[1:] for line in result.stdout.splitlines()
            if (parts := line.split("\t"))}


def migrate(path=None, env=None, dry_run=False):
    command = [sys.executable, str(SCRIPT)]
    if path is not None:
        command += ["--config", str(path)]
    if dry_run:
        command += ["--dry-run"]
    return subprocess.run(command, check=True, capture_output=True, text=True, env=env)


assert read_keys()["alt"] == ["Shift_L"], "Test requires fcitx5's default Shift toggle"
with tempfile.TemporaryDirectory(prefix="monakime-shift-test-") as directory:
    root = Path(directory)
    config = root / "config"
    original = ("[Hotkey/TriggerKeys]\n0=Zenkaku_Hankaku\n1=Hangul\n\n"
                "# [Hotkey/AltTriggerKeys]\n# 0=Shift_L\n\n"
                "[Hotkey/EnumerateBackwardKeys]\n0=Control+Alt+Shift+space\n\n"
                "[Behavior]\nActiveByDefault=True\n")
    config.write_text(original)
    assert read_keys(config)["alt"] == ["Shift_L"]
    assert "+[Hotkey/AltTriggerKeys]" in migrate(config, dry_run=True).stdout
    assert config.read_text() == original
    migrate(config)
    keys = read_keys(config)
    assert keys["alt"] == []
    assert keys["trigger"] == ["Zenkaku_Hankaku", "Hangul"]
    assert keys["backward"] == ["Control+Alt+Shift+space"]
    backup = config.with_name(config.name + ".monakime-before-shift")
    assert backup.read_text() == original
    assert backup.stat().st_mode & 0o777 == 0o600
    current = config.read_bytes()
    before = config.stat().st_mtime_ns
    assert migrate(config).stdout == ""
    assert config.read_bytes() == current
    assert config.stat().st_mtime_ns == before
    assert backup.read_text() == original

    mixed = root / "mixed"
    mixed.write_text("[Hotkey/AltTriggerKeys]\n0=Shift_L\n# Keep combinations.\n"
                     "1=Control+space\n2=Shift_R\n3=Shift+space\n")
    migrate(mixed)
    assert read_keys(mixed)["alt"] == ["Control+space", "Shift+space"]
    assert "# Keep combinations." in mixed.read_text()

    empty = root / "empty"
    empty.write_text("[Hotkey/AltTriggerKeys]\n")
    before = empty.stat().st_mtime_ns
    migrate(empty)
    assert read_keys(empty)["alt"] == []
    assert empty.stat().st_mtime_ns == before
    assert not empty.with_name(empty.name + ".monakime-before-shift").exists()

    # The default path honors XDG_CONFIG_HOME and a symlinked configuration.
    xdg = root / "xdg"
    (xdg / "fcitx5").mkdir(parents=True)
    target = root / "linked"
    target.write_text("[Hotkey/AltTriggerKeys]\n0=Shift_R\n")
    link = xdg / "fcitx5/config"
    link.symlink_to(target)
    migrate(env={**os.environ, "XDG_CONFIG_HOME": str(xdg)})
    assert link.is_symlink()
    assert read_keys(target)["alt"] == []

    missing = root / "missing/fcitx5/config"
    migrate(missing)
    assert read_keys(missing)["alt"] == []
    assert missing.stat().st_mode & 0o777 == 0o600
print("fcitx5 Shift toggle: disabled; other shortcuts and config preserved")
