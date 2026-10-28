# Daily and manual releases

The **Build and Release** Actions workflow builds Windows x64, macOS Intel
x86_64, and Linux x86_64 using the existing platform workflows. All three build
the same commit. A successful run publishes a development prerelease with a
Windows ZIP, macOS DMG, Linux AppImage tarball, and SHA-256 checksums.

- **Daily:** 08:17 UTC, from the latest commit on the default branch (`edition`).
  GitHub may delay scheduled runs during busy periods.
- **Manual:** Actions > Build and Release > Run workflow. Select a branch and
  leave **Publish a prerelease after all three builds succeed** checked. Clear
  it to build downloadable Actions artifacts without publishing a release.

The workflow must be merged into the default branch for the schedule and
manual control to become available.

Each run receives a unique `build-<run ID>` tag pointing to the built commit.
Releases remain prereleases and do not replace the latest stable release.
Earlier releases are retained. Failed or cancelled builds publish no release;
an interrupted upload can leave a draft, which a rerun resumes. Rerunning an
already published run leaves its release intact.

Release runs do not interrupt one another. Their Linux builds use a separate
concurrency group from the existing Linux pull request and branch checks.
Only the publishing job receives release write permission, using GitHub's
provided token; no additional secret is required.

## Automatic changelog

Each new release includes a **Changes since <previous tag>** section with
GitHub's generated merged pull request list and a full source comparison link.
The existing commit, platform, and build information remains above it. Direct
commits can be found through the full comparison link.

For `build-<run ID>` releases, the baseline is the most recently published
Groundzero build or stable `v<version>` release whose tagged commit is an
ancestor of the exact build commit. Daily prereleases are included; drafts,
unrelated tags, and the current release are excluded. The release list is
paginated and sorted by publication time. Tags are resolved through GitHub;
moving branch names recorded in release metadata are not comparison boundaries.

For stable `v<version>` releases, only earlier published stable versions are
eligible, so the notes cover changes since the previous stable version rather
than the last daily build. The manual **Publish Release** workflow adds these
notes to an existing draft using its tagged source commit, preserving any
maintainer-written introduction. It still only uploads assets and does not
publish the draft. Already published release text is left unchanged.

The generated notes record the baseline tag and target SHA in an HTML comment.
Draft retries retain the original notes and baseline, even if another release
has since appeared. Legacy drafts receive a changelog once. An already published
Build and Release run remains unchanged on rerun.

If the source commit is unchanged, the notes say **No source changes since the
previous release**; the daily release still publishes. If there is no previous
release in that channel, the notes identify it as the initial release and link
to the source commit. They do not generate a changelog spanning imported history.
If previous releases exist but none is an ancestor, or any required API request
fails, publication stops rather than using an unrelated or incomplete comparison.

The helper uses the GitHub CLI and the existing Actions token, with no additional
dependencies or secrets. Both publication workflows share a concurrency group
to serialize baseline selection and publication. Changelog fixtures run on PRs
and default-branch changes to the release tooling, independently of native builds:

```sh
python3 -m unittest discover -s .github/scripts -p 'test_release_notes.py' -v
```

