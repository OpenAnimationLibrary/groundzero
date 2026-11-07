# Workflow security audit — 2026-11-07

## Scope and evidence

Reviewed all 16 workflows remaining after CI consolidation, including reusable
jobs, release publishing, dependency downloads, credentials, shell interpolation,
artifact packaging, and the stylesheet/wiki toolchain. The source baseline is
`2e52fb78b70915925393290f32dc09a95a3927ec`. This report covers source and static
analysis; no application build, runtime test, or workflow dispatch ran locally.

The initial scan of the consolidated topology reported 166 findings:
120 unpinned action references, 12 persisted-checkout credential findings,
21 CMD-shell findings, seven same-repository reference findings, five shell
template-interpolation findings, and one ad-hoc npm installation finding.
This differs from the pre-consolidation scan because four workflows were removed.

## Changes

- Pinned all 121 final external action references to full commit hashes. Release
  tags and annotated-tag targets were resolved through the owning repositories'
  GitHub API on the audit date; version comments remain beside every reference.
- Disabled persisted credentials on all 25 checkouts. Existing release commands
  authenticate through their explicit job-scoped `GH_TOKEN`, without git writes.
- Routed comparison outputs through environment variables before shell use.
  This avoids interpreting output text as shell source in the Linux Compat job.
- Scoped write permissions to the jobs that need them and documented the grants.
  The auditor-persona review also replaced 16 shell interpolations of matrix,
  repository, environment, and outcome values with quoted environment references.
- Serialized No PII runs per pull request without cancelling an active run,
  preventing two workflow scans from updating the same description concurrently.
- Corrected ShellCheck findings, including unsafe filename interpolation in the
  macOS Python-symlink cleanup and unquoted toolchain paths.
- Made Windows dependency installs, copies, extraction, directory changes, and
  Qt deployment fail the step when their command fails. Previously a successful
  later command could hide an earlier failure in the same CMD script.
- Copied the existing libmypaint dependency license tree into the Windows
  package, retaining its AUTHORS files. A package step checks all seven required
  notice/author files are present and nonempty. Matching binary provenance remains
  a separate unresolved question; copying notices does not establish provenance.
- Locked the existing Less and TiddlyWiki tools, including 21 package entries and
  integrity hashes, under `ci-scripts/style-toolchain`. CI uses Node 22 and
  `npm ci --ignore-scripts`; automatic setup-node caching is disabled.
- Added grouped weekly Dependabot updates for actions and the toolchain, with
  a seven-day cooldown for version updates. GitHub security updates do not obey
  that version-update cooldown.
- Included `externfx_process`, `plugin_probe_single`, and `plugin_probe_empty`
  in explicit platform regression lists, and their targets in the Linux build.
- Enabled deformer and Auto-Patch test targets in all three platform configurations
  and added their CTest names to the regression lists. The direct `tiio_ffmpeg`
  suite remains Linux/macOS-only: its CMake registration excludes MSVC because
  the tested classes lack DLL exports. The platform follow-up adds HDR source
  preservation and output assertions through the public `ffmpeg_public` fixture,
  which is selected on all three platforms. Execution remains pending CI.
- Added actionlint and zizmor steps to the existing Linux job. Each runs after
  native packaging even when unrelated checks fail, reports its own failures,
  and retains its log. Both release archives are SHA-256 checked before
  extraction; the workflow still creates only ten automatic PR checks.

The application behavior and invocation of legacy external effects are reviewed
separately from workflow security. New fixtures must be included with this patch.

## Action provenance

The listed stable releases were checked in each owning repository's
`releases/latest` response, then resolved with `git/ref/tags/<version>` and the
annotated tag object when applicable. The Rust toolchain action uses its documented
rolling `stable` branch instead of a release tag; that branch was resolved once.

| Action repository | Release | Commit |
| --- | --- | --- |
| actions/checkout | v7.0.1 | `3d3c42e5aac5ba805825da76410c181273ba90b1` |
| actions/cache | v6.1.0 | `55cc8345863c7cc4c66a329aec7e433d2d1c52a9` |
| actions/upload-artifact | v7.0.2 | `cf430e030ddbb5b0abf93d22962f4752f3646cd9` |
| actions/download-artifact | v8.0.2 | `9000827ccba6bdab643e8b6fd33ac0654aef8333` |
| actions/setup-python | v7.1.0 | `6d4fd22b20465ccc0f7354476588e8df7c9ca0e0` |
| actions/setup-node | v7.1.0 | `949feb2413d6458794dcd2491c4babbbce0c15c1` |
| actions/github-script | v9.0.0 | `3a2844b7e9c422d3c10d287c895573f7108da1b3` |
| actions/configure-pages | v6.0.0 | `45bfe0192ca1faeb007ade9deae92b16b8254a0d` |
| actions/upload-pages-artifact | v5.0.0 | `fc324d3547104276b827a68afc52ff2a11cc49c9` |
| actions/deploy-pages | v5.0.1 | `368f82528645a54fb793d4d04e342629a3f51346` |
| Swatinem/rust-cache | v2.9.3 | `d1527c8f67ef9af357290475d4af4b0fab8f12e0` |
| msys2/setup-msys2 | v2.33.0 | `ec48f7c5447b3140e2b088413ae3a55687bccb6e` |
| microsoft/setup-msbuild | v3 | `30375c66a4eea26614e0d39710365f22f8b0af57` |
| dtolnay/rust-toolchain | stable | `b725eda525362e7cf34af9a7243cac1e3f01a7ec` |

Read the corresponding release notes before future major-version updates.
The reviewed action changes use hosted runners with Node 24 support. The
github-script invocation does not use the removed `require('@actions/github')`
interface; setup-node does not configure registry authentication. Runtime
compatibility of this combined change still requires CI.

Less 4.9.1 was checked against the [official npm metadata][less]. TiddlyWiki
5.4.1 retains the existing version. The lock was resolved with a publication
cutoff of `2026-11-06T20:40:00Z`, at least 24 hours before resolution. Future
manual lock refreshes must use an equivalent cutoff; ordinary CI installs the
committed lock without resolving new versions. The seven-day Dependabot policy
provides a longer delay for automatic version updates.

The static CI tools use [actionlint 1.7.12][actionlint] and
[zizmor 1.32.0][zizmor]. Their archive digests were read from the owning GitHub
release metadata, and the actionlint digest also matches its published checksum
file. The downloaded zizmor archive was independently hashed and its upstream
build workflow targets Ubuntu 22.04, matching this project's Linux runner.

## Explicit scanner exceptions

The final offline zizmor scan using its default `regular` persona reports
**28 ignored and 19 suppressed findings**, with no emitted findings. The broader
`auditor` persona reports **32 ignored and 15 informational findings**. The four
additional local exceptions are persona-suppressed before ignore processing in
the regular scan. Across both personas there are 32 local, documented exceptions,
not fixed security defects:

- 21 inherited Windows CMD steps retain their current logging and exit behavior.
  Their inputs are fixed workflow values and runner environment paths, not PR
  titles, descriptions, or arbitrary user-provided command text. Intermediate
  failures now abort the affected install/copy/navigation steps. Migrating these
  scripts requires Windows execution evidence; an ignore is attached only to
  each specific `shell: cmd` declaration.
- Seven reusable calls use `./.github/workflows/...` so each call stays on the
  running revision. GitHub [supports the newer `$/` syntax][self-reference], but
  actionlint 1.7.12 rejects it. The exceptions preserve static lint coverage until
  that parser supports the new syntax. No broad zizmor rule is disabled.
- Three manual/reusable workflows intentionally allow independent concurrent runs;
  their resource-policy reasons are recorded inline and described below.
- The recorder build uses the required MSYS2 shell supplied by setup-msys2;
  the local shell exception names that dependency and its UCRT64 purpose.

The `auditor` persona was run separately to inspect everything hidden by the
default persona. Initially it exposed 45 additional findings. After the follow-up
changes, its 15 remaining findings are classified below; it exits nonzero, so the
default-persona success must not be described as an auditor-persona clean result.

| Category | Initially hidden | Remaining | Disposition |
| --- | ---: | ---: | --- |
| Template interpolation | 16 | 0 | Replaced with quoted environment references |
| Undocumented permissions | 6 | 0 | Added purpose comments to grants |
| Workflow-level write permissions | 3 | 0 | Scoped to the intended job |
| Unnamed jobs | 14 | 14 | Informational; existing job IDs/check names retained |
| Missing concurrency setting | 4 | 0 | No PII serialized; three policy exceptions |
| Extra setup action | 1 | 1 | Informational; pinned Rust setup retained |
| Custom shell | 1 | 0 | Local exception for required MSYS2 launcher |

The 14 unnamed-job findings concern display names rather than missing workflow
identities. Changing display names can affect required-check configuration, so
they are recorded instead of silently renaming the existing checks.

No PII now uses a per-PR concurrency group with `cancel-in-progress: false` so its
description updates are serialized. The three resource-policy exceptions are
Cadmium runtime, Paired Benchmarks, and Linux Qt 6. Native callers already have
their own concurrency policies; manual runs use isolated hosted runners and
run-scoped artifacts. They intentionally remain independent; simultaneous manual
runs can consume additional resources. An ignore is attached only to each
workflow's trigger declaration, with the reason beside it.

The Rust setup action is pinned to a reviewed commit and retains the existing
toolchain setup semantics. Replacing it with direct rustup commands is a possible
cleanup, not an unpinned dependency in this diff. The custom `msys2 {0}` shell is
provided by the pinned setup-msys2 action and is necessary for the recorder's
UCRT64 build environment; it is not arbitrary user-supplied shell text.

Four additional local ignores address the three resource-policy findings and
the required MSYS2 shell. The 15 informational findings remain visible under the
auditor persona. The existing CI step uses the default persona; the broader
result is disclosed here explicitly.

## Remaining findings and limits

1. **P2 — native download provenance:** Linux packaging executes three AppImages
   downloaded from moving `continuous` release URLs in `workflow_linux.yml`.
   Windows downloads the custom Qt archive and third-party library archive without
   a locally pinned digest in `workflow_windows.yml`. Pinning Actions does not
   authenticate these payloads. Obtain reviewed source/build provenance and
   trusted digests, then pin and verify them before extraction or execution.
   The existing Mesa download already checks a SHA-256 digest.
2. **P2 — incomplete dependency locking:** the optional SeExpr qualification
   installs exact PyQt top-level versions without a full hashed dependency lock.
   OS package repositories, Homebrew, Chocolatey and the Rust stable toolchain
   also remain external mutable inputs. These were inspected but not replaced
   with speculative toolchain migrations during this patch.
3. **Runtime validation pending:** new action majors, Windows command propagation,
   notices in the final archive, current Less output, and the new C++ fixtures
   require successful CI. No existing failing native application check is claimed
   fixed by these workflow edits.

## Validation performed

- actionlint 1.7.12, including ShellCheck: no diagnostics across all workflows.
- zizmor 1.32.0 offline regular scan: exit 0, 28 ignored and 19 suppressed.
  The separate auditor-persona scan has 32 ignored and emits 15 informational findings;
  no unsuppressed low, medium, or high-severity findings
  remain in that offline scan. No claim of a clean auditor-persona result is made.
  Online vulnerability and commit-provenance audits were not executed by zizmor;
  action commit resolution was checked separately through primary GitHub APIs.
- `ruby ci-scripts/check_ci_budget.rb`: 10/10 potential automatic PR checks
  (Windows four, macOS three, Linux two, No PII one).
- npm lock resolution and `npm audit --audit-level=moderate`: no vulnerabilities
  reported for the locked toolchain at audit time. No package scripts ran.
- `git diff --check` and the repository's no-PII scan: passed.

[less]: https://registry.npmjs.org/less/4.9.1
[actionlint]: https://github.com/rhysd/actionlint/releases/tag/v1.7.12
[zizmor]: https://github.com/zizmorcore/zizmor/releases/tag/v1.32.0
[self-reference]: https://github.blog/changelog/2026-07-30-reference-same-repository-actions-with-self-repository-syntax/

## Windows native diagnostic follow-up

The [baseline Windows failure][native-baseline] reached a Qt text destructor
through workflow room teardown. Its full-page-heap replay exited with
`0xC0000421` without the application's crash report. The existing artifact glob
also omitted `ui-integration-page-heap.txt`. These are diagnostic gaps; the
underlying application failure remains unresolved by this change. The separate
bare `SceneViewerPanel` fixture failure observed on PR #141 is not evidence for
the baseline Qt text failure's cause.

The existing single page-heap replay now runs under CDB. It intercepts access
violations, breakpoints, heap corruption, and verifier stops, then records the
exception context, registers, stack, loaded modules, and other thread stacks.
It uses the existing process capture and Windows job containment, with a
maximum 1,500-second replay and the existing 15-second tree-retirement bound.
Unconfirmed retirement retains fixture resources and is reported explicitly.
Initial `!gflag`, `!heap`, and verifier-extension queries are omitted: an earlier
retained diagnostic stalled before the application ran while issuing such
queries. Registry configuration alone is labelled as unverified runtime
activation, and CDB's exit status cannot qualify native acceptance.

The helper snapshots and restores the exact values and types of the two IFEO
values it owns, including after partial setup or capture failure. It preserves
unrelated values and never deletes the entire image key. Restoration failures
are reported individually. The Windows workflow now retains this text report
and exposes the already-existing original-process dump/timeout text reports.
Eight mocked-boundary contracts run in the existing Windows job; no PR checks
or duplicate application replays are added. Later independent suites retain
their existing conditions and the original native failure remains authoritative.

This path requires access to create/query/set values in the 64-bit HKLM IFEO
image key and an installed x64 Windows SDK CDB. Denied registry access or a
missing debugger produces a diagnostic setup report; this change does not
elevate permissions or install another debugger. CDB uses only local symbols,
so absent Windows or Qt PDBs can limit symbol names. No new raw dump artifact
is published. Microsoft documents the [CDB options][cdb-options] and why
[verifier stops require debugger inspection][verifier-stops].

Static Ruff, Windows-platform ty, actionlint, and the ten-check budget passed.
The offline auditor scan retains the previously documented 15 informational
findings and 32 narrow ignores, with no low, medium, or high findings. No local
build or test was run. Windows execution, registry privileges, CDB stop capture,
and the new contracts remain pending CI. PR #141 has an overlapping page-heap
implementation; reconcile that proposal separately before combining changes.

[native-baseline]: https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42297300171/job/128167284089
[cdb-options]: https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/cdb-command-line-options
[verifier-stops]: https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/application-verifier-debugging-application-verifier-stops
