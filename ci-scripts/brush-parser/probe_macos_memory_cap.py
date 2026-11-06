#!/usr/bin/env python3
"""CI-only macOS fatal-footprint capability and resident-map coverage experiment.

A successful run qualifies the experiment's observations, not the still-required
strict whole-process 2 GiB resident/commit cap. No production launcher is changed.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import signal
import subprocess
import sys
import tempfile


IDENTITY = "org.openanimationlibrary.groundzero.brush-memory-probe"
MIB = 1024 * 1024
PUBLIC_ROOTS = []


def public_text(text):
    """Keep contributor/machine paths out of the retained public diagnostics."""
    for path, label in sorted(PUBLIC_ROOTS, key=lambda item: len(item[0]), reverse=True):
        text = text.replace(path, label)
    return text


def run_command(command, log):
    timeout = False
    with log.open("wb") as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            result = process.wait(timeout=40)
        except subprocess.TimeoutExpired:
            timeout = True
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            result = process.wait()
    with log.open("rb") as output:
        data = output.read(65537)
        excessive = len(data) > 65536
        if excessive:
            output.seek(-32768, os.SEEK_END)
            data = data[:32768] + b"\n[Diagnostic exceeded 64 KiB; first/last context retained]\n" + output.read(32768)
    text = public_text(data.decode("utf-8", errors="replace"))
    # Replace the raw file as well as the printed output; artifacts must not
    # retain absolute checkout, home or temporary-directory identities.
    log.write_text(text, encoding="utf-8")
    print(text, end="", flush=True)
    if timeout:
        raise RuntimeError("Probe controller exceeded its wall deadline")
    if excessive:
        raise RuntimeError("Probe diagnostic exceeded its 64 KiB output bound")
    if result:
        raise RuntimeError(f"Probe command failed with exit {result}; see {log.name}")
    return text


def records(text, prefix):
    return [json.loads(line[len(prefix):]) for line in text.splitlines()
            if line.startswith(prefix)]


def run_case(parent, child, mode, sentinel, fixture, limit, target, output, name, report):
    text = run_command([str(parent), "--run", str(child), mode, str(sentinel),
                        str(fixture), str(limit), str(target)], output / f"{name}.log")
    outcomes = records(text, "PROBE_RESULT ")
    boundaries = records(text, "CHILD_BOUNDARY ")
    memory = records(text, "CHILD_MEMORY ")
    if len(outcomes) != 1:
        raise RuntimeError(f"{name}: missing exact controller result")
    outcome = outcomes[0]
    outcome["case"] = name
    outcome["memory"] = memory
    outcome["boundary"] = boundaries
    # Preserve pre-main startup/ordinary baseline failures in JSON, including
    # the exact kernel exit identity, before validating capability evidence.
    report["cases"].append(outcome)
    if (outcome["mode"] != mode or outcome["limit_mib"] != limit or
            outcome["target_mib"] != target or outcome["timed_out"] or
            outcome["wait_errno"] or outcome["parent_memory_watchdog"]):
        raise RuntimeError(f"{name}: result does not match the requested experiment")
    required_boundary = {"unprivileged": True, "app_sandbox": True,
                         "sentinel_denied": True, "network_denied": True}
    if boundaries != [required_boundary]:
        raise RuntimeError(f"{name}: child sandbox baseline did not initialize; "
                           f"wait status {outcome['wait_status']}, "
                           f"kernel reason {outcome['reason_namespace']}/{outcome['reason_code']}")
    return outcome


def normal_completion(case):
    return (os.WIFEXITED(case["wait_status"]) and
            os.WEXITSTATUS(case["wait_status"]) == 0)


def classify_resident_coverage(case, limit):
    if case["kernel_per_process_limit_kill"]:
        return "kernel_limit_kill"
    startup = [row for row in case["memory"] if row["mode"] == "startup"]
    final = [row for row in case["memory"] if row["mode"] == case["mode"]]
    if normal_completion(case) and len(startup) == 1 and len(final) == 1:
        growth = final[0]["resident_bytes"] - startup[0]["resident_bytes"]
        case["resident_growth_bytes"] = growth
        if growth > limit * MIB and final[0]["phys_footprint_bytes"] < limit * MIB:
            return "resident_mapping_escape"
    return "inconclusive"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm64", "x86_64"),
                        default=platform.machine())
    parser.add_argument("--limit-mib", type=int, default=128,
                        help="Minimum experimental threshold; same-architecture baseline admission may raise it")
    parser.add_argument("--output-dir", type=Path, required=True)
    arguments = parser.parse_args()
    output = arguments.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    PUBLIC_ROOTS.extend(((str(Path(__file__).resolve().parents[2]), "<repository>"),
                         (str(output), "<probe-output>"),
                         (str(Path.home()), "<home>")))
    report = {
        "experiment": "macos-spawn-fatal-footprint",
        "source_baseline": "apple-oss-distributions/xnu xnu-11215.81.4",
        "macos_version": platform.mac_ver()[0],
        "kernel_release": os.uname().release if hasattr(os, "uname") else None,
        "host_architecture": platform.machine(),
        "probe_architecture": arguments.arch,
        "requested_limit_mib": arguments.limit_mib,
        "parent_memory_watchdog": False,
        "native_fatal_footprint_supported": False,
        "strict_resident_cap_qualified": False,
        "qualification_limit": "Fatal footprint is post-charge enforcement and does not cover every resident mapping.",
        "phase": "environment",
        "cases": [],
    }
    try:
        if sys.platform != "darwin" or os.getuid() == 0 or os.geteuid() == 0:
            raise RuntimeError("Requires unprivileged macOS 15+ CI")
        if int(platform.mac_ver()[0].split(".")[0]) < 15:
            raise RuntimeError("Requires the pinned macOS 15+ runner baseline")
        if arguments.arch not in ("arm64", "x86_64"):
            raise RuntimeError("Unsupported probe architecture")
        if not 64 <= arguments.limit_mib <= 1024:
            raise RuntimeError("Probe minimum limit must be between 64 and 1024 MiB")
        report["macos_build"] = subprocess.check_output(
            ["/usr/bin/sw_vers", "-buildVersion"], timeout=10).decode().strip()
        report["phase"] = "compile-sign"
        info = output / "memory-probe-Info.plist"
        info.write_bytes(plistlib.dumps({
            "CFBundleIdentifier": IDENTITY,
            "CFBundleName": "GroundZero brush memory capability probe",
            "CFBundleInfoDictionaryVersion": "6.0",
            "CFBundleVersion": "1",
        }))
        entitlements = output / "memory-probe.entitlements.plist"
        entitlements.write_bytes(plistlib.dumps({"com.apple.security.app-sandbox": True}))
        source = Path(__file__).with_name("macos_memory_cap_probe.c").resolve(strict=True)
        parent = output / "macos-memory-probe-parent"
        child = output / "macos-memory-probe-child"
        run_command(["/usr/bin/xcrun", "--sdk", "macosx", "clang", "-std=c11",
                     "-O2", "-Wall", "-Wextra", "-Werror", "-arch", arguments.arch,
                     "-mmacosx-version-min=15.0", str(source), "-framework", "Security",
                     "-framework", "CoreFoundation", "-Wl,-sectcreate,__TEXT,__info_plist," + str(info),
                     "-o", str(parent)], output / "compile.log")
        run_command(["/usr/bin/codesign", "--force", "--sign", "-", "--identifier",
                     IDENTITY, str(parent)], output / "sign-parent.log")
        shutil.copy2(parent, child)
        run_command(["/usr/bin/codesign", "--force", "--sign", "-", "--identifier",
                     IDENTITY, "--entitlements", str(entitlements), str(child)],
                    output / "sign-child.log")
        for name, executable in (("parent", parent), ("child", child)):
            run_command(["/usr/bin/codesign", "--verify", "--strict", str(executable)],
                        output / f"verify-{name}.log")
        signed = subprocess.check_output(
            ["/usr/bin/codesign", "--display", "--entitlements", "-", str(child)],
            stderr=subprocess.DEVNULL, timeout=15)
        match = re.findall(rb"<\?xml.*?</plist>", signed, re.DOTALL)
        if len(match) != 1 or plistlib.loads(match[0]) != {"com.apple.security.app-sandbox": True}:
            raise RuntimeError("Probe child has unexpected signing entitlements")
        with tempfile.TemporaryDirectory(prefix="brush-memory-cap-") as temporary:
            PUBLIC_ROOTS.append((temporary, "<probe-private>"))
            private = Path(temporary)
            sentinel = private / "private-sentinel"
            sentinel.write_bytes(b"private original probe sentinel\n")
            fixture = private / "original-random-resident-map.bin"
            report["phase"] = "uncapped-sandbox-baseline"
            baseline = run_case(parent, child, "ordinary", sentinel, fixture,
                                0, 8, output, "ordinary-uncapped-baseline", report)
            if not normal_completion(baseline):
                raise RuntimeError("Uncapped signed sandbox baseline or unprivileged setter denial failed")
            if not baseline["spawn_memory_spi_available"]:
                raise RuntimeError("Private spawn-memory SPI absent on this OS build")
            footprints = [row["phys_footprint_bytes"] for row in baseline["memory"]]
            if len(footprints) != 2:
                raise RuntimeError("Uncapped baseline did not report startup and ordinary-allocation memory")
            baseline_mib = (max(footprints) + MIB - 1) // MIB
            # Admit a trusted same-architecture startup with 96 MiB spare and
            # round up to 64 MiB. This avoids mistaking a Rosetta startup cost
            # for absent enforcement. The capped ordinary case still must pass.
            limit = max(arguments.limit_mib, ((baseline_mib + 96 + 63) // 64) * 64)
            if limit > 1024:
                raise RuntimeError("Signed startup baseline cannot fit the bounded 1024 MiB experiment")
            report["observed_baseline_footprint_mib"] = baseline_mib
            report["admitted_limit_mib"] = limit
            report["baseline_headroom_mib"] = 96
            target = limit * 3
            report["phase"] = "capped-ordinary-control"
            ordinary = run_case(parent, child, "ordinary", sentinel, fixture,
                                limit, 8, output, "ordinary-capped", report)
            if not normal_completion(ordinary):
                raise RuntimeError("Capped ordinary allocation or unprivileged limit-relaxation denial failed")
            report["phase"] = "uncapped-allocation-control"
            control = run_case(parent, child, "malloc", sentinel, fixture,
                               0, target, output, "malloc-uncapped-control", report)
            if not normal_completion(control):
                raise RuntimeError("Uncapped control could not allocate the target")
            report["phase"] = "fatal-footprint-enforcement"
            for mode in ("malloc", "mach", "pre-reserved"):
                case = run_case(parent, child, mode, sentinel, fixture,
                                limit, target, output, mode + "-capped", report)
                if not case["kernel_per_process_limit_kill"]:
                    raise RuntimeError(mode + ": no exact kernel per-process-limit kill")
            report["native_fatal_footprint_supported"] = True
            report["phase"] = "resident-coverage"
            # Original generated data. Read-only MAP_PRIVATE and MAP_SHARED
            # mappings exercise resident pages not automatically charged to the
            # child's dirty footprint; inherited fd authority matches transport.
            block = os.urandom(MIB)
            with fixture.open("xb") as stream:
                for _ in range(target):
                    stream.write(block)
            for mode in ("private-file", "shared-file", "purgeable-volatile"):
                case = run_case(parent, child, mode, sentinel, fixture,
                                limit, target, output, mode + "-capped", report)
                case["coverage_outcome"] = classify_resident_coverage(case, limit)
                if case["coverage_outcome"] == "inconclusive":
                    raise RuntimeError(mode + ": resident-map coverage evidence inconclusive")
            if sentinel.read_bytes() != b"private original probe sentinel\n":
                raise RuntimeError("Sandbox child changed private sentinel")
        report["resident_mapping_escape_observed"] = any(
            case.get("coverage_outcome") == "resident_mapping_escape"
            for case in report["cases"])
        report["phase"] = "completed"
        report["experiment_completed"] = True
        print("Native fatal-footprint observations qualified; strict whole-process resident/commit cap remains unqualified.", flush=True)
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        report["experiment_completed"] = False
        report["error"] = public_text(str(error))
        annotation = report["error"].replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print("::error::macOS memory capability probe: " + annotation, flush=True)
        return 1
    finally:
        (output / "memory-cap-report.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
