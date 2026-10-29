#!/usr/bin/env python3
"""Compare two immutable benchmark bundles on one machine; never gate on speed."""

import argparse
import ctypes
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import signal
import statistics
import subprocess
import sys
import tarfile
import tempfile
import time

from benchmark_bundle import canonical, file_hash


def public_text(text):
    roots = [(os.environ.get("GITHUB_WORKSPACE"), "<checkout>"),
             (str(Path.cwd()), "<working-directory>"),
             (str(Path.home()), "<user-directory>"),
             (tempfile.gettempdir(), "<temporary-directory>")]
    for path, replacement in sorted(roots, key=lambda item: len(item[0] or ""), reverse=True):
        if path:
            text = text.replace(path, replacement).replace(path.replace("\\", "/"), replacement)
    return text


def windows_peak_rss(process):
    # CPython keeps the native process handle alive after wait(), allowing the
    # kernel's lifetime peak to be read without sampling or summing children.
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
            (name, ctypes.c_size_t) for name in (
                "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]

    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    query = ctypes.WinDLL("psapi", use_last_error=True).GetProcessMemoryInfo
    query.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
    query.restype = wintypes.BOOL
    if not query(wintypes.HANDLE(int(process._handle)), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError(ctypes.get_last_error())
    if not counters.PeakWorkingSetSize:
        raise RuntimeError("Windows did not provide a native peak working set")
    return counters.PeakWorkingSetSize


def measured_process(command, directory, environment, log, timeout):
    started = time.perf_counter()
    with log.open("wb") as output:
        with subprocess.Popen(command, cwd=directory, env=environment,
                              stdout=output, stderr=subprocess.STDOUT,
                              start_new_session=(os.name != "nt")) as process:
            if os.name == "nt":
                try:
                    code = process.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    raise RuntimeError(f"Benchmark exceeded {timeout} seconds")
                rss = windows_peak_rss(process)
            else:
                # wait4 returns native usage attributed to this child. The OS
                # may include waited-descendant high-water contributions;
                # this is not simultaneous process-tree resident memory.
                deadline = started + timeout
                while True:
                    pid, status, usage = os.wait4(process.pid, os.WNOHANG)
                    if pid:
                        process.returncode = os.waitstatus_to_exitcode(status)
                        break
                    if time.perf_counter() >= deadline:
                        os.killpg(process.pid, signal.SIGKILL)
                        _, status, _ = os.wait4(process.pid, 0)
                        process.returncode = os.waitstatus_to_exitcode(status)
                        raise RuntimeError(f"Benchmark exceeded {timeout} seconds")
                    time.sleep(0.01)
                code = process.returncode
                rss = usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024)
    return {"exit_code": code, "wall_ms": (time.perf_counter() - started) * 1000,
            "peak_rss_bytes": int(rss)}


def read_bundle(archive, destination, expected):
    # CI uses Python 3.12+: the data filter rejects traversal/outside symlinks,
    # while preserving the relative symlinks required by macOS frameworks.
    with tarfile.open(archive, "r:gz") as stream:
        stream.extractall(destination, filter="data")
    root = destination / "bundle"
    metadata = json.loads((root / "benchmark.json").read_text(encoding="utf-8"))
    if metadata.get("schema") != 1:
        raise ValueError("Unsupported benchmark bundle schema")
    for key in ("revision", "run_id"):
        if metadata.get(key) != expected[key]:
            raise ValueError(f"Bundle {key} does not match the pinned workflow run")
    suite = metadata["suite"]
    if suite.get("schema") != 1 or metadata["suite_sha256"] != hashlib.sha256(canonical(suite)).hexdigest():
        raise ValueError("Invalid benchmark suite identity")
    executable = (root / metadata["executable"]).resolve(strict=True)
    if not executable.is_relative_to(root.resolve()) or file_hash(executable) != metadata["executable_sha256"]:
        raise ValueError("Benchmark executable identity mismatch")
    metadata["artifact"] = expected
    metadata["archive_sha256"] = file_hash(archive)
    return root, executable, metadata


def environment_for(executable, platform_name):
    environment = os.environ.copy()
    # The fixture creates its own fresh stuff/preferences directory. Never
    # record environment dumps, account names or host names in reports.
    environment["QT_QPA_PLATFORM"] = {"windows": "windows", "macos": "cocoa"}[platform_name]
    if platform_name == "windows":
        environment["PATH"] = str(executable.parent) + os.pathsep + environment.get("PATH", "")
        environment["QT_PLUGIN_PATH"] = str(executable.parent)
        environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(executable.parent / "platforms")
    else:
        contents = executable.parent.parent
        environment["DYLD_LIBRARY_PATH"] = str(executable.parent) + ":" + str(contents / "Frameworks")
        environment["DYLD_FRAMEWORK_PATH"] = str(contents / "Frameworks")
        environment["QT_PLUGIN_PATH"] = str(contents / "PlugIns")
    return environment


def phases_from_log(log, suite):
    content = log.read_text(encoding="utf-8", errors="replace")
    if "scenebench_test done" not in content or "ignored argument " in content:
        raise ValueError("Benchmark did not finish or does not support the requested fixture")
    if re.search(r"^NOTICE: .*(?:failed:|check failed:|differs:)", content, re.MULTILINE):
        raise ValueError("Benchmark reported a correctness/phase failure")
    phases = {}
    pattern = r"^NOTICE: (.+) ([0-9]+(?:\.[0-9]+)?) ms \(([0-9]+) ([^)]+)\)$"
    for match in re.finditer(pattern, content, re.MULTILINE):
        name, milliseconds, count, unit = match.groups()
        if name in phases:
            raise ValueError(f"Duplicate benchmark phase: {name}")
        phases[name] = {"ms": float(milliseconds), "count": int(count), "unit": unit}
    missing = set(suite["required_phases"]) - phases.keys()
    if missing:
        raise ValueError("Missing required phases: " + ", ".join(sorted(missing)))
    for name, unit in suite["required_phases"].items():
        if phases[name]["unit"] != unit:
            raise ValueError(f"Unexpected workload units for phase: {name}")
    return phases


def distribution(values):
    ordered = sorted(values)
    return {"samples": values, "median": statistics.median(values),
            "p95": ordered[math.ceil(0.95 * len(ordered)) - 1],
            "min": ordered[0], "max": ordered[-1]}


def comparison(samples, cases, phases):
    result = {}
    for case in cases:
        sides = {side: [sample for sample in samples if not sample["warmup"] and
                       sample["case"] == case["id"] and sample["side"] == side]
                 for side in ("baseline", "candidate")}
        metrics = {"process_wall_ms": lambda sample: sample["process"]["wall_ms"],
                   "process_peak_rss_bytes": lambda sample: sample["process"]["peak_rss_bytes"]}
        metrics.update({"phase/" + phase + "/ms":
                        (lambda sample, name=phase: sample["phases"][name]["ms"])
                        for phase in phases})
        result[case["id"]] = {}
        for name, value in metrics.items():
            baseline = distribution([value(sample) for sample in sides["baseline"]])
            candidate = distribution([value(sample) for sample in sides["candidate"]])
            deltas = [value(right) - value(left) for left, right in
                      zip(sides["baseline"], sides["candidate"])]
            result[case["id"]][name] = {
                "baseline": baseline, "candidate": candidate,
                "paired_candidate_minus_baseline": distribution(deltas),
                "median_change_percent": (100 * (candidate["median"] / baseline["median"] - 1)
                                          if baseline["median"] else None),
            }
    return result


def run(args, report):
    provenance = json.loads(args.provenance.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="paired-benchmark-") as storage:
        workspace = Path(storage)
        bundles = {}
        for side in ("baseline", "candidate"):
            bundles[side] = read_bundle(getattr(args, side), workspace / side, provenance[side])
        left, right = (bundles[side][2] for side in ("baseline", "candidate"))
        for key in ("platform", "suite", "suite_sha256", "build"):
            if left[key] != right[key]:
                raise ValueError(f"Baseline/candidate {key} differ; comparisons require identical workloads/build settings")
        expected_system = {"windows": "Windows", "macos": "Darwin"}[left["platform"]]
        if platform.system() != expected_system or platform.machine() != left["build"]["architecture"]:
            raise ValueError("Benchmark bundle does not match this runner's OS/architecture")
        suite = left["suite"]
        if len({case["id"] for case in suite["cases"]}) != len(suite["cases"]):
            raise ValueError("Benchmark case IDs must be unique")
        report["bundles"] = {side: bundle[2] for side, bundle in bundles.items()}
        report["samples"] = []
        logs = args.output / "logs"
        logs.mkdir()
        for case in suite["cases"]:
            if not re.fullmatch(r"[a-z0-9-]+", case["id"]):
                raise ValueError("Invalid benchmark case ID")
            arguments = case["arguments"]
            if "--repeat" not in arguments or arguments[arguments.index("--repeat") + 1] != "1":
                raise ValueError("Each invocation must measure exactly one internal repetition")
            for iteration in range(args.warmups + args.samples):
                order = ("baseline", "candidate") if iteration % 2 == 0 else ("candidate", "baseline")
                for side in order:
                    _, executable, metadata = bundles[side]
                    label = f"{case['id']}-{iteration:03d}-{side}"
                    profile = workspace / "profiles" / label
                    profile.mkdir(parents=True)
                    log = logs / (label + ".log")
                    sample = {"case": case["id"], "iteration": iteration,
                              "side": side, "order": list(order), "warmup": iteration < args.warmups,
                              "arguments": case["arguments"], "log": log.relative_to(args.output).as_posix()}
                    report["samples"].append(sample)
                    sample["process"] = measured_process(
                        [str(executable)] + case["arguments"], profile,
                        environment_for(executable, metadata["platform"]), log, args.timeout)
                    # Preserve diagnostic content, normalizing private path
                    # prefixes before it becomes a public workflow artifact.
                    log.write_text(public_text(log.read_text(encoding="utf-8", errors="replace")), encoding="utf-8")
                    if sample["process"]["exit_code"]:
                        raise RuntimeError(f"{label} failed; see {sample['log']}")
                    sample["phases"] = phases_from_log(log, suite)
                    if "--trailing-columns" in arguments:
                        trailing = int(arguments[arguments.index("--trailing-columns") + 1])
                        for phase in ("save sparse plain", "save sparse compressed"):
                            if sample["phases"][phase]["count"] != trailing:
                                raise ValueError("Sparse workload size does not match its requested parameters")
                    print(f"{label}: validated")
        report["comparisons"] = comparison(report["samples"], suite["cases"], suite["required_phases"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for side in ("baseline", "candidate"):
        parser.add_argument("--" + side, type=Path, required=True)
    parser.add_argument("--provenance", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=9)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    if not 3 <= args.samples <= 100 or not 1 <= args.warmups <= 10 or args.timeout <= 0:
        parser.error("Use 3..100 samples, 1..10 warmups and a positive timeout")
    args.output.mkdir(parents=True, exist_ok=False)
    report = {
        "schema": 1, "status": "failed", "started_utc": datetime.now(timezone.utc).isoformat(),
        "harness_sha256": file_hash(Path(__file__)),
        "host": {"system": platform.system(), "release": platform.release(),
                 "architecture": platform.machine(), "cpu": platform.processor(),
                 "logical_cpus": os.cpu_count(), "python": platform.python_version(),
                 "image": {key: os.environ.get(key) for key in ("ImageOS", "ImageVersion")}},
        "configuration": {"samples_per_side": args.samples, "warmup_pairs": args.warmups,
                          "timeout_seconds": args.timeout, "order": "alternating AB/BA",
                          "p95": "nearest rank across process repetitions, not UI event latency",
                          "rss": ("GetProcessMemoryInfo.PeakWorkingSetSize: benchmark process only"
                                  if os.name == "nt" else
                                  "wait4.ru_maxrss: OS usage attributed to reaped child; may include waited descendants; not simultaneous process-tree RSS"),
                          "cache": "warm OS caches; fresh process and preferences each invocation",
                          "speed_gate": False},
    }
    try:
        run(args, report)
        report["status"] = "success"
    except (OSError, ValueError, KeyError, IndexError, RuntimeError, subprocess.SubprocessError, tarfile.TarError) as error:
        report["error"] = public_text(str(error))
        print(f"Benchmark failed: {report['error']}", file=sys.stderr)
    finally:
        for log in (args.output / "logs").glob("*.log"):
            log.write_text(public_text(log.read_text(encoding="utf-8", errors="replace")), encoding="utf-8")
        (args.output / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return int(report["status"] != "success")


if __name__ == "__main__":
    sys.exit(main())
