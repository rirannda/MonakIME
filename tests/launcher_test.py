#!/usr/bin/env python3
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
"""Launch the real script with an fcitx5 probe in PATH; leave the desktop untouched."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
probe = Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix="monakime-launcher-test-") as directory:
    temporary = Path(directory)
    bin_dir = temporary / "bin"
    bin_dir.mkdir()
    stub = bin_dir / "fcitx5"
    stub.write_text('#!/bin/sh\nexec "$MONAKIME_LAUNCHER_TEST_PROBE" "$@"\n')
    stub.chmod(0o755)
    env = {**os.environ, "PATH": str(bin_dir) + ":" + os.environ["PATH"],
           "MONAKIME_BUILD_DIR": str(build), "MONAKIME_LAUNCHER_TEST_PROBE": str(probe),
           "MONAKIME_ZENZ": "0", "MONAKIME_LEARNING": "0",
           "XDG_CONFIG_HOME": str(temporary / "config"),
           "XDG_DATA_HOME": str(temporary / "data"),
           "XDG_CACHE_HOME": str(temporary / "cache")}
    for variable in ("FCITX_ADDON_DIRS", "FCITX_DATA_DIRS", "XDG_DATA_DIRS",
                     "SKIP_FCITX_PATH", "SKIP_FCITX_SYSTEM_PATH", "SKIP_FCITX_USER_PATH"):
        env.pop(variable, None)
    custom_dir = temporary / "custom addons"
    custom_dir.mkdir()
    (custom_dir / "monakime-launcher-test-marker").touch()
    fallback_build = temporary / "build without generated paths"
    fallback_build.mkdir()
    (fallback_build / "addon").symlink_to(build / "addon", target_is_directory=True)
    (fallback_build / "test").symlink_to(build / "test", target_is_directory=True)
    for addon_dirs, build_dir in ((None, build), (str(custom_dir), build),
                                  (None, fallback_build)):
        case = env.copy()
        case["MONAKIME_BUILD_DIR"] = str(build_dir)
        if addon_dirs is not None:
            case["FCITX_ADDON_DIRS"] = addon_dirs
            case["MONAKIME_LAUNCHER_TEST_CUSTOM_DIR"] = addon_dirs
        result = subprocess.run([str(root / "scripts/run-fcitx.sh")], cwd=temporary,
                                env=case, capture_output=True, text=True, timeout=10)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        print(result.stdout.strip())
