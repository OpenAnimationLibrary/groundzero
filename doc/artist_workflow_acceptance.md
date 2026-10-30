# Artist workflow acceptance ledger

Current baseline: `edition` at `5fc674f1edefc0a45d377951b8d064ba8b202f44` (2026-10-30),
including the separately owned reference/neural work in #114 and build-artifact
ignore rules in #121, plus the Qt translation configuration correction in #122. Original research
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

The latest executed combined candidate, `5d02222bf`, failed qualification.
Its first mandatory failure was `gradient fixture original owner unavailable`;
the later lifecycle setup also could not read its native scene-transition
fixture. Every isolated process exited unsuccessfully at that lifecycle setup,
including those which printed a component PASS. Cast additionally failed its
fresh-tag undo assertion. Standalone UI passed 18 of 19 tests; the width-profile
owner case failed its exact authored-sample check. The independent artist
workload and 26 native-runner contract tests passed, but do not qualify the
failed application sequence. Installed checks were skipped.

Reviewed corrections preserve those acceptance contracts. `b722d6810` marks
authored lifecycle inputs dirty before native save and independently reopens
the actual scene, drawing and palette. `c99fbf0aa` compares width cancellation
against the actual loaded profile samples and requests/verifies native window
activation before owner interruption. `9f15d6b81` prevents programmatic scene
root labels from entering folder-rename undo, with visible/hidden selected-root
regressions and bounded fresh-tag diagnostics. The old compound tag assertion
did not localize its mismatched field; the unintended rename path is established
by source review, and the full tag workflow still needs new-head execution.
`7815bdc93` preserves the first native assertion in CI reports and exposes later
lifecycle failures separately. No timeout, exact state comparison, undo count
or file assertion was relaxed. These changes are integrated and source-reviewed;
complete supported-platform and installed-package qualification remains pending.

All 60 items remain in scope. Function Editor revision `efdd9139f` passed
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
its assertions and cleanup checkpoints. The mandatory isolated gate at
`b00db72a` reproduced it and both LLDB captures identify an active thumbnail
worker reading `QAction::isChecked` through `StageBuilder::addFrame` while the
main thread destroys UI actions. Reviewed correction `db83112a9` snapshots the
GUI Camera Test value and removes that worker-side widget access. Reviewed core
correction `cd3ee23e0` rejects new icon work, accounts for every admitted task
including superseded requests, and requires native thread/TLS retirement.
Reviewed foundation `630acbe34` completes accepted/canceled Quit, programmatic
exit and ordinary New/Load transitions. Scene/Xsheet readers retain their actual
source scenes, including already replaced scenes; final releases remain on the
GUI thread. Destructive scene changes temporarily drain workers and resume on
a prepared executor, with reentry guards and a deferred repaint. Terminal exit
never reopens admission. Native fixtures cover the real decisions, source
retirement, queued operations and resumed rendering. Runner correction
`d40884741` requires both lifecycle and drain evidence as well as zero exit.
Details and remaining CI gates: [native icon lifecycle](native_icon_lifecycle.md).
This is distinct from the Function segment observer defect. Final qualification
must retain the isolated lifecycle gate, zero exit and bounded LLDB capture;
the old process remains failed. The separate macOS gradient
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
overwrite and then cancelling already passes. Reviewed correction `b49b14421`
tracks only currently executing dialog actions, preserving both paths and every
export assertion. Its alignment fixture also checks exact intervening opacity,
frame identity and full drawing content before cleanup. These fixture changes
need fresh execution. The full application stopped at the old
pose fixture; uncertain descendant cleanup retained its renderer, so the artist
workload correctly refused to overwrite those files and did not execute.

A later independent review reopened two Function Editor source contracts.
Dialogs parented as stack objects to the viewer must survive panel destruction
without double deletion or stale viewer/graph access, and hiding a panel must
cancel pending writes. Edit Keys must also reject same-owner curve mutations
between opening the dialog and rebuilding its plan: inserted/deleted keys can
otherwise shift captured indices to different keys. Reviewed correction
`a88f2ce710` uses independently owned dialogs, guarded native transient windows
and exact curve baselines refreshed only after successful own Apply. Native
fixtures exercise 24 actual panel hide/delete routes, including nested prompts,
and widget fixtures exercise eight foreign-key drift cases with exact frozen
comparison, document and history oracles. Saved-set Save/Rename/Delete/reopen is
also covered. These are source-reviewed changes awaiting execution; prior
Function passes do not qualify the newly identified boundaries.

Source review also reopened Cast panel lifetime handling. The new Collections
and Actions menus and several asset dialogs are stack objects parented to the
browser, with raw browser accesses after nested event loops. Browser deletion
can destroy those stack objects; hiding does not consistently cancel queued
actions. This is a source exposure, not a reproduced crash. Reviewed correction
`aee66fbed` shares sticky browser/ancestor/scene invalidation across nested
dialogs, progress, confirmations and publication. It also guards source accesses
after event-pumping scans and resolves folder items again after confirmation.
Thirty-three actual interruption boundaries, repeated cancellation and exact
state/file/navigation/history checks are prepared. Earlier Cast passes do not
qualify these new guards; fresh supported-platform execution is required.

Linux candidate `3dd99b2f6` passed 19 native UI tests and the corrected pose and
width-interruption cases, but the mandatory sequence failed at repeated width
Undo. Source diagnosis found that native PLI saving evaluates animation keys on
the live palette and leaves its displayed frame changed. Reviewed correction
`6a505c932` serializes a detached palette, with unchanged exact Undo oracles,
source/palette binding and byte invariance, and independent animation reload.
The isolated Function process also crashed in `ParamCalculatorNode::onChange`
from `TDoubleParam::setKeyframe` inside the real panel-lifetime matrix. The
expression-observer lifetime was subsequently diagnosed below. Original stack-dialog
ownership review does not substitute for resolving this runtime crash. Other
five isolated groups and the artist workload passed; reliability, EXR and the
installed application were skipped. None of these passes replace the failed
mandatory or isolated Function results.

Source diagnosis identified a missing observer removal in
`FunctionSegmentViewer` destruction: both setters register the widget with its
curve, but destruction only releases that curve. A later source-key update can
reach the freed dependent-channel widget. Reviewed correction `83dcde0e7`
adds symmetric removal and deterministic post-close source/dependent mutation
regressions while preserving all 24 original panel routes. This does
not establish the cause of the separate historical macOS shutdown failure.

Linux candidate `9629a7c5c` passed the standalone native UI suite and the isolated
Function process, including all 24 panel-lifetime cases and post-close observer
notifications, with zero exit. Its mandatory process timed out after 600 seconds.
The last vector checkpoint preceded the second post-save Undo/Redo pair; it
does not prove that pair completed. The earlier palette-save invariance checks
and first pair passed. Reviewed correction `d8f696db3` adds precise action/settle
checkpoints and drives the real modified-resource New Scene confirmation with
an independent timer. It does not clear dirty resources or relax comparisons,
and the observed timeout cause remains unlocalized until fresh execution.

The same candidate's isolated Cast process crashed after a folder tree remained
registered as the current selection beyond its destruction. Reviewed correction
`1630d4366` detaches that selection while the tree is alive and adds a real
destruction/replacement witness. Its Cast/vector diagnostic also failed the
consolidation confirmation oracle: the driver closed the outer report before
the nested action returned. Correction `ec76ca8cf` waits for the actual click to
return while preserving all validation, confirmation and history assertions.
Timing, export, renderer and artist diagnostics passed; reliability/EXR and the
installed application were skipped. Reporting correction `a31e8dcf3` keeps the
mandatory timeout primary and reports isolated failures separately. The original
failed process and its deadlines remain unchanged.

The full-scope source audit also reopened original-window lifetime handling in
width-profile, fill, alignment and opacity reviews. These stack dialogs used
a deletable active window as their QObject parent; nested width-library and
file dialogs additionally needed sticky cancellation after owner hide/destruction.
Morph and Make Unique Gradient also needed original-owner hide/close cancellation,
although their existing allocation avoids the stack-parent deletion exposure.
This is a source exposure, not a reproduced crash. Reviewed correction `279f6eab2`
provides independently owned dialogs, sticky original-owner and ancestor
cancellation, guarded nested library/file actions, and actual queued-delivery
witnesses with exact source/history/byte preservation. Custom gradient choices
retain their real button semantics; fresh reusable morph reviews re-enable.
Source review is complete; supported-platform execution remains pending.

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
| Function Editor `efdd9139faff46242aaee54302556dbb222917a4` | Linux, Windows and macOS application and installed-package checks passed, including owner-removal guards and clean exit. | New panel-lifetime, same-owner key-drift, saved-set Delete and worker-retirement changes are later revisions and need fresh qualification. |
| Scene Cast `0b4e5032a34c88198204f38982e1d4d82dd7f919` | Native Cast and actual Library workflows passed on Linux, Windows and macOS. Windows/macOS Cast reliability helpers passed. Matching component-context macOS add/remove/rename measured 318/303/304 ms versus 1559/1714/1558 ms on `4016d693f`; identity/order/scroll and one-refresh checks passed. | Overall jobs failed later inherited converter/Shell paths; this is component evidence, not full platform qualification. Later published Cast revisions require their own final checks. |
| Combined workflow `b00db72a35be48bf7852cbbc49a113622985761d` | Linux passed native UI checks, all six isolated diagnostics and the independent artist workload. Windows passed corrected Shell/profile thumbnails, 19 of 20 native UI tests, all 16 reliability tests including EXR, and all 16 SeExpr tests. | Both mandatory sequences failed in the old pose modal fixture before new native vector editing guards ran. Windows width export failed its extra-dialog oracle and its artist workload did not execute because retained renderer files blocked setup. Linux reliability and both installed checks were skipped. Passing diagnostics do not replace the failed sequences. |
| Linux validation `15a4593e2247b37000ef22b8c4bd58474002cc7f` | The corrected pose driver passed the full animation workflow. All six isolated diagnostics and the independent artist workload passed. | The mandatory sequence next failed its exact width drawing comparison because the fixture captured an evaluated palette from a different frame. Reviewed fixture correction `8d2f19c5a` retains exact comparisons and gradient assertions but is unexecuted. Reliability and installed checks were skipped. |
| Linux validation `3dd99b2f69262dbe7612efc90dc452b6fbc48deb` | All 19 native UI tests passed. The full sequence passed pose context changes and width interruption guards. Five isolated groups and the independent artist workload passed. Native saved-set Save/Rename/Delete/reopen, target-range reload and removed-owner guards passed before the separate Function failure. | Mandatory integration failed repeated width Undo after a native PLI save changed the live evaluated palette. The isolated Function process crashed during the actual panel-lifetime matrix in `ParamCalculatorNode::onChange`; observer lifetime is under investigation. Reliability/EXR and installed checks were skipped. This revision predates the Cast modal-lifetime correction. |
| Linux validation `9629a7c5c216fd72ce4cb5418207ea00dcb5de3a` | Standalone native UI and isolated Function passed, including the 24 panel routes and actual post-close source/dependent notifications. Palette-save invariance and the first post-save width Undo/Redo pair passed. Timing, export, renderer and artist diagnostics passed. | Mandatory integration timed out after 600 seconds; the last checkpoint preceded the second post-save pair. Isolated Cast crashed through stale current selection, and Cast/vector failed the consolidation confirmation oracle. Reliability/EXR and installed checks were skipped. New Cast detach, vector driver, reporting and shutdown corrections are unexecuted. |
| Linux validation `5d02222bf8f8fbc4735229d32604ba31a25b2dc1` | Standalone UI passed 18 of 19 tests. All 33 Cast interruption checkpoints completed. The independent artist workload and 26 native-runner contract tests passed. | Mandatory integration first failed gradient owner setup, then lifecycle file setup. All six isolated processes failed lifecycle setup; Cast also failed its fresh-tag undo assertion. Component PASS markers do not count as process passes. The standalone width owner case failed its loaded-sample comparison. Installed checks were skipped. Reviewed fixture, Cast root-label and reporting corrections need new-head execution. |
| Combined workflow `10ad236fa5e0ae91b71fae025a260bb7a070d240` | Linux application and installed-package jobs passed, including the mandatory native sequence, SeExpr, reliability/EXR and GLB checks. Windows passed its full native sequence, artist workload and reliability/EXR gates. | Windows overall failed standalone width-export and old scene-thumbnail checks, followed by retained-renderer preflight refusals; installed checks were skipped. macOS was superseded during compilation. This revision predates the six interrupted-dialog corrections and the standalone scene-thumbnail fix. Later readiness must use the actual final heads. |
| Windows combined `47dbae4c9a048af1f795179aeaac1c4006fe61c1` | Build, packaging, native neural host, Cadmium adapters, animation workflow and independent Function/timing/Cast-vector/export diagnostics passed. The 2D artist workload passed correctness. | Function and width-profile standalone fixtures crashed on Qt offscreen QMessageBox; the Shell test wrapper could not print UTF-8 to cp1252; Cast reliability exposed an EXR new-asset state defect. The old 1080p and final Cast preview fixtures also failed. Each has a source correction prepared, not a current-head pass. |

Current work remains explicit: execute the reviewed scene-transition,
worker-retirement, vector owner-lifetime and Cast-selection corrections,
localize the mandatory vector timeout, and obtain complete final-head platform
and installed-package results. The macOS shutdown failure has an actual worker
stack and must pass on the corrected revision; it is not a speculative defect.
The shared
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
| 3 | Saved channel sets | Stable owner identities; rename/delete, reload and workspace persistence. | Source-ready; panel-lifetime guard and actual Delete/reopen coverage await CI |
| 4 | Curve solo and edit locks | Every editing command respects locks; visible reference/solo state. | Source-ready; CI pending |
| 5 | Multi-key numeric editing | Mixed values; absolute/relative operations, collisions, cancel, one undo transaction. | Source-ready; panel lifetime and same-owner key-index drift corrections await CI |
| 6 | Precise multi-channel retiming | Multiple channels, explicit target start/end preview, pivot/scale, quantization, collisions, repeat/undo. | Source-ready; reviewed-key baselines and panel-lifetime corrections await CI |
| 7 | Normalized curve display | Display-only normalized coordinates; native values and edits preserved. | Source-ready; CI pending |
| 8 | Clear curve identification | Stable identities/colors with labels and high-DPI/theme readability. | Source-ready; CI pending |
| 9 | Visible interpolation controls | Native interpolation/handle actions; multi-selection and keyboard access. | Source-ready; CI pending |
| 10 | Explicit snapping controls | Frame/neighbor/marker targets; bypass modifier; unchanged default semantics. | Source-ready; CI pending |
| 11 | Before/after curve overlays | Snapshot comparison, accept/revert, linked/native/SeExpr ownership. | Source-ready; exact frozen comparison and safe hide/destruction restoration await CI |
| 12 | Interactive breakdown slider | Preview/cancel; stage, Plastic and offset channels; driven channels protected. | Source-ready; shared dialog lifetime and same-owner mutation guard await CI |
| 13 | Controlled key reduction | Independent sampled error bound, protected/held keys and undo. | Source-ready; shared dialog lifetime and same-owner mutation guard await CI |
| 14 | Bake expressions to keys | Sampling range/step, units and output equivalence; cancellation and undo. | Source-ready; shared dialog lifetime and same-owner mutation guard await CI |
| 15 | Visual motion-preset browser | Existing curve compatibility; browse/search/preview and amplitude/duration adaptation. | Source-ready; main/nested preset dialog ownership and interruption coverage await CI |
| 16 | Channel dependency inspector | Sources/dependents, invalid references and owner navigation without second evaluator. | Source-ready; dependency dialog lifetime correction awaits CI |
| 17 | Large-rig graph performance | Large-rig correctness fixture and measured CI report; no unmeasured speed claim. | Source-ready; fresh CI pending |
| 18 | Keyboard/tablet editing | Focus, accessible targets, keyboard nudges/constraints and tablet manual checklist. | Source-ready; CI pending |
| 19 | Opacity readback | Single/mixed opacity selection follows context without mutating drawing. | Source-ready; CI pending |
| 20 | Style opacity consistency | Relevant vector style render paths preserve stroke alpha with independent pixel fixtures. | Source-ready; focused Linux ROI equivalence passed, final-head/platform CI pending |
| 21 | SVG stroke opacity | External SVG output carries palette alpha times stroke opacity. | Source-ready; CI pending |
| 22 | Visual width-profile manager | CRUD/import/export/previews, errors, overwrite/cancel and restart persistence. | Source-ready; reviewed original-owner/nested cancellation and strict repeated Undo/save qualification await CI |
| 23 | Width profiles across drawings | Selected drawings/level range, repeated exposures once, locked resources and undo. | Source-ready; shared manager owner-lifetime correction and strict repeated Undo/save qualification await CI |
| 24 | Pressure/profile blending | Zero-strength identity, replace/multiply semantics and pressure preservation. | Source-ready; shared manager owner-lifetime correction and full undo/save invariance await CI |
| 25 | Fill-safe reshaping | Actual region-change preview/recovery in geometry edit flow; fills/undo saved. | Source-ready; original-owner cancellation and actual queued Apply coverage await CI |
| 26 | Persistent morph hints | Stable drawing/stroke identity, edits/deletions, reopen and compatibility. | Source-ready; CI pending |
| 27 | Correspondence preview | Matched/manual/unmatched preview matches committed inbetweens. | Source-ready; original Filmstrip hide/close cancellation and actual owner-interruption coverage await CI |
| 28 | Unmatched stroke handling | Explicit birth/death policy including Fade; endpoint, fill and opacity preservation, undo. | Source-ready; shared morph original-owner hide/close guard awaits CI; reviewed-range assertions retained |
| 29 | Cross-frame gradient uniqueness | Single local/shared other-frame style, other drawings unchanged, undo. | Source-ready; original-owner cancellation before accepted palette changes awaits CI |
| 30 | Animated gradient copying | Copy current vs animated style choice; keyframes, unlinking and reload. | Source-ready; original-owner cancellation awaits CI; cancellation/reload passed on Linux baseline |
| 31 | Outline stacking preservation | Native overlap/group pixel fidelity through outline conversion and undo. | Source-ready; CI pending |
| 32 | Closed-loop outlines | Inner/outer boundaries, tight loops, holes/winding and undo. | Source-ready; CI pending |
| 33 | Multi-drawing alignment | Explicit frame/reference scope, preview/cancel, matching, groups and no index-only assumptions. | Source-ready; original window lifetime correction and actual owner hide/delete coverage await CI |
| 34 | Paint Inside/Selection | Clipping to region/selection; holes/groups, pressure, undo. | Source-ready; CI pending |
| 35 | Guided Drawing mode consistency | Guided Drawing honors paint mode or provides actionable explicit incompatibility. | Source-ready; CI pending |
| 36 | Along-stroke opacity | Opacity variation survives draw/split/join/morph/deform/render/export and versioned storage. | Source-ready; original window lifetime correction and actual owner hide/delete coverage await CI; full converter/render baseline passed on Linux |
| 37 | Global Cast search | Name/path, folder/global scope, stable selection. | Source-ready; CI pending |
| 38 | Cast filters | Types and resource status predicates including modified assets, combined with search. | Source-ready; fresh CI pending |
| 39 | Where Used navigation | All nested occurrences, cycles bounded, exact instance navigation and multiselection. | Source-ready; Cast panel lifetime and queued navigation guards await CI |
| 40 | Batch relink preview | Ambiguous matches, explicit preview, identity/exposure preservation and undo. | Source-ready; original operation scope and interrupted preparation guards await CI |
| 41 | Distinct asset states | New/dirty/missing/external/read-only/cache states truthful without changing artwork. | Source-ready; fresh CI pending |
| 42 | Unused-asset cleanup preview | Review/checklist, keep-spare metadata, undo without deleting source files. | Source-ready; Cast panel lifetime and queued confirmation guards await CI |
| 43 | Folder undo | Create/rename/delete hierarchy and membership restoration, redo. | Source-ready; snapshot/fresh-item validation and interrupted confirmation plus repeated Undo await CI |
| 44 | Docked asset inspector | Resolution/DPI/frame gaps/palette/storage follow selection in a persistent docked panel. | Source-ready; fresh CI pending |
| 45 | Scrubbable thumbnails | Scrub/choose cover, bounds, persistence and bounded decoding. | Source-ready; reviewed source leases, scene-change pause/resume and terminal worker retirement require fresh exact-head CI, including macOS clean exit |
| 46 | Audio audition | Native waveform/transport, stop-on-close/selection and no leaked playback. | Source-ready; audio-error prompt guards await CI; physical playback remains separately unqualified |
| 47 | Tags and collections | Stable asset IDs, multi-tag queries, saved named collections and save/reload. | Source-ready; Cast menu/nested dialog lifetime and late-action guards await CI |
| 48 | Stable refresh state | Identity selection/scroll/expanded folders/sort retained through changes. | Source-ready; owner hide/delete, context-menu and folder refresh interruption checks await CI |
| 49 | Large-Cast responsiveness | Large fixture correctness and measured incremental update/ordering. | Source-ready; focused Linux measured improvement, final-head/platform qualification pending |
| 50 | Explicit duplicate consolidation | Content/identity/palette comparisons, explicit confirmation only, exposure-preserving undo. | Source-ready; modal/digest/publication guards and actual interruption checks await CI |
| 51 | Collection/dependency report | Missing/conflicting dependencies and sizes, exact inclusion and relocated output verification. | Source-ready; nested report/chooser/original-context guards await CI |
| 52 | Linked ownership and local copy | Actual Library import/link provenance and explicit local copy with undo/collection preservation; external paths alone do not prove linkage. | Source-ready; interrupted preparation/publication and retained user-file checks await CI |
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
