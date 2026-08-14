#!/usr/bin/env python3
"""Process-level resource probe for benchmark attribution.

Runs a command repeatedly and reports wall time plus getrusage(RUSAGE_CHILDREN)
deltas: user/sys CPU time, max RSS, minor/major page faults and voluntary/
involuntary context switches. Output is one JSON object per run on stdout,
plus a `summary` object, so callers can parse or diff two implementations.

Usage:
    benchmark_probe.py --iterations 7 --warmups 2 --tag luna -- ./binary arg1
"""
import argparse
import json
import os
import resource
import shutil
import subprocess
import sys
import time

PERF_EVENTS = "instructions,cycles,branches,branch-misses,cache-references,cache-misses"


def perf_available():
    return shutil.which("perf") is not None


def sample_perf(cmd):
    """One perf stat pass over the command; returns raw stdout or None."""
    perf_cmd = ["perf", "stat", "-e", PERF_EVENTS, "--"]
    perf_cmd.extend(cmd)
    try:
        proc = subprocess.run(perf_cmd, stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE, text=True)
    except OSError:
        return None
    if proc.returncode != 0:
        return None
    return proc.stderr


def parse_perf(stderr_text):
    """Extract event values from `perf stat` stderr lines like
    '1,234,567,890  instructions  # 2.34  insn per cycle'."""
    counts = {}
    for line in stderr_text.splitlines():
        parts = line.strip().split()
        if len(parts) < 2:
            continue
        value = parts[0].replace(",", "")
        if not value.isdigit():
            continue
        for event in ("instructions", "cycles", "branches", "branch-misses",
                      "cache-references", "cache-misses"):
            if parts[1] == event:
                counts[event] = int(value)
    return counts


def rusage_delta(before, after):
    return {
        "user_ms": (after.ru_utime - before.ru_utime) * 1000.0,
        "sys_ms": (after.ru_stime - before.ru_stime) * 1000.0,
        "max_rss_kib": after.ru_maxrss,
        "minflt": after.ru_minflt - before.ru_minflt,
        "majflt": after.ru_majflt - before.ru_majflt,
        "nvcsw": after.ru_nvcsw - before.ru_nvcsw,
        "nivcsw": after.ru_nivcsw - before.ru_nivcsw,
    }


def run_once(cmd):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter()
    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    wall_ms = (time.perf_counter() - start) * 1000.0
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if proc.returncode != 0:
        return None
    sample = rusage_delta(before, after)
    sample["wall_ms"] = wall_ms
    return sample


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--tag", default="run")
    parser.add_argument("--perf", action="store_true",
                        help="also run one perf stat pass if perf is installed")
    parser.add_argument("cmd", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.cmd:
        parser.error("missing command")
    if args.cmd[0] == "--":
        args.cmd = args.cmd[1:]

    runs = []
    for _ in range(args.warmups):
        run_once(args.cmd)
    for _ in range(args.iterations):
        sample = run_once(args.cmd)
        if sample is None:
            print(json.dumps({"tag": args.tag, "error": "nonzero exit"}))
            sys.exit(1)
        runs.append(sample)

    keys = ["wall_ms", "user_ms", "sys_ms", "max_rss_kib",
            "minflt", "majflt", "nvcsw", "nivcsw"]
    summary = {"tag": args.tag, "samples": args.iterations}
    for key in keys:
        values = sorted(sample[key] for sample in runs)
        summary[key + "_median"] = values[len(values) // 2]
        summary[key + "_min"] = values[0]
        summary[key + "_max"] = values[-1]

    for sample in runs:
        print(json.dumps({"tag": args.tag, **sample}))
    print(json.dumps(summary))

    if args.perf:
        perf_report = {"tag": args.tag, "perf": None}
        if perf_available():
            stderr_text = sample_perf(args.cmd)
            if stderr_text is not None:
                perf_report["perf"] = parse_perf(stderr_text)
                if not perf_report["perf"]:
                    print(json.dumps({"tag": args.tag, "perf": "no events counted"}))
                    return 0
        else:
            perf_report["perf"] = "perf not installed"
        print(json.dumps(perf_report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
