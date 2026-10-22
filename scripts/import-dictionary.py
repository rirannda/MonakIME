#!/usr/bin/env python3
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile UTF-8 Mozc, SKK or TSV dictionaries to a sorted mmap dictionary.

Input licenses are not changed by this conversion. Keep the license files next
to the generated dictionary. This is an offline build tool; no Python runtime
is used by the IME.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sqlite3
import struct
import tempfile


def hiragana(text):
    return "".join(chr(ord(c) - 0x60) if "ァ" <= c <= "ヶ" else c for c in text)


def read_entries(path, kind):
    encoding = "euc_jp" if kind == "skk" and path.name.endswith(".euc") else "utf-8"
    with path.open(encoding=encoding, errors="replace") as source:
        for line in source:
            if len(line) > 32768 or line.startswith(("#", ";")):
                continue
            if kind == "skk":
                pair = line.strip().split(" ", 1)
                if len(pair) != 2 or not pair[1].startswith("/"):
                    continue
                reading = pair[0]
                # SKK okuri markers require conjugation and are not plain words.
                if reading and reading[-1].isascii() and reading[-1].isalpha():
                    continue
                for rank, value in enumerate(pair[1].split("/")[1:17]):
                    surface = value.split(";", 1)[0]
                    if surface:
                        yield reading, surface, 2.0 + rank * 0.05
            else:
                fields = line.rstrip("\n\r").split("\t")
                try:
                    if kind == "mozc":
                        if len(fields) < 5:
                            continue
                        cost = float(fields[3]) / 1000
                        if math.isfinite(cost):
                            yield fields[0], fields[4], max(1.0, min(15.0, cost))
                    elif len(fields) >= 2:
                        yield fields[0], fields[1], float(fields[2]) if len(fields) >= 3 else 1.0
                except ValueError:
                    continue


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", nargs="+", type=Path)
    parser.add_argument("--format", choices=("mozc", "skk", "tsv"), default="mozc")
    parser.add_argument("--output", "-o", type=Path, required=True)
    parser.add_argument("--max-entries", type=int, default=300000)
    parser.add_argument("--candidates", type=int, default=8)
    args = parser.parse_args()
    if not 1 <= args.max_entries <= 2000000 or not 1 <= args.candidates <= 16:
        parser.error("max-entries must be 1..2000000 and candidates 1..16")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    metadata = {"format": "NIMEDIC1", "sources": [], "max_entries": args.max_entries}
    # Disk-backed staging bounds build-time memory even with multi-million word inputs.
    with tempfile.TemporaryDirectory(prefix="monakime-dictionary-") as tmp:
        db = sqlite3.connect(str(Path(tmp) / "words.sqlite"))
        db.execute("PRAGMA journal_mode=OFF")
        db.execute("PRAGMA synchronous=OFF")
        db.execute("PRAGMA cache_size=-8192")
        db.execute("CREATE TABLE words(reading TEXT, surface TEXT, cost REAL, PRIMARY KEY(reading,surface)) WITHOUT ROWID")
        for path in args.sources:
            digest = hashlib.sha256()
            with path.open("rb") as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(chunk)
            metadata["sources"].append({"file": path.name, "sha256": digest.hexdigest()})
            batch = []
            for reading, surface, cost in read_entries(path, args.format):
                reading = hiragana(reading)
                if not reading or not surface or len(reading) > 48 or len(surface) > 128:
                    continue
                if "\ufffd" in reading + surface or not math.isfinite(cost):
                    continue
                if any(c in reading + surface for c in "\0\t\r\n"):
                    continue
                batch.append((reading, surface, max(-10.0, min(100.0, cost))))
                if len(batch) == 8192:
                    db.executemany("INSERT INTO words VALUES(?,?,?) ON CONFLICT(reading,surface) DO UPDATE SET cost=min(cost,excluded.cost)", batch)
                    batch.clear()
            db.executemany("INSERT INTO words VALUES(?,?,?) ON CONFLICT(reading,surface) DO UPDATE SET cost=min(cost,excluded.cost)", batch)
            db.commit()
        query = """SELECT reading,surface,cost FROM
          (SELECT reading,surface,cost,ROW_NUMBER() OVER (PARTITION BY reading ORDER BY cost,surface) AS rank
             FROM words)
          WHERE rank<=? ORDER BY cost,reading,surface LIMIT ?"""
        selected = list(db.execute(query, (args.candidates, args.max_entries)))
        selected.sort(key=lambda row: (row[0].encode("utf-8"), row[2], row[1].encode("utf-8")))
        records = bytearray()
        blob = bytearray()
        previous_reading, previous_offset = None, 0
        for reading, surface, cost in selected:
            if reading != previous_reading:
                previous_offset = len(blob)
                blob.extend(reading.encode("utf-8") + b"\0")
                previous_reading = reading
            surface_offset = len(blob)
            blob.extend(surface.encode("utf-8") + b"\0")
            records.extend(struct.pack("<IIf", previous_offset, surface_offset, cost))
        with tempfile.NamedTemporaryFile(dir=args.output.parent, prefix=args.output.name, delete=False) as output:
            partial = Path(output.name)
            output.write(struct.pack("<8sII", b"NIMEDIC1", len(selected), len(blob)))
            output.write(records)
            output.write(blob)
        os.replace(partial, args.output)
        args.output.chmod(0o644)
        metadata.update(entries=len(selected), bytes=args.output.stat().st_size,
                        sha256=hashlib.sha256(args.output.read_bytes()).hexdigest())
        args.output.with_name(args.output.name + ".sources.json").write_text(
            json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"output": str(args.output), "entries": len(selected), "bytes": metadata["bytes"]}))


if __name__ == "__main__":
    main()
