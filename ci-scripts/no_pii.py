#!/usr/bin/env python3
"""Refuse text that carries personal data or private session links.

Two modes:

  no_pii.py scan [FILE ...]   scan files (or stdin), print each hit, exit 1 on any
  no_pii.py clean             print stdin without its session links and session
                              trailers (the link the GitHub integration adds to
                              a description becomes its plain claude.ai/code)
  no_pii.py hook              Claude Code PreToolUse hook: read the tool call as
                              JSON on stdin, exit 2 (block) when a publishing
                              command would carry a hit, or when it holds a git
                              commit whose repository or changes it cannot read

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
        line for line in text.splitlines(True) if not re.match(r"\s*Claude-Session:", line, re.I)
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

# One shell word: quoted parts, escaped characters and plain characters
WORD = r"""(?:"[^"]*"|'[^']*'|\\.|[^\s;&|()<>'"\\])+"""
# "git" and the -C <path> or -c <name>=<value> options before its subcommand
GIT = rf"\bgit((?:\s+-[Cc]\s+{WORD})*)\s+"
GIT_OPTION = re.compile(rf"-([Cc])\s+({WORD})")
# Commands that put text somewhere other people can read it.
PUBLISHING = re.compile(
    GIT + r"(?:commit|tag|notes|merge|revert|cherry-pick|am)\b|"
    r"\bgh\s+(?:pr|issue|release|gist|api|discussion)\b"
)
GIT_COMMIT = re.compile(GIT + r"commit\b")
# Commands that change the index: the hook runs before the whole command line,
# so with "git add -A && git commit" the index does not hold the commit yet
GIT_STAGE = re.compile(GIT + r"(?:add|stage|rm|mv)\b")
# A cd that starts a command: at the start of a line, or after ; & | or (
CD = re.compile(rf"(?:^|[;&|(])[ \t]*cd(?:[ \t]+({WORD}))?[ \t]*(?=$|[;&|)])", re.M)
# Words whose value the shell decides: variables, substitutions, globs, "cd -"
SHELL_EXPANDED = re.compile(r"[$`*?]|^-$")
UNTRACKED_SIZE_LIMIT = 4 * 1024 * 1024
FILE_FLAGS = {"-F", "--file", "--body-file", "--input", "--notes-file"}
LITERAL_PATH_HINT = "name the repository directly, for example: git -C /path/to/repo commit ..."


class CommitTargetError(Exception):
    """The repository a git commit works in, or its changes, cannot be read."""


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


def literal_path(word, base, how):
    """The path a shell word names, relative to base, if the shell would not expand it."""
    try:
        parts = shlex.split(word)
    except ValueError:
        parts = []
    if len(parts) != 1 or SHELL_EXPANDED.search(parts[0]):
        raise CommitTargetError(f"{how} {word} is not a literal path")
    return os.path.normpath(os.path.join(base, os.path.expanduser(parts[0])))


def commit_repo(command, commit, cwd):
    """Directory a git commit match works in: the cds before it, then its -C options."""
    directory = cwd
    for cd in CD.finditer(command, 0, commit.start()):
        directory = literal_path(cd.group(1) or "~", directory, "cd")
    for flag, word in GIT_OPTION.findall(commit.group(1)):
        if flag == "C":
            directory = literal_path(word, directory, "git -C")
    if not os.path.isdir(directory):
        raise CommitTargetError(f"{directory} is not a directory")
    return directory


def git_output(repo, args):
    """Standard output of a git command in repo; CommitTargetError if it fails."""
    try:
        result = subprocess.run(
            ["git", "-C", repo, *args], capture_output=True, text=True, timeout=30
        )
    except (OSError, subprocess.SubprocessError) as error:
        raise CommitTargetError(f"git {args[0]} in {repo} failed: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or f"exit status {result.returncode}"
        raise CommitTargetError(f"git {args[0]} in {repo} failed: {detail}")
    return result.stdout


def staged_additions(command, cwd):
    """Lines the git commits in this command would add.

    Raises CommitTargetError when the repository of a commit cannot be resolved
    or its changes cannot be read, so that nothing passes unscanned.
    """
    commit_all = re.search(r"\bcommit\b[^;&|]*\s(?:-a\b|--all\b|-[A-Za-z]*a[A-Za-z]*\b)", command)
    stages = GIT_STAGE.search(command)
    # Staging in the same command can bring any change in: scan all of them
    diff = ["diff", "--no-color", "-U0", "HEAD" if commit_all or stages else "--cached"]
    repos = dict.fromkeys(commit_repo(command, m, cwd) for m in GIT_COMMIT.finditer(command))
    added = []
    for repo in repos:
        out = git_output(repo, diff)
        added += [
            line[1:]
            for line in out.splitlines()
            if line.startswith("+") and not line.startswith("+++")
        ]
        if stages:
            added += untracked_contents(repo)
    return "\n".join(added)


def untracked_contents(repo):
    """Contents of the untracked files that "git add" could stage."""
    out = git_output(repo, ["ls-files", "-z", "--others", "--exclude-standard"])
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
        try:
            staged = staged_additions(command, cwd)
        except CommitTargetError as error:
            sys.stderr.write(
                "Blocked: cannot read the changes this git commit would publish: "
                f"{error}.\nThe hook follows git -C and cd only with literal paths: "
                f"{LITERAL_PATH_HINT}\n"
            )
            return 2
        hits += [("staged changes", what, value) for what, value in find_hits(staged)]

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
