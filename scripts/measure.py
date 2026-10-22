#!/usr/bin/env python3
# Copyright (C) 2026 rirannda
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure core latency, resident memory, idle CPU ticks and optional CPU models."""
import argparse
import json
import os
from pathlib import Path
import platform
import select
import subprocess
import time


def process_stats(pid):
    # comm can contain spaces and parentheses; split after its final ')'.
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    status = {}
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith(("VmRSS:", "VmHWM:", "Threads:")):
            key, value = line.split(":", 1)
            status[key] = int(value.strip().split()[0])
    return {"cpu_ticks": int(fields[11]) + int(fields[12]), **status}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("build/monakime-cli"))
    parser.add_argument("--requests", type=int, default=2000)
    parser.add_argument("--output", type=Path, default=Path("reports/performance.json"))
    parser.add_argument("--model", action="append", type=Path, default=[])
    args = parser.parse_args()
    binary = str(args.binary.resolve())
    env = {**os.environ, "MONAKIME_LEARNING": "0", "NEWIME_LEARNING": "0"}
    benchmark = subprocess.run([binary, "--no-model", "--benchmark", str(args.requests)],
                               env=env, text=True, capture_output=True, check=True)
    report = {"platform": platform.platform(), "machine": platform.machine(),
              "core": json.loads(benchmark.stdout), "models": {}}

    def idle_stats(extra):
        child = subprocess.Popen([binary, "--stdin", "--json", *extra], env=env,
                                 text=True, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, bufsize=1)
        try:
            child.stdin.write("nihongonyuuryoku\n")
            child.stdin.flush()
            if not select.select([child.stdout], [], [], 15)[0]:
                raise RuntimeError("conversion did not complete")
            conversion = json.loads(child.stdout.readline())
            time.sleep(0.05)
            before = process_stats(child.pid)
            start = time.monotonic()
            time.sleep(1.0)
            after = process_stats(child.pid)
            elapsed = time.monotonic() - start
            cpu_s = (after["cpu_ticks"] - before["cpu_ticks"]) / os.sysconf("SC_CLK_TCK")
            return {"rss_kib_after_conversion": after["VmRSS"],
                    "peak_rss_kib": after["VmHWM"], "threads": after["Threads"],
                    "idle_window_s": elapsed, "idle_cpu_s": cpu_s,
                    "idle_cpu_percent_one_core": cpu_s / elapsed * 100,
                    "conversion": conversion}
        finally:
            child.communicate(timeout=15)

    report["core_idle"] = idle_stats(["--no-model"])
    corpus = ["kyouhatennkigaiidesu", "nihongonyuuryoku", "watashihanihongowohanashimasu",
              "toukyoudaigaku", "jinkouchinou", "hashiwowataru", "hashidegohanwotaberu",
              "kyouhagoogledekensaku", "githubdecodewokaku", "shinkansen"]
    for model in args.model:
        run = subprocess.run([binary, "--stdin", "--json", "--model", str(model.resolve())],
                             input="\n".join(corpus) + "\n", capture_output=True, text=True, env=env,
                             check=True, timeout=60)
        rows = [json.loads(line) for line in run.stdout.splitlines() if line]
        latencies = sorted(row["model_ms"] for row in rows[1:])
        report["models"][model.name] = {
            "cold_ms": rows[0]["model_ms"],
            "warm_median_ms": latencies[len(latencies) // 2],
            "cases": [{"input": raw, **row} for raw, row in zip(corpus, rows)],
            "idle": idle_stats(["--model", str(model.resolve())])}
    # RAPL reports the whole CPU package, not a process. Do not fabricate energy
    # measurements when permissions or hardware cannot provide them.
    readable = []
    for energy in Path("/sys/class/powercap").glob("*/energy_uj"):
        try:
            energy.read_text()
            readable.append(str(energy))
        except (OSError, PermissionError):
            pass
    report["energy"] = {"measured": False, "readable_package_counters": readable,
                        "reason": "No process-attributed electrical energy measurement was performed. Idle CPU and latency are resource proxies only."}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"output": str(args.output), "core": report["core"],
                      "core_idle": report["core_idle"],
                      "models": {name: {key: value for key, value in data.items() if key != "cases"}
                                 for name, data in report["models"].items()}}, ensure_ascii=False))


if __name__ == "__main__":
    main()
