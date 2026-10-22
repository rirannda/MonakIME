#!/usr/bin/env python3
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure the native CLI + persistent Mozc helper; Zenz stays disabled."""
import argparse
import json
import os
from pathlib import Path
import selectors
import statistics
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=root / 'build/monakime-cli')
parser.add_argument('--output', type=Path, default=root / 'reports/live-mozc.json')
args = parser.parse_args()
corpus = ["nihongo", "kyouhatennkigaiidesu", "hashidegohanwotaberu",
          "Googledeshirabete,GitHubnoko-dowosyuuseishimashita.",
          "CPUsiyourituha12.5%,memorishiyouryouha256MBdesita."]

def usage(pid):
    status = Path(f"/proc/{pid}/status").read_text()
    rss = int(next(line.split()[1] for line in status.splitlines() if line.startswith('VmRSS:')))
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(')', 1)[1].split()
    return {"rss_kib": rss, "cpu_ticks": int(fields[11]) + int(fields[12])}

with tempfile.TemporaryDirectory(prefix="monakime-live-measure-") as directory:
    env = {**os.environ, "MONAKIME_LEARNING": "0", "NEWIME_LEARNING": "0", "MONAKIME_MOZC_PROFILE": directory, "NEWIME_MOZC_PROFILE": directory,
           "MONAKIME_MOZC_HELPER": str(root / "third_party/mozc/meltype_mozc_helper")}
    p = subprocess.Popen([str(args.binary.resolve()), "--live", "--stdin", "--json"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, bufsize=1, env=env)
    selector = selectors.DefaultSelector()
    selector.register(p.stdout, selectors.EVENT_READ)
    def request(raw):
        start = time.perf_counter()
        p.stdin.write(raw + '\n')
        p.stdin.flush()
        if not selector.select(5):
            raise RuntimeError("live request timed out")
        result = json.loads(p.stdout.readline())
        assert result['mozc'] and not result['neural'], result
        return (time.perf_counter() - start) * 1000, result
    try:
        cold_ms, _ = request(corpus[0])
        children = Path(f"/proc/{p.pid}/task/{p.pid}/children").read_text().split()
        assert len(children) == 1, children
        helper = int(children[0])
        before_cpu = sum(usage(pid)['cpu_ticks'] for pid in (p.pid, helper))
        samples = [request(corpus[i % len(corpus)])[0] for i in range(100)]
        after_cpu = sum(usage(pid)['cpu_ticks'] for pid in (p.pid, helper))
        parent_usage, helper_usage = usage(p.pid), usage(helper)
        before_idle = sum(usage(pid)['cpu_ticks'] for pid in (p.pid, helper))
        time.sleep(1)
        after_idle = sum(usage(pid)['cpu_ticks'] for pid in (p.pid, helper))
        examples = [{"raw": raw, "reading": result['reading'], "preview": result['candidates'][0]}
                    for raw in corpus for _, result in [request(raw)]]
        report = {"scope": "CLI plus real Mozc child; excludes fcitx5 UI; warm OS file cache; no Zenz",
                  "requests": len(samples), "cold_request_ms": cold_ms,
                  "warm_p50_ms": statistics.median(samples),
                  "warm_p95_ms": sorted(samples)[94],
                  "conversion_cpu_s": (after_cpu - before_cpu) / os.sysconf('SC_CLK_TCK'),
                  "cli_rss_kib": parent_usage['rss_kib'], "mozc_rss_kib": helper_usage['rss_kib'],
                  "total_rss_kib": parent_usage['rss_kib'] + helper_usage['rss_kib'],
                  "idle_seconds": 1, "idle_cpu_ticks": after_idle - before_idle,
                  "clock_ticks_per_second": os.sysconf('SC_CLK_TCK'),
                  "power_watts": None, "examples": examples}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
        print(json.dumps({k: v for k, v in report.items() if k != 'examples'}, ensure_ascii=False, indent=2))
    finally:
        selector.close()
        p.stdin.close()
        p.wait(timeout=5)
        if p.returncode:
            raise RuntimeError(p.stderr.read())
