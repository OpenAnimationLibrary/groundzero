# Artist workflow acceptance ledger

Current baseline: `edition` at `b584755dca36e9cc5da1c325a2f11c8b83cd2aa2` (2026-10-30),
including the separately owned reference/neural work in #114. Original research
baseline: `aa01975f38d6226156e5b07df7271b8fc199d003`.

All 60 requested improvements remain tracked. Source implementation, executed CI and physical artist validation are separate states. No item is complete merely because a control or test exists. Builds and tests run in supported CI, not locally. Every PR must be reconciled against the then-current `edition` before publication; passing results must identify its exact final head. Drafts remain open while qualification is pending.

## Ownership and publication

| Track | Items | Branch | PR |
|---|---|---|---|
| Function Editor | 1–18 | `improve/function-editor-workflow` | [Draft #116](https://github.com/OpenAnimationLibrary/groundzero/pull/116) |
| Vector editing | 19–36 | `improve/vector-editing-workflow` | [Draft #120](https://github.com/OpenAnimationLibrary/groundzero/pull/120) |
| Scene Cast | 37–52 | `improve/scene-cast-workflow` | [Draft #119](https://github.com/OpenAnimationLibrary/groundzero/pull/119) |
| Animation workflow and reliability | 53–58 | `improve/animation-workflow-reliability` | [Draft #117](https://github.com/OpenAnimationLibrary/groundzero/pull/117) |
| Performance and cache management | 59–60 | `improve/artist-workflow-performance` | [Draft #118](https://github.com/OpenAnimationLibrary/groundzero/pull/118) |

Review and validation follow the dependency chain #116 → #117 → #119 → #120 → #118. The final PR includes the shared acceptance fixtures and combined performance measurements.

Raster Krita/Photoshop brush compatibility is separately owned and excluded. Neural/color-style work in PR #114 is separately owned and excluded. Vector opacity/width/paint modes here operate on native vector artwork, not the external raster brush engine. Reuse existing native SeExpr, particle, collection and UI capabilities. Coordinate common CMake, command registrations, preferences and native integration fixtures explicitly.

## Current progress

No item is CI-qualified yet. Draft publication starts validation and does not
close the remaining work. The detailed ledgers record implemented source and
remaining checks separately:

The original 60-item scope remains authoritative. The scope audit reopened
items 3, 6, 28, 33, 38, 41, 44, 46, 47, 52 and 56 for missing functionality.
Follow-ups `acf0f5d0c`, `a65fbab01`, `250d78891`, `065b699d8`, `7d83791e0`
and `351e54bfc` now supply their source and acceptance fixtures. This is not
an all-60 completion claim: those additions need fresh exact-head CI, and
the failures below remain open. Fade preserves unmatched stroke appearance
and rejects unmatched boundaries of painted regions before any edit; native
per-region opacity is outside this workflow. Matched filled regions remain
supported. Collections are explicit UUID memberships, and Library attribution
comes from actual native Library imports rather than external-path inference.

A later source reconciliation reopened items 11, 15, 17 and 60: driven
comparison curves must freeze their evaluated display, the motion browser must
import existing native `.curve` assets, the graph benchmark needs a genuinely
multi-channel large-rig fixture, and cache cancellation must be rechecked after
waiting for the retirement gate. These are implementation gaps being corrected,
not merely pending CI.

Function Editor head `d7c34e6ef2096b255eec7077e51c8074901e6188` passed Linux
and Windows, including installed applications, both Function test suites and
native unkeyed-channel save/reload. macOS remained pending at this checkpoint.
Those results validate the identity/export corrections but do not qualify the
later rename, target-range and full-precision serialization additions.

Current unresolved checks include native 3D opacity (items 20 and 36), Windows
width-profile manager (item 22), macOS batch relink (item 40), rendered SVG
export on Windows (item 57), and combined clone/persistence/export acceptance.
Windows vector-analysis and shell-thumbnail rendering are also under investigation.
The relink diagnostic reached review without a replacement candidate. Windows
SVG selected the correct controls but failed inside scene rendering; detailed
native render/write errors now reach the existing diagnostic. Original content,
choice, cancellation and replacement assertions remain required.

Vector head `913efdb12` passed the stricter Linux color/depth/stencil attachment
checks before detecting a secondary-color enable-state leak. Correction
`0cc8df85d` restores that state explicitly. Its Linux run stopped earlier at
the new alignment-preview visual assertion: the preview's flattened-Z matrix
is singular in 4D, so opacity rendering incorrectly omits its strokes. A
plane-coordinate correction is in progress; the depth-state and timing checks
have not yet run on that revision.
macOS passed D24, rollback, patterned depth, RGB10_A2, separate read targets and
sRGB checks, then failed the HDR fixture's initial seed before opacity drawing.
The representative depth-rendering timing has not yet executed; no practical
speed claim follows from the allocation estimate. Synthetic native application
benchmarks are distinct from physical tablet and artist pacing qualification.

Linux validation `3090f9d9d` passed all eight original native 3D style-identity
comparisons, then exposed a one-depth-step mismatch in the stricter attachment
transaction fixture. Its independent combined scene roundtrip retained the
unkeyed channel identity. The nested-export fixture now recalculates its duration
after removing temporary preflight columns, retaining the exact single-frame
placement oracle. Clone acceptance materializes and authors the existing source
stage before counting newly inserted nodes. Production clone redo now retains
the original inserted stage and curve objects: a later Function Editor undo
entry must still modify the active curve after undoing and redoing the clone.
The combined `506741eb7` run stopped at a numeric-pointer conversion in its new
clone fixture; `f951b9bea` corrects that compile error. The current combined
checks also require UUID/provenance retention, distinct clone identities,
collection remapping and exact undo, plus native reload of consolidated scenes.
The subsequent `f951b9bea` Linux run passed all five isolated diagnostics:
Function Editor, Cast, timing, Cast/vector and export fidelity, including native
scene reload, interleaved clone/key undo, atomic image publication and queued
render cancellation. Its mandatory sequence still failed at the known
secondary-color state assertion. These diagnostic passes neither replace that
sequence nor qualify the later collections/provenance and reopened workflows.
The Windows profile-manager review found an unchecked transient row index;
`2d1282055` validates it and blocks selection signals during list rebuilding.
The relationship to the observed crash remains pending fresh execution.

The scene-loading ownership correction `623763b59` has executed successfully on
Linux: repeated output-properties assignment and scene/PLI reload no longer
crash. Independent combined diagnostics then reached the identity and clone
assertions above. This is evidence for that correction, not a passing result for
the full combined sequence. Dedicated CI validation refs preserve running PR
checks; their changes will be folded into the same five PRs before final
qualification. No additional feature ownership or PR is created by these refs.

* Function Editor: source and full-application fixtures published, including quantization,
  marker snapping and preset filtering; exact-head CI is pending.
  See `doc/function_editor_acceptance.md` in #116.
* Vector editing: source and native acceptance fixtures published in #120, including
  versioned opacity envelopes, production rendering and command-line conversion.
  Cross-feature export and clone/persistence checks are being qualified together.
* Scene Cast: source and fixtures published, including confirmed consolidation,
  saved covers, cache restoration and actual relocated-project acceptance.
  See `doc/scene_cast_acceptance.md` in #119; exact-head CI is pending.
* Animation reliability: source and a review correction preserving untouched storyboard key fields
  are published, together with full export write/cancel checks. The combined source
  now includes opacity-profile compatibility findings and atomic export publication.
  See `doc/animation_workflow_acceptance.md` in #117.
* Performance/cache: source and fixtures published, including two deterministic
  concurrency regressions and actual native-workload correctness checks.
  See `doc/artist_workload_and_media_cache.md` in #118.

Combined native hooks and strict CI manifests require all component fixtures.
`functiontimingintegrationtest.cpp` checks locks, channel identities and nested
timing through storyboard moves. `castvectorintegrationtest.cpp` checks clone,
relink, consolidation and renumber metadata across vector and Cast operations.
`combinedfidelityintegrationtest.cpp` checks real Lottie/SVG publication, cancel,
independent SVG rendering and the full-camera PNG fallback. These fixtures are
source-ready and awaiting execution; they do not replace platform qualification.
The standalone performance/cache head `eae9ff422` passed Linux build, native
workloads and installed-application checks. The combined final revision adds an
opacity-envelope workload and must be qualified again.

When the mandatory native pass fails with `--require-combined-workflows`, the
runner also collects independent Function Editor, Cast, timing, Cast/vector,
export and vector-renderer diagnostics. The renderer diagnostic independently
reaches attachment checks and representative timing when another UI workflow
fails first; it cannot qualify the original sequence.
Each group gets a fresh application process and disposable profile, runs
sequentially, and has a 120-second limit. The original failure remains the CI
result; isolated completion markers cannot satisfy the mandatory pass. Reports
are retained separately as `ui-integration-output-*.txt` and appended to
the original report. Passing these diagnostics does not qualify the shared
application sequence. CI-only harness contracts are in
`ci-scripts/tests/test_native_ui_diagnostics.py`.

## Acceptance

`Pending` means not yet accepted, including source work in progress. `Source-ready` requires complete implementation and fixtures; `CI-qualified` additionally requires all selected checks to pass on the exact revision. `Already satisfied` requires a source/test citation. `Blocked` requires the exact dependency, action and reason. Each track's detailed ledger supplies baseline evidence and implementation/test links.

| # | Improvement | Acceptance contract | State |
|---|---|---|---|
| 1 | Proper Fit Selection | Selected multi-channel keys, single points and constants fit; current/all actions separate. | Source-ready; CI pending |
| 2 | Searchable channels | Object/FX/parameter matching, clear/expansion restoration, selected curves remain explicit. | Source-ready; CI pending |
| 3 | Saved channel sets | Stable owner identities; rename/delete, reload and workspace persistence. | Source-ready; fresh CI pending |
| 4 | Curve solo and edit locks | Every editing command respects locks; visible reference/solo state. | Source-ready; CI pending |
| 5 | Multi-key numeric editing | Mixed values; absolute/relative operations, collisions, cancel, one undo transaction. | Source-ready; CI pending |
| 6 | Precise multi-channel retiming | Multiple channels, explicit target start/end preview, pivot/scale, quantization, collisions, repeat/undo. | Source-ready; fresh CI pending |
| 7 | Normalized curve display | Display-only normalized coordinates; native values and edits preserved. | Source-ready; CI pending |
| 8 | Clear curve identification | Stable identities/colors with labels and high-DPI/theme readability. | Source-ready; CI pending |
| 9 | Visible interpolation controls | Native interpolation/handle actions; multi-selection and keyboard access. | Source-ready; CI pending |
| 10 | Explicit snapping controls | Frame/neighbor/marker targets; bypass modifier; unchanged default semantics. | Source-ready; CI pending |
| 11 | Before/after curve overlays | Snapshot comparison, accept/revert, linked/native/SeExpr ownership. | Pending: freeze evaluated driven comparison |
| 12 | Interactive breakdown slider | Preview/cancel; stage, Plastic and offset channels; driven channels protected. | Source-ready; CI pending |
| 13 | Controlled key reduction | Independent sampled error bound, protected/held keys and undo. | Source-ready; CI pending |
| 14 | Bake expressions to keys | Sampling range/step, units and output equivalence; cancellation and undo. | Source-ready; CI pending |
| 15 | Visual motion-preset browser | Existing curve compatibility; browse/search/preview and amplitude/duration adaptation. | Pending: native curve asset import |
| 16 | Channel dependency inspector | Sources/dependents, invalid references and owner navigation without second evaluator. | Source-ready; CI pending |
| 17 | Large-rig graph performance | Large-rig correctness fixture and measured CI report; no unmeasured speed claim. | Pending: multi-channel rig fixture |
| 18 | Keyboard/tablet editing | Focus, accessible targets, keyboard nudges/constraints and tablet manual checklist. | Source-ready; CI pending |
| 19 | Opacity readback | Single/mixed opacity selection follows context without mutating drawing. | Source-ready; CI pending |
| 20 | Style opacity consistency | Relevant vector style render paths preserve stroke alpha with independent pixel fixtures. | Source-ready; CI pending |
| 21 | SVG stroke opacity | External SVG output carries palette alpha times stroke opacity. | Source-ready; CI pending |
| 22 | Visual width-profile manager | CRUD/import/export/previews, errors, overwrite/cancel and restart persistence. | Pending: Windows native crash investigation |
| 23 | Width profiles across drawings | Selected drawings/level range, repeated exposures once, locked resources and undo. | Source-ready; CI pending |
| 24 | Pressure/profile blending | Zero-strength identity, replace/multiply semantics and pressure preservation. | Source-ready; CI pending |
| 25 | Fill-safe reshaping | Actual region-change preview/recovery in geometry edit flow; fills/undo saved. | Source-ready; CI pending |
| 26 | Persistent morph hints | Stable drawing/stroke identity, edits/deletions, reopen and compatibility. | Source-ready; CI pending |
| 27 | Correspondence preview | Matched/manual/unmatched preview matches committed inbetweens. | Source-ready; CI pending |
| 28 | Unmatched stroke handling | Explicit birth/death policy including Fade; endpoint, fill and opacity preservation, undo. | Source-ready; fresh CI pending |
| 29 | Cross-frame gradient uniqueness | Single local/shared other-frame style, other drawings unchanged, undo. | Source-ready; CI pending |
| 30 | Animated gradient copying | Copy current vs animated style choice; keyframes, unlinking and reload. | Source-ready; CI pending |
| 31 | Outline stacking preservation | Native overlap/group pixel fidelity through outline conversion and undo. | Source-ready; CI pending |
| 32 | Closed-loop outlines | Inner/outer boundaries, tight loops, holes/winding and undo. | Source-ready; CI pending |
| 33 | Multi-drawing alignment | Explicit frame/reference scope, preview/cancel, matching, groups and no index-only assumptions. | Pending: preview plane rendering correction |
| 34 | Paint Inside/Selection | Clipping to region/selection; holes/groups, pressure, undo. | Source-ready; CI pending |
| 35 | Guided Drawing mode consistency | Guided Drawing honors paint mode or provides actionable explicit incompatibility. | Source-ready; CI pending |
| 36 | Along-stroke opacity | Opacity variation survives draw/split/join/morph/deform/render/export and versioned storage. | Source-ready; CI pending |
| 37 | Global Cast search | Name/path, folder/global scope, stable selection. | Source-ready; CI pending |
| 38 | Cast filters | Types and resource status predicates including modified assets, combined with search. | Source-ready; fresh CI pending |
| 39 | Where Used navigation | All nested occurrences, cycles bounded, exact instance navigation and multiselection. | Source-ready; CI pending |
| 40 | Batch relink preview | Ambiguous matches, explicit preview, identity/exposure preservation and undo. | Source-ready; CI pending |
| 41 | Distinct asset states | New/dirty/missing/external/read-only/cache states truthful without changing artwork. | Source-ready; fresh CI pending |
| 42 | Unused-asset cleanup preview | Review/checklist, keep-spare metadata, undo without deleting source files. | Source-ready; CI pending |
| 43 | Folder undo | Create/rename/delete hierarchy and membership restoration, redo. | Source-ready; CI pending |
| 44 | Docked asset inspector | Resolution/DPI/frame gaps/palette/storage follow selection in a persistent docked panel. | Source-ready; fresh CI pending |
| 45 | Scrubbable thumbnails | Scrub/choose cover, bounds, persistence and bounded decoding. | Source-ready; CI pending |
| 46 | Audio audition | Native waveform/transport, stop-on-close/selection and no leaked playback. | Source-ready; fresh CI pending |
| 47 | Tags and collections | Stable asset IDs, multi-tag queries, saved named collections and save/reload. | Source-ready; fresh CI pending |
| 48 | Stable refresh state | Identity selection/scroll/expanded folders/sort retained through changes. | Source-ready; CI pending |
| 49 | Large-Cast responsiveness | Large fixture correctness and measured incremental update/ordering. | Source-ready; CI pending |
| 50 | Explicit duplicate consolidation | Content/identity/palette comparisons, explicit confirmation only, exposure-preserving undo. | Source-ready; CI pending |
| 51 | Collection/dependency report | Missing/conflicting dependencies and sizes, exact inclusion and relocated output verification. | Source-ready; CI pending |
| 52 | Linked ownership and local copy | Actual Library import/link provenance and explicit local copy with undo/collection preservation; external paths alone do not prove linkage. | Source-ready; fresh CI pending |
| 53 | Pose-library undo | Entire pose/base state, import/replace/delete, duplicate names and undo/reload. | Source-ready; CI pending |
| 54 | Timing-aware storyboard moves | Cells/audio/native animation move as one undo; locked/boundary rules explicit. | Source-ready; CI pending |
| 55 | Source/playback timing indicators | Clear edit source vs playback drawing, nested/remap/range transitions. | Source-ready; CI pending |
| 56 | Scene compatibility report | Read-only known-extension inventory, unsupported/lossy warnings and navigation; no certification of arbitrary historical builds. | Source-ready; fresh CI pending |
| 57 | Export fidelity preflight | Lottie/SVG unsupported-feature inspection before write; cancel and explicit conversion. | Source-ready; CI pending |
| 58 | Recovery completeness status | Current snapshot complete/partial/errors/retry visible without losing pending resources. | Source-ready; CI pending |
| 59 | Artist-workload benchmarks | Native synthetic drawing/scrub/playback/resource-save/memory metrics, correctness and provenance; physical tablet qualification separate. | Source-ready; CI pending |
| 60 | Safe disk-cache management | Usage/budget/manual cleanup, cross-process leases, atomic retirement, cancellation/errors. | Pending: cancellation during retirement gate wait |

## Shared regression gates

- Document edits are one coherent undo operation; repeated commands, cancellation, failed persistence and reload preserve identity and prior data.
- Multi-selection and nested xsheets use stable owners and exact instance paths; no hidden or stale selection is edited inadvertently.
- Existing scenes, SeExpr bindings, particle resources and library import/link semantics retain their behavior.
- Search, previews and analysis do not mutate scene data. Destructive asset operations require a concrete preview and do not silently remove source files.
- Keyboard focus, translated captions, accessible names, high-DPI hit targets and bounded work are covered where applicable. Automated offscreen/native tests do not establish physical tablet validation.
