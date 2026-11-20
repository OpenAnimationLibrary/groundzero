"""Break each critical guard in a throwaway worktree and prove a test notices.

Usage: python3 ci-scripts/mcp_break_checks.py [--repo DIR] [--guards FILE] [--output DIR]
           [--only ID ...] [--app APP] [--gate COMMAND] [--env NAME=VALUE ...]
           [--media-tools DIR] [--native-build COMMAND --native-app APP]
           [--check-anchors] [--list]

A person runs this; no workflow does. `mcp_break_guards.json` is a table of guards: a file of
the repository, the exact text of the guard and the text that breaks it, and the tests that
must then fail. For each guard the script

1. applies the patch in a throwaway worktree of the repository's HEAD (the checkout is never
   touched; the text to replace must occur exactly once);
2. runs the guard's tests there, after running them unpatched once to prove they pass;
3. expects at least one of them to fail, then restores the file and checks the worktree is
   clean again.

A guard whose tests all still pass has survived. Survivors, guards that could not be run and
guards whose patch no longer applies are written to the gaps file (`break-gaps.md` in the
output folder) and make the exit status 1. The output folder also gets `break-results.json`.

Test kinds:
  unit        python unittest modules of ci-scripts/tests, run in the worktree.
  acceptance  NativeAcceptance cases run in the worktree with `--app` (a built app, which must
              come from the HEAD under test), one worker at a time. `--gate` names a command
              that is put in front of each run, for a shared machine's slot gate; `--env` sets
              environment variables for the run (QT_QPA_PLATFORM, for one).
  static      a script of ci-scripts that exits non-zero when it finds a problem.

A guard with `"native": true` patches C++ and so needs a rebuilt app: it runs only when
`--native-build` (a command run in the worktree, with {worktree} replaced) and `--native-app`
(the app that command builds) are given; otherwise it is listed as not run. `--check-anchors`
applies every patch to the text of HEAD without running anything and fails on any that does
not apply exactly once.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

PYTHON = sys.executable
GUARDS = Path(__file__).with_name("mcp_break_guards.json")
SCRATCH = Path("/tmp")  # Short on purpose: the worker's socket paths have a length limit.
TIMEOUT_SECONDS = 900


@dataclass
class Outcome:
    """What happened to one guard."""

    guard: str
    status: str
    detail: str = ""
    killed_by: str = ""


def git(repo: Path, *arguments: str) -> str:
    """Run git in a repository and return its output."""
    done = subprocess.run(
        ["git", "-C", str(repo), *arguments], capture_output=True, text=True, check=True
    )
    return done.stdout


def apply_patch(text: str, guard: dict[str, Any]) -> str:
    """Return the text with the guard broken; fail unless the guard's text occurs once."""
    count = text.count(guard["find"])
    if count != 1:
        raise ValueError(
            f"{guard['id']}: the text to replace occurs {count} times in {guard['file']}; "
            "update mcp_break_guards.json"
        )
    return text.replace(guard["find"], guard["replace"], 1)


def check_anchors(repo: Path, guards: list[dict[str, Any]]) -> list[Outcome]:
    """Apply every patch to the text of HEAD without running a test."""
    outcomes = []
    for guard in guards:
        try:
            apply_patch(git(repo, "show", f"HEAD:{guard['file']}"), guard)
            outcomes.append(Outcome(guard["id"], "applies"))
        except (ValueError, subprocess.CalledProcessError) as problem:
            outcomes.append(Outcome(guard["id"], "invalid", str(problem)))
    return outcomes


def run(command: list[str], cwd: Path, log: Path) -> int:
    """Run a command with a time limit, appending its output to a log; return its status."""
    with log.open("ab") as handle:
        handle.write(f"\n$ {shlex.join(command)}\n".encode())
        try:
            done = subprocess.run(
                command, cwd=cwd, stdout=handle, stderr=subprocess.STDOUT, timeout=TIMEOUT_SECONDS
            )
        except subprocess.TimeoutExpired:
            handle.write(b"\n[timed out]\n")
            return 124
    return done.returncode


def test_commands(
    test: dict[str, Any], worktree: Path, app: Path | None, options: argparse.Namespace
) -> list[list[str]]:
    """The commands that run one group of tests in a worktree."""
    if test["kind"] == "unit":
        pattern = test["pattern"]
        discover = ["-m", "unittest", "discover", "-s", "ci-scripts/tests", "-p", pattern]
        return [[PYTHON, *discover]]
    if test["kind"] == "static":
        return [[PYTHON, f"ci-scripts/{test['script']}.py"]]
    if app is None:
        raise ValueError("an acceptance test needs --app")
    gate = shlex.split(options.gate or "")
    runner = [*gate, PYTHON, str(Path(__file__).resolve())]
    runner += ["--acceptance-worktree", str(worktree), "--app", str(app)]
    for setting in options.env:
        runner += ["--env", setting]
    if options.media_tools:
        runner += ["--media-tools", str(options.media_tools)]
    return [[*runner, *test["names"]]]


def run_tests(
    guard: dict[str, Any],
    worktree: Path,
    app: Path | None,
    options: argparse.Namespace,
    log: Path,
) -> tuple[str, str]:
    """Run a guard's tests; return ("failed", the test that failed) or ("passed", "")."""
    for test in guard["tests"]:
        for command in test_commands(test, worktree, app, options):
            if run(command, worktree, log) != 0:
                return "failed", test.get("pattern") or test.get("script") or ",".join(
                    test["names"]
                )
    return "passed", ""


def baseline(
    guards: list[dict[str, Any]],
    worktree: Path,
    app: Path | None,
    options: argparse.Namespace,
    log: Path,
) -> None:
    """Require every distinct test group to pass before anything is broken."""
    seen: set[str] = set()
    for guard in guards:
        for test in guard["tests"]:
            key = json.dumps(test, sort_keys=True)
            if key in seen:
                continue
            seen.add(key)
            for command in test_commands(test, worktree, app, options):
                if run(command, worktree, log) != 0:
                    raise SystemExit(
                        f"break_checks: {key} fails before any guard is broken; see {log}"
                    )


def break_one(
    guard: dict[str, Any], worktree: Path, options: argparse.Namespace, log: Path
) -> Outcome:
    """Break one guard, run its tests, restore the file."""
    path = worktree / guard["file"]
    original = path.read_text(encoding="utf-8")
    app = options.native_app if guard.get("native") else options.app
    try:
        path.write_text(apply_patch(original, guard), encoding="utf-8")
        if guard.get("native") and run(
            shlex.split(options.native_build.format(worktree=worktree)), worktree, log
        ):
            return Outcome(guard["id"], "error", "the native build failed")
        status, test = run_tests(guard, worktree, app, options, log)
    except ValueError as problem:
        return Outcome(guard["id"], "invalid", str(problem))
    finally:
        path.write_text(original, encoding="utf-8")
    dirty = git(worktree, "status", "--porcelain", "--untracked-files=no").strip()
    if dirty:
        raise SystemExit(f"break_checks: the worktree is not clean after {guard['id']}:\n{dirty}")
    if status == "failed":
        return Outcome(guard["id"], "killed", killed_by=test)
    return Outcome(guard["id"], "survived", "every named test still passed")


def runnable(guard: dict[str, Any], options: argparse.Namespace) -> str:
    """Why a guard cannot run here, or the empty string."""
    if guard.get("native") and not (options.native_build and options.native_app):
        return "patches native code; needs --native-build and --native-app"
    needs_app = any(test["kind"] == "acceptance" for test in guard["tests"])
    if needs_app and not (options.native_app if guard.get("native") else options.app):
        return "runs acceptance cases; needs --app"
    return ""


def break_all(guards: list[dict[str, Any]], options: argparse.Namespace) -> list[Outcome]:
    """Break every guard that can run in one throwaway worktree of HEAD."""
    outcomes: list[Outcome] = []
    ready = []
    for guard in guards:
        reason = runnable(guard, options)
        if reason:
            outcomes.append(Outcome(guard["id"], "not run", reason))
        else:
            ready.append(guard)
    if not ready:
        return outcomes
    options.output.mkdir(parents=True, exist_ok=True)
    log = options.output / "break-run.log"
    worktree = Path(tempfile.mkdtemp(prefix="break-", dir=options.output)) / "tree"
    git(options.repo, "worktree", "add", "--detach", str(worktree), "HEAD")
    try:
        non_native = [g for g in ready if not g.get("native")]
        baseline(non_native, worktree, options.app, options, log)
        outcomes.extend(break_one(guard, worktree, options, log) for guard in ready)
    finally:
        git(options.repo, "worktree", "remove", "--force", str(worktree))
        shutil.rmtree(worktree.parent, ignore_errors=True)
    return outcomes


def write_report(outcomes: list[Outcome], options: argparse.Namespace) -> int:
    """Write the results and the gaps file; return the exit status."""
    options.output.mkdir(parents=True, exist_ok=True)
    commit = git(options.repo, "rev-parse", "HEAD").strip()
    rows = [outcome.__dict__ for outcome in outcomes]
    result = {"commit": commit, "finished": time.strftime("%Y-%m-%d %H:%M:%S"), "guards": rows}
    (options.output / "break-results.json").write_text(json.dumps(result, indent=1) + "\n")
    gaps = [o for o in outcomes if o.status not in {"killed", "applies"}]
    lines = [f"# Break-check gaps at {commit[:9]}", ""]
    lines += [f"- {o.guard} ({o.status}): {o.detail}" for o in gaps] or ["No gaps."]
    (options.output / "break-gaps.md").write_text("\n".join(lines) + "\n")
    for outcome in outcomes:
        print(f"{outcome.status:9} {outcome.guard} {outcome.killed_by or outcome.detail}")
    blocking = [o for o in gaps if o.status != "not run"]
    return int(bool(blocking))


def run_acceptance(worktree: Path, app: Path, names: list[str], options: argparse.Namespace) -> int:
    """Run named NativeAcceptance cases of a worktree against a built app."""
    import unittest

    for setting in options.env:
        name, _, value = setting.partition("=")
        os.environ[name] = value
    if sys.platform == "darwin":
        os.environ["DYLD_LIBRARY_PATH"] = str(app.parent)
    sys.path.insert(0, str(worktree / "ci-scripts"))
    import mcp_acceptance

    case_class = mcp_acceptance.NativeAcceptance
    case_class.executable = app
    # The worker takes paths as they are stored, so the folder is named without a link.
    case_class.output = Path(tempfile.mkdtemp(prefix="gzq-break-", dir=SCRATCH)).resolve()
    case_class.media_tools = options.media_tools
    suite = unittest.TestSuite(case_class(f"test_{name}") for name in names)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return 0 if result.wasSuccessful() else 1


def parse_arguments() -> argparse.Namespace:
    """Read the command line."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--guards", type=Path, default=GUARDS)
    parser.add_argument("--output", type=Path, default=SCRATCH / "gzq-break-checks")
    parser.add_argument("--gate", help="command put in front of every acceptance run")
    parser.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    parser.add_argument("--media-tools", type=Path)
    parser.add_argument("--only", nargs="*", default=[])
    parser.add_argument("--app", type=Path)
    parser.add_argument("--native-build")
    parser.add_argument("--native-app", type=Path)
    parser.add_argument("--check-anchors", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--acceptance-worktree", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("cases", nargs="*", help=argparse.SUPPRESS)
    return parser.parse_args()


def main() -> int:
    """Check or break the guards of the table."""
    options = parse_arguments()
    if options.acceptance_worktree:
        return run_acceptance(options.acceptance_worktree, options.app, options.cases, options)
    options.repo = Path(git(options.repo, "rev-parse", "--show-toplevel").strip())
    guards = json.loads(options.guards.read_text(encoding="utf-8"))
    if options.only:
        guards = [guard for guard in guards if guard["id"] in options.only]
    if options.list:
        print("\n".join(f"{g['id']}: {g['file']}" for g in guards))
        return 0
    if options.check_anchors:
        outcomes = check_anchors(options.repo, guards)
        for outcome in outcomes:
            print(f"{outcome.status:8} {outcome.guard} {outcome.detail}")
        return int(any(outcome.status != "applies" for outcome in outcomes))
    return write_report(break_all(guards, options), options)


if __name__ == "__main__":
    sys.exit(main())
