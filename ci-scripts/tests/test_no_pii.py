"""Contracts for the no-PII hook: which repository a git commit is scanned in."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "no_pii.py"
# Built at run time so this file does not itself carry an address
ADDRESS = "@".join(("reviewer", "mail.invalid"))
GIT_ENV = {**os.environ, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"}


def git(repo, *args):
    subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, env=GIT_ENV)


def make_repo(path, staged_text):
    """A repository with one commit and staged_text staged in notes.txt."""
    path.mkdir(parents=True)
    git(path, "init", "-q")
    (path / "notes.txt").write_text("first\n")
    git(path, "add", "notes.txt")
    git(path, "-c", "user.name=test", "-c", "user.email=test@example.com", "commit", "-qm", "a")
    (path / "notes.txt").write_text(f"first\n{staged_text}\n")
    git(path, "add", "notes.txt")
    return path


def hook(command, cwd):
    """Exit status and error output of the hook for a Bash tool call."""
    payload = {"tool_name": "Bash", "tool_input": {"command": command}, "cwd": str(cwd)}
    result = subprocess.run(
        [sys.executable, str(SCRIPT), "hook"],
        input=json.dumps(payload),
        capture_output=True,
        text=True,
        env=GIT_ENV,
        timeout=60,
    )
    return result.returncode, result.stderr


class CommitTargetTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.clean = make_repo(self.root / "clean", "nothing personal")
        self.private = make_repo(self.root / "private", f"contact {ADDRESS}")
        self.spaced = make_repo(self.root / "private copy", f"contact {ADDRESS}")

    def assertBlocked(self, command, cwd, reason):
        status, error = hook(command, cwd)
        self.assertEqual(status, 2, error)
        self.assertIn(reason, error)

    def assertAllowed(self, command, cwd):
        status, error = hook(command, cwd)
        self.assertEqual(status, 0, error)

    def test_scans_the_session_directory_without_cd_or_dash_c(self):
        self.assertAllowed("git commit -m a", self.clean)
        self.assertBlocked("git commit -m a", self.private, ADDRESS)

    def test_follows_a_quoted_dash_c_path_with_spaces(self):
        self.assertBlocked(f'git -C "{self.spaced}" commit -m a', self.clean, ADDRESS)
        self.assertAllowed(f"git -C {self.clean} commit -m a", self.private)

    def test_follows_cd_on_an_earlier_line(self):
        self.assertBlocked(f"cd {self.private}\ngit commit -m a", self.clean, ADDRESS)
        self.assertBlocked(f"echo start\n  cd {self.private}\ngit commit", self.clean, ADDRESS)
        self.assertBlocked(f"cd '{self.spaced}'\ngit commit -m a", self.clean, ADDRESS)

    def test_resolves_relative_cd_and_dash_c_from_earlier_cds(self):
        self.assertBlocked(f"cd {self.root}\ncd private\ngit commit", self.clean, ADDRESS)
        self.assertBlocked(f"cd {self.root}\ngit -C 'private copy' commit", self.clean, ADDRESS)

    def test_ignores_a_cd_after_the_commit(self):
        self.assertBlocked(f"git commit -m a && cd {self.clean}", self.private, ADDRESS)

    def test_scans_every_commit_in_the_command(self):
        command = f"git -C {self.clean} commit -m a; git -C {self.private} commit -m b"
        self.assertBlocked(command, self.clean, ADDRESS)

    def test_rejects_a_dash_c_variable(self):
        self.assertBlocked('git -C "$REPO" commit -m a', self.clean, "not a literal path")

    def test_rejects_a_cd_variable_or_previous_directory(self):
        self.assertBlocked('cd "$REPO" && git commit -m a', self.clean, "not a literal path")
        self.assertBlocked("cd -\ngit commit -m a", self.clean, "not a literal path")

    def test_rejects_a_missing_directory(self):
        missing = self.root / "missing"
        self.assertBlocked(f"git -C {missing} commit -m a", self.clean, "is not a directory")
        self.assertBlocked(f"cd {missing}\ngit commit -m a", self.clean, "is not a directory")

    def test_rejects_a_directory_outside_any_repository(self):
        outside = self.root / "outside"
        outside.mkdir()
        self.assertBlocked(f"git -C {outside} commit -m a", self.clean, "git diff")

    def test_leaves_commands_that_publish_nothing_alone(self):
        self.assertAllowed('cd "$REPO" && git status', self.clean)
        self.assertAllowed('git -C "$REPO" log -1', self.clean)


if __name__ == "__main__":
    unittest.main()
