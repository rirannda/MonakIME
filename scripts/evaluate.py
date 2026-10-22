#!/usr/bin/env python3
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare dictionary and CPU Zenz conversion on explicit functional examples."""
import argparse
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--binary", type=Path, default=root / "build/monakime-cli")
parser.add_argument("--model", type=Path, default=root / "models/zenz-v3.2-xsmall-Q5_K_M.gguf")
parser.add_argument("--cases", type=Path, default=root / "tests/conversion_cases.tsv")
parser.add_argument("--output", type=Path, default=root / "reports/conversion.json")
args = parser.parse_args()
cases = [line.split("\t", 1) for line in args.cases.read_text().splitlines()
         if line and not line.startswith("#")]
env = {**os.environ, "MONAKIME_LEARNING": "0", "NEWIME_LEARNING": "0"}


def run(options):
    result = subprocess.run([str(args.binary.resolve()), "--stdin", "--json", *options],
                            input="\n".join(raw for raw, _ in cases) + "\n", env=env,
                            check=True, capture_output=True, text=True, timeout=60)
    return [json.loads(line) for line in result.stdout.splitlines()]


baseline = run(["--no-model"])
neural = run(["--model", str(args.model.resolve())])
rows = []
for (raw, expected), core, zenz in zip(cases, baseline, neural, strict=True):
    rows.append({"input": raw, "expected": expected, "dictionary": core["candidates"][0],
                 "zenz": zenz["candidates"][0], "neural_accepted": zenz["neural"],
                 "reading_verified": zenz["reading_verified"],
                 "model_error": zenz["model_error"]})
report = {"scope": f"{len(cases)} handwritten functional cases; not general accuracy or a Meltype comparison",
          "model": args.model.name, "total": len(rows),
          "dictionary_matches": sum(row["dictionary"] == row["expected"] for row in rows),
          "zenz_matches": sum(row["zenz"] == row["expected"] for row in rows), "cases": rows}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
print(json.dumps({key: value for key, value in report.items() if key != "cases"}, ensure_ascii=False))
