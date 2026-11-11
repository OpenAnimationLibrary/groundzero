# Audit regression scenarios and validation plan

Baseline: `2e52fb78b70915925393290f32dc09a95a3927ec` (`edition`), 2026-11-07.
The prepared fixes are under review in [PR 142](https://github.com/OpenAnimationLibrary/groundzero/pull/142).
This plan preserves F01–F23 from the [main audit](codebase_audit_2026_11_07.md) and adds
the integrated Swing/audio follow-ups as F24/F25, timeout conversion as F26, local batch
lifetime/list ownership as F27/F28, external-FX pixels as F29, and Room/Brush/clipboard/custom-panel
repairs as F30–F34.
Prepared ownership follow-ups F35–F48 are recorded below; all new runtime evidence remains pending.
R01/R02/R03/R08/R09 remain aliases for those same bugs, not additional unresolved defects.
The ledger has 48 prepared findings and 14 distinct residual/qualification records, including a separately scoped PR 141 fixture defect.
F27/F28/F29 retain explicit partial-scope and runtime qualifications in their open records.
The companion [JSON ledger](audit_findings.json) contains affected paths, exact fixture names,
platform limits, missing coverage and a targeted mutation proposal for each prepared fix.
Neither document claims that every bug has been found or repaired.

## Evidence states and review order

- **Source-confirmed:** a defective operation and reachable caller or violated invariant were
  traced. A source trace is not an executed test or a runtime simulation.
- **Patched:** a remedy is present in the reviewed diff. This does not establish compilation,
  correct output or a passing regression.
- **CI-verified:** a named scenario passed on an exact revision and platform/configuration,
  with a run URL and retained log/artifact. Record baseline or mutation detection separately.
  A passing scenario never automatically closes a finding.
- **Reported / observed failure / qualification gap:** retain the distinction between an
  upstream symptom, a prior failed CI run, and missing evidence. An unknown crash cause stays unknown.

Every ledger record remains **open**. Final-head application regressions await CI; the prior native failures
are failure evidence, not execution of the new regression cases. Static checks and live reporter
inspection described in the main audit do not establish application behavior.
Follow [CLAUDE.md](../CLAUDE.md): builds, tests and mutations run in CI, not locally.

Review in this order: **architecture → code quality → tests → performance**. First settle
ownership, transaction and compatibility rules; then inspect bounds, cleanup and error paths;
then run behavior/error cases and targeted mutations; finally measure fixed workloads before
optimizing. P1/P2 are engineering priorities, not vulnerability ratings.

## Existing fixture routes

L/M/W mean Linux/macOS/Windows; they do not imply every architecture or device is qualified.
Use each platform workflow's existing setup, build options, runtime and regression launcher.
The JSON `suites` map contains the repository-relative paths and exact CTest registrations.

| Route | Existing entry point | Platform and limit |
| --- | --- | --- |
| PSD | [tiio_psd_test.cpp](../toonz/sources/image/tests/tiio_psd_test.cpp), CTest `tiio_psd` | L/M/W; file-size failure injection is L/M only |
| FFmpeg | [ffmpeg_public_test.cpp](../toonz/sources/image/tests/ffmpeg_public_test.cpp), `ffmpeg_public`; [tiio_ffmpeg_test.cpp](../toonz/sources/image/tests/tiio_ffmpeg_test.cpp), `tiio_ffmpeg` | Public L/M/W; direct suite L/M; requires FFmpeg/ffprobe |
| Tasks | [nativeuiintegrationtest.cpp](../toonz/sources/toonz/nativeuiintegrationtest.cpp), `exerciseTaskRemoval`, `exerciseFarmTaskRemoval`, `exerciseActiveChildRemoval`, `exerciseMovieBatchSetup` | L/M/W native application; keep each checkpoint, not just the runner exit |
| Batch lifetime | [batchtaskintegrationtest.cpp](../toonz/sources/toonz/batchtaskintegrationtest.cpp), `exerciseBatchTaskLifetime`; [view guards](../toonz/sources/toonz/batchtaskviewintegrationtest.cpp) | L/M/W native application; gated real controller/executor plus real-child cases; runtime pending |
| Vector/undo | [vectorworkflowintegrationtest.cpp](../toonz/sources/toonz/vectorworkflowintegrationtest.cpp), `runVectorWorkflowNativeChecks` | L/M/W native application; require `--require-vector-workflows` |
| Lazy Room lifetime | [workflowlifetimeintegrationtest.cpp](../toonz/sources/toonz/workflowlifetimeintegrationtest.cpp), `exerciseWorkflowRoomLifetime` | L/M/W native application; real nested event loop and pending timer; W4, not W1 reproduction |
| Split vector Brush | [brushworkflowintegrationtest.cpp](../toonz/sources/toonz/brushworkflowintegrationtest.cpp), `checkAutoCreatedSplitStrokes` via `runBrushWorkflowNativeChecks` | L/M/W native application; real gestures; new-level and lettered-ID cases missing |
| Raster paste | [rasterpasteintegrationtest.cpp](../toonz/sources/toonz/rasterpasteintegrationtest.cpp), `runRasterPasteIntegrationTest` | L/M/W native application; actual MI_Paste and external chooser Cancel |
| Custom panel loading | [custompanelintegrationtest.cpp](../toonz/sources/toonz/custompanelintegrationtest.cpp), `exerciseCustomPanelLoading` | L/M/W native application; real Room factory and editor slot; no automatic repaired-file reload |
| Deformer | [deformer_test.cpp](../toonz/sources/toonzlib/tests/deformer_test.cpp), `deformer` | L/M/W; numeric fixtures |
| Auto-Patch | [autopatchoutline_test.cpp](../toonz/sources/stdfx/tests/autopatchoutline_test.cpp), `autopatchoutline` | L/M/W; helpers only |
| External FX | [externfx_process_test.cpp](../toonz/sources/tnzbase/tests/externfx_process_test.cpp), `externfx_process` | L/M/W; process/decode plus authored public tile-pixel cases; new pixels await CI |
| Native probes | [plugin_probe_test.cpp](../toonz/sources/toonzqt/tests/plugin_probe_test.cpp), `plugin_probe_single`, `plugin_probe_empty` | L/M/W; real private module; require Qt 5 coverage for F23 |
| CI reporting | [test_ci_audit.py](../ci-scripts/tests/test_ci_audit.py), unittest discovery | Linux job; 22 boundary-mocked cases |
| CI topology | [test_ci_budget.rb](../ci-scripts/tests/test_ci_budget.rb), Ruby test entry point | Linux job; 15 topology cases |

Tasks and vector checks are application scenarios run through
[run_native_ui_test.py](../ci-scripts/run_native_ui_test.py), not additional CTest names.
An earlier native crash can prevent them from running; absent checkpoints are not passes.
Test names below are existing functions unless explicitly described as **missing** or **pending integration**.

## Prepared-fix scenario matrix

Source links identify the main implementation; the JSON lists additional touched files.
Every row needs final combined-revision qualification and baseline/mutation detection.
The first-head script-suite passes are recorded separately below.

| ID / priority | Baseline → patched invariant | Reproduction and observable result | Existing coverage / remaining gap |
| --- | --- | --- | --- |
| [F01](../toonz/sources/image/psd/tiio_psd.cpp) P1 | Truncating PSD overwrite → publish only complete flushed output | Replace red with blue under a 128-byte limit at 4×4 and 512×512; original bytes remain, no temp survives, retry succeeds | PSD `testFailedOverwrite`, `testUnwritableDestination`; Windows write/rename failure injection missing |
| [F02](../toonz/sources/image/psd/tiio_psd.cpp) P2 | Shared selected layer → per-reader immutable selection | Construct two readers before loading; alternate distinct pixels and repeat `loadInfo` | PSD `testIndependentFrameReaders` |
| [F08](../toonz/sources/image/ffmpeg/tiio_ffmpeg.cpp) P1 | Export clamps shared HDR data → source unchanged, output clamped | Both APNG paths preserve negative/above-one channels; independently decoded pixels match | Public `exportHdr` on L/M/W; duplicate direct HDR case removed |
| [F09](../toonz/sources/toonzlib/tdeformerstack.cpp) P2 | Invalid grids/casts → validated count/geometry and clipped brush bounds | Reject non-finite/overflowing/underflowing grids; remote brush changes nothing, large finite brush covers/erases cells | `testWeightPaintingInvalidGeometry`, `testWeightPaintingLargeBrush`; old-scene comparison pending |
| [F10](../toonz/sources/stdfx/autopatchfx.cpp) P2 | Outline arithmetic overflow → checked support and actual tile byte count | Zero/fractional/extreme radii and 4/8/16-byte boundaries; valid dry-pass subdivision remains allowed | `testOutlineMargin`, `testOutlineSize`, `testInvalidOutlineSize`; Fx exception/subdivision fixture missing |
| [F26](../toonz/sources/image/ffmpeg/tiio_ffmpeg.cpp) P2 | Signed timeout multiplication overflow → qint64 conversion and saturation | Two-second fake ffprobe succeeds for large-positive/default budgets and times out at one second | Direct `testProbeTimeoutConversion` on L/M; equivalent Windows public case missing |

F10's missing integration case should declare a 16000×16000 RGBA request with outline width
4000: the full padded region exceeds the raster byte limit, but a 1000×1000 compute tile
fits. Check declaration/subdivision without allocating the full oversized padded raster, then
compare smaller tiled/untiled outputs. This is a proposed fixture, not an executed simulation.

| ID / priority | Baseline → patched invariant | Reproduction and observable result | Existing coverage / remaining gap |
| --- | --- | --- | --- |
| [F03](../toonz/sources/toonz/tasksviewer.cpp) P1 | Selected child read after group deletion → snapshot IDs first | Select parent+child; second panel selects child; delete and remove an orphan | Tasks `exerciseTaskRemoval`; sanitizer/PageHeap evidence pending |
| [F04](../toonz/sources/toonz/tasksviewer.cpp) P1 | Null/borrowed dependency dereference → dependency IDs and removable missing references | Display absent dependency and remove it through the UI action | Tasks `exerciseTaskRemoval`; once-valid dependency deleted with panel open remains missing |
| [F05](../toonz/sources/toonz/batches.cpp) P1 | Stale farm mapping → retired mapping and guarded polling | Remove child/group, poll, recreate local IDs and start; no old remote job reused | Tasks `exerciseFarmTaskRemoval`; real farm qualification separate |
| [F06](../toonz/sources/toonz/batches.cpp) P1 | Suspended parent deletes active child → descendants block removal | Waiting and Running child both produce warning; stopped child then permits removal | Tasks `exerciseActiveChildRemoval`; queued-worker repair and validation tracked separately in F27 |
| [F07](../toonz/sources/toonz/batches.cpp) P1 | Movie chunks overwrite shared output → one new movie task spans range | Five-frame scene, chunk preference two: movies stay whole, PNG splits | Tasks `exerciseMovieBatchSetup`; c208 failed the AVI expectation on L/M; effective-format fixture correction prepared, pending CI; real export/old task-list cases missing |
| [F27](../toonz/sources/toonz/batchtaskexecution.cpp) P1 | Raw worker task borrows → immutable plans and controller-owned retirement | Gate queued/assigned/terminal work, cancel shared dependencies selectively, reject affected UI edits, replay retired outcomes/progress, exercise real process polling/reaping | Native `exerciseBatchTaskLifetime` plus `exerciseBatchTaskViewGuards`, selected-tree start/stop, submission-error and chunk-collision editor cases; exact cases and limits in [task review](audit_task_lifetime.md); CI/mutations pending |
| [F28](../toonz/sources/toonz/batches.cpp) P2 | Bulk owner loss/destructive failed load → root cleanup and staged replacement | Refuse active replacement; preserve prior list on failure; accept legacy pointer aliases and reject distinct duplicate IDs | `exerciseClearAndLoad`, `exerciseOwnershipRoots`, `exerciseLoadAliasesAndDuplicates`, `exerciseInvalidSerializedOwnership`; leak-detector evidence pending; nested malformed parser allocation gap remains |

| ID / priority | Baseline → patched invariant | Reproduction and observable result | Existing coverage / remaining gap |
| --- | --- | --- | --- |
| [F11](../toonz/sources/toonz/filmstripcommand.cpp) P1 | Sorted-position undo swaps drawings → inverse frame-ID mapping | Distinct 1/1a/1b/2/2a/3, sparse exposures, sync on/off, Duplicate/Add/Paste/Insert undo/redo | `filmstripUndoChecks`, `filmstripInsertionUndoChecks`, `rasterInsertionUndoChecks`; complete image/palette comparison pending |
| [F12](../toonz/sources/toonz/drawingdata.cpp) P1 | Cache collisions and blank-raster aliasing → independent drawing/history identity | Reuse IDs across two Each entries; edit one of two redone blanks | `eachHistoryUndoChecks`, `filmstripUndoChecks`, `rasterInsertionUndoChecks` |
| [F13](../toonz/sources/tnztools/imagegrouping.cpp) P2 | Ungroup undo merges partitions → original separate/nested groups | Compare stroke geometry, depth and pairwise common-group depth through undo/redo | `ungroupUndoChecks` |
| [F22](../toonz/sources/toonz/cellselection.cpp) P2 | Uninitialized simplification palette choice → initialized existing-path choice | Simplify multi-style drawing, undo/redo/save/reload; compare style IDs/colors/pixels | Palette round-trip fixture **missing**; include shared/external palettes and cancellation |
| [F24](../toonz/sources/toonz/filmstripcommand.cpp) P1 | Swing loses shifted originals / wrong clone count → exact restoration and bounded cloning | Empty/single, sparse and lettered selections; repeated undo/redo, following drawings and sync preference changes | `swingUndoChecks`, `checkSwingState`; c208 L/M full native runs did not reach Swing; formerly R03 |

| ID / priority | Baseline → patched invariant | Reproduction and observable result | Existing coverage / remaining gap |
| --- | --- | --- | --- |
| [F14](../toonz/sources/toonzlib/thirdparty.cpp) P2 | Empty preference indexing/path disagreement → application-relative resolution | FFmpeg/ffprobe/Rhubarb: empty, dot, relative/absolute paths with spaces | Public `verifyToolDirectory`; debug and release Qt qualification |
| [F15](../toonz/sources/CMakeLists.txt) P2 | Missing PNG header skipped; TIFF copy exceeds advertised minimum → compatible conditional copies | Clean Windows configure without workflow pre-copy; then preserve an existing marked header | Configure fixture **missing**; isolate header operations from dependency minimum-version claims |
| [F16](../toonz/sources/tnzbase/texternfx.cpp) P2 | Bad POSIX buffer/unchecked Windows launch → errors and cleanup | Quoted arguments, nonzero exit, missing/corrupt PNG; Windows 4096-byte expansion, missing/oversized command, handle count | `externfx_process` passed c208 macOS process cases; temp cleanup/POSIX start-failure assertions missing; new pixel cases are F29 |
| [F17](../toonz/sources/toonzqt/pluginhost.cpp) P2 | One-effect probe rejected → nonempty sentinel range accepted | Real module loads/creates in single mode, remains rejected in empty mode | Both `plugin_probe_*` cases |
| [F23](../toonz/sources/toonzqt/pluginhost.h) P2 | Qt 5 queued custom pointer unregistered → registration before publication | Real module publishes from worker; public creation succeeds without queued-type warning | Both probe cases; require Qt 5 and warning capture |
| [F25](../toonz/sources/toonzlib/thirdparty.cpp) P2 | Fixed audio wait / incomplete exit handling → bounded preference and cleaned failure state | Missing/unstarted/completed/killed/nonzero helpers, short/long/overflow/zero timeouts, real spaced-path PCM WAV | Public `verifyAudioLifecycle`, `verifyAudioDecode`; macOS c208 passed, L/W and final head pending; formerly R09 |
| [F29](../toonz/sources/tnzbase/texternfx.cpp) P2 | Decoded output discarded → strict matching RGBA32 copy into existing tile | Real child emits asymmetric colors/alpha into an extracted tile; compare exact pixels, premultiplication, orientation, storage/position and borders; small/large/gray output must fail | `publicRendering`, `checkRenderedPixels`, `renderWorker` in `externfx_process`; new cases await CI; old scenes and 64-bit/float destinations remain open |

| ID / priority | Baseline → patched invariant | Reproduction and observable result | Existing coverage / remaining gap |
| --- | --- | --- | --- |
| [F18](../ci-scripts/ci_audit.py) P2 | False substring causes/hidden later errors → actual failure classification and pagination | Success checkpoint words alongside real errors; check/job/annotation failures beyond page one | All 22 Python cases; retain pending/no-check, MSVC, skipped-check and malformed-endpoint cases |
| [F19](../ci-scripts/check_ci_budget.rb) P2 | 31 worst-case automatic PR checks → maximum ten baseline validation jobs with mapped coverage | Reusable/matrix expansion, eleven-check rejection, cycles and invalid/dynamic shapes | 15 cases/50 assertions passed c208; final-head and consolidated-step fault injection pending |
| [F20](audit_workflow_security.md) P2 | Mutable inputs/credentials/interpolation/error masking → pins, scoped access and failure propagation | Scan workflows; controlled install/copy failure must stay failed despite later success | Static evidence in security report; runtime tool/action compatibility and isolated failure fixture pending |
| [F21](../.github/workflows/workflow_windows.yml) P2 | DLL package lacks identified notice copy → required supplied notice tree shipped | Extract produced archive; match DLL list and seven nonempty notice/author files | c208 staged notice assertions passed; extracted-archive inspection and owner provenance review pending |

| ID / priority | Baseline → patched invariant | Reproduction and observable result | Existing coverage / remaining gap |
| --- | --- | --- | --- |
| [F30](../toonz/sources/toonz/mainwindow.cpp) P1 | Queued switch deletes initializing Room → hold guard and defer pending replacement | Queue two profiles, enter lazy Room initialization, observe nested callback with Room alive, then exactly one latest-profile switch | `exerciseWorkflowRoomLifetime`, `exercisePendingSwitch`; CI/lifetime instrumentation pending; separate W4 hazard, no W1 attribution |
| [F31](../toonz/sources/tnztools/toonzvectorbrushtool.cpp) P1 | Every split claims creation → first record alone restores frame/renumber ownership | Actual Break-on/Auto-Group-off Brush gestures on blank/held exposures; three undo/redo cycles preserve original drawing identities/exposures and recreated geometry/style | `checkAutoCreatedSplitStrokes`, `checkSplitStrokeHistory`; no exact #5190 reproduction; new-level/lettered-ID variants missing |
| [F32](../toonz/sources/toonz/cellselection.cpp) P2 | External preview displaces native payload → native raster precedence | Six indexed/full-color occupied/empty-row/empty-column MI_Paste targets with contrasting preview; verify type/pixels/DPI/undo/redo identity | `checkNativePasteTargets`, `checkPasteCase`; broader #2445 policy and cross-process MIME/conversion qualification remain open |
| [F33](../toonz/sources/toonz/cellselection.cpp) P1 | Early undo block drops redo on Cancel → open only after preparation | Native paste then undo to count 1/index 0; cancel actual external-image chooser; old scene/history unchanged and redo restores original drawing/pixels | `checkExternalCancel`; runtime and baseline/mutation pending |
| [F34](../toonz/sources/toonz/custompanelmanager.cpp) P2 | Unchecked loader null → panel fallback or preserved editor preview | Missing/malformed/valid Room panels; plain-text path/error, ownership/destruction, valid MI_Undo binding; malformed selection retains valid path/preview/fields; later command edit succeeds, then corrupted-file reload preserves preview pixels | `exerciseCustomPanelLoading`, `checkFallback`, `checkValidPanel`, `checkEditorFailure`, `checkCorruptedTemplate`; runtime pending, no W1 attribution |

## Prepared memory ownership scenarios

The memory follow-up has source/fixture diffs; runtime and baseline/mutation evidence remain
pending. F38 is merge-exception double deletion and F40 is array-deleter mismatch, not measured
normal-success growth. F36 releases builder metadata; F45/F46 retire progress UI, not the full
render graph. See the [memory plan](audit_memory_lifetime.md),
[scene ownership review](audit_memory_ownership.md) and [LSan qualification](audit_image_cache_lsan.md).

| ID | Priority | Prepared invariant | Authored checks |
| --- | --- | --- | --- |
| F35 | P1 | ChildStack owned-root teardown: Recover the owned root before deleting ancestor records; only that root is cleared or released. | `destroyWhileNested`, `closeBeforeDestruction`, `clearNavigationWhileNested`, `clearSceneWhileNested`; CI pending |
| F36 | P2 | Compressed cache ImageBuilder metadata ownership: Both compressed-item destructors delete their uniquely owned builder before other owned metadata. | `testRepeatedCompressedRemoval`, `qualify`, `remove_builder_delete`, `require_result`; CI pending |
| F37 | P2 | Paste conversion temporary payload ownership: Scope-own conversion and external-image payloads across the helper/Selection call, reuse RasterSelection transferredData ownership, and use an intrusive palette owner during external preparation. | `checkStrokeConversionOwnership`, `undoConvertedRaster`, `checkRasterToVectorCycles`, `checkExternalRasterCycles`; CI pending |
| F38 | P1 | Null-target palette ownership on merge failure: Hold the temporary palette with TPaletteP throughout both raster getData paths, so normal return and merge exceptions use one reference-counting ownership model. | `checkNullTargetPaletteCopies`, `checkNullPaletteFailure`; CI pending |
| F39 | P2 | External-FX input-port graph ownership: A private unique_ptr vector owns every allocated input port, including allocations hidden by duplicate names. | `inputPortOwnership`, `failedLoadPortOwnership`; CI pending |
| F40 | P1 | Plugin enum and spectrum array deleters: Use typed delete[] deleters for both trait arrays while preserving SDK pointers, values and the current parameter publication behavior. | `setup`, `checkReplacedParameters`; CI pending |
| F41 | P2 | Custom-panel submenu retention during rebuild: Delete owned direct submenus before clearing/rebuilding actions. | `checkNestedMenus`, `checkMenuCycles`; CI pending |
| F42 | P2 | About dialog retention after closing: The About dialog has scoped lifetime and owns its child widgets; returning from the modal action destroys the dialog and descendants. | `checkAboutCycles`; CI pending |
| F43 | P2 | Failed and overlapping update-checker lifetime: Parent each checker to the window, resolve the actual sender on completion, copy its result, disconnect only that sender from the window and schedule deletion before either success or error return. | `checkUpdateCompletion`, `checkOverlappingUpdates`, `checkUpdateCycles`; CI pending |
| F44 | P2 | Drawing snapshot shared cache ownership: A private shared lease removes owned snapshot IDs after the last clone releases them. | `checkClipboardClones`, `checkUndoOwner`, `checkTransferredGenerations`, `checkReplacementAndFailure`, `checkConsumingPaste`; CI pending |
| F45 | P2 | Normal render progress startup and terminal ownership: Scope-own listener/build-progress objects through startup, then hand off only after start. | `checkNormalRenders`, `checkRealCancellation`, `checkNestedCancellation`, `checkHandoffAndLateMessage`, `checkNestedProgressDelivery`, `checkStartupFailure`; CI pending |
| F46 | P2 | Multimedia render progress scoped ownership: Own the multimedia progress dialog for the synchronous start call and destroy it afterward; queued messages use QPointer and ignore an already retired target. | `checkMultimedia`; CI pending |
| F47 | P2 | Floating-paste undo original-image cache ownership: Destroying the noncopyable undo owner also removes its unique old-floating-image key, leaving unrelated entries and live undo behavior intact. | `oldFloatingCacheKeys`, `checkStrokeConversionOwnership`, `undoConvertedRaster`; CI pending |
| F48 | P2 | Optional plugin destroy callback reference balance: Invoke an available destroy callback, then release the host-held metadata reference independently; preserve callback-before-release ordering. | `PluginHostLifetimeTest::check`, `checkReplacedParameters`; CI pending |

F37 owns returned conversions; exceptions inside factories remain outside scope. F38 drives a
real throwing merge; normal null-target copy was not a leak. F40 value checks need mismatch
instrumentation. F44 orphan snapshots were previously cleared at scene reset. F45 graph-build
exceptions are source-only; early cancel is covered, active-window shutdown is not. F47 checks
keys before undo/after reset, with pixel-checked undo/redo between, not each intermediate key.

## Residual scenario queue

These IDs remain separate from prepared fixes. A proposed or separately prepared follow-up
does not change their resolution. Full source paths and evidence distinctions are in the ledger.

| ID / priority | Required scenario and invariant | Missing evidence or decision |
| --- | --- | --- |
| R01 → F27 | Stable alias for local batch execution lifetime. | Prepared plan/retirement and gated fixtures; CI/mutations, active-process quit, legacy wire correlation and remote restart remain open. |
| R02 → F28 | Stable alias for list ownership/replacement. | Prepared normal root cleanup and transactionality; fresh group leak detection and malformed nested extraction provenance remain open. |
| R03 → F24 | Stable alias for Swing restoration. | Integrated follow-up now tracked under F24; validation remains pending. |
| R04 P1 | Recover and replay the original baseline Windows native failure with executable/symbols/dump. | Keep historical 0xC0000421 distinct from later status 1/120-second diagnostic timeout; no root cause established. |
| R05 P1 | Replay PR 141 Linux/macOS native SIGSEGV paths using their exact builds/backends. | R16 identifies a corroborated fixture cause; tie each recorded crash to that path and verify its correction. |
| R06 P1 investigation | Controlled external-FX name/XML through deserialize→clone→initialize→render; establish command reachability. | Attacker-control evidence and compatibility decision before containment/consent changes. |
| R07 P1 investigation | Alias parent segments/symlinks through actual save/rename/collect, with valid external-resource controls. | Observe writes/prompts and agree trust policy; blanket external-path rejection is not a remedy. |
| R08 → F29 | Stable alias for external-FX output publication. | Prepared exact-pixel cases; CI/mutation and old-scene render comparisons pending. Existing 64-bit/float destination early return remains unresolved. |
| R09 → F25 | Stable alias for bounded audio lifecycle. | Integrated follow-up now tracked under F25; no timing or cleanup success inferred before CI. |
| R10 P2 | Inventory actual native release/debug link inputs and source/patch/build/artifact digests. | Rebuild/provenance evidence and owner disposition; notice copying is separate. |
| R11 P2 | Reject a mismatched native payload before execution; record unresolved package resolution. | Trusted Qt/prebuilt/AppImage identities and optional dependency qualification. |
| R12 P2 | Recheck recorded Homebrew deadlines, then qualify the selected acquisition path on target hardware. | Owner migration/acquisition decision; dated availability report is not today's build failure. |
| R13 P2 | Reproduce raster paste target/type across level kinds, mixed columns, gaps and preferences (#2445). | F32/F33 cover narrower payload/Cancel defects; expected destination policy and OT-Dev PR280 review remain separate. |
| R14 P2 | Reproduce vector Brush exposure/frame shifts with exact reporter auto-create/hold/stretch/renumber/Break/Auto Group preferences (#5190). | F31 is a source-confirmed narrower variant with authored gestures, not exact reporter reproduction; navigation unchanged. |
| R15 P2 | Native Apple Silicon, tablet/DPI/audio and bounded long sessions; compare saved state and output. | Actual device/backend evidence; offscreen tests are insufficient. |
| R16 P2, PR 141 only | `exerciseSceneViewerContextOwners` directly constructs a viewer without factory title-bar initialization; use the factory before showing/querying it. | Source/debugger-supported defect and isolated fixture correction; absent from the edition baseline and not included in PR 142. |
| R17 P2 | Plugin nested storage/per-host UI and descriptor borrowers | F40/F48 do not repair these owners; separate lifetime design needed. |
| R18 P2 | Legacy cache units/range and disk/process accounting | No counter correction; disk methods return zero rather than measured usage. |
| R20 P2 | Loader controller/thread/queued-publication retirement | Listener quit wiring/callers require separate qualification; F49 is a later wave. |

R16 is traced at [PR 141 revision a0406ef, fixture line 411](https://github.com/OpenAnimationLibrary/groundzero/blob/a0406efc4a8f90a74d77bb5ddbab1d0d8aeabeeb/toonz/sources/toonz/nativeuiintegrationtest.cpp#L411).
The reported [CDB job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42342872309/job/128314993429),
artifact 13563883652, shows `getPreviewButtonStates` reading the poisoned preview-button
pointer before faulting. Reported macOS/Linux stacks corroborate the function; collect each
original artifact and verify the factory correction before claiming platform closure.
This is distinct from the unresolved edition Qt-text teardown failure in R04.
R04 also tracks the integrated [PageHeap/CDB diagnostic mitigation](../ci-scripts/native_ui_page_heap.py)
and [eight contract cases](../ci-scripts/tests/test_native_ui_page_heap.py): bounded debugger,
registry restoration and retained timeout/exception evidence. The eight contracts passed on c208 Windows (CI18); runtime heap activation and W1 diagnosis
remain separate. Improved diagnostics are not an additional application crash fix.

F27's retired-outcome replay proves that an absent submission record rejects stale/duplicate
outcomes. The separate worker-progress case checks per-task incarnation across replacement and
preserves valid queued updates across unrelated idle edits. Generation rejection and duplicate
retirement delivery are not independently mutation-proven. Group roots are not directly counted
because `TFarmTaskGroup` is final; valid load/clear needs leak detection. F28 does not claim that
malformed nested extraction reclaims an object that throws before its group adopts it.

The native failure links and signature distinctions remain in the
[main audit](codebase_audit_2026_11_07.md#existing-ci-failures-and-interpretation).
The [132-report upstream inventory](upstream_bug_triage.md) is the source of truth for that
backlog: 124 reported, three source-confirmed, four reviewed/unproven and one current
mitigation in the captured snapshot. Do not duplicate those reports or promote their status
because a nearby regression passed.

## Missing tests, mutations and acceptance records

The [Linux job at initial PR head 2758123](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42414551454/job/128529701035)
passed the 20 CI-auditor Python cases and 15 CI-budget Ruby cases, as reported by the
coordinating audit. JSON records CI01/CI02 retain the full SHA and scope. This is executed
script-test evidence, not a C++ or final-head pass. The later
`test_cli_enforces_ten_check_boundary` change explicitly accepts nine/ten checks and rejects
eleven. Its later c208 evidence is recorded below; the final combined changes still need CI.

At exact revision `c20884ffb45660f79a7f213329e5e9c1ff2dc479`,
[Linux](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42419847476/job/128545710903) and
[macOS](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42419847532/job/128545825336) each passed the F03–F06 native checkpoints.
Linux passed 22 reporter contracts and 15 budget cases/50 assertions; macOS passed all 22
required reliability CTests. Linux reliability was skipped. Both full native processes failed
F07's platform-specific AVI expectation before Swing/vector workflows; no Swing pass is
recorded. The prepared movie fixture correction verifies the reloaded format, including the
existing non-Windows AVI-to-TIF conversion, and awaits CI. F27/F28/F29 postdate this run.
CI10–CI16 retain exact logs/artifacts alongside unchanged initial-head CI01–CI09.

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

At PR head `49cf834fa27c6101ba01013ae6e63fe76f506c3c`,
[Linux](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42425034831/job/128561369500) and
[macOS](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42425034857/job/128561493163)
builds rejected two `const TVectorImageP` locals in the Brush fixture because its conversion
operator is non-const. The prepared correction removes only those two qualifiers. OpenToonz
did not link; native/reliability tests were skipped. Task/paste/panel object compilation and
external-FX/probe links are compilation evidence only. Linux's separate SeExpr sanitizer graph
passed; it does not qualify application memory paths. CI22/CI23 preserve the failure.
F35–F48 postdate this head; no new owner fixture or cache LSan mutation has run.

macOS also emitted existing vendored SuperLU platform-load-command and duplicate OpenEXR
archive-link warnings. These remain unresolved dependency warnings, separate from the failure.
For c208 Windows, the PR head was c20884ffb, while checkout/build used merge
`27381fa6e2358d68df5370b1a4c75ee81321035b`. CI17–CI19 retain PR-head association; the ledger
records checkout provenance separately from log lines 215/274.

F29 requires old-scene before/after rendering because valid external-program output now
changes the final tile. Exact-size 32-bit RGBA output is supported; smaller/larger/grayscale
output is rejected, and the existing early return for 64-bit/float destinations remains.
The new public-render cases are not validated by c208's earlier `externfx_process` pass.

Implement missing behavior cases before relying on mutation results. Existing fixtures should
fail against the defective baseline or a narrow intentional mutation. Examples:

- F01: reintroduce truncating output; original-byte preservation must fail.
- F02/F08: restore shared selected-layer state or in-place HDR clamping; identity/source
  assertions must fail even if some rendered output still looks correct.
- F03/F05/F06: restore raw selection iteration, stale farm mappings or parent-only status
  checking; use deterministic state and sanitizer evidence, not scheduling sleeps.
- F10: remove numeric guards for helper tests; separately restore the premature dry-pass
  rejection for the proposed subdivision fixture.
- F11/F12/F13: restore positional undo, colliding keys or whole-range regrouping; compare
  exact identities/partitions rather than only command success.
- F17/F23: reject one-element probes or remove queued meta-type registration; require the
  real module and a Qt 5 run.
- F18/F19: stop pagination or omit caller-matrix multiplication; relevant cases must fail.
- F27: retire assigned work on cancellation/terminal display, omit queued withdrawal, or remove
  progress incarnation rejection; corresponding gated lifetime/identity assertions must fail.
- F28: restore raw-map clear or publish an invalid replacement; owner counts or retained-list
  assertions must fail. Use leak instrumentation for freshly deserialized group roots.
- F29: restore local-pointer-only decode or omit size/type rejection; exact destination pixels
  or invalid-output assertions must fail. Preserve parent borders and tile position/storage.

- F30: remove the initialization guard or consume the blocked pending request; require lifetime
  and eventual latest-profile assertions to detect the change.
- F31: restore creation ownership to every split; original drawings/exposures must fail restoration.
- F32/F33: prefer the external preview or open undo before the chooser; native-payload or
  preserved-redo assertions must fail respectively.
- F34: restore either unchecked loader result; its missing/malformed case must fail while valid
  panel command binding stays covered.

The ledger specifies a mutation proposal for every F record. Run mutations only in isolated
CI revisions, retain the failure log, and restore the patch before final qualification.
Do not run publishing/deployment jobs as fault-injection targets.

For each pass or failure, attach: exact revision, scenario/test, platform/architecture,
Qt/backend/build configuration, command, run URL, retained log/artifact, observed result,
baseline/mutation result and remaining limits. Link a native checkpoint or pixel/state
comparison, not just a job conclusion. Closing a record additionally requires explicit review
of its pending items and compatibility/owner decisions. A green first PR job is insufficient.

## Bounded follow-up coverage

This map identifies work not established by this audit. It does not represent completed review
of each directory. IDs and exact scopes are repeated in JSON for tracking.

| ID / stage | Bounded next inspection | Required output |
| --- | --- | --- |
| C01 Architecture | One save/load/collect route per native level type and one expression-save case | Destination/transaction map, valid external controls, cancel/failure and scene round-trip fixtures |
| C02 Architecture | Swing, plastic alt-drag, TLV merge and ordered FX-port undo, one minimal case each | Exact frame/key/palette/connection snapshots through edit, undo/redo and save/load |
| C03 Code quality | Active TIFF/TZP, EXR and PSD entry points before expanding to other codecs | Length/allocation/cleanup review and bounded corrupt/truncated fixtures; prove platform linkage |
| C04 Code quality | Process lifetime, plugin/config trust and audio boundaries | Launch/exit/timeout/cleanup cases plus explicit trust/compatibility decisions |
| C05 Tests | Representative RGBA8/RGBA16/float scenes, alpha, deformation, cache invalidation and tiled outlines | Baseline/patched pixels, scene keys and FX topology; tiled/untiled comparison |
| C06 Tests | Exact builds/backends, original native crashes, package inputs and archive contents | Stacks/symbols, run IDs, installed startup and dependency manifest; then scheduled variants |
| C07 Performance | Fixed long scene and repeated edit/undo/export after correctness gates | Warm/cold time, peak/retained memory, workload duration and variance on matched hardware |
| C08 Tests | Prioritized upstream lost-edit/crash queue, at most ten reports per reproduction wave | Minimal fixture and exact-build result per report; retain unverified reports in the existing triage |

Do not fold PR 141's GL/brush work or PR 133's memory work into unrelated fixes without reviewing
their current changes and evidence. No performance profile, complete artist-workflow pass or
release approval is supplied by this plan.
