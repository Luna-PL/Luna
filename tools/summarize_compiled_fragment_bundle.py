#!/usr/bin/env python3
"""Read-only descriptive statistics for a matched-source compiled Fragment bundle.

Runs only the trusted checkout's CMake reader, never archived code or a probe.
Independent-process medians are the aggregation units, not pooled round samples.
"""

import argparse
import csv
import hashlib
import io
import json
from decimal import Decimal, InvalidOperation, localcontext
from pathlib import Path
import subprocess
import sys


CASES = (
    "plain", "private_erased", "static_resume", "static_discard",
    "dynamic_none", "dynamic_one", "dynamic_chain_2", "dynamic_chain_4",
    "dynamic_override_none",
)
SETUP = ("compile_encode_decode_ms", "verified_load_jit_ms", "discovery_binding_ms")
ROOT = Path(__file__).resolve().parent.parent


def read_bytes(path, limit):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"expected regular non-symlink file: {path.name}")
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if not data or len(data) > limit:
        raise ValueError(f"input size limit exceeded: {path.name}")
    return data


def digest(data):
    return hashlib.sha256(data).hexdigest()


def metadata(text):
    return dict(line[2:].split("=", 1) for line in text.splitlines() if line.startswith("# "))


def rows(text):
    return list(csv.DictReader(io.StringIO(
        "\n".join(line for line in text.splitlines() if line and not line.startswith("# ")))))


def describe(values):
    ordered = sorted(values)
    count = len(ordered)
    if not count:
        raise ValueError("cannot summarize an empty sample set")
    # Input files are capped at 64 KiB. Keep enough precision for any accepted
    # decimal spelling plus the one digit required by an even-count median.
    with localcontext() as context:
        context.prec = 65552
        median = (ordered[(count - 1) // 2] + ordered[count // 2]) / 2
    return {"count": count, "min": format(ordered[0], "f"),
            "median": format(median, "f"), "max": format(ordered[-1], "f")}


def summarize(bundle, cmake, expected_commit=None, expected_manifest=None, mode="uncontrolled"):
    # Mode is explicit caller policy, never inferred from untrusted metadata.
    if mode == "uncontrolled":
        anchor = "LUNA_COMPILED_BUNDLE"
        reader = "compiled_fragment_bundle.cmake"
        protocol = "luna.compiled-fragment-summary.v1"
        affinity_limit = "Affinity, power policy, shared machine load and process scheduling are uncontrolled."
    elif mode == "pinned":
        anchor = "LUNA_COMPILED_PINNED_BUNDLE"
        reader = "compiled_fragment_pinned_bundle.cmake"
        protocol = "luna.compiled-fragment-pinned-summary.v1"
        affinity_limit = (
            "Only the dispatch measurement thread is pinned and verified at sample boundaries; "
            "setup affinity, power policy, shared machine load and between-process scheduling remain uncontrolled. "
            "Boundary checks can affect scheduling/cache state; pinned and uncontrolled records are not interchangeable."
        )
    else:
        raise ValueError("unsupported summary mode")
    if not bundle.is_dir() or bundle.is_symlink():
        raise ValueError("bundle requires an existing non-symlink directory")
    bundle = bundle.resolve()
    manifest_bytes = read_bytes(bundle / "manifest.csv", 65536)
    manifest_hash = digest(manifest_bytes)
    command = [cmake, f"-D{anchor}_DIR={bundle}"]
    if expected_commit is not None:
        command.append(f"-D{anchor}_EXPECTED_COMMIT={expected_commit}")
    # Always anchor the reader to the bytes this reporting pass initially saw.
    # This self-derived anchor is a consistency check, NOT authentication.
    command.append(f"-D{anchor}_EXPECTED_MANIFEST_SHA256=" +
                   (expected_manifest if expected_manifest is not None else manifest_hash))
    command.extend(["-P", str(ROOT / "tests" / reader)])
    accepted = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=60, check=False)
    if accepted.returncode != 0 or accepted.stderr:
        raise ValueError("matched-source bundle reader rejected input: " +
                         (accepted.stderr or accepted.stdout)[-8192:].strip())
    if read_bytes(bundle / "manifest.csv", 65536) != manifest_bytes:
        raise ValueError("manifest changed during acceptance")
    manifest = manifest_bytes.decode("utf-8")
    declared = metadata(manifest)
    processes = []
    raw_hashes = []
    for row in rows(manifest):
        filename = row["record"]
        data = read_bytes(bundle / filename, 65536)
        if digest(data) != row["sha256"]:
            raise ValueError(f"raw bytes changed after acceptance: {filename}")
        raw_hashes.append((filename, row["sha256"]))
        record = data.decode("utf-8")
        samples = rows(record)
        facts = metadata(record)
        cases = {}
        for case in CASES:
            times = [Decimal(sample["ns_per_op"]) for sample in samples if sample["case"] == case]
            if len(times) != 9:
                raise ValueError(f"expected nine rounds per process/case: {filename}/{case}")
            cases[case] = describe(times)
        processes.append({"process": int(row["process"]), "cycle": int(row["cycle"]),
                          "block": int(row["block"]), "profile_position": int(row["profile_position"]),
                          "profile": row["profile"], "record": filename,
                          "cases_ns_per_op": cases,
                          "setup_ms": {key: facts[key] for key in SETUP}})
    profiles = {}
    for profile in ("O0", "O2", "O3"):
        group = [process for process in processes if process["profile"] == profile]
        profiles[profile] = {
            "independent_processes": len(group),
            "cases_process_medians_ns_per_op": {
                case: describe([Decimal(process["cases_ns_per_op"][case]["median"])
                                for process in group]) for case in CASES},
            "setup_single_observations_ms": {
                key: describe([Decimal(process["setup_ms"][key]) for process in group]) for key in SETUP},
        }
    # Do not emit a partial report if inputs changed during aggregation. This
    # detects byte changes but is not an adversarial filesystem snapshot.
    for filename, expected in raw_hashes + [("manifest.csv", manifest_hash),
                                          ("samples.csv", declared["combined_sha256"])]:
        limit = 4194304 if filename == "samples.csv" else 65536
        if digest(read_bytes(bundle / filename, limit)) != expected:
            raise ValueError(f"input changed during aggregation: {filename}")
    if {path.name for path in bundle.iterdir()} != {name for name, _ in raw_hashes} | {"manifest.csv", "samples.csv"}:
        raise ValueError("bundle inventory changed during aggregation")
    return {
        "protocol": protocol, "approval": "none",
        "input_manifest_sha256": manifest_hash, "input_metadata": declared,
        "verification": "matched-source-bundle-integrity-and-protocol",
        "summary_tool_sha256": digest(Path(__file__).read_bytes()),
        "aggregation": {
            "process": "median/min/max of nine round samples per case",
            "profile": "median/min/max of independent-process medians per case",
            "setup": "one observation per process per setup stage; not a setup benchmark",
            "numbers": "exact decimal strings; even-count median averages both central values",
        },
        "limitations": [
            "Self-reported provenance and checksums do not authenticate the publisher or a clean build.",
            affinity_limit,
            "No confidence intervals, timing thresholds, winner selection or performance/release approval.",
            "Different entry ABIs and timed harness costs prevent isolated Slot-instruction conclusions.",
            "Inputs must remain unchanged; byte checks are not an adversarial filesystem snapshot.",
        ],
        "processes": processes, "profiles": profiles,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", required=True, type=Path)
    parser.add_argument("--mode", choices=("uncontrolled", "pinned"), default="uncontrolled",
                        help="explicit reader/report mode; never auto-detected (default: uncontrolled)")
    parser.add_argument("--cmake", default="cmake", help="trusted CMake executable (default: PATH)")
    parser.add_argument("--expected-commit")
    parser.add_argument("--expected-manifest-sha256")
    args = parser.parse_args()
    try:
        report = summarize(args.bundle, args.cmake, args.expected_commit, args.expected_manifest_sha256, args.mode)
        serialized = json.dumps(report, sort_keys=True, indent=2, ensure_ascii=True)
    except (ValueError, KeyError, csv.Error, InvalidOperation, OSError, subprocess.SubprocessError) as error:
        print(f"compiled Fragment summary: {error}", file=sys.stderr)
        return 1
    print(serialized)
    return 0


if __name__ == "__main__":
    sys.exit(main())
