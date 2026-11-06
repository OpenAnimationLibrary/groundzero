# PR 115 stabilization plan

## Current scope: finish the support already implemented

Scope changed explicitly on 2026-11-06. This phase finishes and stabilizes the
existing native brush workflow and bounded Krita candidates. Photoshop emulation
and broader Krita compatibility research are deferred. This supersedes the
previous full-RCP1 completion plan; it does not claim full RCP1 is complete.

Keep existing ABR decoding, sampled-tip extraction and explicit native conversion.
Preserve native dry/wet painting, currently admitted Krita settings, preview and
editing, collection import, portable presets/repair, undo/cancel/save, and existing
resource and isolation protections. Keep supported, converted, experimental and
reference-verified statuses distinct. Do not widen candidate admission or invent
missing source behavior to make the feature list larger.

## What this work delivers to users

The PR foundation already contains real application features. The recovery adds
fixes, safer imports/resource handling, clearer support reporting and new controls
for existing candidates. The working tree adds preview lifecycle and source-editor
improvements. Finish and demonstrate these changes rather than create more systems.

The source review traced all six outcomes to actual UI entries. Against the locally
tracked edition branch, the native engine and native preset implementation are new
files; this is a substantive product change. Recheck the final diff against the
integration target before review. Source wiring is not proof of successful execution.

| User outcome | Actual delivery in this phase | Existing proof to reuse |
|---|---|---|
| Paint with native dry, Wet Mix and Smear brushes | Finish the original opt-in brush foundation with recovery lifecycle/storage corrections; preserve undo, cancel and saved pixels. | Native tool lifecycle and scene/PNG reopen cases in `rasterbrushintegrationtest.cpp`; native engine math cases. |
| Import and find usable brushes | Keep existing ABR/native conversions and bounded Krita support, with collection selection, safe installation/reimport and searchable presets. | Real collection picker/worker/selector/paint cases in `rasterbrushcollectionintegrationtest.h`; direct installer fault cases remain complementary. |
| Try a brush in the editor scratchpad | Complete moved-release, stationary timer, ownership and cancellation behavior without modifying the drawing. | Actual QWidget preview lifecycle cases in `rasterbrushintegrationtest.cpp`. |
| Edit supported settings and save a copy | Expose the already admitted source controls through the new profile-aware panel; keep native controls meaningful, original presets unchanged and edited state portable. | Nine-family cancel/save/reopen cases in `rasterbrushsourceeditorintegrationtest.h`; existing native Time/Distance editor cases. |
| Understand what was imported or converted | Show supported/retained/omitted settings and explicit native conversion consequences through Brush details and import reports. | `rasterbrushsupport_test.h` plus actual conversion/acceptance dialog cases. |
| Export, reopen and repair presets | Preserve settings and resource identities/rights; missing resources can be relinked without silently substituting a different asset. | Existing portable/repair/private-copy and retained-mask application cases. |

A finished milestone must cite the actual behavior and its evidence. More test
files, metadata fields or documents do not independently count as a user feature.
All current working-tree execution evidence remains pending.

## Milestone 1: close the finite known-fix list

Implementation checkpoint: all six items below are written and independently
source-reviewed. FIX-01 also rejects a failed finish whose observed dab count
moves backwards; its paired-counter regression is authored. The preview/editor,
collection workflow and opt-in boundary received a focused source review with no
additional concrete defects found. The native zero-weight and alpha-lock cases
needed no production renderer changes. No builds/tests have run for this batch.
Integration of the refreshed edition branch is the remaining pre-CI step.

These are the six known items. The first four are confirmed source-review defects;
the last two are focused proof questions, not established production mismatches.

| Item | Implemented change | Completion condition | Owner |
|---|---|---|---|
| FIX-01 | Correct benchmark dab accounting after failed add/finish. | Record observed progress before cleanup; mark unobserved checkpoints explicitly. Existing failure cases cover both paths. | Benchmark owner |
| FIX-02 | Pin the original benchmark workload in the report validator. | Correct the existing synthetic fixture and reject changed size/spacing/resources/curves without weakening performance limits. | Benchmark owner |
| FIX-03 | Correct the tablet-hover fixture event. | Right-button hover has no left-tip contact; auxiliary-button events during contact remain distinct. Reuse current cases. | GUI owner |
| FIX-04 | Bound older modal test callback lifetimes. | Each affected queued callback has a fixture-owned context; failed startup cannot outlive captured locals. Leave already scoped new callbacks intact. | GUI owner |
| Native proof A | Specify/confirm empty-tip pickup behavior. | The existing transparent pickup when total weight is zero is explicitly documented and independently asserted by an existing or narrowly added case. | Native math owner |
| Native proof B | Confirm wet Normal painting under alpha lock. | An independent pixel/spatial expectation checks unchanged destination alpha. Reuse the reviewed spatial oracle where sufficient. | Native math owner |

Work in parallel only on disjoint files. Do not replace the written preview/editor,
collection helper, spatial oracle or benchmark. If a proof case reveals a real
in-scope defect, fix that behavior and its cause; do not start a new brush model.

## Milestone 2: make the existing product flows reviewable

The integrator and an independent reviewer perform one focused pass over the final
local changes and the six user outcomes above. Confirm UI entry -> actual engine/
importer behavior -> saved/reopened state, including cancellation and error paths.
Reuse the authored coverage; add a focused case only for a demonstrated gap.

Confirm the opt-in boundary and preserve existing MyPaint, TLV and vector behavior.
Retain parser budgets, sandbox boundaries, atomic publication, hash/rights checks
and exact undo/cancel expectations. Reconcile necessary integration-target changes
inside the isolated clone, never in the other active checkout. Give shared
engine/importer/codec files one writer at a time.

Exit: all six known items have source-review disposition, each user outcome has an
actual reachable path and an evidence case, no known in-scope implementation defect
is left, and coherent local commits preserve the work. This is **implementation
complete**, not a claim that tests ran. Only after this exit does CI deferral end.

## Milestone 3: validate the final integrated commit

Run the existing required parser configurations, fuzzing, engine/regression,
application, installed Windows/macOS/Linux package, isolation and performance
checks for the current features. Reuse the benchmark/report runner and current
application fixtures. Record the exact commit and result for each required gate.
A historical pass does not qualify changed code.

For each failure, read its actual log/annotation, identify the cause, fix it and
rerun the affected checks. Broaden reruns when the changed behavior warrants it.
Do not skip failing tests, loosen comparisons or relabel current-feature failures
as future research. If an expectation genuinely covers deferred behavior, review
and record that scope distinction explicitly rather than silently deleting a gate.

Exit: required checks for the resulting integrated commit pass, and every claimed
platform has its actual installed workflow evidence. Unavailable or failing
mandatory package/isolation evidence is an unresolved readiness item, not a pass.
Source candidates stay experimental/unverified where reference evidence is absent;
no physical tablet or display-latency qualification is inferred from these checks.

## Milestone 4: deliver the reviewed result and stop

Prepare the final PR description around the six user outcomes, actual new changes,
validation results and remaining limitations. Include a compact demonstration
checklist tied to the existing application cases: paint/undo/reopen, import/select,
scratchpad, edit/cancel/save-copy, details/conversion, and export/repair. It is a
record of evidence, not another demo application or test framework.

Finish when:

- The six known items and any demonstrated in-scope regressions are resolved.
- All six user outcomes are implemented and demonstrated by the applicable
  actual/installed application checks; MyPaint/TLV/vector regressions are covered.
- Required checks pass on the final integrated commit and independent review has
  no unresolved blocking findings. Current-feature failures are not hidden.
- The branch and PR description are ready for review/integration, with precise
  supported/converted/experimental labels and no unsupported fidelity, device or
  strict macOS memory guarantee.
- The deferred roadmap remains separate. No extra source model, generic framework,
  reference research or new feature is added merely to keep the effort running.

Call the result **stabilized native brush features and bounded Krita candidates**.
Readiness for review does not establish reference verification, hardware
qualification or release readiness on a platform with an unmet mandatory gate.
Actual integration must retain the checked commit or requalify subsequent changes.

Progress updates name the current milestone, items closed, concrete issues still
open and the next action. Do not turn fixture/commit counts into a completion
percentage. Planning is now specific enough to execute; re-plan only if a concrete
blocker or an explicit scope decision changes these outcomes.

## Deferred work and honest limits

- All Photoshop P2711/D work: new rendering behavior, descriptor/resource binding
  expansion, reference exports and precision investigations (R-04).
- Broader Krita Pixel and smudge support: new geometry/ranges/sensor products,
  general automatic/zero spacing, random-process research, full interaction
  coverage, general Dulling pickup and New Dulling texture research.
- New generic composition frameworks and a separate host timing subsystem.
- Strict macOS whole-process 2 GiB architecture research remains an open future
  decision. Existing sandbox/limits and their regression checks stay in scope;
  do not claim the stronger guarantee, remove protections or call an unqualified
  platform ready.
- Missing exact-app reference and physical-tablet evidence stays explicitly
  unverified. Neither broad parity nor measured tablet feel/display latency is
  promised by this phase. These unavailable inputs do not justify more scaffolding.

The [audit](raster_brush_acceptance_audit.md) records the concrete fixes and current
code inventory. The [deferred roadmap](raster_brush_work_packages.md) retains the
original requirements for a possible later phase; it is not this phase's checklist.

## Work rules

Implementation has resumed against the six-item stabilization list. Use the independent
`codex/pr115-raster-brush-recovery` branch; do not modify the other active checkout.
Current work includes local commits `fb44b5e1a` and `6064436c2` plus uncommitted
source. Preserve it. One writer owns shared GUI files, another the benchmark;
reviewers can work in parallel without rebuilding existing components.

No local project builds, tests or generators under CLAUDE.md. No CI status, log,
artifact, dispatch or rerun access before the current implementation steps are
finished. No push merely to save progress. This scope update changes documents
only. The subsequent implementation resumption follows the milestones above;
no remote PR update or CI access is implied before its gate.

The history below describes earlier work and the larger original target. It does
not restore deferred work to the current phase or override the scope above.

## Historical handoff baseline


At the original handoff, the published implementation was 1c62ec606;
a40370a60 added the pause handoff. The following is historical evidence, not a
current-head check or an instruction to access CI during implementation.
Completed checks for that implementation show all six parser jobs and fuzzing
failed; application compilation passed on Windows/macOS/Linux but raster brush
application tests failed; installed-application jobs were skipped. Reference
cache validation failed and the Krita comparison job was skipped. Native metrics
passed. Detailed causes require log/annotation review; the handoff diagnoses are
not a complete explanation of all final failures.

The two unpublished commits are absent from the initial local object database.
Their absence here does not establish that they are lost elsewhere.

## External prerequisites and release assessment

Authorized Photoshop execution/export evidence and physical tablet measurements
must remain explicitly open until acquired. Optional paid-pack selection does
not block the existing contract. No universal Krita/Kyle/Photoshop parity is
promised. A smaller release would require an explicit scope decision, not silent
removal of unmet acceptance requirements. No merge readiness is claimed.

## Recovery execution checkpoint

The dedicated branch now merges edition 1c49d532b. Four conflicts were resolved
by retaining both raster-brush integration and edition additions. The merge is
not platform-qualified yet. Neither preserved unpublished SHA was available in
the searched local repositories or through a direct GitHub fetch. Reconstruction
is proceeding from the handoff and observed failure logs; this does not recover
all unpublished features or the separate application-test commit.

The first correction batch normalizes stroke time before interpolation, repairs
the quarter-turn wet HSV and nonzero-pickup footprint fixtures, tests semantic
rejection separately from successful structural decoding, corrects portable
package/settings mutation, preserves airbrush conversion provenance and aligns
wet-HSV/GIH cardinalities with generated manifests. The comparator now validates
manifest identity/cardinality before reference acquisition; CI negative controls
cover truncation, duplicate/mismatched identities and baseline changes.

Observed application logs on all three platforms failed at portable conversion
identity/rights. The explicit airbrush source identity was absent from the safe
portable provenance list. Source cache validation stopped at the stale wet-HSV
count, and fuzz corpus generation stopped at the same two preset regressions.
These diagnoses come from completed logs, not inferred sanitizer failures.

No local builds or tests ran. Final qualification and the remaining feature
contract remain open. See raster_brush_acceptance_audit.md.

## Reconstruction and first CI observations

The original unpublished work was in an old cloud session and is unavailable.
The recovery branch reconstructs Distance length/repeat state in native
conversions and enables Time/Distance interval editing. Source admission remains
separate. New fixtures cover interval saturation/repeat, invalid metadata,
portable persistence, editor acceptance/cancellation and preservation.

The combined native-wet application fixture now edits HSV and texture depth,
paints and checks undo/redo/cancellation, reopens scene pixels, exports a manifest,
repairs its shared tip/texture resource and replays it through the worker/tool.
This reconstructs required lifecycle coverage, not the unavailable commit itself.

At c7a7d71cb, Linux/Windows Qt5/Qt6 engine suites passed, along with the new
fixture matrix checks. The reference cache validation and native metrics passed.
The preset suite progressed to a stale assertion that still excluded wet V2
identity-pressure HSV, despite the published importer/runtime explicitly admitting
it. The test now covers that supported candidate and keeps nonidentity/rotation
negative controls. Generated-tip acquisition exposed a missing explicit source
engine declaration; those original dry fixtures now declare paintbrush and retain
effective-engine validation. A separate new-smear 10000-dab drift gate failed;
its diagnostics are under investigation without changing thresholds.

These observations qualify only the named c7a7d71cb checks. Later reconstruction
and correction commits require their own CI. Broad source behavior and external
reference/device evidence are still incomplete.

## Parallel recovery checkpoint

Independent review strengthened the wet editor workflow: nonneutral foreground,
editor cancellation, and per-control pixel comparisons with unchanged opacity.
Import auditing found and corrected legacy sampled ABR negative/out-of-range
spacing fallback, hidden per-axis fade ranges, and malformed interval XML.

The 703b77b82 Linux/Windows Qt5/Qt6 preset suites passed, including worker and
response-file cases; its new Distance renderer fixture failed exact path partition
comparison. The correction advances native scheduling in physical segment distance
and time rather than accumulated normalized fractions. This changes native
scheduling only; source scheduling/thresholds stay intact. CI must qualify it.

Full c7a7d71cb source execution exposed genuine Smear placement and adaptive
Dulling sampling discrepancies. New bounded wetgeometry and public Lab convergence
diagnostics collect generalized evidence; no speculative sampler or offset rule
has been installed. Wethsv composition036/037 differed by exactly one alpha byte;
normalizing before subtraction falsely exceeded the one-byte threshold. Alpha
error now uses the integer code difference divided by255, with exhaustive adjacent
alpha-code and two-code negative metric controls; RGB gates are unchanged.

Pixel-precision capture sealing rejected a safe '+' in original signed boundary
names. Cache support preserves the existing path/suffix restrictions. A separate
compatibility fixture generated random Cast UUIDs on each build; fixed authored
UUIDs and explicit Cast-ID dump comparisons preserve identity checks.

New auto-spacing/scatter acquisition groups retain actual settings/inputs and
100 independent randomized active strokes per scatter configuration. These are
measurement and statistical gates, not completed source runtime admission. The
full source matrix includes them and all prior groups. The original Linux
application workload advanced past portable conversion/resource/source-wet stages
and then failed stationary airbrush accumulation; callback/lifecycle evidence is
being added before any timing-policy change. Cross-platform package and final-head
qualification remain open.

## Timer lifecycle and complete early matrix coverage

The 810a4fd2b parser suites passed Windows Qt5/Qt6 and macOS Qt5/Qt6; Linux
parser/fuzz remained queued at this checkpoint. Its Linux Qt6 build also passed.
These are scoped results, not full branch or package qualification.

The full-color brush tool is a static registry instance. Its QTimer member was
constructed before QApplication, a lifecycle Qt does not support. Timer creation
and connection now happen lazily during an actual airbrush stroke, with application
ownership and QPointer invalidation. Dispatch/rates,250ms fixture holds and exact
accumulation/cancel assertions stay unchanged. Actual application CI must establish
whether this resolves the observed stationary-airbrush failure; diagnostic state
snapshots remain available.

Metadata-only validation now derives all70 groups from the owning workflow and
runs each generated manifest through the native comparator, including negative
schema/identity cases. The paced-sensor guard now includes all40 authored cases
(original28 plus12 causal clock controls). The expanded300-second fixture budget
reflects the complete workload rather than relaxing a fidelity gate.

Two script API tests in edition still expected null palettes for newly created
Vector levels, despite the fallback palette introduced in #130. They now check the
actual default schema and read-only copy semantics; empty Level objects retain
null/None assertions. Production palette behavior is unchanged.

The703b77b82 generated-tip source acquisition completed all32 cases. Comparison
then exposed genuine missing Soft generator import/runtime behavior. Work is
proceeding from those original captures; no silent native approximation replaces
that required source-profile work.

## Diagnostic corrections before application/source execution

The a953850e9 Ubuntu Qt6 parser run passed, including all70 generated workflow
matrices. The earlier810a4fd2b Linux application compile exposed a diagnostic-only
raster type error: clone() was inferred as generic TRasterP while snapshot code
needed pixels(). The fixture now retains an explicit TRaster32P. Stage logs also
include monotonic elapsed milliseconds to distinguish actual workload cost from
modal/timer stalls without changing holds or deadlines.

The810a4fd2b source-cache job exposed a mismatch in the new geometry helper's
malformed-XML error API. ParseError is now normalized to ValueError, with additional
empty/truncated/mismatched XML controls. The malformed-input guard remains strict;
no source comparison has been waived. These corrections require fresh CI.

## Source admission and application recovery evidence

All six Ubuntu/Windows/macOS Qt5/Qt6 parser configurations and fuzzing passed
at a953850e9. At 9111a8970, Linux Qt6 and compatibility output comparison also
passed. These results qualify those commits and workloads only. The full source,
installed-application, device and external-reference obligations remain open.

The latest Linux application diagnostics show native stationary airbrush
accumulation now succeeds. The 5 Hz source airbrush receives its first timer at
approximately 200 ms, but the tangent modifier retains that event as a preview
until the next input. The recovery bypasses that lookahead only for the strict
source airbrush profile; the existing delivered-event filter, assistants and
replicator restrictions, 250 ms fixture and accumulation assertions remain.
Corrected-head application execution is required.

The macOS native-UI failure stack passes through palette QWidgetAction release
during QToolBar base destruction. Both palette bars now release their actions
while the toolbar and palette ancestry are intact. A lifecycle regression probes
both actual bars, requiring release while QToolBar is alive and widget retirement.
This follows the same lifetime rule as the existing playback-toolbar fix.

Bitmap source acquisition stopped on the equivalent angles -23 and
337.0000241088671 degrees. Acquisition and cache validation now share circular
angle comparison, keeping the original resource and ordinary tolerances. Invalid
and nonfinite inputs and unrelated view-state mismatches remain rejected.

Spacing-dynamics source fixtures include 48 raw spacing8 cases, outside the
published 0.01-5 RCP1 envelope. The authored values and captures remain intact;
precise expected rejection is distinguished from comparison, with no supported
or verified claim and no fabricated zero-error metrics. In-envelope spacing1/4
cases retain their complete pixel gates. Source-admission metadata is a closed,
validated contract and cannot waive arbitrary importer failures.

The complete 30-case wetgeometry acquisition at 9111a8970 passed independent
repeats. Both Smear source models show one-pixel placement changes dependent on
path subdivision, while dry controls do not. This is evidence about the pinned
public Node API, not yet a general renderer equation. Additional generated-mask,
kernel-convergence, spacing/scatter and mask-lifecycle captures preserve original
inputs and failures; no fitted lookup, discarded warm-up stroke or relaxed
comparison threshold substitutes for a model.

## Sampling convergence and stochastic-process diagnostics

The 9111a8970 kernel-sampling source run 42261285883 completed its public color
conversion sidecar with 2176 observations. Cumulative U16 RGB means converted to
full Lab U16, tested in 16-sample batches with a delta below3, predict all16
measured extent32 gray/chromatic/polarity RGB8 outputs exactly. U8 means do not.
This supports a convergence candidate, not an implemented generalized sampler:
minimum sample-count geometry remains unresolved. New70-case U16 single-mark
controls reuse seven original presets, keep two exact independent captures and
uniform controls, and retain raw center/crop evidence for count/duplicate-hit
analysis. Mandatory cache validation binds this evidence to original presets.

The independent100-stroke scatter acquisitions show100 unique source images
but one repeated native fallback image. Some aggregate RCP1 metric gates pass
that fallback because their measured dimensions do not distinguish the process.
New read-only displacement/component reports expose first-versus-later dab
behavior, authored-axis moments and process uniqueness without replacing the
original statistical thresholds or admitting the fallback. Auto-spacing source
case000 stopped before a PNG was retained; failure-only capture retention now
preserves that observation before re-raising its original diagnostic error.
No no-op behavior or spacing equation is inferred without the captured pixels.

## Confirmed application and parser checkpoint at bd06d01b9

The corrected Linux build passed native UI integration and the full raster
brush application fixture, including all four stationary-airbrush profiles,
delivered-event segmentation, deferred taps, undo/redo/cancellation, frame
switching, rollback and saved-pixel persistence. Installed-application worker
qualification and Windows/macOS application execution remain separate pending
gates. All six parser OS/Qt configurations and fuzzing also passed at this
commit, including exact source-admission rejection/forgery regressions and both
signed macOS production-worker startup qualifications.

The first new mask and generated-curve source acquisitions completed, then
exposed a comparator bug in the new admission binding: a mutable Qt JSON
subscript inserted null for absent metadata, differing from the authored
undefined value. Const value lookup now preserves absence, with missing/null,
roundtrip and changed-value regressions in the comparator's CI metric test.
Source pixels remain retained and the original fidelity gates are unchanged.

The separate macOS memory-cap experiment compiled and signed on both
architectures but did not execute allocation cases: modern codesign's default
entitlement display is human-readable DER rather than XML. The driver now
requests XML explicitly and retains exact sole-key Boolean validation. Its
continue-on-error workflow status never qualifies the memory bound; actual
case evidence is still required. Physical-device validation also remains open;
synthetic input evidence cannot close the tablet requirement.

## Saved implementation batch: fb44b5e1a and 6064436c2

This section records the saved implementation before the audit pause. Build/check review and
execution are deferred until the implementation plan is fulfilled. The changes
below have not received final compilation, execution or platform qualification.
They do not close the complete RCP1 acceptance ledger.

- Generated masks now have a separate versioned source definition and runtime
  candidate for Soft/Gaussian circle/rectangle responses, preserving independent
  fades, cubic knots and explicit antialias state across worker/private/portable
  records. Independent analysis covers148 original masks within one alpha code.
  Known Pixel transfer/blend behavior is shared; broader generated geometry and
  sensor combinations remain explicit work, with original heldouts authored.
- Smear stamp placement now uses discrete raster origins and recursive future
  interpolation with a residual segment distance. The recovered rule accounts
  for1036 geometry bounds and19998 heldout drift bounds. Nominal sensors, times,
  cadence and native scheduling retain their separate definitions.
- Source Dulling pickup now has calibrated cumulative Lab16 convergence in
  16-sample batches. Its general ICC curve/matrix/33-cube transform reproduces544
  public conversion observations exactly; source RGBA8 means use bounded wide
  integer accumulators. The minimum-window/count equation and broader modern
  clipping/partial-alpha behavior remain unfinished rather than fitted.
- Source scatter receives explicit versioned state, preserved controls and a
  distinct experimental uniform displacement path. Native random behavior is
  separate. Scaling/dynamics/origin heldouts remain necessary beyond the measured
  static source domain.
- Random GIH now has an experimental proposal/cache mechanism supported by
  conditional and lag observations, with stroke-local state and complete cache
  invalidation. Fine subpixel phase granularity is not identified by existing
  whole/half-pixel captures; independent phase-boundary heldouts are authored.
- Legacy ABR extraction now uses the bounded isolated worker, background staging,
  atomic cancellation and complete-collection publication. The UI explicitly
  identifies sampled-tip extraction. Structural sample completeness propagates
  separately from harmless descriptor/pattern interpretation warnings, preventing
  malformed native or legacy imports from publishing a readable subset.
- Native file/collection installation now publishes atomically without replacing
  concurrent entries, verifies reuse, excludes private staging from selectors,
  shares encoded images and observes cancellation at bounded resource boundaries.

Final validation must exercise the newly authored regressions and original
source gates unchanged. Photoshop behavior/dynamic associations, New-Dulling
texture reduction, complete generated/dynamic combinations, strict macOS memory
coverage, physical tablets and the other open ledger rows still require their
specified implementation/evidence. No unsupported row becomes complete because
another experimental component was implemented.

## Declaration preservation and source review checkpoint

The current unvalidated batch also preserves original scoped K/S sensor
declarations and source secondary-mask definitions/resources through private,
worker and portable records. These are metadata, independent of active native
controls and the narrow existing source renderers. The legacy ABR sampled-tip
path records a bounded, content-bound collection manifest so exact parsed sample
names/IDs, depth and order survive safe filesystem naming.

A Photoshop 27.11 input contract and operator recipe now specify producer
identity, original assets, controlled one-setting exports, container variants,
observations and privacy constraints. Authored inputs and unexecuted regression
sources are preparation; they do not establish Photoshop execution or parity.

Source-only peer review corrected aggregate sensor-knot preallocation,
generated transfer expectations (the subsequent build-up/Wash correction is recorded above), retained-mask resource identity during
native conversion, and explicit wet surface storage semantics. It also closed
source-control XML/effective-file binding gaps and tightened duplicate metadata
and portable resource declarations. These are implementation findings, not
results of a build or test run.

Import, preview and Brush details now show bounded original declarations and
current control counts separately, with actionable conversion/omission reasons.
Safe omission categories survive portable export; raw pressure/use-curve switches
are reported independently without guessing their combined activity. See
[raster_brush_support_diagnostics.md](raster_brush_support_diagnostics.md).

All CI access, including existing artifacts and logs, remains deferred by
instruction. No new CI pass or final-head qualification is claimed for this
batch. Regression sources have been authored but not executed.

The full plan is still open. Remaining source-equation work includes general
generated geometry/dynamics; automatic and zero spacing; scatter scaling/dynamics;
random GIH phase boundaries; non-generated K/S sensor normalization, curve
products and source-clock timing; bitmap/RGBA transforms; Pixel transfer, HSV,
blend and texture operation order; secondary-mask interactions; Dulling sampling
windows/clipping; New Dulling texture; and Photoshop dynamic/resource
associations. This list is not a reduced scope: every row and combination in the
acceptance audit and normative tables remains required. Discriminating original
source observations are needed before unidentified equations can be completed.
Physical tablet evidence is unavailable. Once implementation is complete and
validation resumes, failing checks must be diagnosed from their actual logs
before any fix or rerun.
