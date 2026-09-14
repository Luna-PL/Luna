#!/usr/bin/env python3
"""Process-level resource probe for benchmark attribution.

Runs a command repeatedly and reports wall time plus getrusage(RUSAGE_CHILDREN)
deltas: user/sys CPU time, max RSS, minor/major page faults and voluntary/
involuntary context switches. Output is one JSON object per run on stdout,
plus a `summary` object, so callers can parse or diff two implementations.

Usage:
    benchmark_probe.py --iterations 7 --warmups 2 --tag luna -- ./binary arg1
    benchmark_probe.py --iterations 7 --warmups 2 --tag luna \
        --compare-tag cpp -- ./luna-binary --vs ./cpp-binary
"""
import argparse
import ctypes
import ctypes.wintypes
import json
import os
import shutil
import subprocess
import sys
import time

try:
    import resource
except ImportError:
    resource = None

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
        "cpu_cycles": 0,
    }


class WindowsProcessMemoryCounters(ctypes.Structure):
    _fields_ = [
        ("cb", ctypes.wintypes.DWORD),
        ("PageFaultCount", ctypes.wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


def windows_filetime_ms(value):
    ticks = (value.dwHighDateTime << 32) | value.dwLowDateTime
    return ticks / 10000.0


def windows_process_usage(proc):
    creation = ctypes.wintypes.FILETIME()
    exit_time = ctypes.wintypes.FILETIME()
    kernel = ctypes.wintypes.FILETIME()
    user = ctypes.wintypes.FILETIME()
    counters = WindowsProcessMemoryCounters()
    counters.cb = ctypes.sizeof(counters)
    handle = ctypes.wintypes.HANDLE(proc._handle)
    if not ctypes.windll.kernel32.GetProcessTimes(
            handle, ctypes.byref(creation), ctypes.byref(exit_time),
            ctypes.byref(kernel), ctypes.byref(user)):
        return None
    if not ctypes.windll.psapi.GetProcessMemoryInfo(
            handle, ctypes.byref(counters), counters.cb):
        return None
    cycles = ctypes.c_ulonglong()
    if not ctypes.windll.kernel32.QueryProcessCycleTime(
            handle, ctypes.byref(cycles)):
        cycles.value = 0
    return {
        "user_ms": windows_filetime_ms(user),
        "sys_ms": windows_filetime_ms(kernel),
        "max_rss_kib": counters.PeakWorkingSetSize / 1024.0,
        # Windows exposes one aggregate process fault count rather than the
        # POSIX minor/major split. Keep it in minflt so reports remain
        # comparable in shape; majflt and context-switch fields are unknown.
        "minflt": counters.PageFaultCount,
        "majflt": 0,
        "nvcsw": 0,
        "nivcsw": 0,
        "cpu_cycles": cycles.value,
    }


def run_once(cmd):
    before = (resource.getrusage(resource.RUSAGE_CHILDREN)
              if resource is not None else None)
    start = time.perf_counter()
    if os.name == "nt":
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        return_code = proc.wait()
        sample = windows_process_usage(proc)
    else:
        completed = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
        return_code = completed.returncode
        sample = None
    wall_ms = (time.perf_counter() - start) * 1000.0
    if return_code != 0:
        return None
    if sample is None:
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        sample = rusage_delta(before, after)
    sample["wall_ms"] = wall_ms
    return sample


def summarize(tag, runs):
    keys = ["wall_ms", "user_ms", "sys_ms", "max_rss_kib",
            "minflt", "majflt", "nvcsw", "nivcsw", "cpu_cycles"]
    summary = {"tag": tag, "samples": len(runs)}
    for key in keys:
        values = sorted(sample[key] for sample in runs)
        count = len(values)
        p95_index = max(0, (95 * count + 99) // 100 - 1)
        summary[key + "_mean"] = sum(values) / count
        summary[key + "_median"] = values[count // 2]
        summary[key + "_p95"] = values[p95_index]
        summary[key + "_min"] = values[0]
        summary[key + "_max"] = values[-1]
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iterations", type=int, default=7)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--tag", default="run")
    parser.add_argument("--compare-tag", default="compare",
                        help="tag for the command following a --vs delimiter")
    parser.add_argument("--perf", action="store_true",
                        help="also run one perf stat pass if perf is installed")
    parser.add_argument("cmd", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.cmd:
        parser.error("missing command")
    if args.cmd[0] == "--":
        args.cmd = args.cmd[1:]
    if not args.cmd:
        parser.error("missing command")

    compare_cmd = None
    if "--vs" in args.cmd:
        delimiter = args.cmd.index("--vs")
        compare_cmd = args.cmd[delimiter + 1:]
        args.cmd = args.cmd[:delimiter]
        if not args.cmd or not compare_cmd:
            parser.error("--vs requires a command on each side")

    runs = []
    compare_runs = []

    def run_pair(run_index, record):
        commands = [(args.tag, args.cmd, runs)]
        if compare_cmd is not None:
            commands.append((args.compare_tag, compare_cmd, compare_runs))
        if run_index % 2:
            commands.reverse()
        for tag, command, destination in commands:
            sample = run_once(command)
            if sample is None:
                print(json.dumps({"tag": tag, "error": "nonzero exit"}))
                return False
            if record:
                destination.append(sample)
        return True

    for run_index in range(args.warmups):
        if not run_pair(run_index, False):
            return 1
    for run_index in range(args.iterations):
        if not run_pair(run_index, True):
            return 1

    for sample in runs:
        print(json.dumps({"tag": args.tag, **sample}))
    print(json.dumps(summarize(args.tag, runs)))
    if compare_cmd is not None:
        for sample in compare_runs:
            print(json.dumps({"tag": args.compare_tag, **sample}))
        print(json.dumps(summarize(args.compare_tag, compare_runs)))

    if args.perf:
        perf_commands = [(args.tag, args.cmd)]
        if compare_cmd is not None:
            perf_commands.append((args.compare_tag, compare_cmd))
        for tag, command in perf_commands:
            perf_report = {"tag": tag, "perf": None}
            if perf_available():
                stderr_text = sample_perf(command)
                if stderr_text is not None:
                    perf_report["perf"] = parse_perf(stderr_text)
                    if not perf_report["perf"]:
                        perf_report["perf"] = "no events counted"
            else:
                perf_report["perf"] = "perf not installed"
            print(json.dumps(perf_report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
