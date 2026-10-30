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

All 60 items have source implementations and prepared acceptance coverage in
this stack. No current item is yet qualified on its final published revision.
The source audit's missing subdeliverables have been added; this is not an
all-60 completion claim, because platform failures and fresh execution remain
open. Drafts stay in validation until their exact heads pass the required checks.

The audited follow-ups include saved-set rename and explicit target-range
retiming; frozen evaluated comparisons with exact native restoration; native
`.curve` import; a real multi-owner graph workload; explicit Fade and alignment
preview; distinct Cast status filters, a docked inspector, audio waveforms,
UUID collections and actual Library provenance. Later corrections add bounded
thumbnail requests, identity-preserving Cast refresh, strict shared native
sampling for Function Editor and storyboard operations, context-driven timing
labels, and cache cancellation after lock contention. Actual dialog acceptance
now covers width-profile creation/import/export, pose authoring and enabled
recovery retries as well as the existing helper-level contracts.

Fade rejects unmatched boundaries of painted regions before any edit; matched
filled regions remain supported. Native per-region opacity is outside this
workflow. Library provenance comes from actual native imports and registered
Library metadata, never an external-path heuristic. Scene compatibility is a
read-only inventory of known unsupported extensions, not certification of
arbitrary historical application versions. These limits are explicit in the UI
and the component ledgers.

| Executed revision | Evidence | What it does not qualify |
|---|---|---|
| Function Editor `d7c34e6ef2096b255eec7077e51c8074901e6188` | Linux, Windows and macOS application and installed-package checks passed, including both Function suites, unkeyed-channel reload and clean shutdown. | Later rename/retime, checked sampling, frozen comparisons, native curve import and large-rig additions. |
| Animation workflow `19841c2cabebf5806ea4d6887a43f7207b7ba387` | Linux and macOS application/package checks passed. Windows selected the correct export controls but failed inside rendered SVG export. | New strict baking, timing subscriptions and expanded pose/recovery checks; Windows render correctness. |
| Scene Cast `ea42f65311ed8fc0e3e8041a0cefb356650aea42` | Linux application/package checks passed. macOS reached Cast but selected no relink candidate; Windows stopped at the preceding export failure. | New ownership, collections, dock/waveform, queue and incremental-update workflows. |
| Combined diagnostic `f951b9bea7ad16838cae5876b8c48293112f497c` | Five isolated Function/Cast/timing/Cast-vector/export diagnostics passed, including scene reload, interleaved clone/key undo and atomic export cancellation. | The mandatory sequence failed its secondary-color state assertion; later metadata and audit additions are unqualified. |
| Vector diagnostic `0cc8df85d0d5664ecbbb6ed304bf13cc78bd33b8` | Linux and macOS compiled; macOS passed all 18 standalone UI tests. Both native sequences exposed blank alignment previews. | Renderer state/HDR/timing checks were not reached. |
| Combined diagnostic `ebac3062ad9e09a5c249db9096dce6bde0371c38` | All three platforms identified two Qt 5 QStringList assignment ambiguities in Cast fixtures. `f8ab55239` corrects both. Windows passed the native-isolation and bounded-GL-launcher contracts. | This revision did not reach native renderer acceptance. |
| Scene Cast `1c2c1a968b263cfde74daa578c0af18759378af5` | Linux compiled the new Cast and icon components, then rejected an unsupported TXsheet constructor in the nested timing fixture. `566e1bc5a` uses the child level's existing owned sheet and retains all assertions. | Native application workflows were not executed on this revision. |
| Function Editor `ebf078049581c76bfad470cc383563067430c146` | Linux compiled, then the native preset-import widget test timed out in its file chooser. `208f938b8` drives the actual filename and enabled Open control instead of relying on ignored focused-field selection. | The corrected driver and subsequent application checks require fresh execution. |
| Vector editing `ab7a8f4ee54ca36ef23b57a7ca46164f0c2d5cd1` | Linux passed 17 standalone UI tests; the width manager passed creation/capture/save-retry, then timed out at Import. `4c4ff918d` corrects the same focused-file-chooser driver while preserving overwrite, cancellation and error assertions. | Native vector rendering and the corrected manager workflow did not execute on this revision. |
| Scene Cast `c7fdf2aecd719fed34b0f34608e1d7019838e152` | Linux compiled and passed the animation workflow, frozen Function comparisons, unkeyed defaults reload and saved-set rename. Fractional retime scene reload then failed its keyframe-field equality check. Source review found interpolation undo omitted the adjacent endpoint; `dbf7ffa38` fixes that defect and a final-key boundary while retaining strict comparisons and adding field diagnostics. | The predicted mismatch and correction require runtime confirmation; Cast/Library checks were not reached. |

Current investigations remain explicit: fractional retime persistence, native
file-chooser acceptance, Windows SVG/vector-analysis/thumbnail rendering, the
Windows width-profile manager crash, macOS relinking, and native opacity
attachment/state/HDR qualification. Source corrections and diagnostics
are present, but their existence does not establish a runtime fix. The preview
correction uses the drawing-plane inverse when native preview widgets flatten Z,
while preserving the ordinary invertible 3D path. The HDR fixture now seeds the
actual float attachment before testing production rendering. Original pixel,
depth, stencil, rollback, cancellation and saved-content assertions remain strict.

The representative 1080p depth-rendering measurement has not yet executed.
The approximately 99.5 MB attachment snapshot per 1080p stroke is a resource
estimate, not measured practical performance. Synthetic native application
benchmarks and widget-to-image graph timings are distinct from physical tablet
latency, playback pacing and artist hardware qualification.
The depth fixture now records synchronized frame samples, actual GL target and
renderer provenance, and process peak resident memory before and after each
mode. The shared memory probe reports a cumulative high-water mark that can
include earlier fixture allocations; it does not attribute a peak delta to one
stroke. Neither these prepared probes nor the separate 2D measurements establish
the practical cost of depth rendering until that workload executes.

The component ledgers are `doc/function_editor_acceptance.md`,
`doc/vector_workflow_acceptance.md`, `doc/scene_cast_acceptance.md`,
`doc/animation_workflow_acceptance.md` and
`doc/artist_workload_and_media_cache.md`. The shared native fixtures cover locks
and timing, clone/asset identity, collections/provenance, scene persistence and
actual Lottie/SVG publication. Supported CI, including installed applications,
must run on the final integrated heads. No local builds or tests are permitted.

After a mandatory combined native failure, the runner also collects isolated
Function Editor, Cast, timing, Cast/vector, export and vector-renderer evidence.
Each diagnostic gets a fresh disposable profile, runs sequentially and has a
120-second limit. Neither a diagnostic pass nor a completion marker can replace
the original full-sequence result. Reports remain in separate artifacts and the
original report; the CI-only harness verifies isolation and failure retention.

## Acceptance

`Pending` means not yet accepted, including source work in progress. `Source-ready` requires complete implementation and fixtures; `CI-qualified` additionally requires all selected checks to pass on the exact revision. `Already satisfied` requires a source/test citation. `Blocked` requires the exact dependency, action and reason. Each track's detailed ledger supplies baseline evidence and implementation/test links.

| # | Improvement | Acceptance contract | State |
|---|---|---|---|
| 1 | Proper Fit Selection | Selected multi-channel keys, single points and constants fit; current/all actions separate. | Source-ready; CI pending |
| 2 | Searchable channels | Object/FX/parameter matching, clear/expansion restoration, selected curves remain explicit. | Source-ready; CI pending |
| 3 | Saved channel sets | Stable owner identities; rename/delete, reload and workspace persistence. | Source-ready; fresh CI pending |
| 4 | Curve solo and edit locks | Every editing command respects locks; visible reference/solo state. | Source-ready; CI pending |
| 5 | Multi-key numeric editing | Mixed values; absolute/relative operations, collisions, cancel, one undo transaction. | Source-ready; CI pending |
| 6 | Precise multi-channel retiming | Multiple channels, explicit target start/end preview, pivot/scale, quantization, collisions, repeat/undo. | Implemented; endpoint undo correction awaits reload verification |
| 7 | Normalized curve display | Display-only normalized coordinates; native values and edits preserved. | Source-ready; CI pending |
| 8 | Clear curve identification | Stable identities/colors with labels and high-DPI/theme readability. | Source-ready; CI pending |
| 9 | Visible interpolation controls | Native interpolation/handle actions; multi-selection and keyboard access. | Source-ready; CI pending |
| 10 | Explicit snapping controls | Frame/neighbor/marker targets; bypass modifier; unchanged default semantics. | Source-ready; CI pending |
| 11 | Before/after curve overlays | Snapshot comparison, accept/revert, linked/native/SeExpr ownership. | Source-ready; fresh CI pending |
| 12 | Interactive breakdown slider | Preview/cancel; stage, Plastic and offset channels; driven channels protected. | Source-ready; CI pending |
| 13 | Controlled key reduction | Independent sampled error bound, protected/held keys and undo. | Source-ready; CI pending |
| 14 | Bake expressions to keys | Sampling range/step, units and output equivalence; cancellation and undo. | Source-ready; CI pending |
| 15 | Visual motion-preset browser | Existing curve compatibility; browse/search/preview and amplitude/duration adaptation. | Source-ready; fresh CI pending |
| 16 | Channel dependency inspector | Sources/dependents, invalid references and owner navigation without second evaluator. | Source-ready; CI pending |
| 17 | Large-rig graph performance | Large-rig correctness fixture and measured CI report; no unmeasured speed claim. | Source-ready; fresh CI pending |
| 18 | Keyboard/tablet editing | Focus, accessible targets, keyboard nudges/constraints and tablet manual checklist. | Source-ready; CI pending |
| 19 | Opacity readback | Single/mixed opacity selection follows context without mutating drawing. | Source-ready; CI pending |
| 20 | Style opacity consistency | Relevant vector style render paths preserve stroke alpha with independent pixel fixtures. | Source-ready; CI pending |
| 21 | SVG stroke opacity | External SVG output carries palette alpha times stroke opacity. | Source-ready; CI pending |
| 22 | Visual width-profile manager | CRUD/import/export/previews, errors, overwrite/cancel and restart persistence. | Source-ready; native chooser and Windows crash verification pending |
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
| 33 | Multi-drawing alignment | Explicit frame/reference scope, preview/cancel, matching, groups and no index-only assumptions. | Source-ready; plane rendering correction awaits CI |
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
| 45 | Scrubbable thumbnails | Scrub/choose cover, bounds, persistence and bounded decoding. | Source-ready; fresh CI pending |
| 46 | Audio audition | Native waveform/transport, stop-on-close/selection and no leaked playback. | Source-ready; fresh CI pending |
| 47 | Tags and collections | Stable asset IDs, multi-tag queries, saved named collections and save/reload. | Source-ready; fresh CI pending |
| 48 | Stable refresh state | Identity selection/scroll/expanded folders/sort retained through changes. | Source-ready; CI pending |
| 49 | Large-Cast responsiveness | Large fixture correctness and measured incremental update/ordering. | Source-ready; fresh CI pending |
| 50 | Explicit duplicate consolidation | Content/identity/palette comparisons, explicit confirmation only, exposure-preserving undo. | Source-ready; CI pending |
| 51 | Collection/dependency report | Missing/conflicting dependencies and sizes, exact inclusion and relocated output verification. | Source-ready; CI pending |
| 52 | Linked ownership and local copy | Actual Library import/link provenance and explicit local copy with undo/collection preservation; external paths alone do not prove linkage. | Source-ready; fresh CI pending |
| 53 | Pose-library undo | Entire pose/base state, import/replace/delete, duplicate names and undo/reload. | Source-ready; fresh CI pending |
| 54 | Timing-aware storyboard moves | Cells/audio/native animation move as one undo; locked/boundary rules explicit. | Source-ready; fresh CI pending |
| 55 | Source/playback timing indicators | Clear edit source vs playback drawing, nested/remap/range transitions. | Source-ready; fresh CI pending |
| 56 | Scene compatibility report | Read-only known-extension inventory, unsupported/lossy warnings and navigation; no certification of arbitrary historical builds. | Source-ready; fresh CI pending |
| 57 | Export fidelity preflight | Lottie/SVG unsupported-feature inspection before write; cancel and explicit conversion. | Source-ready; CI pending |
| 58 | Recovery completeness status | Current snapshot complete/partial/errors/retry visible without losing pending resources. | Source-ready; fresh CI pending |
| 59 | Artist-workload benchmarks | Native synthetic drawing/scrub/playback/resource-save/memory metrics, correctness and provenance; physical tablet qualification separate. | Source-ready; CI pending |
| 60 | Safe disk-cache management | Usage/budget/manual cleanup, cross-process leases, atomic retirement, cancellation/errors. | Source-ready; under-gate cancellation checks await CI |

## Shared regression gates

- Document edits are one coherent undo operation; repeated commands, cancellation, failed persistence and reload preserve identity and prior data.
- Multi-selection and nested xsheets use stable owners and exact instance paths; no hidden or stale selection is edited inadvertently.
- Existing scenes, SeExpr bindings, particle resources and library import/link semantics retain their behavior.
- Search, previews and analysis do not mutate scene data. Destructive asset operations require a concrete preview and do not silently remove source files.
- Keyboard focus, translated captions, accessible names, high-DPI hit targets and bounded work are covered where applicable. Automated offscreen/native tests do not establish physical tablet validation.
