# Groundzero source, reliability, and CI audit

Audit baseline: `2e52fb78b70915925393290f32dc09a95a3927ec` on `edition`, November 7, 2026.
Prepared with Codex assistance. This is an engineering source audit and remediation record,
not an independent certification or a release approval.

The integrated changes are published for validation in
[draft PR 142](https://github.com/OpenAnimationLibrary/groundzero/pull/142).
The draft status does not establish runtime correctness or readiness to merge.

## Assessment

The immediate risks are artwork integrity, undo identity, and task lifetime. The audit found
paths that truncate an existing PSD before a failed save, change shared HDR source pixels
during export, associate the wrong image with a lettered drawing on undo, and dereference
deleted task objects. These findings have identifiable source paths; their new regression
cases still require CI execution.

The proposed CI topology reduces the worst-case automatic PR budget from 31 job checks to
10, including matrix expansion and reusable workflow jobs. Consolidated steps preserve
Linux coverage; additional platform/backend matrices move to scheduled or manual runs.
This is a change in qualification timing, not evidence that those configurations pass.
See [the topology and coverage map](ci_audit_topology.md).

Release readiness remains unestablished. Existing native application failures, batch-lifetime
qualification, binary provenance, and platform evidence gaps require further work. A green
build from a different run cannot establish that an intermittent crash is fixed.

## Scope and method

Ten review tracks were assigned across a maximum of eight concurrent agents, each using an
isolated worktree. The coordinating review integrates patches and checks interactions.

| Track | Reviewed boundaries |
| --- | --- |
| Architecture and integration | Ownership, file transactions, test registration, patch interactions |
| CI topology | PR triggers, matrices, reusable jobs, failure propagation, coverage budget |
| Task reliability | Selection, dependencies, removal, farm mappings, worker lifetime, movie chunks |
| File safety | PSD output transactions, layer readers, failure preservation |
| Rendering | HDR conversion, deformation weights, numeric bounds, Auto-Patch support sizes |
| Undo and timeline | Group restoration, frame renumbering, clipboard and undo cache identity |
| Plugins and processes | Plugin discovery, native probes, PureRef boundaries, external FX launch |
| Platforms and dependencies | Helper discovery, CMake configuration, native link inputs, notices |
| GitHub and historical findings | Open upstream bugs and the October audit at the current baseline |
| CI diagnostics and workflow security | Failure attribution, pagination, action provenance, credentials |

The baseline has 12,621 tracked files, including 1,478 `.cpp`, 1,667 `.h`, and 1,359 `.c`
files across application and bundled dependencies. These are inventory counts, not reviewed
line counts. The review follows selected callers, ownership, error paths, and existing
mitigations; it does not claim exhaustive inspection of every codec, FX, dependency, or
artist workflow. No penetration test, performance profile, or package certification was run.

This is a C++/Qt/CMake application. Its 79 `.ts` files are Qt translation XML, not TypeScript.
All 79 parsed successfully in the static XML check. Translation accuracy, placeholder
semantics, and compiled UI behavior were not established by that check.

The review order was architecture, code quality, tests, then performance. P1 denotes crash,
edit-loss, or wrong-artwork risk; P2 denotes narrower correctness, reliability, or maintenance
risk. These are project priorities, not CVSS ratings. "Source-confirmed" means a defective
operation and reachable caller or invariant were traced. "Patch prepared" does not mean
the regression ran, the binary compiled, or the issue is closed upstream.

## Architecture findings

The application combines Qt UI controllers, mutable scene/level objects, reference-counted
images, raw task ownership, and asynchronous workers. The defects cluster at their boundaries:

- A render/export consumer must not mutate a raster shared with the scene or image cache.
- Frame numbers alone do not identify drawings: letter suffixes, order changes, and separate
  undo entries all matter. Undo must preserve identity through renumbering.
- Task status is not an ownership contract. A queued worker can retain an object even when
  the UI marks it suspended. Removing an index entry does not retire that borrow.
- Successful output must mean bytes were written and committed. Failure must preserve the
  existing destination and enough state to retry.
- Fewer check names do not justify less evidence. Consolidated jobs need independent steps,
  retained diagnostics, and explicit failures.

The patches target these boundaries. They do not migrate Qt, replace the scene model, change
rendering policy, or duplicate the GL ownership work in PR 141 or memory reporting in PR 133.

## Findings and remediation register

All entries below are source-confirmed, with patches prepared. Added regression cases and
remaining configuration checks still require CI; none is marked closed. Related variants
share a finding ID rather than inflating the count for each affected command.

| ID | Priority | Defect and resulting behavior | Implementation / regression |
| --- | --- | --- | --- |
| F01 | P1 | PSD save truncated existing artwork before writes could fail. Checked temporary output now commits atomically after a successful flush. | `image/psd/tiio_psd.cpp`, `image/tests/tiio_psd_test.cpp` |
| F02 | P2 | Creating a PSD frame reader changed the layer used by another reader. Each reader now retains its own layer ID. | Same PSD implementation and fixture |
| F03 | P1 | Removing a selected task group freed a selected child before the selection loop read it. Removal snapshots IDs first and tolerates a missing parent. | `toonz/tasksviewer.cpp`, `toonz/batches.cpp`, native UI fixture |
| F04 | P1 | A missing dependency dereferenced a null task; dependency items also retained borrowed pointers. Items now retain IDs and missing references remain removable. | `toonz/tasksviewer.cpp`, native UI fixture |
| F05 | P1 | Deleted tasks left farm mappings, causing null dereferences or reuse of an old farm job. Removal retires mappings and polling checks existence before copying. | `toonz/batches.cpp`, native UI farm-boundary fixture |
| F06 | P1 | A suspended group could be deleted while a child was waiting/running. Active descendants now block group deletion. | `toonz/batches.cpp`, native UI fixture; queued-worker lifetime is F27 |
| F07 | P1 | Batch MP4/WebM/APNG jobs could split into chunks writing one movie destination. New tasks use the shared movie classifier. | `toonz/batches.cpp`, saved-scene batch fixture; upstream #4629 |
| F08 | P1 | FFmpeg export clamped shared HDR source rasters in place. Conversion now leaves input untouched and uses the existing output clamp. | `image/ffmpeg/tiio_ffmpeg.cpp`, public FFmpeg fixture on all platforms; duplicate new direct HDR case consolidated with preserved alpha/reused-source assertions |
| F09 | P2 | Painted deformation grids accepted invalid geometry/counts and converted brush bounds before clipping. Bounds are validated before integer operations. | `toonzlib/tdeformerstack.cpp`, `toonzlib/tests/deformer_test.cpp` |
| F10 | P2 | Auto-Patch outline support could overflow integer dimensions and allocation arithmetic. Unrepresentable requests now fail with an actionable error. | `stdfx/autopatchfx.cpp`, `stdfx/autopatchoutline.h`, helper fixture |
| F11 | P1 | Lettered drawing renumbering changed sorted order, so undo restored the wrong images. Duplicate/Add/Paste/Insert preserve the inverse identity mapping. | `toonz/filmstripcommand.cpp`, `toonz/drawingdata.cpp`, vector workflow fixture; upstream #2426 |
| F12 | P1 | Numeric-only or shared cache keys confused different drawings and undo entries; blank raster redo aliased images. Cache identities now preserve each drawing and history entry. | Filmstrip/clipboard implementation and vector workflow fixture |
| F13 | P2 | Undoing Ungroup merged originally separate groups. Undo restores the individual ranges, including nested groups. | `tnztools/imagegrouping.cpp`, vector workflow fixture; upstream #1840 |
| F14 | P2 | Empty FFmpeg/ffprobe/Rhubarb preferences indexed an empty string and disagreed with discovery. Launch now resolves the application directory consistently. | `toonzlib/thirdparty.cpp`, `image/tests/ffmpeg_public_test.cpp` |
| F15 | P2 | Windows PNG initialization checked a directory instead of the missing header; TIFF copying exceeded the advertised CMake minimum. Header checks and compatible copies are corrected. | `toonz/sources/CMakeLists.txt` |
| F16 | P2 | External FX used an uninitialized POSIX argument buffer and unchecked Windows process/expansion results. Launch errors now have explicit handling and cleanup. | `tnzbase/texternfx.cpp`, external-process fixture |
| F17 | P2 | Function-form native probes rejected a valid single-effect range. The loader accepts a nonempty range and has a real plugin-load fixture. | `toonzqt/pluginhost.cpp`, native probe fixtures |
| F18 | P2 | CI diagnosis matched successful checkpoint substrings, hid failures among notices, and omitted later API pages. It now classifies actual failures and follows explicit paginated check/job links. | `ci-scripts/ci_audit.py`, 22 Python cases including MSVC and skipped-check reporting |
| F19 | P2 | Independent PR workflows expanded to 31 checks. Consolidated coverage and a topology guard enforce the requested limit of ten. | Workflows, `ci-scripts/check_ci_budget.rb`, 15 added Ruby cases |
| F20 | P2 | Mutable action references, checkout credentials, shell interpolation, and floating build tools weakened workflow provenance. Actions and tools are pinned and credentials/interpolation narrowed. | Workflows and locked stylesheet/wiki toolchain |
| F21 | P2 | Windows packaging copied libmypaint dependency DLLs without an identified copy of their notice directory. Packaging now carries the supplied notice tree. | Windows workflow; c208 staged notice check passed, extracted-archive/provenance review remains required |
| F22 | P2 | Vector simplification passed an uninitialized palette choice into the raster/vector conversion round trip. The choice now matches the existing rasterization path. | `toonz/cellselection.cpp`; source-validated, palette round-trip execution pending |
| F23 | P2 | Qt 5 queued plugin publication passed an unregistered custom pointer type across threads. The type is now declared and registered before loader execution. | `toonzqt/pluginhost.h/.cpp`, real native probe fixture |
| F24 | P1 | Filmstrip Swing undo left original drawings and synchronized exposures shifted. Its debug clone-count assertion also failed. Undo now restores identity and remembers the applied synchronization mode. | `toonz/filmstripcommand.cpp`, `swingUndoChecks`; numeric/lettered collisions, sparse selections, repeated cycles, preference changes before undo |
| F25 | P2 | Audio import ignored its configured timeout, rejected already-finished successes, and retained stale data on failure. Its reader now uses a bounded wait, validates normal exit, clears failed output, and closes the process. | `toonzlib/thirdparty.cpp`, `verifyAudioLifecycle` and real WAV decoding in `ffmpeg_public` |
| F26 | P2 | Image-side FFmpeg timeout conversion overflowed signed milliseconds for large valid preferences. Conversion now widens before multiplication and saturates while preserving the existing nonpositive fallback. | `image/ffmpeg/tiio_ffmpeg.cpp`, delayed fake-probe regression `testProbeTimeoutConversion` on non-MSVC builds |
| F27 | P1 | Local runners borrowed task graphs after status allowed removal and accessed task/process state across threads. Immutable execution plans, pending identity records, queued outcomes and worker-owned processes now separate cancellation from retirement. | `toonz/batches.cpp`, `toonz/batchtaskexecution.cpp`, guarded task-sheet edits and gated native fixtures; runtime qualification pending |
| F28 | P2 | Bulk clear/load dropped owner roots and discarded lists before failed replacement. Actual ownership-root cleanup and staged loading preserve prior state and historical pointer aliases. | `toonz/batches.cpp`, normal-owner/load fixtures; malformed nested extraction ownership remains deferred |
| F29 | P2 | External-effect decoding replaced only a local pointer, leaving output pixels unapplied. Matching 32-bit RGBA output now copies into the existing tile; invalid size/type is rejected. | `tnzbase/texternfx.cpp`, public `externfx_process` pixel cases; old-scene comparisons and 64-bit/float destination support remain open |
| F30 | P1 | A pending workflow timer could delete a lazy Room inside its own initialization event loop. Initialization now holds the switching guard and reschedules the latest pending request afterward. | `toonz/mainwindow.cpp`, real Room/timer fixture; W4 is distinct from the unresolved eager-room W1 crash |
| F31 | P1 | Each split vector Brush undo record claimed frame creation, so reverse undo erased the frame before the first record restored renumbered drawings. Only the first split now owns creation. | `tnztools/toonzvectorbrushtool.cpp`, real Break-on/Auto-Group-off gestures over blank and held exposures |
| F32 | P2 | Native raster clipboard data was mistaken for its external-image preview. Native payload now takes precedence, preserving type and content. | `toonz/cellselection.cpp`, actual MI_Paste across six indexed/full-color targets |
| F33 | P1 | Cancelling an external-image paste discarded existing redo history when its undo block opened early. The block now starts only after preparation succeeds. | Same paste path; actual chooser Cancel checks history and subsequent redo identity/pixels |
| F34 | P2 | Missing/malformed custom panels and malformed editor templates supplied null loader results to unchecked consumers. The factory now shows a plain-text error; the editor keeps the last valid template path/preview and checks command-preview reloads. | Custom panel manager/editor, real Room loading and template-slot fixture; valid MI_Undo binding retained |

Paths beginning with a library name are relative to `toonz/sources/`. The exact source and
fixture diff is the review artifact. F01 deliberately disables direct-write fallback: a
directory that cannot host a temporary file fails safely. Atomic replacement changes hard-link
behavior; Windows rename failure and file-system-specific behavior remain qualification items.
F07 corrects new task creation; it does not rewrite previously saved chunked task lists.
F10 preserves representable calculations rather than silently capping requested widths.
Its byte limit applies to actual compute tiles; the dry pass may request a larger region
that the predictive cache subdivides. An independent review caught and corrected a proposed
first-pass rejection before this integrated result. Runtime subdivision coverage remains pending.

Detailed ownership decisions are in [the task lifetime review](audit_task_lifetime.md).
Dependency selection and acquisition evidence are in
[the platform review](audit_platform_dependencies.md).
The complete upstream inventory and historical reconciliation are in
[the backlog review](upstream_bug_triage.md).
Workflow provenance and explicit scanner exceptions are in
[the workflow security review](audit_workflow_security.md); plugin trust and output behavior
are in [the plugin boundary review](audit_plugin_boundaries.md).
The follow-up [Swing review](audit_swing_undo.md),
[test lifecycle inventory](audit_test_inventory.md), and
[native crash investigation](audit_windows_crashes.md) record additional scenarios
and distinguish prepared repairs from unresolved application crashes.
The [scenario plan](audit_regression_scenarios.md) and
[machine-readable ledger](audit_findings.json) track all 34 prepared findings,
stable residual IDs, platform limits, required mutations, and exact CI evidence.

## Remaining risks and owner decisions

| Priority / evidence | Remaining work | Recommended decision or evidence |
| --- | --- | --- |
| P1/P2, qualification and residual scope | F27/F28 prepare local batch lifetime and normal list ownership repairs. Nested malformed extraction can still leave an unattached child; legacy late progress and remote restart configuration remain outside the repair. | Run exact-revision gated native/real-child cases and leak detection. Preserve parser new-object/alias provenance before adding exception cleanup; see [task lifetime review](audit_task_lifetime.md). |
| P1, observed CI failure | Native application qualification fails on the reviewed baseline and PR 141 for distinct reasons. | Baseline Windows reaches a Qt text-layout destructor during workspace teardown; its corrupting write is unresolved. PR 141 bypasses viewer initialization in its new fixture; a separate factory-based correction is prepared. Verify each failing path independently. |
| Trust decision, exploitability unresolved | External-FX names select local XML without canonical containment; XML supplies executable commands. Scene resource aliases also intentionally resolve external paths. | Trace malicious-input fixtures and actual write/render paths, then decide trust restrictions while preserving explicitly selected external resources. No standalone scene RCE was demonstrated. |
| P2, rendering qualification | F29 applies previously discarded external-FX output to 32-bit tiles. Existing scenes can render differently; 64-bit/float destination tiles still return early. | Run new exact-pixel cases and baseline comparisons, compare old scenes, and address unsupported destination modes separately. The earlier process-test pass predates pixel publication. |
| P2, evidence gap | Selected legacy Windows binaries lack a complete source/patch/build/digest record. | Inventory actual link inputs and establish reproducible rebuilds before targeted updates; do not infer CVE applicability from directory names. |
| P2, source-confirmed | Linux packaging uses moving AppImage download URLs; Windows custom Qt and prebuilt-library downloads lack pinned digests. Optional Python/OS/Rust dependency inputs are not fully locked. | Obtain reviewed provenance and trusted digests before pinning native payloads; the action pins and npm lock do not resolve these inputs. |
| P2, dated availability risk | Homebrew plans to disable `qt@5` on 2027-05-19 and `opencv@4` on 2027-07-09. | Select and qualify a supported acquisition path before those dates; migration needs native device, scene, and rendering evidence. |
| P2, workflow/reproduction pending | F31–F33 address narrower split-Brush ownership, native clipboard precedence and Cancel history defects. Exact #5190 exposure/frame shifts and #2445 destination policy remain unverified. | Run the new fixtures, retain new-level/lettered-ID and mixed-column/gap/input gaps, and review OT-Dev PR280 before broader paste-policy changes. |
| Validation gap | Native Apple Silicon, tablet/DPI/audio, long sessions, complete scene key/FX equality, and all 132 upstream reports are not qualified here. | Use the prioritized reproduction queue and exact-build platform evidence; retain unverified status until supported. |

The backlog inventory classifies 124 reports as unverified, three as source-confirmed, four
as source-reviewed but unproven, and one as having a current source mitigation. The October
register has 20 present mitigations and five partial mitigations; neither count constitutes
runtime closure.

## Existing CI failures and interpretation

The inspected baseline reported 26 observed checks across its actual runs. PR 141 reported
41 observed checks, including overlapping/manual qualification. Those observations differ
from the conservative 31-check topology count, which models one automatic PR update with all
relevant path filters matching. Historical/manual runs remain visible after consolidation.

The Windows native application workflow reported status `3221226529` (`0xC0000421`) during
native UI qualification. The original failure did not provide a sufficient crash report;
a workflow-UI diagnostic also exceeded its 120-second budget. On PR 141, native UI failures
also included Linux/macOS `SIGSEGV` (`-11`). The task-panel changes in this patch do not prove
the cause of those failures, and the failure precedes the newly added task fixtures.

A later read-only audit with the corrected reporter still found 26 checks on the baseline
and 41 on PR 141. The currently linked baseline Windows job reports a native failure with
status 1 and a 120-second workflow-UI diagnostic timeout. Preserve the original PageHeap
evidence as a separate observation; the current annotation is not the same crash signature.
The inspected current baseline failure is
[Windows job 128167284089](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42297300171/job/128167284089).
PR 141 still reports a native crash in
[macOS](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42342857600/job/128314492835)
and [Linux](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42342857517/job/128314174502).

The latest 20 failed runs inspected on November 6–7 comprised seven Windows Build, four
Linux Build, two macOS Build, four retained Windows diagnostics, two Raster Brush Parser,
and one Windows workflow heap replay run. Explicit annotations included native crashes,
test failures, compile/link errors, and timeouts. Generic exit-code messages remained
unclassified; cause buckets overlap and must not be counted as separate failed runs.

The same baseline later had a successful scheduled
[Build and Release run](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42383744593).
That result does not clear the earlier failure or validate this diff. PR 141's existing GL
registry/brush ownership changes need their own runtime evidence and remain a separate PR.

The CI auditor itself incorrectly classified successful checkpoint text containing
`cancelExportFidelity` as cancellation and `capture` as a package-network problem because
it matched unbounded substrings. Notices could bury actual failure annotations, and the
first API page could omit failures. These diagnostics defects are part of this remediation.

The visible default-branch ruleset contained a non-fast-forward restriction but no required
check rule. The branch-protection query was unavailable to the connected credential. This
does not establish that the branch has no other protection. Retired check names require an
owner review before merging the topology change; no repository settings were changed.

## Validation policy and release conditions

[CLAUDE.md](../CLAUDE.md) requires builds and tests to run in CI. No application builds,
unit tests, integration tests, mutation tests, or manual artist sessions ran locally.
Later batch-lifetime and external-FX pixel fixtures await execution. Earlier exact-revision
results below cover only the named cases; static checks and source traces remain separate.

The integrated local validation passed whitespace checks, changed-line C++ formatting,
Ruby syntax and the 10/10 topology calculation, actionlint/ShellCheck across all workflows,
Ruff lint/format at the 100-character limit, ty, privacy scanning, and the two configured
pre-commit hooks. The whitespace hook checks the staged diff; the working diff was also
checked separately. The new Python auditor was exercised against the live read-only APIs
as described above. Its 20 unit cases and the 15 Ruby budget cases were not executed locally.
Those two CI steps passed for initial draft head `2758123e8` in
[Linux job 128529701035](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42414551454/job/128529701035).
That evidence predates the follow-up application fixes and the expanded nine/ten/eleven-check
CLI boundary case; it does not qualify the final combined head.

The first full draft run produced exactly ten checks: four successful, three failed,
and three skipped. Skipped installed-application jobs are not passes. macOS passed the
`externfx_process`, `deformer`, `autopatchoutline`, `ffmpeg_public`, `tiio_psd`,
`plugin_probe_single`, and `plugin_probe_empty` CTests at that first head.
The [macOS native job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42414551576/job/128529820741)
still failed the farm-removal scenario; Linux reported the same assertion.
Windows exposed the `far` SDK macro collision in the deformer fixture and library export
definitions leaking into the external-effect test. Linux's older ShellCheck also rejected
an optional benchmark notice expression. Follow-up changes address these failures without
disabling tests; their combined revision requires a new CI run. The source findings remain open.

At `c20884ffb45660f79a7f213329e5e9c1ff2dc479`, task removal/missing dependencies,
farm mapping removal, and active-child protection passed in both
[Linux job 128545710903](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42419847476/job/128545710903) and
[macOS job 128545825336](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42419847532/job/128545825336). These checkpoints cover F03–F06 at that revision.
The Linux reporter passed all 22 cases and the budget suite passed 15 cases with 50 assertions.
macOS passed all 22 required reliability CTests, including PSD, FFmpeg, deformer, Auto-Patch,
external-process and plugin-probe cases. Linux reliability was skipped after the native failure;
these macOS passes do not establish a Linux or Windows result.

Both native jobs still failed `exerciseMovieBatchSetup`: the fixture expected saved AVI to
remain a movie on every platform, but the non-Windows scene loader intentionally converts AVI
to TIF because no AVI writer is registered. The prepared fixture checks the reloaded effective
format, the existing platform conversion, and resulting movie/image chunking. It has not passed
CI yet. Swing/vector workflows were not reached in either full native process. A secondary
Quit assertion followed the failed fixture; each separately launched reviewed-clear Quit
scenario passed. The [ledger](audit_findings.json) records log lines and artifact digests as
CI10–CI16 without replacing the initial-head evidence. F27/F28 and F29 pixel publication were
added after this revision and receive no runtime credit from it.

At `c20884ffb45660f79a7f213329e5e9c1ff2dc479`, the
[Windows job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42419847544/job/128546431217)
completed the full application build. Eight PageHeap boundary contracts, 35 process
containment/capture cases (including controlled native crash/hang probes), two standalone
Qt UI CTests, and seven staged dependency notice checks also passed. Neural qualification
was then cancelled after the next head superseded this run; native application integration
was skipped. Compilation and these bounded checks do not establish a W1 repair or task
runtime success. CI17–CI19 retain these separately from the older failure evidence.

At `5e9c2f00ccabd48f8f6b80748b924e1f9ea4a23b`,
[Linux](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42422881641/job/128554892608) and
[macOS](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42422881533/job/128554992210)
builds failed because F29's `BlueSource` fixture did not override the pure virtual
`TFx::getPluginId() const`. The override is now prepared. Linux compiled the five task
implementation/fixture objects and linked OpenToonz; macOS task compilation/link completion
was not established. Neither job ran the new native or pixel cases. CI20/CI21 preserve that
build failure; it is not a failed behavioral regression or evidence of a reporter defect.
F30–F34 were prepared after this head and have no runtime evidence from these jobs.

The workflow audit pins 121 external action uses and disables persisted credentials on
25 checkouts. Offline zizmor's default scan passes with documented local exceptions; the
broader auditor scan also exposes informational and policy findings. The workflow security
report records the exact counts, dispositions, and filtering; this is not an online advisory
clearance or a claim that every scanner finding was repaired.
The locked stylesheet/wiki dependency audit reported no known vulnerabilities at inspection
time. New action majors, package contents, C++ linking, and actual output remain CI obligations.

Before release or an assertion that these bugs are resolved:

1. Compile the final combined changes and run the relevant new/existing regression targets
   on their supported platforms, including PSD failure injection and native UI fixtures.
2. Confirm each regression fails against the defective implementation or an intentional
   mutation; do not weaken comparisons to obtain green results.
3. Run all ten automatic PR checks. Verify independent consolidated failures retain logs
   and the Linux job fits its configured budget. Run the moved matrices before claiming
   their configurations are qualified.
4. Capture the original native failure's stack and ownership evidence. Associate any fix
   with that failure; do not infer a fix from an unrelated successful rerun.
5. Inspect packaged notices, installed application startup, and external helper behavior.
   Existing drawing, scene, undo, HDR output, and artist workflows remain compatibility gates.

Recommended review units are CI topology/diagnostics, workflow supply chain, PSD transactions,
task controller safety, rendering bounds/ownership, drawing undo, and plugin/process fixes.
Each unit should carry its own tests and a narrow explanation. Draft PR 142 groups these
units as separate commits for Groundzero integration and CI. No upstream bug was closed
and no release was approved by this audit preparation.
