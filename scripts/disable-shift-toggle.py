#!/usr/bin/env python3
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
"""Disable fcitx5's modifier-only Shift toggle without changing Shift combinations.

This edits the user's fcitx5 configuration once before launch. It performs no
input-event filtering and does not interrupt a running fcitx5 process.
"""
import argparse
import difflib
import os
from pathlib import Path
import re
import stat
import tempfile


SECTION = re.compile(r"^\s*\[([^\]]+)\]\s*(?:[#;].*)?$")
BINDING = re.compile(r"^(\s*)\d+(\s*=\s*)(.*?)\s*$")
SHIFT_ONLY = {"Shift_L", "Shift_R", "Shift+Shift_L", "Shift+Shift_R"}
ALT_TRIGGER = "Hotkey/AltTriggerKeys"


def disable_shift(text: str) -> str:
    """An explicit empty section overrides fcitx5's default Shift_L binding."""
    lines = text.splitlines(keepends=True)
    newline = "\r\n" if "\r\n" in text else "\n"
    section = ""
    has_section = False
    remove = set()
    bindings = []
    for index, line in enumerate(lines):
        match = SECTION.match(line.rstrip("\r\n"))
        if match:
            section = match[1]
            has_section |= section == ALT_TRIGGER
        elif section == ALT_TRIGGER:
            match = BINDING.match(line.rstrip("\r\n"))
            if match:
                if match[3].strip() in SHIFT_ONLY:
                    remove.add(index)
                else:
                    bindings.append((index, match))
    if not has_section:
        separator = "" if not text or text.endswith(("\n", "\r")) else newline
        return text + separator + newline + f"[{ALT_TRIGGER}]" + newline
    if not remove:
        return text
    # fcitx list entries are numbered from zero; avoid leaving a hole when
    # deleting a modifier binding before another configured shortcut.
    for number, (index, match) in enumerate(bindings):
        ending = newline if lines[index].endswith(("\n", "\r")) else ""
        lines[index] = f"{match[1]}{number}{match[2]}{match[3]}{ending}"
    return "".join(line for index, line in enumerate(lines) if index not in remove)


def update(path: Path, dry_run: bool = False) -> bool:
    # Follow an existing symlink so a user's configuration layout is preserved.
    path = path.expanduser().resolve()
    original = path.read_bytes() if path.exists() else b""
    text = original.decode("utf-8")
    replacement = disable_shift(text)
    if replacement == text:
        return False
    if dry_run:
        print("".join(difflib.unified_diff(text.splitlines(keepends=True),
                                         replacement.splitlines(keepends=True),
                                         fromfile=str(path), tofile=str(path))), end="")
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    mode = stat.S_IMODE(path.stat().st_mode) if path.exists() else 0o600
    backup = path.with_name(path.name + ".monakime-before-shift")
    if path.exists():
        try:
            descriptor = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            pass
        else:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(original)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".monakime-shift-",
                                         delete=False) as stream:
            temporary = Path(stream.name)
            os.fchmod(stream.fileno(), mode)
            stream.write(replacement.encode("utf-8"))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f"MonakIME: disabled fcitx5's Shift-only toggle in {path}")
    return True


def main() -> None:
    config_home = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=config_home / "fcitx5/config")
    parser.add_argument("--dry-run", action="store_true", help="show the change without writing")
    args = parser.parse_args()
    update(args.config, args.dry_run)


if __name__ == "__main__":
    main()
