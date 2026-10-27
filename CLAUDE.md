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
- The `No PII` workflow fails any pull request whose title, description,
  commit messages or added lines carry a match.

If a match is a false positive, reword it; don't weaken the check.

## Building and testing

Builds and tests run in CI (`.github/workflows`), not locally.
