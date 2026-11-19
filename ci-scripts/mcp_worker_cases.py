"""Exercise operating-system transport failures using the real native worker."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path
from typing import TYPE_CHECKING, BinaryIO

from mcp_png import decode_png, write_fixture
from mcp_worker import FULL_AUTHORITY, Worker

if TYPE_CHECKING:
    from mcp_acceptance import NativeAcceptance


def _reap(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        process.kill()
    process.wait(timeout=5)
    for stream in (process.stdin, process.stdout, process.stderr):
        if stream is not None:
            stream.close()


def _launch(test: NativeAcceptance, log: BinaryIO, stdin: int) -> subprocess.Popen[bytes]:
    workspace = test.directory / "transport-workspace"
    workspace.mkdir()
    process = subprocess.Popen(
        [
            str(test.executable),
            "--automation-worker",
            "--workspace",
            str(workspace),
            "--input-root",
            str(test.inputs),
            *FULL_AUTHORITY,
        ],
        stdin=stdin,
        stdout=subprocess.PIPE,
        stderr=log,
    )
    test.addCleanup(_reap, process)
    return process


def closed_output_has_controlled_exit(test: NativeAcceptance) -> None:
    """Require an output failure to drain the worker instead of receiving SIGPIPE."""
    if sys.platform == "win32":
        test.skipTest("SIGPIPE handling is a POSIX transport contract")
    log_path = test.directory / "closed-output.log"
    with log_path.open("wb") as log:
        process = _launch(test, log, subprocess.PIPE)
        assert process.stdout is not None and process.stdin is not None
        process.stdout.close()
        process.stdin.write(b'{"id":"output","method":"automation.capabilities","params":{}}\n')
        process.stdin.close()
        test.assertEqual(process.wait(timeout=30), 2)
    test.assertIn("protocol output failed", log_path.read_text(encoding="utf-8"))


def unreadable_input_is_not_clean_eof(test: NativeAcceptance) -> None:
    """Require Linux directory input to report its read error with nonzero exit."""
    if sys.platform != "linux":
        test.skipTest("Directory stdin exercises the Linux EISDIR error path")
    log_path = test.directory / "unreadable-input.log"
    descriptor = os.open(test.inputs, os.O_RDONLY)
    try:
        with log_path.open("wb") as log:
            process = _launch(test, log, descriptor)
            process.communicate(timeout=30)
            test.assertEqual(process.returncode, 2)
    finally:
        os.close(descriptor)
    test.assertIn("protocol input failed", log_path.read_text(encoding="utf-8"))


def unicode_roots_admit_png(test: NativeAcceptance) -> None:
    """Exercise native command-line and codec paths containing non-ASCII characters."""
    test.workspace = test.directory / "workspace-\u4e2d\u6587"
    test.inputs = test.directory / "inputs-\u4e2d\u6587"
    test.workspace.mkdir()
    test.inputs.mkdir()
    test.fixture = test.inputs / "fixture-\u4e2d\u6587.png"
    write_fixture(test.fixture)
    test.worker = Worker(
        test.executable, test.workspace, test.inputs, test.directory / "unicode-worker.stderr.log"
    )
    test.addCleanup(test.worker.close)
    document, _level, _column = test.populated_scene()
    test.assertEqual(decode_png(test.preview(document)), decode_png(test.fixture.read_bytes()))
    saved = test.mutate(
        "scene.save_copy", document, directory=str(test.workspace / "package-\u4e2d\u6587")
    )
    reopened = test.mutate("scene.open", None, path=saved["package_directory"])
    test.assertEqual(
        decode_png(test.preview(reopened["document_id"])), decode_png(test.fixture.read_bytes())
    )


def unicode_names_preserve_data_and_reject_unpaired_surrogates(test: NativeAcceptance) -> None:
    """Preserve Unicode names and refuse malformed escaped UTF-16 before mutation."""
    document = test.create()
    for name in ("Latin", "\u4e2d\u6587", "\U0001f680", "e\u0301"):
        created = test.mutate("object.create", document, kind="pegbar", name=name)
        test.assertEqual(created["name"], name)
    before = test.success("scene.summary", {"document_id": document})
    for name in ("\ud800", "\udc00", "\ud800a", "a\udc00", "\udc00\ud800"):
        params = test.write_params(document, {"kind": "pegbar", "name": name})
        error = test.failure("object.create", params)
        test.assertEqual(error["code"], "invalid_params")
        test.assertEqual(test.success("scene.summary", {"document_id": document}), before)
        params["name"] = "Recovered"
        created = test.success("object.create", params)
        test.revisions[document] = created["revision"]
        test.assertEqual(created["name"], "Recovered")
        before = test.success("scene.summary", {"document_id": document})


def _reject_relative_roots(test: NativeAcceptance, launch: Path, before: set[Path]) -> None:
    """Preserve the worker CLI requirement for absolute roots."""
    for workspace, inputs in (
        ("../workspace", str(test.inputs)),
        (str(test.workspace), "../inputs"),
    ):
        rejected = subprocess.run(
            [
                str(test.executable),
                "--automation-worker",
                "--workspace",
                workspace,
                "--input-root",
                inputs,
                *FULL_AUTHORITY,
            ],
            cwd=launch,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        test.assertEqual(rejected.returncode, 2)
        test.assertEqual(rejected.stdout, "")
        test.assertIn("invalid_argument", rejected.stderr)
        test.assertEqual(set(test.workspace.glob("runtime_*")), before)


def private_bootstrap_ignores_launch_configuration(test: NativeAcceptance) -> None:
    """Keep native startup inside its runtime despite arbitrary launch directories.

    Args:
        test: A fresh real-worker acceptance fixture.
    """
    expected = test.success("automation.capabilities", {})
    for mode in ("plain", "misleading-config", "relative-executable"):
        launch = test.directory / mode
        launch.mkdir()
        synthetic_root = launch / "unused-system-root"
        if mode == "misleading-config":
            for app in (".app", "OpenToonz.app"):
                config = launch / app / "Contents" / "Resources" / "SystemVar.ini"
                config.parent.mkdir(parents=True)
                config.write_text(
                    f"TOONZROOT={synthetic_root}\nTOONZPROFILES={synthetic_root}/profiles\n"
                )
        executable = str(test.executable)
        if mode == "relative-executable":
            executable = os.path.relpath(test.executable, launch)
        requests = [
            {"id": "capabilities", "method": "automation.capabilities", "params": {}},
            {
                "id": "create",
                "method": "scene.create",
                "params": {"name": "Private bootstrap", "operation_id": "bootstrap-create"},
            },
        ]
        result = subprocess.run(
            [
                executable,
                "--automation-worker",
                "--workspace",
                str(test.workspace),
                "--input-root",
                str(test.inputs),
                *FULL_AUTHORITY,
            ],
            input="".join(json.dumps(request) + "\n" for request in requests),
            cwd=launch,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )
        (launch / "stdout.ndjson").write_text(result.stdout)
        (launch / "stderr.log").write_text(result.stderr)
        test.assertEqual(result.returncode, 0, result.stderr)
        test.assertEqual(result.stderr, "", mode)
        responses = [json.loads(line) for line in result.stdout.splitlines()]
        test.assertEqual(len(responses), 2)
        test.assertEqual(responses[0]["result"]["workspace"], str(test.workspace))
        test.assertEqual(responses[0]["result"]["tools"], expected["tools"])
        test.assertEqual(responses[1]["result"]["name"], "Private bootstrap")
        test.assertFalse(synthetic_root.exists())
    runtimes = list(test.workspace.glob("runtime_*"))
    test.assertGreaterEqual(len(runtimes), 2)
    for runtime in runtimes:
        test.assertTrue((runtime / "portablestuff").is_dir())
    _reject_relative_roots(test, launch, set(runtimes))
