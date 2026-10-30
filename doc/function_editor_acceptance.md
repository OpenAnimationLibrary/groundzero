# Function Editor workflow acceptance

Implementation base: `aa01975f38d6226156e5b07df7271b8fc199d003` (`edition`).
The source workflows below are implemented. **Execution acceptance is pending:**
`CLAUDE.md` requires builds and tests in CI, so none ran locally. Passing static
review is not a claim that the application built or that interactive acceptance
passed. The final PR revision must pass the registered tests on supported
platforms; the visual/device observations below also remain to be checked.

The implementation keeps native curve data and the SeExpr evaluator. The new
operation layer plans immutable before/after snapshots, checks ranges, units,
locks and stale previews, then applies one undo transaction. Dialog previews
never mutate the live animation. Scene/Xsheet changes cancel pending dialogs
and invalidate snapshots that contain scene-owned expression grammars.

| # | Scope | Source implementation and evidence | Qualification still required |
|---|---|---|---|
| 1 | Fit selected keys | `FunctionPanel::selectionBounds` uses actual keys across channels, adjoining selected segments, Bezier hull and sampled expression context; `fitRegion` handles one key/constants. Native widget test checks selected extent versus the rest of the curve. | CI; interactive handle/extrema framing. Expression bounds are sampled framing, not an analytical extremum guarantee. |
| 2 | Channel search | `FunctionTreeView::setSearchText/applySearch` matches ancestor/channel/reference text and restores expansion. Browser-only filtering keeps active curves and selected keys visible in graph/spreadsheet. Native search/expansion test prepared. | CI; large channel lists and translated labels. |
| 3 | Named channel sets | Channel Sets stores named UUID lists in user settings. Rename/F2/Enter preserves IDs, order and additional set metadata, rejects empty/duplicate names, and cancels unfinished text on close/context replacement. Workspace renames preserve animation undo/dirty state. Optional `workspaceId` attribute on existing `default` scene tag survives rename/reload; copy/undo retain owner ID, clones/imports do not inherit it. Missing/duplicate IDs are reported. | New rename fixtures require exact-head CI: keyboard/modal controls, fresh-process preferences read, recall after channel rename, native scene membership and cancellation. Existing serialization tests cover stage, offset, Plastic and FX IDs, including unkeyed/nested defaults. Original `default` reader ignores attributes; no new child tag was added. Older builds may discard IDs on save. |
| 4 | Solo and edit locks | Tools offers solo/restore and locks selected channels. Native `KeyframeSetter`, FunctionSelection commands, numeric plans and Load Curve apply reject locked writes; toolbar/segment fields disable. Context Reset Handles now uses native undo setters. | CI lock routes and undo; manual graph/sheet/context-menu route matrix. Locks are session-only; undo remains able to restore history. |
| 5 | Numeric multi-key edit | Edit Keys shows mixed value ranges/counts, set/offset values in each channel's display units, preview, repeated Apply and close/cancel. Single-value toolbar directs multiple selections to Edit Keys. | CI native modal repeated Apply/cancel and unit/all-or-nothing oracle tests. Driven endpoints require explicit bake before numeric value edits. |
| 6 | Precise retiming | Edit Keys offers frame offset, grid quantization, pivot scaling and explicit target first/last frames. Target-range mapping uses the whole multi-channel selection, exact endpoints, relative interior timing and segment handle scaling. It rejects negative/reversed/colliding ranges; point selections require a point destination. Exact repeat is a no-op. Native curve serialization uses full double precision while preserving the caller's stream mode. | New target-range fixtures require exact-head CI: numeric/handle/SeExpr oracles, locked/stale atomic rejection, widget preview/repeat/cancel/context replacement, native Stage/offset/Plastic undo/redo and fractional scene roundtrip. Manual tablet/fractional visual review remains. |
| 7 | Normalized display | Tools normalization maps each curve's sampled display range to 0–1, with inverse mouse coordinate mapping. No stored key values change. Recomputed after finished edits. | CI roundtrip/unchanged snapshot; manual constant/driven curves and multi-channel drag. |
| 8 | Curve identification | Deterministic per-channel colors, visible long-name legend, current/locked annotations; names elide within the graph. Stable UUID colors apply after a set is saved; otherwise structural reference/name hash. | Light/dark themes, 100/150/200% DPI and color-vision accessibility. Color is accompanied by names, not the sole identifier. |
| 9 | Interpolation/handles toolbar | Dedicated Interpolation menu exposes native Constant/Linear/Speed/Ease/%/Exponential and selected handle link/unlink/reset. Multi-key handle commands form one undo block. | CI native type/lock tests and full-application interpolation, handle drag/reset/locked-reset undo. Remaining manual handle route matrix. |
| 10 | Snapping | Tools stores frame increment, nearby-key and scene-frame-marker snapping using native marker distance/offset; Alt bypasses snapping for ordinary moves (Shift+Alt retains moves when Alt alone would stretch). Drag uses the clicked selected key's frame anchor. Existing native whole-frame drag collision handling retained. | CI/native drag fixture checks grid, off-center anchor, key, marker, Alt and locked paths. Varying-zoom interaction remains to observe. Fractional retiming belongs to Edit Keys; fractional targets are not falsely snapped by integer drags. |
| 11 | Before/after comparison | Reopened audit gap corrected in source: exact native restore snapshots are separate from frozen evaluated display samples. Capture reports its visible frame domain, sample interval and approximation; native/SeExpr dependency edits, show/hide and modal cancellation do not reevaluate captured samples. Checked evaluation rejects fallback samples. Restore still uses one validated native undo transaction. | New full-application native/SeExpr dependency-edit, show/hide, preview/cancel and exact restore/undo/redo fixture; widget context invalidation and cache retention require fresh CI. Visual review of sampled approximation remains. |
| 12 | Breakdown/overshoot | Edit Keys slider previews native breakdown favour from -100% to 200%, supports selected native numeric curves and explicit insertion frame. Close cancels; Apply one transaction. | CI breakdown oracle and real application Plastic angle/stage-offset preview/apply/undo/redo/scene reload fixture. Manual Plastic distance visual review. Same native channel type is supported without a rig-specific adapter. |
| 13 | Key reduction | Preview reports removed keys and maximum sampled display error. Independent original curve/key samples form the oracle; only eligible selected linear, unstepped spans reduce. Holds, nonlinear and driven boundaries stay protected. | CI exact/sample oracle cases; artist review of tolerance. Scope deliberately preserves nonlinear segments rather than approximating them without a continuous error proof. |
| 14 | Bake driven curves | Cancellable native evaluation at explicit inclusive first/last/interval points, with exact stored sample values. Invalid/nonfinite/native or SeExpr fallback failures abort the whole plan. Outside-range keys retained. | CI native/SeExpr failures, cancellation and sample oracles. Partial boundaries inside a curve must already be keys; cyclic operations stay within the original period. |
| 15 | Motion presets | Reopened audit gap corrected in source: Import Existing Curve accepts the native raw `.curve` format through `TDoubleParam::loadData`, as well as `.fcurve` wrappers. Raw curves explicitly adopt the current channel's measure, since the legacy file stores none. Valid numeric data is published into the searchable/favorite preset library; preview/adaptation/Apply preserve destination identity and use one undo step. | Actual native file/name dialogs with preexisting raw `.curve`, both cancellation points, driven-data rejection, duplicate preservation, search, preview, locks, adaptation, undo/redo, favorite/reopen and context cancellation require fresh CI. |
| 16 | Dependency inspection | Actual native `referenceParams`/`dependsOn` results, expression validation errors with frame, source/dependent navigation, unresolved/outside-model notices. Raw model pointers are invalidated before refresh/reset. | Prepared native/SeExpr resolved binding navigation fixtures in full application; invalid, deleted, circular and external references still need UI observations. No regex claims of complete bindings. |
| 17 | Dense graph scaling | Visible frame interval limits segment/key/group gadget traversal. The original dense single-curve fixture remains. Reopened coverage gap now has a real native rig workload: 24 parented pegbar owners, 96 Stage/offset channels and 24,576 keys through a 40-frame viewport. Independent curve coordinates and removing individual channels distinguish artwork from background/grid pixels. | New native six-paint timing report includes Qt/platform/architecture/DPI/canvas/dataset provenance and curve-pixel probes. Selection, exact source keys, IDs, dirty state and undo remain checked. Fresh CI execution is pending; no unmeasured speedup or speed budget is claimed. |
| 18 | Keyboard/accessibility | Arrow keys nudge selected frames/values using the same validated plans; Shift multiplies by ten. Configurable value increment and logical-pixel hit radius, control tooltips and discoverable modifier help. | CI arrow/undo path; keyboard-only, screen-reader labels, tablet and DPI observations. |

## Registered CI coverage

The final scope audit reopened items 11, 15 and 17. Their corrected source and
fixtures are now prepared together with the shared checked-evaluation API.
Earlier Function Editor head `d7c34e6ef` passed Linux, Windows and macOS native
and installed-application checks. Those results do not qualify these later
additions or the saved-set/target-range changes. Fresh exact-head CI remains
required, with no local execution.

`BUILD_UI_PORT_TESTS` creates the following targets, both dependencies of the
ordinary `ui_port_tests` target. CTest names use the existing `ui_` selection:

- `ui_functioncurveoperations` (`ui_functioncurveoperations_test`): native data
  operations, units, collisions, stale/locked atomic rejection, undo isolation,
  discrete/range checks, protected reduction, sampled bake, SeExpr preservation
  and failures, cancellation, overshoot, presets, restore and identity persistence.
- `ui_functionworkflow` (`ui_functionworkflow_test`): actual FunctionViewer tree,
  graph, modal controls, repeated Apply/close, fit/search/normalization, native
  lock commands, keyboard undo, saved-set keyboard rename/reopen/fresh-process
  persistence, target-range preview/repeat/undo, scene/Xsheet/model cancellation
  and dense paints.

The native application fixture also requires
`functionrigbenchmarkintegrationtest.cpp`: 24 actual parented rig objects and
96 Stage/offset channels. Its pixel oracle removes one channel at independently
calculated curve coordinates, rather than accepting background pixels. The
`function-rig-graph-metrics` JSON in the ordinary native report describes six
QWidget-to-QImage paint measurements and records the Qt/platform/architecture,
canvas, pixel ratio and exact data set. It does not measure compositor latency
or physical input latency, and the fitted frame range includes normal graph
padding outside the requested bounds.

`functionworkflowintegrationtest.cpp` also runs after the existing native UI
fixture inside the real application. It opens the registered Function Editor
menu action and uses actual application handles, Plastic/offset scene channels,
native/SeExpr dependency navigation, breakdown preview/apply/undo/redo, graph
mouse snapping and handle routes, quantization, and whole-scene persistence.
The ordinary native runner is invoked with `--require-function-workflow` on all
three platforms; a missing `function-workflow-native: PASS` checkpoint fails the
run. The artist benchmark's separate runner mode does not require this checkpoint.
The unit/widget manifest now fails if either required suite is missing or skipped,
while preserving every other registered `ui_` test. CI qualification and platform
runtime staging remain pending. No automated fixture proves the manual
visual/device checks listed above.

## Saved-set rename and explicit target ranges

The follow-up for items 3 and 6 is source-complete and requires a new CI run.
Saved-set renaming changes the stored entry's name while preserving its member
IDs, ordering and other metadata. Its widget fixture drives Up/Down, F2 and
Enter, rejects empty/case-insensitive duplicates, verifies uncommitted text is
discarded on Close or context replacement, and launches a fresh process to read
the persisted preferences. The real-application fixture renames and recalls
Stage, offset and Plastic channels without changing document dirty state or
animation history. Workspace set names remain preferences, outside animation
undo; assigning new identities while capturing a set only happens after its
preferences write succeeds.

Target-range retiming uses shared selected bounds across channels and retains
exact destination endpoints, relative interior timing and native handle data.
The helper fixture covers invalid/colliding ranges, stale/locked atomic
rejection, driven-source retention, point selections, one undo transaction and
repeat no-ops. Widget and native application fixtures cover Preview, repeated
Apply, pending-edit cancellation and undo/redo; the native fixture saves and
reloads nine-decimal Stage/offset/Plastic timing with identities intact and
session locks absent. A scoped full-precision writer in `TDoubleParam::saveData`
prevents truncation of those frames, values and handles and restores the caller's
precision mode. Helper assertions check exact numeric roundtrips with both
incoming precision modes. No local builds or tests were run for this follow-up.

## First CI failure and regression coverage

The initial Linux widget run at `b9df36e8` reported `ui_functionworkflow`
terminating with a segmentation fault before emitting a test checkpoint. The
native operation suite passed in that run. Source review found an uninitialized
Xsheet handle in `FunctionSheet` used by its zoom, show and paint paths,
and unchecked Xsheet access in tree and spreadsheet reference-warning rendering.
All optional sheet pointers are now initialized, and both views share the guarded
`Channel::isIgnored()` query. The widget fixture explicitly constructs an
unattached sheet and queries/paints both views with an absent Xsheet handle and
with an empty handle. Flushed phase checkpoints and a targeted Linux debugger
report preserve further failure evidence without changing the original CTest
result. These are source-backed fixes; attribution of the original crash and
runtime qualification remain pending the updated CI run.

The same audit found a separate queued-notification lifetime bug:
`FunctionTreeModel::onChange` retained a raw model pointer in its deferred
functor. An edit followed by closing an editor or destroying a temporary model
could therefore deliver to a deleted model at the next event loop. The functor
now uses `QPointer`, and the regression explicitly destroys an edited viewer
before delivering queued notifications, then verifies a live replacement still
receives one coalesced update. Draining events before destruction is not used to
hide this failure.

The updated Linux run at `ae113c73e` completed the scene-free and queued-model
regressions and the fit/search test. Its debugger then located a crash in
`TSelectionHandle::setSelection`, reached when the next editor selected a key.
The previous viewer had destroyed its `FunctionSelection` while leaving the
non-owning application selection handle pointing at it. `FunctionSelection`
now detaches itself while its concrete implementation and command handlers are
still alive. The widget regression checks that closing a current editor clears
the handle, selecting in a replacement editor works, and closing an inactive
editor preserves another editor's selection. The original tests and debugger
diagnostic remain enabled; qualification of this fix awaits the next CI run.

At `6f593dee6`, Linux passed both Function Editor suites, including the native
widget fixture in 0.27 seconds. The full-application run then reached the end
of the preceding appearance fixture and timed out before a Function Editor
checkpoint. Source inspection found that this preceding fixture leaves an
unsaved dirty PLI resource, whereas clearing the scene dirty flag alone does
not suppress New Scene's separate modified-resource prompt. The test-only
`resetNativeUiIntegrationScene` now verifies prepared portable storage, discards
disposable fixture level/palette dirty flags and selection/history, checks the
actual dirty-resource inventory, then calls native New Scene and verifies an
empty clean scene. It never accepts an unexpected modal. Bounded reset/modal
deadlines and flushed full-application checkpoints report further blocking
dialogs instead of silently waiting for the outer runner timeout. Execution
qualification of this boundary fix remains pending CI.

At `ba3a8f3be8`, Linux passed both Function Editor suites and every assertion in
the full-application Function Editor fixture, including the scene roundtrip.
The process subsequently reported completed application cleanup and exited
with `SIGSEGV`. That shutdown failure still fails qualification. The Linux
runner now captures a separate bounded GDB stack for native signals, retaining
the original failing status and both reports. No shutdown check is bypassed;
the subsequent diagnostic identifies the teardown path below.

Static shutdown review found a concrete command-registry lifetime
violation: MainWindow-owned actions may be destroyed before a room's current
selection detaches, while `CommandManager` retains raw action, shortcut and
reverse-lookup pointers. The prepared fix unregisters an action synchronously
when it is destroyed, retains its command handler/enabled state for subsequent
action recreation, and disconnects destruction callbacks before registry
teardown. Selection label changes tolerate actions that no longer exist.
`ui_functionworkflow` now prepares action-before-editor destruction with a real
FunctionSelection, menu-action and alternative-name lifetimes, reverse/shortcut
lookup cleanup, same-ID recreation, and exact-address reuse to detect stale
pending-shortcut dispatch. These tests use the ordinary ownership path; no
MainWindow or fixture shutdown bypass is added. CI execution of the prepared fix
remains pending.

At `1088dc316`, Linux run `41007693470`, job `123906413255`, again passed the
widget suites and full native Function Editor acceptance before the shutdown
signal. The bounded debugger captured `QActionPrivate::sendDataChanged` from
`TSelectionHandle::setSelection`, `FunctionSelection::~FunctionSelection`,
`FunctionViewer::~FunctionViewer`, and room/stacked-widget destruction. This
confirms the action/selection teardown path identified above. The registry fix
and deterministic regressions are source-reviewed but must still pass CI; the
original application exit status and every existing acceptance check remain
authoritative.


Combined-workflow CI at `623763b59` exposed a missing serialization boundary:
a zero-valued channel without keys lost its optional workspace identity because
Stage Object and Plastic serializers omitted default curves. `isDefault()`
also controls which 3D channels count towards a full animation keyframe, so its
value semantics remain unchanged. The separate `hasDataToSave()` predicate now
retains identities in Stage Object, offset-layer and Plastic serialization.
FX serialization also retains workspace-identified numeric parameters inside
otherwise-unused parameter containers; obsolete parameters remain excluded.
Column time-remap and color-effect activation rules remain value based.

Prepared regressions preserve the strict zero-key combined fixture and add a
native whole-scene roundtrip for default Stage Object, four 3D channels,
offset, Plastic, ordinary FX, column time-remap, brightness, alpha and nested
tint-red identities. They check zero keys, unchanged values and unchanged
full-keyframe/effect-activation behavior. The direct curve test covers default
omission, opt-in identity persistence, identity removal and session-only locks.
These new assertions await execution in CI; no local builds or tests ran.
