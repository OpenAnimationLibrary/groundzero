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
