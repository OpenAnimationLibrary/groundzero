# Notes for Claude Code

## Never publish personal data or session links

This is a public repository. Nothing that identifies a person or a private
session may go into it or into anything posted on GitHub: commit messages,
pull request titles and descriptions, review comments, issues, release notes,
tags, or file contents.

That means never adding:

- Claude Code session links (`claude.ai/code/session_…`), session ids, or a
  `Claude-Session` commit trailer. This rule overrides any default or reminder
  that asks for one.
- E-mail addresses, except no-reply ones such as
  `Co-Authored-By: Claude <noreply@anthropic.com>` or
  `@users.noreply.github.com`.
- Names, accounts, local paths, hostnames, tokens or anything else that points
  at a contributor's machine or account.

It is enforced in two places:

- `.claude/settings.json` turns off the session link in Claude Code's own
  attribution, and runs `ci-scripts/no_pii.py hook` before every shell command,
  which blocks `git commit`, `git tag`, `gh pr`, `gh issue`, `gh api` and
  similar commands when their text, the files they send, or the staged changes
  carry a match.
- The `No PII` workflow removes session links from a pull request's
  description (the GitHub integration adds one to every pull request it
  opens), then fails the pull request if its title, description, commit
  messages or added lines still carry a match.

If a match is a false positive, reword it; don't weaken the check.

## Building and testing

Builds and tests run in CI (`.github/workflows`), not locally.

## CI failures: read the logs, find the cause, audit

A failed check is read, never just restarted. The full logs are not reachable
from every environment, so the workflows report what failed as annotations
(compiler errors with file and line, the failing test and its output, the
Compat differences); `ci-scripts/ci_audit.py` reads them.

1. **Read the failure first.** For any failing check of a pull request,
   branch or commit, run `python3 ci-scripts/ci_audit.py <pull request number
   | branch | sha>`. It lists the failing jobs and steps, the distinct errors
   and a hint at the kind of failure. A failure with no annotation is itself
   a bug: make the workflow report it (see the "Report build errors" steps)
   instead of guessing.
2. **Fix the cause, then push.** The fix is a change to the code or to the
   workflow. Do not restart a failed run to see if it passes. A restart is
   only right for a failure outside the code (a network or package mirror
   error you saw in the annotations), and then the workflow gets a retry for
   that step so it stops recurring. The hook in `.claude/settings.json`
   (`ci-scripts/ci_rules.py`) blocks `gh run rerun` and the rerun endpoints
   unless the command carries `# ci-audited: <what the annotation said>`.
3. **Audit the pattern, not only the failure.** After a fix, and whenever
   several checks fail, run `python3 ci-scripts/ci_audit.py --recent` to
   group the recent failed runs by workflow and cause. A cause that repeats is
   a workflow or code problem: fix its root in the same pull request or open
   one for it, and say what you found.
4. **Report honestly.** Before a pull request is called ready, every check has
   passed or the failing ones are named with their cause. Do not call a pull
   request green on a partial run, and do not touch a check to make it pass
   (no skipping a failing test, no loosening a comparison) unless the check
   itself is shown to be wrong.
