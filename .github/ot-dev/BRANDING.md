# OAL Groundzero Windows build branding

Groundzero Windows CI packages include the splash, live PR/build information
in the title bar, and the opt-in session recorder. This infrastructure belongs
to `OpenAnimationLibrary/groundzero` and stays out of upstream proposals.

The application display name is **OAL Groundzero**. Local and non-Windows builds
use the compiled Groundzero splash and About text; the live build overlay and
session recorder apply only to Windows CI packages.

## Replace the artwork

| Package path | Purpose |
| --- | --- |
| `OpenToonz.exe` | Groundzero application; executable name retained for compatibility |
| `splash.svg` | Stable layout with populated Groundzero PR/build text |
| `portablestuff/splash.png` | Replaceable artwork |
| `otdev-recorder/` | Encoder, matching sources, licenses, and README |
| `otdev-build.json` | Source, infrastructure, and package identity |

Replace `portablestuff/splash.png` with a valid PNG and restart the application.
No SVG edit or rebuild is needed. Both external assets resolve from the
executable directory. The compiled splash embeds the same artwork and build
metadata as a fallback when either external file is missing.

For future builds, replace `.github/ot-dev/ot-dev-splash-artwork.png`.
Edit `splash-template-area10.svg` only to change its layout or templating.
The original artwork was supplied by Rodney Baker. The preserved legacy SVG
layouts remain reference assets; they are not the active Groundzero template.

## Build coverage

`workflow_windows.yml` builds PRs targeting `edition` and builds pushed to
`edition`. It applies and verifies the complete Groundzero package directly.

**Groundzero Windows Package** (`otdev_branding.yml`) also follows successful
`Windows Build` runs. It resolves the exact application source and uses separate
infrastructure from the default `edition` branch. It skips source runs that
already published a complete verified package, and rejects stale or ambiguous
PR sources. Its own worker name is excluded from the source workflows to avoid
loops. The workflow must exist on the default branch to receive `workflow_run`
events; a PR alone does not activate that listener.

Complete application artifacts are named:

- `Groundzero-PR-N-Windows-SOURCE_SHA-RUN_ID`
- `Groundzero-Branch-REF-Windows-SOURCE_SHA-RUN_ID`

The source workflow uses `Branch-build` and records the actual ref in the
manifest. Supporting encoder/branding artifacts are not complete applications.
The **Groundzero Windows package** commit status links to the follow-up result.

For manual requests, use **Actions → Groundzero Windows Package → Run workflow**
and supply exactly one of `pr_number` or `ref`. The inherited
`otdev-build/pr-NUMBER` branch route also remains available.

## Compatibility and verification

Internal paths, build `OTDEV_*` environment variables, and the
manifest's `full_otdev_build` compatibility field retain their existing names.
Product labels and repository routing use Groundzero. This avoids a migration
of preferences, recordings, executable paths, or consumers of existing metadata.
Recording starts off on every launch. Users can start and stop from the bottom
toolbar, with consent requested on the first start of each session. Legacy saved
recording choices no longer start capture automatically.

```sh
python -m pip install PyQt5==5.15.2
QT_QPA_PLATFORM=offscreen python -m unittest discover -s .github/ot-dev -p 'test_*.py' -v
python -m unittest discover -s .github/ot-dev/recorder -p 'test_*.py' -v
actionlint .github/workflows/workflow_windows.yml .github/workflows/otdev_branding.yml
```

Checks cover SVG rendering and replaceable artwork, source identity, package
completeness, and recorder injection. Windows CI additionally compiles the
recorder and application and tests native playback. Application workflow testing
and a real Windows package remain necessary before release publication.
