#!/usr/bin/env python3
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deliberately broken peers for Rust's subprocess boundary tests."""
import pathlib
import sys
import time

mode = pathlib.Path(sys.argv[1]).name
print('READY', flush=True)
if mode == 'exit':
    raise SystemExit(0)
for line in sys.stdin:
    if mode == 'timeout':
        time.sleep(1)
    try:
        print('invalid' if mode == 'malformed' else 'にほんご\x1f日本語', flush=True)
    except BrokenPipeError:
        break
