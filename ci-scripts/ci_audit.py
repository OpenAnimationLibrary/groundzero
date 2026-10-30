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

import collections
import json
import os
import re
import subprocess
import sys

DEFAULT_REPO = "OpenAnimationLibrary/groundzero"

# Annotations that are about the runner, not about the change
NOISE = re.compile(
    r"Node\.js \d+ is deprecated|ubuntu-latest.* label will migrate|"
    r"^Process completed with exit code|Resource not accessible|"
    r"^The operation was canceled|actions/(checkout|cache|upload-artifact)@",
    re.I,
)

# (pattern, kind, what to do), most specific first
HINTS = [
    (r"No PII|session link|e-mail address|Personal data", "privacy scan",
     "remove the link or address named in the annotation (CLAUDE.md)"),
    (r": (fatal )?error:|error: ‘|undefined reference|ld returned|^FAILED:",
     "compile or link error",
     "fix the file and line named; look for a missing include, a "
     "const/signature mismatch, or a Qt 5/6 API difference"),
    (r"CMake Error", "CMake error",
     "a target defined twice, a missing file or a bad link name"),
    (r"Could not find executable|was not built|tool was not built",
     "a test or tool that did not build",
     "there is a compile error earlier in the same run; fix that first"),
    (r"tests? failed|SegFault|Exception:|\*\*\*Failed|Test +#\d+",
     "a test failed",
     "read the test output in the annotation; run it by hand only in CI"),
    (r"differs|different|Compat", "compatibility difference",
     "an existing scene saves or renders differently: a real regression"),
    (r"apt|mirror|Temporary failure|Timeout|timed out|Connection (reset|"
     r"refused)|Could not resolve|HTTP 5\d\d|rate limit",
     "network or package mirror", "a retry may be fine, but then make the "
     "workflow retry that step so it stops recurring"),
    (r"cancel", "cancelled", "a newer push superseded it, not a failure"),
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
    return json.loads(text) if text.strip() else None


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
    r"was not declared|no member named|cannot convert|incomplete type", re.I)


def hint(text, place=""):
    # The workflows report a compiler error as file:line + message, without
    # the word "error:", so a message that is attached to a source file, or
    # that reads like a compiler diagnostic, is one
    if SOURCE_FILE.search(place) or COMPILER_WORDS.search(text):
        return ("compile or link error", HINTS[1][2])
    for pattern, kind, todo in HINTS:
        if re.search(pattern, text, re.I | re.M):
            return kind, todo
    return "unknown", "open the failing step's annotations and read them"


def clean(message):
    """One line of an annotation, without paths of the runner."""
    message = message.replace("/home/runner/work/groundzero/groundzero/", "")
    return re.sub(r"\s+", " ", message).strip()


def annotations(rp, check_id):
    items = gh(f"repos/{rp}/check-runs/{check_id}/annotations?per_page=100") or []
    seen, out = set(), []
    for a in items:
        path = a.get("path", "")
        # ".github" is only the position in the workflow log, not a file
        place = "" if path in ("", ".github") else f"{path}:{a.get('start_line', '')}"
        place = place or ":"
        message = clean(a.get("message") or "")
        title = clean(a.get("title") or "")
        if not message or NOISE.search(message):
            continue
        text = (f"[{title}] " if title else "") + message
        key = (place if place != ":" else "", text[:200])
        if key in seen:
            continue
        seen.add(key)
        out.append((place if place != ":" else "", text))
    return out


def resolve(rp, target):
    """(sha, description) for a pull request number, branch or commit."""
    if re.fullmatch(r"\d{1,6}", target):
        pr = gh(f"repos/{rp}/pulls/{target}")
        return pr["head"]["sha"], f"pull request #{target} ({pr['head']['ref']})"
    if re.fullmatch(r"[0-9a-f]{7,40}", target):
        return target, f"commit {target[:10]}"
    ref = gh(f"repos/{rp}/commits/{target}")
    return ref["sha"], f"branch {target} ({ref['sha'][:10]})"


def audit_target(target):
    rp = repo()
    sha, what = resolve(rp, target)
    runs = (gh(f"repos/{rp}/commits/{sha}/check-runs?per_page=100") or {}).get(
        "check_runs", [])
    print(f"CI audit of {what}, {len(runs)} checks\n")
    if not runs:
        print("No checks have reported for this commit; validation is pending.")
        return 1
    bad = [r for r in runs if r["conclusion"] not in ("success", "skipped",
                                                      "neutral")]
    pending = [r for r in bad if r["status"] != "completed"]
    for r in runs:
        mark = "ok " if r not in bad else ("..." if r["status"] != "completed"
                                           else "FAIL")
        print(f"  {mark:4} {r['name']}")
    if not bad:
        print("\nEvery check passed.")
        return 0
    for r in bad:
        if r["status"] != "completed":
            continue
        print(f"\n== {r['name']}: {r['conclusion']}")
        job = gh(f"repos/{rp}/actions/jobs/{r['id']}") or {}
        for step in job.get("steps", []):
            if step.get("conclusion") == "failure":
                print(f"   failed step: {step['name']}")
        notes = annotations(rp, r["id"])
        # A build that failed leaves every test it did not build "not found":
        # one line says it, the compile error is the one to read
        missing = [n for n in notes if "Could not find executable" in n[1]]
        if missing:
            notes = [n for n in notes if n not in missing]
            print(f"   {len(missing)} tests were not built (their executables "
                  "were not found): see the compile errors")
        kinds = collections.Counter()
        for place, text in notes[:40]:
            print(f"   {place + ' ' if place else ''}{text[:400]}")
            kinds[hint(text, place)] += 1
        if len(notes) > 40:
            print(f"   ... {len(notes) - 40} more")
        if not notes:
            print("   no annotations: the workflow did not report the error; "
                  "add a report step (see the Linux Build 'Report build "
                  "errors' step) rather than re-running")
        for (kind, todo), n in kinds.most_common(3):
            print(f"   -> {kind}: {todo}")
    if pending:
        print(f"\n{len(pending)} check(s) still running.")
    return 1


def audit_recent(limit):
    rp = repo()
    runs = (gh(f"repos/{rp}/actions/runs?per_page=100&status=failure") or {}
            ).get("workflow_runs", [])[:limit]
    print(f"CI audit: the last {len(runs)} failed runs of {rp}\n")
    groups = collections.defaultdict(list)
    for run_ in runs:
        jobs = (gh(f"repos/{rp}/actions/runs/{run_['id']}/jobs?per_page=30")
                or {}).get("jobs", [])
        causes = set()
        for job in jobs:
            if job.get("conclusion") != "failure":
                continue
            notes = annotations(rp, job["id"])
            text = " ".join(t for _, t in notes[:8]) or "(no annotation)"
            kind, _ = hint(text, notes[0][0] if notes else "")
            if not notes:
                kind = "no annotation (the workflow reports nothing)"
            causes.add(f"{job['name']}: {kind}")
        if not causes:
            causes.add("(no failed job found)")
        for cause in causes:
            groups[(run_["name"], cause)].append(run_)
    for (workflow, cause), items in sorted(groups.items(),
                                            key=lambda kv: -len(kv[1])):
        branches = sorted({i["head_branch"] for i in items})[:4]
        latest = max(i["created_at"] for i in items)[:10]
        print(f"{len(items):3}x  {workflow} / {cause}   (latest {latest}; "
              f"{', '.join(branches)})")
    print("\nA cause that repeats is a workflow or code problem to fix, not "
          "bad luck: audit it with ci_audit.py <branch> and fix the root cause.")
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "--recent":
        return audit_recent(int(argv[2]) if len(argv) > 2 else 60)
    if len(argv) != 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    return audit_target(argv[1])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
