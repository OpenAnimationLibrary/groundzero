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
| 3 | Named channel sets | Channel Sets stores named UUID lists in user settings. Optional `workspaceId` attribute on existing `default` scene tag survives rename/reload; copy/undo retain owner ID, clones/imports do not inherit it. Missing/duplicate IDs are reported. | CI serialization/copy tests and native whole-scene save/reopen of renamed stage, offset and Plastic IDs. Original `default` reader ignores attributes; no new child tag was added. Opening then saving with an older build may discard IDs and requires resaving sets. |
| 4 | Solo and edit locks | Tools offers solo/restore and locks selected channels. Native `KeyframeSetter`, FunctionSelection commands, numeric plans and Load Curve apply reject locked writes; toolbar/segment fields disable. Context Reset Handles now uses native undo setters. | CI lock routes and undo; manual graph/sheet/context-menu route matrix. Locks are session-only; undo remains able to restore history. |
| 5 | Numeric multi-key edit | Edit Keys shows mixed value ranges/counts, set/offset values in each channel's display units, preview, repeated Apply and close/cancel. Single-value toolbar directs multiple selections to Edit Keys. | CI native modal repeated Apply/cancel and unit/all-or-nothing oracle tests. Driven endpoints require explicit bake before numeric value edits. |
| 6 | Precise retiming | Edit Keys offers frame offset, grid quantization and positive time scale around a pivot across selected channels. Plan rejects negative frames, collisions and crossing unselected keys, and scales relevant handle time components. | CI timing/quantization/collision tests and native quantize preview/apply/undo; manual non-integer timing interaction. |
| 7 | Normalized display | Tools normalization maps each curve's sampled display range to 0–1, with inverse mouse coordinate mapping. No stored key values change. Recomputed after finished edits. | CI roundtrip/unchanged snapshot; manual constant/driven curves and multi-channel drag. |
| 8 | Curve identification | Deterministic per-channel colors, visible long-name legend, current/locked annotations; names elide within the graph. Stable UUID colors apply after a set is saved; otherwise structural reference/name hash. | Light/dark themes, 100/150/200% DPI and color-vision accessibility. Color is accompanied by names, not the sole identifier. |
| 9 | Interpolation/handles toolbar | Dedicated Interpolation menu exposes native Constant/Linear/Speed/Ease/%/Exponential and selected handle link/unlink/reset. Multi-key handle commands form one undo block. | CI native type/lock tests and full-application interpolation, handle drag/reset/locked-reset undo. Remaining manual handle route matrix. |
| 10 | Snapping | Tools stores frame increment, nearby-key and scene-frame-marker snapping using native marker distance/offset; Alt bypasses snapping for ordinary moves (Shift+Alt retains moves when Alt alone would stretch). Drag uses the clicked selected key's frame anchor. Existing native whole-frame drag collision handling retained. | CI/native drag fixture checks grid, off-center anchor, key, marker, Alt and locked paths. Varying-zoom interaction remains to observe. Fractional retiming belongs to Edit Keys; fractional targets are not falsely snapped by integer drags. |
| 11 | Before/after comparison | Capture/show/hide dashed curves; restore uses validated snapshot plan and one undo after confirmation. Capture buffers are pruned on model changes and cleared on context replacement. | CI plan restore/staleness; manual driven snapshots, visibility and full scene switch. |
| 12 | Breakdown/overshoot | Edit Keys slider previews native breakdown favour from -100% to 200%, supports selected native numeric curves and explicit insertion frame. Close cancels; Apply one transaction. | CI breakdown oracle and real application Plastic angle/stage-offset preview/apply/undo/redo/scene reload fixture. Manual Plastic distance visual review. Same native channel type is supported without a rig-specific adapter. |
| 13 | Key reduction | Preview reports removed keys and maximum sampled display error. Independent original curve/key samples form the oracle; only eligible selected linear, unstepped spans reduce. Holds, nonlinear and driven boundaries stay protected. | CI exact/sample oracle cases; artist review of tolerance. Scope deliberately preserves nonlinear segments rather than approximating them without a continuous error proof. |
| 14 | Bake driven curves | Cancellable native evaluation at explicit inclusive first/last/interval points, with exact stored sample values. Invalid/nonfinite/native or SeExpr fallback failures abort the whole plan. Outside-range keys retained. | CI native/SeExpr failures, cancellation and sample oracles. Partial boundaries inside a curve must already be keys; cyclic operations stay within the original period. |
| 15 | Motion presets | Native list/curve thumbnail, names, favorites, search, save, preview and explicit duration/amplitude/value-offset adaptation. Numeric curves only; driven/file sources must be baked. Safe temporary write and non-overwriting publish. | CI adaptation/measure tests; manual save/load/favorites restart on all OSes. No foreign expression binding is imported silently. |
| 16 | Dependency inspection | Actual native `referenceParams`/`dependsOn` results, expression validation errors with frame, source/dependent navigation, unresolved/outside-model notices. Raw model pointers are invalidated before refresh/reset. | Prepared native/SeExpr resolved binding navigation fixtures in full application; invalid, deleted, circular and external references still need UI observations. No regex claims of complete bindings. |
| 17 | Dense graph scaling | Visible frame interval limits segment/key/group gadget traversal. Selected handles remain available. Native widget fixture renders a 20,000-key graph through a 50-frame viewport, checks pixels/source integrity and reports timing. | CI timing log and native application benchmark. No speedup or performance budget is claimed without results. |
| 18 | Keyboard/accessibility | Arrow keys nudge selected frames/values using the same validated plans; Shift multiplies by ten. Configurable value increment and logical-pixel hit radius, control tooltips and discoverable modifier help. | CI arrow/undo path; keyboard-only, screen-reader labels, tablet and DPI observations. |

## Registered CI coverage

`BUILD_UI_PORT_TESTS` creates the following targets, both dependencies of the
ordinary `ui_port_tests` target. CTest names use the existing `ui_` selection:

- `ui_functioncurveoperations` (`ui_functioncurveoperations_test`): native data
  operations, units, collisions, stale/locked atomic rejection, undo isolation,
  discrete/range checks, protected reduction, sampled bake, SeExpr preservation
  and failures, cancellation, overshoot, presets, restore and identity persistence.
- `ui_functionworkflow` (`ui_functionworkflow_test`): actual FunctionViewer tree,
  graph, modal controls, repeated Apply/close, fit/search/normalization, native
  lock commands, keyboard undo, scene/Xsheet/model cancellation and dense paints.

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
