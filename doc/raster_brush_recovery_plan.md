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
