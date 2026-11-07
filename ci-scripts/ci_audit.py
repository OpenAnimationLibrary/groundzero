#!/usr/bin/env python3
"""Read why CI failed, from the check annotations (the full logs are not
reachable from every environment), and audit the recent failures.

  ci_audit.py <pull request number | branch | commit sha>
      every check of that target that did not pass: the failing jobs and
      steps, and the distinct errors the workflows reported, with a hint at
      the kind of failure
  ci_audit.py --recent [N]
      the last N (default 60) failed runs of the repository, grouped by
      workflow and by cause, so that a recurring failure shows up

Uses the gh CLI (REST only). The repository is read from the git remotes, or
set CI_AUDIT_REPO=owner/name.
"""

import argparse
import collections
import json
import os
import re
import subprocess
import sys
from urllib.parse import quote, urlsplit

DEFAULT_REPO = "OpenAnimationLibrary/groundzero"

COMPILER_TODO = (
    "fix the file and line named; look for a missing include, a "
    "const/signature mismatch, or a Qt 5/6 API difference"
)

# (pattern, kind, what to do), most specific first
HINTS = [
    (
        r"No PII|session link|e-mail address|Personal data",
        "privacy scan",
        "remove the link or address named in the annotation (CLAUDE.md)",
    ),
    (
        r": (fatal )?error:|error: ‘|undefined reference|ld returned|^FAILED:",
        "compile or link error",
        COMPILER_TODO,
    ),
    (r"CMake Error", "CMake error", "a target defined twice, a missing file or a bad link name"),
    (
        r"Could not find executable|was not built|tool was not built",
        "a test or tool that did not build",
        "there is a compile error earlier in the same run; fix that first",
    ),
    (
        r"\b(?:SIGSEGV|SegFault|segmentation fault|access violation|heap corruption)\b|"
        r"\b(?:3221226529|0xc0000421)\b|"
        r"(?:exit(?:ed)?(?: with)?(?: code| status)?|return(?: code)?|status)[:= ]+-11\b",
        "native crash",
        "inspect the original crash dump or stack trace and the last completed checkpoint",
    ),
    (
        r"\b(?:apt(?:-get)?|package mirror)\b|Temporary failure (?:resolving|in name resolution)|"
        r"Connection (?:reset|refused)|Could not resolve|HTTP 5\d\d|rate limit|"
        r"(?:connection|connect|read) (?:timeout|timed out)",
        "network or package mirror",
        "confirm the failed network operation, then add a bounded retry for that operation",
    ),
    (
        r"\b(?:timeout|timed out|time limit exceeded)\b",
        "timeout",
        "inspect the timed-out step and its diagnostics before deciding whether to retry",
    ),
    (
        r"\btests? failed\b|Exception:|\*\*\*Failed|Test +#\d+|\bNative UI regression\b",
        "a test failed",
        "read the test output in the annotation; run it by hand only in CI",
    ),
    (
        r"\b(?:differs|different|Compat)\b",
        "compatibility difference",
        "an existing scene saves or renders differently: a real regression",
    ),
    (
        r"\b(?:operation|job|workflow|run) (?:was |has been )?cancel(?:led|ed)\b|"
        r"^cancel(?:led|ed)$",
        "cancelled",
        "check whether cancellation was manual or caused by a newer run",
    ),
]


def run(args, check=True):
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as e:
        if check:
            sys.exit(f"ci_audit: {' '.join(args[:3])}: {e}")
        return ""
    if out.returncode != 0:
        if check:
            sys.exit(f"ci_audit: gh failed: {out.stderr.strip()[:300]}")
        return ""
    return out.stdout


def gh(path, *flags):
    text = run(["gh", "api", *flags, path])
    try:
        return json.loads(text) if text.strip() else None
    except json.JSONDecodeError as error:
        sys.exit(f"ci_audit: invalid JSON from {path}: {error}")


def gh_items(path, key=None, limit=None):
    """Collect a paginated REST list, stopping at the requested item limit.

    Args:
        path: REST endpoint, optionally with existing query parameters.
        key: Envelope key containing the list, or None for a top-level list.
        limit: Maximum number of items, or None for every page.

    Returns:
        The collected items in API order.
    """
    page_size = min(100, limit) if limit is not None else 100
    separator = "&" if "?" in path else "?"
    items = []
    page = 1
    while True:
        response = gh(f"{path}{separator}per_page={page_size}&page={page}")
        batch = (response or {}).get(key, []) if key else (response or [])
        items.extend(batch)
        if limit is not None and len(items) >= limit:
            return items[:limit]
        if len(batch) < page_size:
            return items
        page += 1


def repo():
    if os.environ.get("CI_AUDIT_REPO"):
        return os.environ["CI_AUDIT_REPO"]
    for remote in ("groundzero", "origin"):
        url = run(["git", "remote", "get-url", remote], check=False).strip()
        m = re.search(r"github\.com[:/]([^/]+/[^/.]+?)(?:\.git)?$", url)
        if m:
            return m.group(1)
    return DEFAULT_REPO


SOURCE_FILE = re.compile(r"\.(c|cc|cpp|cxx|h|hpp|hxx|mm)(:\d+)?$", re.I)
COMPILER_WORDS = re.compile(
    r"not declared|does not name a type|no matching function|ambiguous|"
    r"discards qualifiers|invalid use of|expected .* before|has no member|"
    r"was not declared|no member named|cannot convert|incomplete type",
    re.I,
)


def hint(text, place=""):
    # The workflows report a compiler error as file:line + message, without
    # the word "error:", so a message that is attached to a source file, or
    # that reads like a compiler diagnostic, is one
    for pattern, kind, todo in HINTS:
        if re.search(pattern, text, re.I | re.M):
            return kind, todo
    if SOURCE_FILE.search(place) or COMPILER_WORDS.search(text):
        return "compile or link error", COMPILER_TODO
    return "unknown", "open the failing step's annotations and read them"


def clean(message):
    """One line of an annotation, without paths of the runner."""
    message = message.replace("/home/runner/work/groundzero/groundzero/", "")
    return re.sub(r"\s+", " ", message).strip()


def annotations(rp, check_id):
    items = gh_items(f"repos/{rp}/check-runs/{check_id}/annotations")
    seen, out = set(), []
    for a in items:
        if a.get("annotation_level") != "failure":
            continue
        path = a.get("path", "")
        # ".github" is only the position in the workflow log, not a file
        place = "" if path in ("", ".github") else f"{path}:{a.get('start_line', '')}"
        message = clean(a.get("message") or "")
        title = clean(a.get("title") or "")
        if not message:
            continue
        text = (f"[{title}] " if title else "") + message
        key = (place, text)
        if key in seen:
            continue
        seen.add(key)
        out.append((place, text))
    return out


def action_job(rp, check):
    """Read the Actions job linked by a check; other apps have no Actions steps.

    Args:
        rp: Repository in owner/name form.
        check: GitHub check-run object.

    Returns:
        The linked job object, or an empty mapping if no Actions job is linked.
    """
    if (check.get("app") or {}).get("slug") != "github-actions":
        return {}
    url = urlsplit(check.get("details_url") or "")
    match = re.fullmatch(
        rf"/{re.escape(rp)}/(?:actions/runs/\d+/job|runs/\d+/jobs)/(\d+)", url.path
    )
    if url.netloc != "github.com" or not match:
        return {}
    return gh(f"repos/{rp}/actions/jobs/{match.group(1)}") or {}


def job_annotations(rp, job):
    """Read failure annotations through a workflow job's check-run link.

    Args:
        rp: Repository in owner/name form.
        job: GitHub Actions job object.

    Returns:
        Failure location/message pairs, or an empty list if no check is linked.
    """
    url = urlsplit(job.get("check_run_url") or "")
    match = re.fullmatch(rf"/repos/{re.escape(rp)}/check-runs/(\d+)", url.path)
    if url.netloc != "api.github.com" or not match:
        return []
    return annotations(rp, match.group(1))


def resolve(rp, target):
    """(sha, description) for a pull request number, branch or commit."""
    if re.fullmatch(r"\d{1,6}", target):
        pr = gh(f"repos/{rp}/pulls/{target}")
        return pr["head"]["sha"], f"pull request #{target} ({pr['head']['ref']})"
    if re.fullmatch(r"[0-9a-f]{7,40}", target):
        return target, f"commit {target[:10]}"
    ref = gh(f"repos/{rp}/commits/{quote(target, safe='')}")
    return ref["sha"], f"branch {target} ({ref['sha'][:10]})"


def audit_target(target):
    rp = repo()
    sha, what = resolve(rp, target)
    runs = gh_items(f"repos/{rp}/commits/{sha}/check-runs", "check_runs")
    print(f"CI audit of {what}, {len(runs)} checks\n")
    if not runs:
        print("No checks have reported for this commit; validation is pending.")
        return 1
    bad = [r for r in runs if r["conclusion"] not in ("success", "skipped", "neutral")]
    pending = [r for r in bad if r["status"] != "completed"]
    for r in runs:
        mark = "ok " if r not in bad else ("..." if r["status"] != "completed" else "FAIL")
        print(f"  {mark:4} {r['name']}")
    if not bad:
        print("\nEvery check passed.")
        return 0
    for r in bad:
        if r["status"] != "completed":
            continue
        report_failure(rp, r)
    if pending:
        print(f"\n{len(pending)} check(s) still running.")
    return 1


def report_failure(rp, check):
    """Print a failed check's steps, error annotations, and diagnosis hints.

    Args:
        rp: Repository in owner/name form.
        check: Completed, unsuccessful GitHub check-run object.
    """
    print(f"\n== {check['name']}: {check['conclusion']}")
    job = action_job(rp, check)
    for step in job.get("steps", []):
        if step.get("conclusion") == "failure":
            print(f"   failed step: {step['name']}")
    notes = annotations(rp, check["id"])
    # A failed build can leave many missing executables; summarize those
    # annotations so they do not bury the original build diagnostics.
    missing = [note for note in notes if "Could not find executable" in note[1]]
    if missing:
        notes = [note for note in notes if note not in missing]
        print(f"   {len(missing)} test executables were not found: inspect the build errors")
    kinds = collections.Counter(hint(text, place) for place, text in notes)
    for place, text in notes[:40]:
        print(f"   {place + ' ' if place else ''}{text[:400]}")
    if len(notes) > 40:
        print(f"   ... {len(notes) - 40} more")
    if not notes and not missing:
        print(
            "   no failure annotations: the workflow did not report the error; "
            "add a report step (see the Linux Build 'Report build "
            "errors' step) rather than re-running"
        )
    for (kind, todo), _ in kinds.most_common(3):
        print(f"   -> {kind}: {todo}")


def audit_recent(limit):
    rp = repo()
    runs = gh_items(f"repos/{rp}/actions/runs?status=failure", "workflow_runs", limit)
    print(f"CI audit: the last {len(runs)} failed runs of {rp}\n")
    groups = collections.defaultdict(list)
    for run_ in runs:
        jobs = gh_items(f"repos/{rp}/actions/runs/{run_['id']}/jobs", "jobs")
        causes = set()
        for job in jobs:
            if job.get("conclusion") != "failure":
                continue
            notes = job_annotations(rp, job)
            kinds = {hint(text, place)[0] for place, text in notes}
            if not notes:
                kinds.add("no failure annotation (inspect the job log)")
            causes.update(f"{job['name']}: {kind}" for kind in kinds)
        if not causes:
            causes.add("(no failed job found)")
        for cause in causes:
            groups[(run_["name"], cause)].append(run_)
    for (workflow, cause), items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        branches = sorted({i["head_branch"] or "(no branch)" for i in items})[:4]
        latest = max(i["created_at"] for i in items)[:10]
        print(f"{len(items):3}x  {workflow} / {cause}   (latest {latest}; {', '.join(branches)})")
    print(
        "\nA cause that repeats is a workflow or code problem to fix, not "
        "bad luck: audit it with ci_audit.py <branch> and fix the root cause."
    )
    return 0


def positive_count(value):
    try:
        count = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("count must be a positive integer") from error
    if count < 1:
        raise argparse.ArgumentTypeError("count must be a positive integer")
    return count


def main(argv):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("target", nargs="?", help="pull request number, branch or commit")
    parser.add_argument(
        "--recent",
        nargs="?",
        const=60,
        type=positive_count,
        metavar="N",
        help="group the last N failed runs (default: 60)",
    )
    args = parser.parse_args(argv[1:])
    if bool(args.target) == (args.recent is not None):
        parser.error("provide a target or --recent, but not both")
    return audit_recent(args.recent) if args.recent is not None else audit_target(args.target)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
