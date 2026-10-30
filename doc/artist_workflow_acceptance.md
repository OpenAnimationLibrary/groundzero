# Artist workflow acceptance ledger

Baseline: `edition` at `aa01975f38d6226156e5b07df7271b8fc199d003` (2026-10-30).

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
runner also collects independent timing, Cast/vector and export diagnostics.
Each group gets a fresh application process and disposable profile, runs
sequentially, and has a 120-second limit. The original failure remains the CI
result; isolated completion markers cannot satisfy the mandatory pass. Reports
are retained separately as `ui-integration-output-combined-*.txt` and appended to
the original report. Passing these diagnostics does not qualify the shared
application sequence. CI-only harness contracts are in
`ci-scripts/tests/test_native_ui_diagnostics.py`.

## Acceptance

`Pending` means not yet accepted, including source work in progress. `Source-ready` requires complete implementation and fixtures; `CI-qualified` additionally requires all selected checks to pass on the exact revision. `Already satisfied` requires a source/test citation. `Blocked` requires the exact dependency, action and reason. Each track's detailed ledger supplies baseline evidence and implementation/test links.

| # | Improvement | Acceptance contract | State |
|---|---|---|---|
| 1 | Proper Fit Selection | Selected multi-channel keys, single points and constants fit; current/all actions separate. | Source-ready; CI pending |
| 2 | Searchable channels | Object/FX/parameter matching, clear/expansion restoration, selected curves remain explicit. | Source-ready; CI pending |
| 3 | Saved channel sets | Stable owner identities; rename/delete, reload and workspace persistence. | Source-ready; CI pending |
| 4 | Curve solo and edit locks | Every editing command respects locks; visible reference/solo state. | Source-ready; CI pending |
| 5 | Multi-key numeric editing | Mixed values; absolute/relative operations, collisions, cancel, one undo transaction. | Source-ready; CI pending |
| 6 | Precise multi-channel retiming | Multiple channels, pivot/range, quantization, collisions, repeat/undo. | Source-ready; CI pending |
| 7 | Normalized curve display | Display-only normalized coordinates; native values and edits preserved. | Source-ready; CI pending |
| 8 | Clear curve identification | Stable identities/colors with labels and high-DPI/theme readability. | Source-ready; CI pending |
| 9 | Visible interpolation controls | Native interpolation/handle actions; multi-selection and keyboard access. | Source-ready; CI pending |
| 10 | Explicit snapping controls | Frame/neighbor/marker targets; bypass modifier; unchanged default semantics. | Source-ready; CI pending |
| 11 | Before/after curve overlays | Snapshot comparison, accept/revert, linked/native/SeExpr ownership. | Source-ready; CI pending |
| 12 | Interactive breakdown slider | Preview/cancel; stage, Plastic and offset channels; driven channels protected. | Source-ready; CI pending |
| 13 | Controlled key reduction | Independent sampled error bound, protected/held keys and undo. | Source-ready; CI pending |
| 14 | Bake expressions to keys | Sampling range/step, units and output equivalence; cancellation and undo. | Source-ready; CI pending |
| 15 | Visual motion-preset browser | Existing curve compatibility; browse/search/preview and amplitude/duration adaptation. | Source-ready; CI pending |
| 16 | Channel dependency inspector | Sources/dependents, invalid references and owner navigation without second evaluator. | Source-ready; CI pending |
| 17 | Large-rig graph performance | Large-rig correctness fixture and measured CI report; no unmeasured speed claim. | Source-ready; CI pending |
| 18 | Keyboard/tablet editing | Focus, accessible targets, keyboard nudges/constraints and tablet manual checklist. | Source-ready; CI pending |
| 19 | Opacity readback | Single/mixed opacity selection follows context without mutating drawing. | Source-ready; CI pending |
| 20 | Style opacity consistency | Relevant vector style render paths preserve stroke alpha with independent pixel fixtures. | Source-ready; CI pending |
| 21 | SVG stroke opacity | External SVG output carries palette alpha times stroke opacity. | Source-ready; CI pending |
| 22 | Visual width-profile manager | CRUD/import/export/previews, errors, overwrite/cancel and restart persistence. | Source-ready; CI pending |
| 23 | Width profiles across drawings | Selected drawings/level range, repeated exposures once, locked resources and undo. | Source-ready; CI pending |
| 24 | Pressure/profile blending | Zero-strength identity, replace/multiply semantics and pressure preservation. | Source-ready; CI pending |
| 25 | Fill-safe reshaping | Actual region-change preview/recovery in geometry edit flow; fills/undo saved. | Source-ready; CI pending |
| 26 | Persistent morph hints | Stable drawing/stroke identity, edits/deletions, reopen and compatibility. | Source-ready; CI pending |
| 27 | Correspondence preview | Matched/manual/unmatched preview matches committed inbetweens. | Source-ready; CI pending |
| 28 | Unmatched stroke handling | Explicit birth/death policy; endpoint and fill preservation, undo. | Source-ready; CI pending |
| 29 | Cross-frame gradient uniqueness | Single local/shared other-frame style, other drawings unchanged, undo. | Source-ready; CI pending |
| 30 | Animated gradient copying | Copy current vs animated style choice; keyframes, unlinking and reload. | Source-ready; CI pending |
| 31 | Outline stacking preservation | Native overlap/group pixel fidelity through outline conversion and undo. | Source-ready; CI pending |
| 32 | Closed-loop outlines | Inner/outer boundaries, tight loops, holes/winding and undo. | Source-ready; CI pending |
| 33 | Multi-drawing alignment | Explicit frame/reference scope; matching, groups and no index-only assumptions. | Source-ready; CI pending |
| 34 | Paint Inside/Selection | Clipping to region/selection; holes/groups, pressure, undo. | Source-ready; CI pending |
| 35 | Guided Drawing mode consistency | Guided Drawing honors paint mode or provides actionable explicit incompatibility. | Source-ready; CI pending |
| 36 | Along-stroke opacity | Opacity variation survives draw/split/join/morph/deform/render/export and versioned storage. | Source-ready; CI pending |
| 37 | Global Cast search | Name/path, folder/global scope, stable selection. | Source-ready; CI pending |
| 38 | Cast filters | Types and resource status predicates, combined with search. | Source-ready; CI pending |
| 39 | Where Used navigation | All nested occurrences, cycles bounded, exact instance navigation and multiselection. | Source-ready; CI pending |
| 40 | Batch relink preview | Ambiguous matches, explicit preview, identity/exposure preservation and undo. | Source-ready; CI pending |
| 41 | Distinct asset states | New/dirty/missing/external/read-only/cache states truthful without changing artwork. | Source-ready; CI pending |
| 42 | Unused-asset cleanup preview | Review/checklist, keep-spare metadata, undo without deleting source files. | Source-ready; CI pending |
| 43 | Folder undo | Create/rename/delete hierarchy and membership restoration, redo. | Source-ready; CI pending |
| 44 | Asset inspector | Resolution/DPI/frame gaps/palette/storage selection updates. | Source-ready; CI pending |
| 45 | Scrubbable thumbnails | Scrub/choose cover, bounds, persistence and bounded decoding. | Source-ready; CI pending |
| 46 | Audio audition | Native waveform/transport, stop-on-close/selection and no leaked playback. | Source-ready; CI pending |
| 47 | Tags and collections | Stable asset IDs, multi-tag queries and save/reload. | Source-ready; CI pending |
| 48 | Stable refresh state | Identity selection/scroll/expanded folders/sort retained through changes. | Source-ready; CI pending |
| 49 | Large-Cast responsiveness | Large fixture correctness and measured incremental update/ordering. | Source-ready; CI pending |
| 50 | Explicit duplicate consolidation | Content/identity/palette comparisons, explicit confirmation only, exposure-preserving undo. | Source-ready; CI pending |
| 51 | Collection/dependency report | Missing/conflicting dependencies and sizes, exact inclusion and relocated output verification. | Source-ready; CI pending |
| 52 | Linked ownership and local copy | Import vs linked state and explicit local copy with undo/collection preservation. | Source-ready; CI pending |
| 53 | Pose-library undo | Entire pose/base state, import/replace/delete, duplicate names and undo/reload. | Source-ready; CI pending |
| 54 | Timing-aware storyboard moves | Cells/audio/native animation move as one undo; locked/boundary rules explicit. | Source-ready; CI pending |
| 55 | Source/playback timing indicators | Clear edit source vs playback drawing, nested/remap/range transitions. | Source-ready; CI pending |
| 56 | Scene compatibility report | Read-only target-feature inventory, unsupported/lossy semantics and navigation. | Source-ready; CI pending |
| 57 | Export fidelity preflight | Lottie/SVG unsupported-feature inspection before write; cancel and explicit conversion. | Source-ready; CI pending |
| 58 | Recovery completeness status | Current snapshot complete/partial/errors/retry visible without losing pending resources. | Source-ready; CI pending |
| 59 | Artist-workload benchmarks | Actual native pen/scrub/playback/resource-save/memory metrics, correctness and provenance. | Source-ready; CI pending |
| 60 | Safe disk-cache management | Usage/budget/manual cleanup, cross-process leases, atomic retirement, cancellation/errors. | Source-ready; CI pending |

## Shared regression gates

- Document edits are one coherent undo operation; repeated commands, cancellation, failed persistence and reload preserve identity and prior data.
- Multi-selection and nested xsheets use stable owners and exact instance paths; no hidden or stale selection is edited inadvertently.
- Existing scenes, SeExpr bindings, particle resources and library import/link semantics retain their behavior.
- Search, previews and analysis do not mutate scene data. Destructive asset operations require a concrete preview and do not silently remove source files.
- Keyboard focus, translated captions, accessible names, high-DPI hit targets and bounded work are covered where applicable. Automated offscreen/native tests do not establish physical tablet validation.

