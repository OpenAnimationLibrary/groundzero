#!/usr/bin/env python3
"""Generate release notes with an explicit, ancestry-checked release baseline."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import quote


MARKER = "<!-- groundzero-changelog:v1 "


class GitHub:
    def request(self, endpoint, *, data=None, paginate=False):
        command = ["gh", "api", endpoint]
        if paginate:
            command += ["--paginate", "--slurp"]
        if data is not None:
            command += ["--method", "POST", "--input", "-"]
        result = subprocess.run(
            command,
            input=json.dumps(data) if data is not None else None,
            text=True,
            capture_output=True,
            check=True,
        )
        return json.loads(result.stdout)


def commit_sha(api, repo, ref):
    return api.request(f"repos/{repo}/commits/{quote(ref, safe='')}")["sha"]


def release_channel(tag):
    if re.fullmatch(r"build-[0-9]+", tag):
        return "build"
    if re.match(r"v[0-9]", tag):
        return "stable"
    raise ValueError("Expected a build-<run ID> or v<version> release tag")


def generate_notes(api, repo, tag, *, target=None, intro="", existing_only=False):
    """Return notes; never edit a release or change an existing generated body."""
    channel = release_channel(tag)
    pages = api.request(f"repos/{repo}/releases?per_page=100", paginate=True)
    releases = [release for page in pages for release in page]
    current = next((release for release in releases if release["tag_name"] == tag), None)
    if existing_only and current is None:
        raise ValueError(f"Release {tag} does not exist")
    if current is not None:
        body = current.get("body") or ""
        if not current["draft"]:
            return body
        if MARKER in body:
            metadata = json.loads(body.split(MARKER, 1)[1].split(" -->", 1)[0])
            if target is not None and metadata["target_sha"] != target:
                raise ValueError("Draft notes describe a different build commit")
            return body
        # Preserve a maintainer's introduction, including legacy draft notes.
        intro = body or intro

    if target is not None:
        if not re.fullmatch(r"[0-9a-f]{40}", target):
            raise ValueError("The build target must be a full commit SHA")
        if current is not None:
            # Draft creation may not have created its git tag yet.
            current_target = commit_sha(api, repo, current["target_commitish"])
            if current_target != target:
                raise ValueError("Existing draft targets a different build commit")
    else:
        # Manual asset publication must describe the tagged source, not HEAD.
        target = commit_sha(api, repo, tag)

    candidates = []
    for release in releases:
        candidate_tag = release["tag_name"]
        if release["draft"] or candidate_tag == tag:
            continue
        is_build = re.fullmatch(r"build-[0-9]+", candidate_tag)
        is_stable = re.match(r"v[0-9]", candidate_tag) and not release["prerelease"]
        if is_stable or (channel == "build" and is_build):
            candidates.append(release)
    candidates.sort(key=lambda release: release["published_at"], reverse=True)

    baseline = None
    comparison = None
    for release in candidates:
        # Resolve the tag through compare, rather than using target_commitish:
        # the latter can be a moving branch name such as "edition".
        base_tag = release["tag_name"]
        comparison = api.request(
            f"repos/{repo}/compare/{quote(base_tag, safe='')}...{target}"
        )
        if comparison["status"] in ("ahead", "identical"):
            baseline = base_tag
            break
    if candidates and baseline is None:
        raise ValueError("No published release is an ancestor of the build commit")

    if baseline is None:
        changelog = (
            "## Changes\n\nInitial release in this channel; no previous published "
            "release to compare.\n\n"
            f"Source: https://github.com/{repo}/commit/{target}"
        )
    else:
        changelog = f"## Changes since {baseline}\n\n"
        if comparison["status"] == "identical":
            changelog += "No source changes since the previous release."
        else:
            generated = api.request(
                f"repos/{repo}/releases/generate-notes",
                data={
                    "tag_name": tag,
                    "target_commitish": target,
                    "previous_tag_name": baseline,
                },
            )["body"].strip()
            if not generated:
                raise ValueError("GitHub returned empty generated release notes")
            changelog += generated
        # Always expose direct commits as well as GitHub's merged PR list.
        changelog += (
            f"\n\n[Full comparison](https://github.com/{repo}/compare/"
            f"{quote(baseline, safe='')}...{target})"
        )

    metadata = json.dumps({"base_tag": baseline, "target_sha": target}, sort_keys=True)
    parts = [intro.rstrip(), changelog, f"{MARKER}{metadata} -->"]
    return "\n\n".join(part for part in parts if part) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=os.environ.get("GH_REPO"), required=False)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--target", help="Exact 40-character build commit SHA")
    parser.add_argument("--intro-file", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--existing-only", action="store_true")
    args = parser.parse_args()
    if not args.repo or not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repo):
        parser.error("Set GH_REPO or provide --repo owner/repository")
    intro = args.intro_file.read_text(encoding="utf-8") if args.intro_file else ""
    # Write only after every API call succeeds, even when intro and output coincide.
    notes = generate_notes(
        GitHub(), args.repo, args.tag, target=args.target,
        intro=intro, existing_only=args.existing_only,
    )
    args.output.write_text(notes, encoding="utf-8")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, subprocess.CalledProcessError) as error:
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr, file=sys.stderr)
        sys.exit(f"Release notes failed: {error}")
