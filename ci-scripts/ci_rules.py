#!/usr/bin/env python3
"""Claude Code PreToolUse hook: no blind re-runs of a failed CI run.

A failed check is read, not restarted. This blocks the commands that restart
a run (gh run rerun, the rerun and rerun-failed-jobs REST endpoints) unless
the command says what the failure was and why a restart is the right fix, in a
comment:

    gh run rerun 123 --failed   # ci-audited: apt mirror timed out in Install libraries

Run ci-scripts/ci_audit.py <pr | branch | sha> first (CLAUDE.md, "CI
failures"). A restart is only right for a failure outside the code, and then
the workflow should be changed to retry that step so it does not come back.
"""

import json
import re
import sys

RERUN = re.compile(
    r"\bgh\s+run\s+rerun\b|"
    r"\bgh\s+api\b[^\n;|&]*?/actions/(?:runs|jobs)/\d+/rerun(?:-failed-jobs)?\b",
    re.I,
)
AUDITED = re.compile(r"#\s*ci-audited:\s*\S.{11,}")


def main():
    try:
        payload = json.load(sys.stdin)
    except ValueError:
        return 0
    if payload.get("tool_name") != "Bash":
        return 0
    command = (payload.get("tool_input") or {}).get("command") or ""
    if not RERUN.search(command) or AUDITED.search(command):
        return 0
    sys.stderr.write(
        "Blocked: do not restart a failed CI run before reading why it failed.\n"
        "  1. python3 ci-scripts/ci_audit.py <pull request number | branch | sha>\n"
        "  2. fix the cause (code, or the workflow if it keeps coming back)\n"
        "  3. only if the failure is outside the code (a network or mirror\n"
        "     error you saw in the annotations), restart it and say what you\n"
        "     saw: add  # ci-audited: <what the annotation said>  to the command,\n"
        "     and change the workflow to retry that step.\n"
        "See CLAUDE.md, \"CI failures\".\n"
    )
    return 2


if __name__ == "__main__":
    sys.exit(main())
