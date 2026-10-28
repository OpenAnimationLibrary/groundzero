#!/usr/bin/env python3
"""Refuse text that carries personal data or private session links.

Two modes:

  no_pii.py scan [FILE ...]   scan files (or stdin), print each hit, exit 1 on any
  no_pii.py clean             print stdin without its session links and session
                              trailers (the link the GitHub integration adds to
                              a description becomes its plain claude.ai/code)
  no_pii.py hook              Claude Code PreToolUse hook: read the tool call as
                              JSON on stdin, exit 2 (block) when a publishing
                              command would carry a hit

What counts as a hit:
  - Claude Code session links (claude.ai/code/session_...) and session ids
  - "Claude-Session:" commit trailers
  - e-mail addresses, except no-reply ones and documentation domains
"""

import json
import os
import re
import shlex
import subprocess
import sys

SESSION_PATTERNS = [
    (re.compile(r"claude\.ai/code/session_[A-Za-z0-9_-]+", re.I), "Claude session link"),
    (re.compile(r"\bsession_0[0-9A-Za-z]{20,}\b"), "Claude session id"),
    (re.compile(r"^\s*Claude-Session:", re.I | re.M), "Claude-Session trailer"),
]

EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}")
ALLOWED_EMAIL = re.compile(
    r"(^|[.+])no-?reply@|@users\.noreply\.github\.com$|@example\.(com|org|net)$|"
    r"^git@github\.com$",
    re.I,
)


def find_hits(text):
    hits = []
    for pattern, what in SESSION_PATTERNS:
        for m in pattern.finditer(text):
            hits.append((what, m.group(0).strip()))
    for m in EMAIL.finditer(text):
        if not ALLOWED_EMAIL.search(m.group(0)):
            hits.append(("e-mail address", m.group(0)))
    return hits


def clean(text):
    text = re.sub(r"(claude\.ai/code)/session_[A-Za-z0-9_-]+", r"\1", text, flags=re.I)
    return "".join(
        line for line in text.splitlines(True)
        if not re.match(r"\s*Claude-Session:", line, re.I)
    )


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def scan_main(paths):
    sources = [(p, read(p)) for p in paths] if paths else [("<stdin>", sys.stdin.read())]
    found = False
    for name, text in sources:
        for what, value in find_hits(text):
            found = True
            print(f"{name}: {what}: {value}")
    return 1 if found else 0


# -- hook ---------------------------------------------------------------------

# Commands that put text somewhere other people can read it.
PUBLISHING = re.compile(
    r"\bgit\s+(?:-C\s+\S+\s+)?(?:commit|tag|notes|merge|revert|cherry-pick|am)\b|"
    r"\bgh\s+(?:pr|issue|release|gist|api|discussion)\b"
)
GIT_COMMIT = re.compile(r"\bgit\s+(?:-C\s+(\S+)\s+)?commit\b")
# Commands that change the index: the hook runs before the whole command line,
# so with "git add -A && git commit" the index does not hold the commit yet
GIT_STAGE = re.compile(r"\bgit\s+(?:-C\s+\S+\s+)?(?:add|stage|rm|mv)\b")
UNTRACKED_SIZE_LIMIT = 4 * 1024 * 1024
FILE_FLAGS = {"-F", "--file", "--body-file", "--input", "--notes-file"}


def referenced_files(tokens, cwd):
    """Files whose contents a command sends along (-F file, body=@file...)."""
    files = []
    for i, tok in enumerate(tokens):
        candidate = None
        if tok in FILE_FLAGS and i + 1 < len(tokens):
            candidate = tokens[i + 1]
        elif "=" in tok and tok.split("=", 1)[0] in FILE_FLAGS:
            candidate = tok.split("=", 1)[1]
        if candidate and "=@" in candidate:
            candidate = candidate.split("=@", 1)[1]
        elif "=@" in tok:
            candidate = tok.split("=@", 1)[1]
        if candidate and candidate != "-":
            path = os.path.join(cwd, os.path.expanduser(candidate))
            if os.path.isfile(path):
                files.append(path)
    return list(dict.fromkeys(files))


def staged_additions(command, cwd):
    """Lines a git commit in this command would add."""
    m = GIT_COMMIT.search(command)
    repo = os.path.join(cwd, m.group(1)) if m and m.group(1) else cwd
    cd = re.search(r"(?:^|[;&|]\s*)cd\s+(\S+)", command)
    if cd and not (m and m.group(1)):
        repo = os.path.join(cwd, os.path.expanduser(cd.group(1).strip("'\"")))
    commit_all = re.search(r"\bcommit\b[^;&|]*\s(?:-a\b|--all\b|-[A-Za-z]*a[A-Za-z]*\b)", command)
    stages = GIT_STAGE.search(command)
    args = ["git", "-C", repo, "diff", "--no-color", "-U0"]
    # Staging in the same command can bring any change in: scan all of them
    args += ["HEAD"] if commit_all or stages else ["--cached"]
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return ""
    added = [
        line[1:] for line in out.splitlines() if line.startswith("+") and not line.startswith("+++")
    ]
    if stages:
        added += untracked_contents(repo)
    return "\n".join(added)


def untracked_contents(repo):
    """Contents of the untracked files that "git add" could stage."""
    args = ["git", "-C", repo, "ls-files", "-z", "--others", "--exclude-standard"]
    try:
        out = subprocess.run(args, capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    contents = []
    for name in filter(None, out.split("\0")):
        path = os.path.join(repo, name)
        try:
            if os.path.getsize(path) > UNTRACKED_SIZE_LIMIT:
                continue
        except OSError:
            continue
        contents.append(read(path))
    return contents


def hook_main():
    try:
        payload = json.load(sys.stdin)
    except ValueError:
        return 0
    if payload.get("tool_name") != "Bash":
        return 0
    command = (payload.get("tool_input") or {}).get("command") or ""
    if not PUBLISHING.search(command):
        return 0
    cwd = payload.get("cwd") or os.getcwd()

    try:
        tokens = shlex.split(command, posix=True)
    except ValueError:
        tokens = command.split()

    hits = [("command", what, value) for what, value in find_hits(command)]
    for path in referenced_files(tokens, cwd):
        hits += [(path, what, value) for what, value in find_hits(read(path))]
    if GIT_COMMIT.search(command):
        hits += [("staged changes", what, value)
                 for what, value in find_hits(staged_additions(command, cwd))]

    if not hits:
        return 0
    lines = [f"  {where}: {what}: {value}" for where, what, value in hits]
    sys.stderr.write(
        "Blocked: this would publish personal data or a private session link.\n"
        + "\n".join(lines)
        + "\nRemove it (see the no-PII rule in CLAUDE.md) and try again.\n"
    )
    return 2


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "scan"
    if mode == "hook":
        sys.exit(hook_main())
    if mode == "clean":
        sys.stdout.write(clean(sys.stdin.read()))
        sys.exit(0)
    sys.exit(scan_main(sys.argv[2:] if mode == "scan" else sys.argv[1:]))
