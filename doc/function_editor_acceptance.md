# Function Editor workflow acceptance

Implementation base: `aa01975f38d6226156e5b07df7271b8fc199d003` (`edition`).
Current integration base: `edition` `d510cf089e03e8887f315bb6ff8b88d985687539`,
including the merged native UI, particle-cache safety, Qt 6 build and
SeExpr/neural state-management fixes through #126.
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

At `ebf0780495`, Linux compiled the new tranche and passed the native curve
operations suite, saved-set rename, target-range and context-cancellation
widget checks. The preset-import widget fixture then hit its bounded deadline
with `FunctionPresetImportFile` still open. The debugger confirms a waiting
file-dialog event loop rather than a memory fault. The fixture had called
`selectFile()` after the chooser was visible with its filename field focused;
Qt deliberately leaves that field unchanged in this case. The corrected
driver enters the exact existing path in the actual filename control, verifies
the chooser's selected path and enabled Open button, then queues a real Open
click. Native file validation, both cancellation points, import/adaptation and
undo assertions remain unchanged. Fresh CI is required; the full-application
fixture and installed package were skipped after the original failure.

The same head's macOS run `41042351273` passed native curve operations but
aborted the widget fixture at `Stale comparison confirmation timed out` and
failed the full application at `Expected comparison restore confirmation was
not shown`. Both test drivers identified the confirmation by its window title,
which Qt 5 deliberately ignores on macOS. The corrected drivers require the
actual Function Editor owner, exact question text, Question icon, exact Yes/No
button set and enabled answer button. They still click the real button; the
No/cancel, exact restore, undo/redo and stale-context rejection assertions are
unchanged. A mismatched widget confirmation now reports the failed identity
check before its bounded deadline. No production dialog behavior changed;
fresh supported-platform CI remains required.

At `a6bbf59aa`, Linux passed both Function suites, the full native workflow and
installed-package lifecycle. Windows compiled and passed native curve operations;
its step results also report successful full-application integration. The widget
suite reported an access violation after constructing its stale-comparison fixture. Its last
checkpoint preceded capture and confirmation, so the original report does not
establish the crashing operation. Targeted flushed checkpoints and a bounded
Windows CDB capture now distinguish capture, modal setup, model reset and the
actual Yes click. The original failed result remains authoritative; no product
behavior or assertion was changed without a stack establishing the cause.

Further source review identified an incompatible test platform in the pinned
Windows Qt 5.15.2: `QMessageBox::showEvent` calls
[`qt_getWindowsSystemMenu`](https://github.com/qt/qtbase/blob/v5.15.2/src/widgets/dialogs/qmessagebox.cpp#L73-L79),
which dereferences the platform native interface. The
[`offscreen` integration](https://github.com/qt/qtbase/blob/v5.15.2/src/plugins/platforms/offscreen/qoffscreenintegration.h#L49-L80)
does not implement that interface and inherits the base
[`nullptr` result](https://github.com/qt/qtbase/blob/v5.15.2/src/gui/kernel/qplatformintegration.cpp#L108-L111).
The failing widget sequence reaches its first actual message box here, while
the completed same-Qt full-application run passed both comparison answers and
the strict retime/rig assertions using the `windows` platform. The widget suite
and its diagnostic rerun now use that native Windows platform; Linux/macOS
settings, product code and every modal/state assertion are unchanged. Runtime
platform provenance is printed. The original crash stack was not captured;
fresh CI must qualify this source-backed fixture correction.

Combined-workflow Linux head `c7fdf2aec` subsequently reached the real application
and passed frozen native/SeExpr comparisons, existing graph interactions,
unkeyed identity persistence and saved-set renaming. The fractional retime scene
roundtrip then failed its exact authored-key comparison. That original report
did not identify a field or value, so the same strict predicate now reports the
channel, key and each differing field at full double precision. Bounded
failure evidence also includes at most 8 KiB for the numeric channel's expected
and reopened curve representations, with identities removed and driven/file data excluded;
whole scenes and temporary asset paths are not retained. Independent
source review found a native undo defect: switching interpolation to Speed or
Ease changes both segment endpoints but only recorded the first endpoint.
Undo therefore retained the other endpoint's changed incoming tangent after
restoring a Linear segment. The setter now records both changed endpoints,
and its final-key guard prevents access to a nonexistent following endpoint.
Prepared regressions cover exact native undo/redo, ease conversions, locks and
final-key requests; the real editor fixture now checks full-curve equality
immediately after interpolation undo. This source-proven defect predicts a
dormant incoming-handle mismatch, but the actual failed field remains
unobserved until CI. No serialization rule, field predicate or comparison
tolerance has been changed to accept the original failure.

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


A final source audit reopened the same-sheet owner-removal boundary. Ordinary
model refresh emits layout signals, while the key and preset dialogs previously
watched only scene/xsheet replacement and model reset. Their retained parameter
references could therefore authorize edits to a removed column, offset layer or
FX owner. The dialogs now check full-model membership after layout refresh and
before preview, Apply, Save and Import, including nested file/name prompts and
sampling cancellation. A harmless refresh leaves the dialog open for repeated
Apply. This is a source-proven boundary defect; no runtime crash is attributed
to it.

Selection pruning now belongs to FunctionSelection, which can inspect every
owned curve, including spreadsheet ranges between keys. Removing an owner
invalidates the old cell rectangle and segment domain, releases detached/empty
references, and preserves surviving explicitly selected keys. This prevents a
rejected dialog from leaving detached curves accessible to later commands.

Prepared native application coverage removes real column, offset and FX owners
in twelve cases: key/preset dialogs, nested Save/import file/import name routes,
and all three owner types with a cell range containing no keys. It verifies
unchanged detached and surviving curves, history/index, dirty state and library
publication, rejects late callbacks, clears stale previews, and exercises Cut,
Delete and inclusive Step after selection invalidation. Widget coverage retains
repeated Apply/no-op assertions across harmless layout refresh. These additions
are peer-reviewed and await execution at the new CI head; no local builds or
tests ran. Earlier successful runs do not qualify this added boundary.

## Pending panel lifetime and intervening key edits

A source audit after `efdd9139f` identified two additional boundaries. They were
not observed crashes in that revision's passing native runs. A stack-owned
workflow dialog was also a QObject child of its Function panel, so deleting the
panel during its modal loop could destroy the stack dialog before unwinding.
Separately, the key editor retained key indices while rebuilding a fresh plan
for each Apply: another operation inserting or deleting an earlier key could
make a still-valid index refer to a different key.

The prepared correction keeps stack dialogs independently owned and associates
their native windows with the panel through a non-owning transient relationship.
Panel hide/destruction rejects pending Edit, Presets, Sets, Dependencies,
comparison Restore and preference prompts. Nested Save/Import/name dialogs
follow that cancellation, and late callbacks cannot change the model or write
preferences/presets. QPointer checks protect post-modal comparison restoration.
An ordinary panel hide restores the prior frozen comparison without reevaluation;
existing show/rebuild behavior still invalidates cached scene context on reopen.
The key editor checks exact native snapshots from dialog open before planning,
and advances that baseline only after its own successful Apply, preserving
repeated edits while rejecting intervening same-owner mutations.

Prepared widget regressions exercise queued Apply after panel hide/destruction,
and insertion/deletion both before and after Preview for value and target-range
operations. They retain exact foreign-edit undo/redo, both channels, identities,
dirty state and history. The full application fixture creates actual
FunctionViewerPanel instances with application handles and exercises 24
hide/destruction routes, including nested preset file/name and set-name prompts,
late dependency navigation, Restore and numeric preferences. It checks preserved
animation/history, exact preferences and library bytes, and frozen comparison
samples on hide. A separate real-control Save/Rename/Delete/Reopen channel-set
cycle verifies preferences persistence and that deleted sets cannot be recalled,
without introducing animation undo for workspace preferences. These new checks
are authored and source-reviewed only until fresh exact-head CI executes them;
no local build or test was run.

The subsequent focused Linux run at `3dd99b2f69262dbe7612efc90dc452b6fbc48deb`
compiled and passed the widget fixtures, including queued panel closure and
key-drift checks. Its isolated native Function workflow crashed during the new
panel sequence; GDB reproduced `ParamCalculatorNode::onChange` called from
`TDoubleParam::setKeyframe` in `panelLifetimeDialogs`, after the dependency route.
This remains a failed native qualification, independently of the original
full-suite vector failure.

Source inspection found a separate observer-lifetime defect:
`FunctionSegmentViewer` registers itself on its current curve but its destructor
released that curve without unregistering. A source edit can consequently
propagate through a native expression to the already destroyed segment editor.
The correction unregisters before releasing. Prepared regressions check exact
observer membership through both native segment-binding slots and destruction,
and retain all 24 panel routes in their original order. After every native panel
is destroyed, source and dependent key edits exercise propagated and direct
notifications, restore exact authored snapshots, and add no undo entry. These
new source/tests require fresh exact-head CI; no local execution occurred.
