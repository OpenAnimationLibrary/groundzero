"""Offline release-range and retry fixtures; no builds or GitHub writes."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import release_notes as notes


SHA = "a" * 40
OTHER_SHA = "b" * 40
REPO = "OpenAnimationLibrary/groundzero"
ROOT = f"repos/{REPO}"


def release(tag, *, draft=False, prerelease=True, published="2026-10-26T00:00:00Z", body=""):
    return {
        "tag_name": tag, "draft": draft, "prerelease": prerelease,
        "published_at": None if draft else published, "body": body,
        "target_commitish": SHA,
    }


class FakeGitHub:
    def __init__(self, releases=(), statuses=None, generated="* Fix drawing by @artist in #42"):
        self.pages = [list(releases)]
        self.statuses = statuses or {}
        self.generated = generated
        self.calls = []

    def request(self, endpoint, *, data=None, paginate=False):
        self.calls.append((endpoint, data, paginate))
        if endpoint == f"{ROOT}/releases?per_page=100":
            if not paginate:
                raise AssertionError("Release listing must paginate")
            return self.pages
        if endpoint.startswith(f"{ROOT}/commits/"):
            return {"sha": SHA}
        if endpoint.startswith(f"{ROOT}/compare/"):
            base = endpoint.split("/compare/", 1)[1].split("...", 1)[0]
            return {"status": self.statuses.get(base, "ahead")}
        if endpoint == f"{ROOT}/releases/generate-notes":
            return {"body": self.generated}
        raise AssertionError(f"Unexpected request: {endpoint}")


class ReleaseNotesTests(unittest.TestCase):
    def generate(self, api, tag="build-3", **kwargs):
        return notes.generate_notes(api, REPO, tag, **kwargs)

    def test_build_uses_latest_published_prerelease_and_exact_sha(self):
        api = FakeGitHub([
            release("build-2"),
            release("v0.0.0", prerelease=False, published="2026-10-25T00:00:00Z"),
        ])
        result = self.generate(api, target=SHA, intro="Platform and build details\n")
        self.assertIn("Platform and build details\n\n## Changes since build-2", result)
        self.assertIn("#42", result)
        self.assertIn(f"/compare/build-2...{SHA}", result)
        self.assertIn((f"{ROOT}/releases/generate-notes", {
            "tag_name": "build-3", "target_commitish": SHA,
            "previous_tag_name": "build-2",
        }, False), api.calls)

    def test_squash_merge_titles_are_kept_from_github(self):
        api = FakeGitHub([release("build-2")], generated="* Squashed PR title in #55\n* Merge PR in #56")
        result = self.generate(api, target=SHA)
        self.assertIn("* Squashed PR title in #55\n* Merge PR in #56", result)

    def test_drafts_current_release_and_unrelated_tags_are_excluded(self):
        api = FakeGitHub([
            release("build-3", draft=True), release("build-4", draft=True),
            release("unrelated", published="2026-10-28T00:00:00Z"),
            release("build-2"),
        ])
        result = self.generate(api, target=SHA)
        self.assertIn("Changes since build-2", result)

    def test_stable_compares_with_previous_stable(self):
        api = FakeGitHub([
            release("build-2"), release("v0.2.0-rc1"),
            release("v0.1.0", prerelease=False, published="2026-10-25T00:00:00Z"),
        ])
        self.assertIn("Changes since v0.1.0", self.generate(api, "v0.2.0", target=SHA))

    def test_build_can_fall_back_to_stable(self):
        api = FakeGitHub([release("v0.0.0", prerelease=False)])
        self.assertIn("Changes since v0.0.0", self.generate(api, target=SHA))

    def test_all_pages_and_publication_time_are_used(self):
        api = FakeGitHub()
        api.pages = [[release("build-1")], [release("build-2", published="2026-10-27T00:00:00Z")]]
        self.assertIn("Changes since build-2", self.generate(api, target=SHA))

    def test_non_ancestor_releases_are_skipped(self):
        for status in ("behind", "diverged"):
            with self.subTest(status=status):
                api = FakeGitHub([
                    release("build-2", published="2026-10-27T00:00:00Z"), release("build-1"),
                ], statuses={"build-2": status})
                self.assertIn("Changes since build-1", self.generate(api, target=SHA))

    def test_no_compatible_baseline_fails(self):
        api = FakeGitHub([release("build-2")], statuses={"build-2": "diverged"})
        with self.assertRaisesRegex(ValueError, "ancestor"):
            self.generate(api, target=SHA)

    def test_identical_source_does_not_call_generator(self):
        api = FakeGitHub([release("build-2")], statuses={"build-2": "identical"})
        result = self.generate(api, target=SHA)
        self.assertIn("No source changes", result)
        self.assertIn("Full comparison", result)
        self.assertFalse(any(data is not None for _, data, _ in api.calls))

    def test_initial_release_does_not_use_an_implicit_github_baseline(self):
        for releases in ([], [release("build-2")]):
            api = FakeGitHub(releases)
            result = self.generate(api, "v0.0.0", target=SHA)
            self.assertIn("Initial release in this channel", result)
            self.assertFalse(any(data is not None for _, data, _ in api.calls))

    def test_manual_publication_resolves_tag_not_edition(self):
        api = FakeGitHub([release("v0.1.0", draft=True), release("v0.0.0", prerelease=False)])
        self.generate(api, "v0.1.0", existing_only=True)
        self.assertIn((f"{ROOT}/commits/v0.1.0", None, False), api.calls)
        self.assertNotIn((f"{ROOT}/commits/edition", None, False), api.calls)

    def test_legacy_draft_introduction_is_preserved(self):
        api = FakeGitHub([release("build-3", draft=True, body="Maintainer's notes"), release("build-2")])
        result = self.generate(api, target=SHA, intro="New default introduction")
        self.assertTrue(result.startswith("Maintainer's notes\n\n"))
        self.assertNotIn("New default introduction", result)

    def test_draft_retry_reuses_notes_without_reselecting_baseline(self):
        original = self.generate(FakeGitHub([release("build-1")]), target=SHA)
        api = FakeGitHub([release("build-3", draft=True, body=original), release("build-2")])
        self.assertEqual(original, self.generate(api, target=SHA, existing_only=True))
        self.assertEqual(len(api.calls), 1)

    def test_retry_rejects_different_build_commit(self):
        original = self.generate(FakeGitHub(), target=SHA)
        api = FakeGitHub([release("build-3", draft=True, body=original)])
        with self.assertRaisesRegex(ValueError, "different build commit"):
            self.generate(api, target=OTHER_SHA)

    def test_legacy_draft_rejects_different_target(self):
        api = FakeGitHub([release("build-3", draft=True)])
        with self.assertRaisesRegex(ValueError, "different build commit"):
            self.generate(api, target=OTHER_SHA)

    def test_published_release_body_is_returned_unchanged(self):
        api = FakeGitHub([release("build-3", body="Already published\n")])
        self.assertEqual("Already published\n", self.generate(api, target=SHA))
        self.assertEqual(len(api.calls), 1)

    def test_missing_existing_release_fails(self):
        with self.assertRaisesRegex(ValueError, "does not exist"):
            self.generate(FakeGitHub(), target=SHA, existing_only=True)

    def test_empty_generated_body_fails(self):
        with self.assertRaisesRegex(ValueError, "empty"):
            self.generate(FakeGitHub([release("build-2")], generated=""), target=SHA)

    def test_only_exact_sha_is_accepted_for_a_build(self):
        for target in ("edition", SHA[:7]):
            with self.subTest(target=target), self.assertRaisesRegex(ValueError, "full commit SHA"):
                self.generate(FakeGitHub(), target=target)

    def test_unknown_release_channel_fails(self):
        with self.assertRaisesRegex(ValueError, "release tag"):
            self.generate(FakeGitHub(), "other-tag", target=SHA)

    def test_gh_request_uses_json_stdin_and_paginated_arrays(self):
        api = notes.GitHub()
        with patch("release_notes.subprocess.run", return_value=subprocess.CompletedProcess([], 0, "[[{}]]")) as run:
            self.assertEqual([[{}]], api.request("repos/o/r/releases", paginate=True))
            self.assertEqual(run.call_args.args[0], ["gh", "api", "repos/o/r/releases", "--paginate", "--slurp"])
        payload = {"previous_tag_name": "v0.0.0", "target_commitish": SHA}
        with patch("release_notes.subprocess.run", return_value=subprocess.CompletedProcess([], 0, '{"body":"notes"}')) as run:
            api.request("repos/o/r/releases/generate-notes", data=payload)
            self.assertEqual(json.loads(run.call_args.kwargs["input"]), payload)
            self.assertIn("--input", run.call_args.args[0])
            self.assertTrue(run.call_args.kwargs["check"])

    def test_cli_api_failure_keeps_existing_file_intact(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "notes.md"
            output.write_text("Existing introduction\n", encoding="utf-8")
            argv = ["release_notes.py", "--repo", REPO, "--tag", "build-3", "--target", SHA,
                    "--intro-file", str(output), "--output", str(output)]
            error = subprocess.CalledProcessError(1, ["gh"], stderr="API unavailable")
            with patch.object(sys, "argv", argv), patch.object(notes.GitHub, "request", side_effect=error):
                with self.assertRaises(subprocess.CalledProcessError):
                    notes.main()
            self.assertEqual(output.read_text(encoding="utf-8"), "Existing introduction\n")


class WorkflowTests(unittest.TestCase):
    def test_workflow_publication_with_fake_github(self):
        """Exercise the actual publisher shell and real helper against a fake gh."""
        root = Path(__file__).resolve().parents[1]
        workflow = (root / "workflows/build_release.yml").read_text(encoding="utf-8")
        publisher = workflow.split("      - name: Publish prerelease\n", 1)[1]
        publisher = publisher.split("        run: |\n", 1)[1]
        script = "\n".join(line[10:] for line in publisher.splitlines())
        fake_gh = '''#!/usr/bin/env python3
import json, os, pathlib, sys
p = pathlib.Path(os.environ["FAKE_STATE"])
state = json.loads(p.read_text())
a = sys.argv[1:]
state["calls"].append(a)
p.write_text(json.dumps(state))
releases = state["releases"]
tag = os.environ["RELEASE_TAG"]
current = next((r for r in releases if r["tag_name"] == tag), None)
if a[0] == "api":
    endpoint = a[1]
    if "releases?" in endpoint: print(json.dumps([releases]))
    elif "/commits/" in endpoint: print(json.dumps({"sha": os.environ["BUILD_SHA"]}))
    elif "/compare/" in endpoint: print(json.dumps({"status": "ahead"}))
    elif endpoint.endswith("generate-notes"):
        if state.get("notes_failure"): sys.exit(1)
        print(json.dumps({"body": "* Merged drawing fix in #42"}))
    else: sys.exit(2)
elif a[:2] == ["release", "view"]:
    if current is None or state.get("view_failure"): sys.exit(1)
    print(str(current["draft"]).lower())
elif a[:2] == ["release", "create"]:
    body = pathlib.Path(a[a.index("--notes-file") + 1]).read_text()
    releases.append({"tag_name": tag, "draft": True, "body": body,
                     "target_commitish": os.environ["BUILD_SHA"],
                     "prerelease": True, "published_at": None})
elif a[:2] == ["release", "edit"]:
    if "--notes-file" in a:
        current["body"] = pathlib.Path(a[a.index("--notes-file") + 1]).read_text()
    if "--draft=false" in a: current["draft"] = False
elif a[:2] == ["release", "upload"]:
    if state.get("upload_failure"): sys.exit(1)
else: sys.exit(2)
p.write_text(json.dumps(state))
'''
        original = notes.generate_notes(FakeGitHub([release("build-1")]), REPO, "build-3", target=SHA)
        cases = {
            "new": {"releases": [release("build-2")]},
            "legacy_draft": {"releases": [release("build-3", draft=True, body="Keep introduction"), release("build-2")]},
            "retry": {"releases": [release("build-3", draft=True, body=original), release("build-2")]},
            "published": {"releases": [release("build-3", body="Published notes")]},
            "notes_failure": {"releases": [release("build-2")], "notes_failure": True},
            "upload_failure": {"releases": [release("build-2")], "upload_failure": True},
        }
        for name, state in cases.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as directory:
                directory = Path(directory)
                state["calls"] = []
                state_file = directory / "state.json"
                state_file.write_text(json.dumps(state), encoding="utf-8")
                executable = directory / "gh"
                executable.write_text(fake_gh, encoding="utf-8")
                executable.chmod(0o755)
                (directory / ".github/scripts").mkdir(parents=True)
                (directory / ".github/scripts/release_notes.py").write_text(
                    (root / "scripts/release_notes.py").read_text(encoding="utf-8"), encoding="utf-8"
                )
                (directory / "release-notes.md").write_text("Build details\n", encoding="utf-8")
                (directory / "out").mkdir()
                (directory / "out/package.zip").write_text("package", encoding="utf-8")
                env = dict(os.environ, PATH=f"{directory}:{os.environ['PATH']}",
                           FAKE_STATE=str(state_file), RELEASE_TAG="build-3", BUILD_SHA=SHA,
                           GH_REPO=REPO, GITHUB_SERVER_URL="https://github.com",
                           GITHUB_REPOSITORY=REPO, GITHUB_STEP_SUMMARY=str(directory / "summary.md"))
                result = subprocess.run(["bash", "-c", script], cwd=directory, env=env,
                                        text=True, capture_output=True)
                final = json.loads(state_file.read_text(encoding="utf-8"))
                failed = name in ("notes_failure", "upload_failure")
                self.assertEqual(result.returncode == 0, not failed, result.stderr)
                current = next((r for r in final["releases"] if r["tag_name"] == "build-3"), None)
                if name == "notes_failure":
                    self.assertIsNone(current)
                    self.assertFalse(any(a[:2] == ["release", "upload"] for a in final["calls"]))
                elif name == "upload_failure":
                    self.assertTrue(current["draft"])
                elif name == "published":
                    self.assertEqual(current["body"], "Published notes")
                    self.assertEqual(len(final["calls"]), 1)
                else:
                    self.assertFalse(current["draft"])
                    self.assertIn("Changes since", current["body"])
                    if name == "retry":
                        self.assertEqual(current["body"], original)
                    if name == "legacy_draft":
                        self.assertTrue(current["body"].startswith("Keep introduction\n"))

    def test_manual_status_api_failure_stops_the_step(self):
        root = Path(__file__).resolve().parents[1]
        workflow = (root / "workflows/publish_release.yml").read_text(encoding="utf-8")
        block = workflow.split("      - name: Add changelog to draft release\n", 1)[1]
        block = block.split("        run: |\n", 1)[1].split("\n      - name:", 1)[0]
        script = "\n".join(line[10:] for line in block.splitlines())
        with tempfile.TemporaryDirectory() as directory:
            gh = Path(directory) / "gh"
            gh.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
            gh.chmod(0o755)
            env = dict(os.environ, PATH=f"{directory}:{os.environ['PATH']}", TAG="v0.1.0")
            result = subprocess.run(["bash", "-c", script], env=env, capture_output=True)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
