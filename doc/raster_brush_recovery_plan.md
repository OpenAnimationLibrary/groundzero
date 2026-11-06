# PR 115 recovery and completion plan

## Scope and completion rule

Resume the opt-in native regular-RGBA8 raster brush work from a40370a60.
The normative RCP1 revision 2 contract and acceptance ledger in
raster_brush_compatibility.md govern completion. This plan does not narrow them.
Decoded resources, semantic support, converted approximations and reference
verification remain distinct. Passing existing tests alone is not completion.

## Isolation

Use the dedicated codex/pr115-raster-brush-recovery branch in its independent
clone. Do not change another working checkout, its index or its branches.
Publish only this dedicated branch when a correction batch is ready; updating
PR 115's original branch is a separate integration step. Reconcile concurrent
changes deliberately before final qualification. Run builds and tests in CI.

## Ordered milestones

- [x] Create an independent clone and dedicated recovery branch at the published head.
- [x] Refresh remote PR and edition refs and inventory current CI evidence.
- [ ] Recover/review unpublished implementation tip 44022b4c81ae60083a157fe8cd051e18ee2456ed.
- [ ] Recover/review separate application-test commit 73073046dae45144d020f9a9f22fee32a77a31f6.
- [ ] If recovery is unavailable, document missing work and reconstruct it from the handoff and acceptance requirements.
- [ ] Map every acceptance-ledger row and normative requirement to code, tests, source evidence and outstanding work.
- [x] Reconcile current edition changes in application integration, tool options, CMake and workflows (validation pending).
- [ ] Read complete parser, fuzz, application and source-cache failure evidence before correcting causes.
- [ ] Verify timestamp normalization, wet footprint/HSV fixtures, semantic rejection, airbrush nesting and matrix completeness corrections.
- [ ] Resolve all other failures; retain fidelity thresholds, meaningful assertions and resource limits.
- [ ] Complete required source dynamics/resource combinations, automatic spacing and scatter.
- [ ] Complete required Gaussian/Soft generators and secondary-mask geometry/dynamics.
- [ ] Complete adaptive smudge sampling, required texture behavior and wet combinations.
- [ ] Complete required source timing/tap semantics and resource holdouts.
- [ ] Close required ABR setting/resource bindings with evidence; preserve explicit rejection/conversion for unsupported inputs.
- [ ] Qualify preset editing, previews, import, export, resource repair and persistence.
- [ ] Qualify actual painting, undo/redo, cancel, frame lifecycle and save/reopen on all supported desktop platforms.
- [ ] Obtain required authorized Photoshop reference/export evidence and physical tablet measurements.
- [ ] Qualify the exact final head: six parser configurations, fuzzing, application, installed packages, isolation, source comparisons and performance.
- [ ] Refresh public scope/evidence documentation and PR description; obtain review before merge.

## Known baseline

The last published implementation is 1c62ec606; a40370a60 adds the pause handoff.
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
