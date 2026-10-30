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

All 60 items remain in scope. Function Editor revision `e16fab840` passed
application and installed-package checks on all three platforms. Combined
revision `10ad236fa` passed Linux application and installed-package checks,
including the mandatory full native workflow sequence, converter, actual EXR
coverage and required reliability gates. New interrupted-dialog corrections
are newer than these revisions and need complete qualification. This is not an
all-60 completion claim. Drafts stay in validation until their exact final heads
pass required checks. Depth-opacity and Cast optimizations have focused Linux
correctness and timing evidence; substantial software depth cost remains.

Independent review reopened six source contracts. Reviewed corrections are now
integrated: Function edit/preset dialogs reject removed owners and prune detached
selections; width-profile dialogs retain their reviewed level/frame/selection;
morph Apply checks every reviewed drawing and its hints; opacity Apply rejects
intervening drawing changes; pose dialogs reject stale scene/library state;
and fill review retains cancellation across frame switches and queued acceptance.
Native fixtures exercise the real callers, interrupted dialogs, panel closure,
Apply/Cancel, repeated undo/redo and persistence where relevant. Their execution
is pending; source review alone does not close these acceptance items.

Historical macOS isolated combined-timing execution exited with SIGSEGV after
its assertions and cleanup checkpoints. Its native teardown cause is not yet
localized. The next combined qualification must exercise that isolated lifecycle
even if the main sequence passes, with zero exit required and bounded LLDB
capture on failure. The old process remains failed. The separate macOS gradient
fixture title check was corrected using the actual prompt and button roles;
Qt ignores QMessageBox titles on that platform. No product workaround or
weakened pixel, state, undo or persistence oracle substitutes for either check.

Windows vector revision `0b5e2c4f` exposed two additional runtime boundaries:
the width-manager export group encountered an unexpected nested dialog, and
the full native process timed out after the Envelope Apply/undo checkpoint.
The exact blocked call is not yet localized. Narrow driver/phase diagnostics
retain the original assertions and process timeout; no product fix is inferred
from their presence. The old Shell thumbnail failure has a reviewed standalone
TNZ correction. Subsequent SeExpr/neural launcher failures rejected retained
renderer DLLs before execution and do not establish new product regressions.
All these paths require successful execution on the corrected final revisions.

Combined Linux revision `b00db72a` stopped in the pose modal fixture. Source
review identified a driver clicking a newly created row before Qt's queued show
event; the production stale-row guard correctly rejects hidden controls. The
corrected fixture waits for an actual visible/enabled row, preserving every
choice, count, undo and saved-state assertion. Linux candidate `15a4593e`
subsequently passed the full animation workflow, including interrupted pose
actions. It then exposed a width-fixture palette baseline captured at frame 10
while the application's current frame was 0. Correction `8d2f19c5a` evaluates
the palette at the actual frame before capturing the baseline and independently
rechecks gradient animation; exact drawing comparisons remain unchanged. This
correction needs fresh execution. The animation/timing failure prefixes are
also normalized so CI annotations retain the actual cause instead of unrelated
audio-backend warnings.

Windows combined revision `b00db72a` passed 19 of 20 native UI tests, including
the corrected Shell scene/profile thumbnails, and all 16 reliability checks,
including EXR. Its width-export diagnostics identify the extra dialog as the
same file chooser observed while its overwrite confirmation returns. Declining
overwrite and then cancelling already passes. The driver correction must retain
both paths and every export assertion. The full application stopped at the old
pose fixture; uncertain descendant cleanup retained its renderer, so the artist
workload correctly refused to overwrite those files and did not execute.

A later independent review reopened two Function Editor source contracts.
Dialogs parented as stack objects to the viewer must survive panel destruction
without double deletion or stale viewer/graph access, and hiding a panel must
cancel pending writes. Edit Keys must also reject same-owner curve mutations
between opening the dialog and rebuilding its plan: inserted/deleted keys can
otherwise shift captured indices to different keys. The Function owner is
implementing guarded dialog lifetimes and exact curve baselines refreshed only
after successful own Apply, with native/widget interruption coverage. Prior
Function CI passes do not qualify these newly identified boundaries. Actual
saved-set Delete/reopen coverage is also being completed; the feature exists.

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
| Function Editor `ebf078049581c76bfad470cc383563067430c146` | Linux compiled, then the native preset-import widget test timed out in its file chooser. `208f938b8` drives the actual filename and enabled Open control instead of relying on ignored focused-field selection. macOS exposed title-based comparison-confirmation lookup in both Function fixtures; Qt ignores message-box titles there. `a6bbf59aa` matches the exact prompt, viewer owner, Question icon and enabled Yes/No controls instead. | Corrected drivers were not executed on this revision; later evidence follows below. No restore/cancel/undo assertion was relaxed. |
| Vector editing `ab7a8f4ee54ca36ef23b57a7ca46164f0c2d5cd1` | Linux passed 17 standalone UI tests; the width manager passed creation/capture/save-retry, then timed out at Import. `4c4ff918d` corrects the same focused-file-chooser driver while preserving overwrite, cancellation and error assertions. | Native vector rendering and the corrected manager workflow did not execute on this revision. |
| Scene Cast `c7fdf2aecd719fed34b0f34608e1d7019838e152` | Linux compiled and passed the animation workflow, frozen Function comparisons, unkeyed defaults reload and saved-set rename. Fractional retime scene reload then failed its keyframe-field equality check. Source review found interpolation undo omitted the adjacent endpoint; `dbf7ffa38` fixes that defect and a final-key boundary while retaining strict comparisons and adding field diagnostics. | Cast/Library checks were not reached on this revision. Later strict reload passes verify corrected behavior without establishing the exact old mismatched field. |
| Function Editor `a6bbf59aaaff6b87661b0f4485e28abb791768c8` | Linux and macOS application and installed-package jobs passed. Windows passed the full native Function workflow, including strict retime reload, but its standalone offscreen widget fixture crashed at QMessageBox. | The native Windows platform correction and bounded diagnostics are published at `e16fab840`; fresh exact-head checks are pending. |
| Animation workflow `169d240ac3fe8a423ba106ed21aaf512140f194c` | Linux and macOS application and installed-package jobs passed. Main-job logs confirm both Function component suites, strict fractional retime reload, animation workflows and clean shutdown. The Linux 96-channel/24,576-key graph painted in 31.67–31.85 ms at 1000×960. Windows failed the offscreen Function widget fixture and the legacy DIB OpenGL SVG export path. | The graph timing measures QWidget-to-QImage only. Installed checks exercise the packaged native SeExpr lifecycle. The shared Qt renderer must move earlier in the dependency stack before Windows qualification. |
| Combined workflow `47dbae4c9a048af1f795179aeaac1c4006fe61c1` | Linux compiled and passed all 19 UI tests, native cache/animation workflows, and independent Function, timing, Cast/vector and export diagnostics. The independent artist workload passed saved-data correctness. Native opacity plane, depth/stencil, secondary-color, sRGB, HDR and rollback checks passed before the 1080p visibility oracle failed. Isolated Cast also failed its final preview-submission check. | The mandatory suite failed and installed checks were skipped. These depth timings are not accepted. `79a51a99b` restores normal zero stencil-clear setup; `10e062a05` selects the actual Cast target before the scene-switch fixture. Both retain the original oracles; their later evidence is below. |
| Linux validation `6f95648db6736487ce680f5f979183317ffe9060` | The corrected 1080p workload passed all eight visible-stroke/color-mixing checks. Isolated Cast passed its actual Library provenance, ownership, UUID, import-undo and collection-rollback checks. Function, timing, Cast/vector and export diagnostics passed. GUI/background context contention passed. | The mandatory sequence and isolated renderer failed a later ordinary PLI-to-PNG oracle. `8c5b21283` corrects the fixture's missing saved palette-page membership and adds native reload preflight; it requires fresh execution. This candidate does not qualify later platform, EXR or performance changes. |
| Linux validation `b12e763a63e6df3c6b086a51f4417060298c95f7` | All cropped/full-frame attachment comparisons, procedural/shader fallbacks and eight 1080p witnesses passed. Cast fresh-file/dirty/cache state, identity, Library ownership and measured edit checks passed in isolation. The input PLI reload preflight and independent Function/timing/Cast-vector/export checks passed. | The mandatory sequence and isolated renderer still failed ordinary PLI conversion: the decoded output was 1×1 despite valid source geometry and palette. Source review identified palette-less bounds calculation in the converter. Reliability/EXR and installed checks were not reached. This candidate also predates later runner cleanup and gradient-cancel corrections. |
| Function Editor `e16fab84080d915ee15ab7f18d6dcabcfcb689ae` | Linux, macOS and Windows application and installed-package jobs passed. The native Function workflows, strict fractional-retime reload and clean application exits passed. Windows standalone widgets used their native platform successfully. | New same-xsheet owner-removal, detached-selection and interrupted preset corrections in `4d1e2fc38`/`efdd9139f` still need exact-head CI. Widget-to-image timings do not qualify interaction latency. |
| Function Editor `efdd9139faff46242aaee54302556dbb222917a4` | Linux, Windows and macOS main application checks passed, including owner-removal guards and clean exit. Linux and Windows installed-package checks also passed. | macOS installed checks remain pending. New panel-lifetime, same-owner key-drift and saved-set Delete fixtures are later source changes and need fresh qualification. |
| Scene Cast `0b4e5032a34c88198204f38982e1d4d82dd7f919` | Native Cast and actual Library workflows passed on Linux, Windows and macOS. Windows/macOS Cast reliability helpers passed. Matching component-context macOS add/remove/rename measured 318/303/304 ms versus 1559/1714/1558 ms on `4016d693f`; identity/order/scroll and one-refresh checks passed. | Overall jobs failed later inherited converter/Shell paths; this is component evidence, not full platform qualification. Later published Cast revisions require their own final checks. |
| Combined workflow `b00db72a35be48bf7852cbbc49a113622985761d` | Linux passed native UI checks, all six isolated diagnostics and the independent artist workload. Windows passed corrected Shell/profile thumbnails, 19 of 20 native UI tests, all 16 reliability tests including EXR, and all 16 SeExpr tests. | Both mandatory sequences failed in the old pose modal fixture before new native vector editing guards ran. Windows width export failed its extra-dialog oracle and its artist workload did not execute because retained renderer files blocked setup. Linux reliability and both installed checks were skipped. Passing diagnostics do not replace the failed sequences. |
| Linux validation `15a4593e2247b37000ef22b8c4bd58474002cc7f` | The corrected pose driver passed the full animation workflow. All six isolated diagnostics and the independent artist workload passed. | The mandatory sequence next failed its exact width drawing comparison because the fixture captured an evaluated palette from a different frame. Reviewed fixture correction `8d2f19c5a` retains exact comparisons and gradient assertions but is unexecuted. Reliability and installed checks were skipped. |
| Combined workflow `10ad236fa5e0ae91b71fae025a260bb7a070d240` | Linux application and installed-package jobs passed, including the mandatory native sequence, SeExpr, reliability/EXR and GLB checks. Windows passed its full native sequence, artist workload and reliability/EXR gates. | Windows overall failed standalone width-export and old scene-thumbnail checks, followed by retained-renderer preflight refusals; installed checks were skipped. macOS was superseded during compilation. This revision predates the six interrupted-dialog corrections and the standalone scene-thumbnail fix. Later readiness must use the actual final heads. |
| Windows combined `47dbae4c9a048af1f795179aeaac1c4006fe61c1` | Build, packaging, native neural host, Cadmium adapters, animation workflow and independent Function/timing/Cast-vector/export diagnostics passed. The 2D artist workload passed correctness. | Function and width-profile standalone fixtures crashed on Qt offscreen QMessageBox; the Shell test wrapper could not print UTF-8 to cp1252; Cast reliability exposed an EXR new-asset state defect. The old 1080p and final Cast preview fixtures also failed. Each has a source correction prepared, not a current-head pass. |

Current work remains explicit: integrate and execute the six interrupted-dialog
corrections; verify the standalone scene-thumbnail fallback and UTF-8 failure
reporting; obtain complete final-head platform and installed-package results;
and localize the historical macOS shutdown failure if it recurs. The shared
renderer dependency, native converter and retained EXR state corrections are
implemented; the combined Linux revision above supplies their current execution
evidence. Measured depth opacity and Cast responsiveness retain the limits below. Fractional retime reload, file-chooser acceptance,
macOS relinking and corrected Cast scene-switch/Library acceptance now have
execution evidence on the revisions above; later revisions still need their
own checks. Source corrections and diagnostics do not establish a runtime fix.
The preview
correction uses the drawing-plane inverse when native preview widgets flatten Z,
while preserving the ordinary invertible 3D path. The HDR fixture now seeds the
actual float attachment before testing production rendering. Original pixel,
depth, stencil, rollback, cancellation and saved-content assertions remain strict.

The original 1080p workload on `47dbae4c` failed its visibility oracle, so its
timings are not accepted evidence. Corrected Linux candidate `6f95648db` passed
all visibility/mixing checks on a 1920×1080 D24S8 target with Qt 5.15.3 and
llvmpipe. For eight strokes and three synchronized frames per mode, the mandatory
sequence measured native frames at 8.24–8.45 ms, scalar opacity at 428–667 ms and
profile opacity at 405–423 ms. The isolated renderer measured the same material
cost: native 8.22–8.34 ms, scalar 382–687 ms and profile 420–446 ms. This workload
is not accepted as interactive depth performance. A conservative color/depth
region optimization now retains full stencil coverage and a full-frame fallback. Candidate `b12e763a6` subsequently passed forced-full versus
cropped attachment equivalence, rollback, state and fallback checks. Its
mandatory scalar frames measured 234/176/176 ms and profile frames 219/206/210
ms; isolated scalar frames measured 234/164/163 ms and profile frames 215/207/210
ms. Native frames measured 10.6–11.0 ms on this run, so the raw observations also
show runner variance. Residual depth cost remains substantial; these results
do not establish interactive performance on artist hardware.

The full snapshot allocates 99,532,800 bytes per modulated 1080p stroke. Observed
process peak resident memory rose from 588.5 to 827.2 MB in the mandatory
sequence and 470.5 to 692.3 MB in isolation. These are cumulative high-water
marks, not attributed per-stroke allocations. The cropped workload's actual
peak snapshot payload was 26,446,760 bytes. Per three-frame modulated mode it
transferred 17,744,328 color pixels and the same number of depth pixels, while
retaining all 149,299,200 stencil transfers across 24 transactions. Cumulative
process peaks on this run were 585.6 MB in the mandatory sequence and 469.7 MB
in isolation; unchanged per-mode high-water marks do not mean zero allocations.

Large-Cast single-add/remove/
rename notifications on component `4016d693f` measured 422/421/421 ms on Linux,
approximately 1.6–1.7 seconds on macOS and 3.0 seconds on Windows with 3,000
assets. Isolated Linux candidate `6f95648db` measured 387/384/391 ms; use this
matching isolated context for later comparisons, not the component sequence
after preceding workflows. Identity and scroll checks passed, but responsiveness
remains pending across platforms. `6adb14baa` shares status work within each
refresh and avoids empty-query matching. Candidate `b12e763a6` measured
115/114/114 ms in the matching isolated context, with exactly one refresh per
edit. Filter/status work still accounted for about 108 ms and approximately
3,006 logical disk queries. Its setup/show/explicit-refresh timing is not a
comparable one-refresh baseline, and no speedup is inferred from that field.

Synthetic native application benchmarks and widget-to-image graph timings are
distinct from physical tablet latency, playback pacing and artist hardware
qualification. No physical tablet or real-time pacing claim is made.
The depth fixture now records synchronized frame samples, actual GL target and
renderer provenance, and process peak resident memory before and after each
mode. The shared memory probe reports a cumulative high-water mark that can
include earlier fixture allocations; it does not attribute a peak delta to one
stroke. The separate 2D measurements do not substitute for depth rendering or
physical input/presentation measurements.

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
120-second child deadline, with bounded termination/diagnostic handling after it.
Neither a diagnostic pass nor a completion marker can replace the original
full-sequence result. Separate report files and the original report are uploaded;
the CI-only harness verifies isolation and failure retention.

## Acceptance

`Pending` means not yet accepted, including source work in progress. `Source-ready` requires complete implementation and fixtures; `CI-qualified` additionally requires all selected checks to pass on the exact revision. `Already satisfied` requires a source/test citation. `Blocked` requires the exact dependency, action and reason. Each track's detailed ledger supplies baseline evidence and implementation/test links.

| # | Improvement | Acceptance contract | State |
|---|---|---|---|
| 1 | Proper Fit Selection | Selected multi-channel keys, single points and constants fit; current/all actions separate. | Source-ready; CI pending |
| 2 | Searchable channels | Object/FX/parameter matching, clear/expansion restoration, selected curves remain explicit. | Source-ready; CI pending |
| 3 | Saved channel sets | Stable owner identities; rename/delete, reload and workspace persistence. | Pending; panel-lifetime guard and actual Delete/reopen coverage in progress |
| 4 | Curve solo and edit locks | Every editing command respects locks; visible reference/solo state. | Source-ready; CI pending |
| 5 | Multi-key numeric editing | Mixed values; absolute/relative operations, collisions, cancel, one undo transaction. | Pending; panel lifetime and same-owner key-index drift corrections in progress |
| 6 | Precise multi-channel retiming | Multiple channels, explicit target start/end preview, pivot/scale, quantization, collisions, repeat/undo. | Pending; reviewed-key baselines and panel-lifetime corrections in progress |
| 7 | Normalized curve display | Display-only normalized coordinates; native values and edits preserved. | Source-ready; CI pending |
| 8 | Clear curve identification | Stable identities/colors with labels and high-DPI/theme readability. | Source-ready; CI pending |
| 9 | Visible interpolation controls | Native interpolation/handle actions; multi-selection and keyboard access. | Source-ready; CI pending |
| 10 | Explicit snapping controls | Frame/neighbor/marker targets; bypass modifier; unchanged default semantics. | Source-ready; CI pending |
| 11 | Before/after curve overlays | Snapshot comparison, accept/revert, linked/native/SeExpr ownership. | Pending; comparison restoration must remain safe after panel hide/destruction |
| 12 | Interactive breakdown slider | Preview/cancel; stage, Plastic and offset channels; driven channels protected. | Pending; shared dialog lifetime and same-owner mutation guard in progress |
| 13 | Controlled key reduction | Independent sampled error bound, protected/held keys and undo. | Pending; shared dialog lifetime and same-owner mutation guard in progress |
| 14 | Bake expressions to keys | Sampling range/step, units and output equivalence; cancellation and undo. | Pending; shared dialog lifetime and same-owner mutation guard in progress |
| 15 | Visual motion-preset browser | Existing curve compatibility; browse/search/preview and amplitude/duration adaptation. | Pending; main/nested preset dialog ownership and interruption coverage in progress |
| 16 | Channel dependency inspector | Sources/dependents, invalid references and owner navigation without second evaluator. | Pending; dependency dialog lifetime correction in progress |
| 17 | Large-rig graph performance | Large-rig correctness fixture and measured CI report; no unmeasured speed claim. | Source-ready; fresh CI pending |
| 18 | Keyboard/tablet editing | Focus, accessible targets, keyboard nudges/constraints and tablet manual checklist. | Source-ready; CI pending |
| 19 | Opacity readback | Single/mixed opacity selection follows context without mutating drawing. | Source-ready; CI pending |
| 20 | Style opacity consistency | Relevant vector style render paths preserve stroke alpha with independent pixel fixtures. | Source-ready; focused Linux ROI equivalence passed, final-head/platform CI pending |
| 21 | SVG stroke opacity | External SVG output carries palette alpha times stroke opacity. | Source-ready; CI pending |
| 22 | Visual width-profile manager | CRUD/import/export/previews, errors, overwrite/cancel and restart persistence. | Source-ready; frozen targets and actual interrupted-manager acceptance await CI |
| 23 | Width profiles across drawings | Selected drawings/level range, repeated exposures once, locked resources and undo. | Source-ready; reviewed level/frame/selection and off-frame content guards await CI |
| 24 | Pressure/profile blending | Zero-strength identity, replace/multiply semantics and pressure preservation. | Source-ready; frozen-target blended Apply and full undo/reload checks await CI |
| 25 | Fill-safe reshaping | Actual region-change preview/recovery in geometry edit flow; fills/undo saved. | Source-ready; sticky frame-switch cancellation and actual queued Apply checks await CI |
| 26 | Persistent morph hints | Stable drawing/stroke identity, edits/deletions, reopen and compatibility. | Source-ready; CI pending |
| 27 | Correspondence preview | Matched/manual/unmatched preview matches committed inbetweens. | Source-ready; full range/hint snapshots, actual caller and panel-lifetime checks await CI |
| 28 | Unmatched stroke handling | Explicit birth/death policy including Fade; endpoint, fill and opacity preservation, undo. | Source-ready; reviewed-range guards and interrupted native morph checks await CI |
| 29 | Cross-frame gradient uniqueness | Single local/shared other-frame style, other drawings unchanged, undo. | Source-ready; CI pending |
| 30 | Animated gradient copying | Copy current vs animated style choice; keyframes, unlinking and reload. | Source-ready; cancellation/reload passed on Linux baseline, corrected macOS driver awaits CI |
| 31 | Outline stacking preservation | Native overlap/group pixel fidelity through outline conversion and undo. | Source-ready; CI pending |
| 32 | Closed-loop outlines | Inner/outer boundaries, tight loops, holes/winding and undo. | Source-ready; CI pending |
| 33 | Multi-drawing alignment | Explicit frame/reference scope, preview/cancel, matching, groups and no index-only assumptions. | Source-ready; native preview/cancel/apply passed on Linux baseline, final-head CI pending |
| 34 | Paint Inside/Selection | Clipping to region/selection; holes/groups, pressure, undo. | Source-ready; CI pending |
| 35 | Guided Drawing mode consistency | Guided Drawing honors paint mode or provides actionable explicit incompatibility. | Source-ready; CI pending |
| 36 | Along-stroke opacity | Opacity variation survives draw/split/join/morph/deform/render/export and versioned storage. | Source-ready; reviewed snapshot/context guards await CI; full converter/render baseline passed on Linux |
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
| 49 | Large-Cast responsiveness | Large fixture correctness and measured incremental update/ordering. | Source-ready; focused Linux measured improvement, final-head/platform qualification pending |
| 50 | Explicit duplicate consolidation | Content/identity/palette comparisons, explicit confirmation only, exposure-preserving undo. | Source-ready; CI pending |
| 51 | Collection/dependency report | Missing/conflicting dependencies and sizes, exact inclusion and relocated output verification. | Source-ready; CI pending |
| 52 | Linked ownership and local copy | Actual Library import/link provenance and explicit local copy with undo/collection preservation; external paths alone do not prove linkage. | Source-ready; fresh CI pending |
| 53 | Pose-library undo | Entire pose/base state, import/replace/delete, duplicate names and undo/reload. | Source-ready; detached scene/library context guards and native interruption checks await CI |
| 54 | Timing-aware storyboard moves | Cells/audio/native animation move as one undo; locked/boundary rules explicit. | Source-ready; fresh CI pending |
| 55 | Source/playback timing indicators | Clear edit source vs playback drawing, nested/remap/range transitions. | Source-ready; fresh CI pending |
| 56 | Scene compatibility report | Read-only known-extension inventory, unsupported/lossy warnings and navigation; no certification of arbitrary historical builds. | Source-ready; fresh CI pending |
| 57 | Export fidelity preflight | Lottie/SVG unsupported-feature inspection before write; cancel and explicit conversion. | Source-ready; CI pending |
| 58 | Recovery completeness status | Current snapshot complete/partial/errors/retry visible without losing pending resources. | Source-ready; fresh CI pending |
| 59 | Artist-workload benchmarks | Native synthetic drawing/scrub/playback/resource-save/memory metrics, correctness and provenance; physical tablet qualification separate. | Source-ready; focused Linux baseline/optimization measurements captured, final-head/platform CI pending |
| 60 | Safe disk-cache management | Usage/budget/manual cleanup, cross-process leases, atomic retirement, cancellation/errors. | Source-ready; Linux cache checks passed on 6f95648db, final-head/platform CI pending |

## Shared regression gates

- Document edits are one coherent undo operation; repeated commands, cancellation, failed persistence and reload preserve identity and prior data.
- Multi-selection and nested xsheets use stable owners and exact instance paths; no hidden or stale selection is edited inadvertently.
- Existing scenes, SeExpr bindings, particle resources and library import/link semantics retain their behavior.
- Search, previews and analysis do not mutate scene data. Destructive asset operations require a concrete preview and do not silently remove source files.
- Keyboard focus, translated captions, accessible names, high-DPI hit targets and bounded work are covered where applicable. Automated offscreen/native tests do not establish physical tablet validation.
