# Groundzero roadmap, November 2026

This plan follows an audit of `edition` at `1c49d53` on 2026-11-06. It covers
stabilization, the remaining Toon Boom Harmony gaps, foundations and upstream
work. It adds to the [reliability and performance roadmap](performance_roadmap_2026_10_29.md)
from the [October 29 audit](codebase_audit_2026_10_29.md) and does not repeat
its items (save and undo integrity, cache ownership, export completion,
profiling): that roadmap stays the plan for them.

Nothing here was built or run locally; `CLAUDE.md` directs builds and tests
through CI. Findings come from reading the code and CI logs, and each one
says how it was established.

## How the work is run

- **One branch per item,** from `edition`, one topic per PR, as
  `CONTRIBUTING.md` describes. This plan lives on `plan/roadmap-2026-11`.
- **Parallel agents work in separate clones or worktrees.** Before starting
  an item, check the open PRs for the files it touches, and record the item's
  branch name in the tracking table below so two agents do not take the same
  one. PR #115 (raster brush compatibility) is being worked in a separate
  clone on `codex/pr115-raster-brush-recovery`. It changes raster brush code
  in `tnztools`, `include` and `toonz`, and
  `.github/workflows/seexpr-package-smoke.yml`. No item here edits those
  files as planned, but check its file list before changing SeExpr workflows
  (S1) or drawing tools (D1, D2).
- **An item is done** when its "Done when" condition is met, CI passes, and
  any application testing it lists has been done. A green build alone is
  not done.

**Sizes:** S up to a week, M 2 to 4 weeks, L 1 to 3 months, XL more than 3
months, for one experienced contributor.

**Order:** Track S first. Track H can start alongside it; Tracks R and D wait
for the format decision below. Tracks F and U run in parallel throughout.

## Tracking

| Item | Branch or PR | Status |
| --- | --- | --- |
| I1 | #134 (`fix/rename-product-themes`) | Open, CI running |
| U1 | opentoonz/opentoonz #7241 to #7261 | Open, CI running, no review yet |
| everything else | | Not started |

## Where things stand

- **Against Harmony:** the parity catalogue in `doc/harmony_parity_plan.md`
  is current with Harmony 25 (June 2025) and Harmony 27 (June 2026). Most
  rows it marks Partial or N have since been built: Auto-Patch, Substitution
  Slider, Rig Library, particle baking, per-stroke opacity profiles, the
  deformer stack and the Shape-Aware deformer, and nearly all of
  `doc/fx_and_production_tools_plan.md` (Light Wrap, Edge Extend, Matte
  Choke, Colour Script, camera focus, multiplane depth, motion arcs, spacing
  chart, ghost fade, Scene Checker). The parity table no longer matches the
  code.
- **The remaining Harmony gaps** are mostly in rigging: deformers warp the
  rendered raster rather than the vector strokes, they are not nodes in a rig
  graph, and controllers live in a panel rather than in the viewer.
- **SeExpr works.** Its 17 tests pass in the Linux Build on `edition`, and
  its native integration passes on Windows. The review found six bugs (S1).
- **The release workflow fails intermittently** on the native integration
  test (S2): 3 of the last 10 `edition` runs failed, 4 passed.

## Track S: stabilization (first)

| # | Item | Size | Done when |
|---|---|---|---|
| S1 | **SeExpr bugs**, listed in the SeExpr section below. | M in total | Each has a fix and a regression test, or a recorded reason to keep it. |
| S2 | **Native integration test hangs.** "Test native application integration" times out after 120 s. On 11-02, 11-03 and 11-04 it stopped in `vector-alignment-native` after Align Left on two vector drawings, on Windows (and on macOS on 11-03). In the 11-03 run a `DVGui::Dialog` warning became visible just before the hang, so a modal warning during Align Left is the first suspect. A separate Windows run stopped in a workflow-switch diagnostic ("loading Character"). | M | Each hang has a known cause (application or test) and a fix. Align Left on several drawings shows no unexpected modal dialog. The test passes on Windows and macOS in five consecutive release runs. |
| S3 | **Plans match the code.** Update the parity table in `doc/harmony_parity_plan.md` and the status columns in `doc/master_plan.md`. Check whether the existing Xsheet/Timeline lock button blocks timing edits, as Harmony 25's Lock Timeline Ruler does; if it does not, add H8. | S | Every row's status matches the code, with file references. |

The full test suite does not run on `edition` or on pull requests: this is
V03 of the October audit, in its Phase 0. It is not repeated here.

## Track I: names

| # | Item | Size | Done when |
|---|---|---|---|
| I1 | **Rename the product-named themes.** `ToonBoom`, `Animate` and `Premiere` become `Slate`, `Charcoal` and `Midnight`, with their LESS sources and images. A saved preference that names an old theme selects the new one. | S | #134 merged with CI passing; opening with a preferences file that names an old theme shows the renamed theme. |

Other names were checked and need no change: naming PureRef, Spine, TVPaint
and Photoshop where Groundzero reads or writes their formats is ordinary
interoperability, feature names such as Master Controller are generic, the
deformers use published methods, and the features are Groundzero's own
implementations. Comments that compare an FX to Harmony's ("similar to Toon
Boom Harmony's Curve deformer") are fine here; reword them only when a file
is proposed upstream.

## Track H: Harmony gaps, small and medium

| # | Item | Size | Depends on | Done when |
|---|---|---|---|---|
| H1 | **Automatic line-art matte for Tone / Highlight.** Add Tone / Highlight Shading (`toonz/tonehighlightcommand.cpp`) inserts the FX and leaves their Matte ports for the user to connect. Harmony builds the matte from the line art. Add an option that builds it from the column's Line Art (Toonz levels: ink; vector levels: Line Art strokes). | M | none | The command produces working shading without manual connections, and the manual workflow is unchanged. |
| H2 | **Variable deformer sizes.** The FX fix their counts: Bone 4 bones, Curve 4 anchors, Free-Form 4x4, Envelope 8, and smaller in Deformer Stack layers. `TBoneDeformer`, `TCurveDeformer` and `TFreeFormDeformer` already take any count. Add a count parameter and dynamic parameter sets; old scenes load with their current counts. | M | none | Counts can be changed per FX; old scenes render identically. |
| H3 | **Controllers in the viewer.** The Master Controllers panel (`toonz/mastercontrollerspane.cpp`) has sliders and 2D pads, but nothing in the viewer, and widget modes are not saved. Draw them as viewer gadgets (`tnztools/edittoolgadgets.cpp`) and save the modes in the scene. | L | none | A controller can be driven from the viewer with undo, and its widget survives save and load. |
| H4 | **Time remap shown in the Xsheet.** The remap applies at render and preview, but the cells show the original timing and the scene length follows the cells. Show the remapped timing (an overlay or an indicator) and let the length follow the remap. | M | none | The Xsheet shows what renders, and playback length matches. |
| H5 | **Auto-Patch joint detection.** The patch region is a joint point and radius placed by hand (`stdfx/autopatchfx.cpp`). Derive it from the parent and child overlap. | M | none | Patching a standard arm rig needs no manual placement. |
| H6 | **Pose thumbnails.** The Rig Library shows poses as name badges (`toonz/riglibrarypane.cpp`). Render pose thumbnails with the icon generator. Symbols can already be saved to the library. | S | none | Poses show images. |
| H7 | **Weighted Spine meshes.** Spine export writes unweighted meshes and no draw order (`toonz/spineexport.cpp`). | M | none | An exported rig imports into Spine with bone weights and the right order. |

## Track R: rigging core (large)

| # | Item | Size | Depends on | Done when |
|---|---|---|---|---|
| R1 | **Live vector deformation.** Deformers warp the rendered raster; strokes change only through Bake Deformers into Vector Level (`deformVectorImage` in `toonzlib/tdeformer.cpp`, called from `toonz/deformerbake.cpp`). Apply the same displacement to stroke control points at render and in the viewer, so lines stay sharp and editable. | XL | format decision | A deformed vector column renders sharp lines at any zoom with no bake; baking stays available. |
| R2 | **Deformers as rig graph nodes.** The Rig view shows badges and published ports; deformers are FX or object attributes. Make deformers nodes in one graph with pegs and drawings, editing the existing data. | L | R1 recommended first | A rig can be built and rewired in the node view alone. |
| R3 | **Painted deformer weights.** `TDeformerWeightMap` is uniform or distance based. Add painted per-point weights, in the style of the Plastic rigidity tool. | L | R1 | Weights are painted, saved and used by every deformer type. |

## Track D: drawing

| # | Item | Size | Depends on | Done when |
|---|---|---|---|---|
| D1 | **Retouch along a stroke.** Strokes already carry an opacity and an opacity profile along their length (`TStroke::getOpacityProfile`). Compare the Pump "Opacity Brush" with Harmony 25's Pencil Retouch, and add what is missing, such as retouching width and opacity together. | M | none | A stroke's opacity and width can be painted along its length in one tool, with undo. |
| D2 | **Animated pencil texture.** A per-frame texture offset for textured strokes. | M | none | Texture can change per frame without redrawing. |

## Track F: foundations

| # | Item | Size | Done when |
|---|---|---|---|
| F1 | **Qt 6 at runtime.** The tree builds with Qt 6 on Linux; nothing has been run with it, and Windows and macOS are untested (`doc/qt6_port_status.md`). | L | The application runs and the test suite passes with Qt 6 on all three platforms; then decide when to make it the default. |
| F2 | **OpenGL modernization.** Fixed-function calls remain in 62 files and `QGLWidget` in 4. Port the viewer path (`toonzlib/stagevisitor.cpp`, `toonz/sceneviewer.cpp`) to `tgl::Batch` first. | XL | The viewer draws without fixed-function calls on all platforms. |
| F3 | **Cadmium model license.** Cadmium downloads its models from the `latentspacelabs/cadmium-oss` releases (`toonz/cadmiuminstaller.cpp`), and the repository does not record their license. Record it in `doc/cadmium.md` and the third-party notices. | S | The license is recorded and allows distribution. |

## Track U: upstream to OpenToonz

| # | Item | Size | Done when |
|---|---|---|---|
| U1 | **Land the FX series** (opentoonz/opentoonz #7241 to #7261): answer review, keep CI green, rebase as prerequisites merge. | ongoing | Each PR merged, or closed with a reason. |
| U2 | **Next candidates:** Reference panel, Storyboard panel, Gradient Transform tool, Lasso Cutter, Filter Ports, and the renamed themes. Extract each onto upstream `master`, one topic per PR. The panels and Gradient Transform conflict with upstream and need real porting; Lasso Cutter merges cleanly. Open them after U1 has review feedback, not before. | M each | Each opened with CI passing. |

## SeExpr

**It works.** On `edition` at `1c49d53`, the Linux Build ran all 17 SeExpr
tests and they passed. They cover the FX, the surface FX, the editor and
controls, animation, and native application and batch integration. The 1- and
4-thread benchmarks give identical pixels. The Windows build ran the SeExpr
native integration successfully. SeExpr qualification passes for the engine,
LLVM 14, the SDK and the Python bindings.

**Gaps in what CI checks:**
- The application is built without LLVM, so the JIT is qualified only in the
  standalone SeExpr build.
- The FX tests call `doCompute` directly, so they skip bounding-box clipping
  and input transforms.
- Only a scale transform is tested, not rotation.
- Parallel tests use separate FX instances, never several threads on one.

**Bugs.** Line references are to `edition` at `1c49d53`. Findings 1, 3 and 4
were checked again against the code for this plan.

| # | Bug | Severity | Evidence |
|---|---|---|---|
| E1 | **Expression Color is cut off at the Source bounds.** `doGetBBox` returns the Source's box and never adds the sampling radius (`stdfx/seexprfx.cpp:1382`), and the renderer computes only the part of the tile inside that box. Example: `alpha = max($a, sample_alpha(-6,-6))` for a drop shadow loses the shadow outside the drawing. The same applies to `alpha = 1` fills and to Repeat and Mirror outside the Source. | Medium | Read and rechecked; the tests miss it because they bypass clipping. |
| E2 | **Clamp, Repeat and Mirror are wrong under rotation.** The sampling canvas is the screen-aligned box of the transformed Source, and `readAt` clamps and wraps in screen coordinates. Example: rotating the column 30° with Repeat changes the tiling period, and Clamp returns transparent corners. | Medium | Read; untested. |
| E3 | **Controls are never released.** Each new control gets a new ID, and removed controls move to an archive that is never cleared. The 128-control limit counts both (`stdfx/seexprfx.cpp:33`, `:411`, `:467`), so after 128 controls over the FX's life, Apply is rejected even with few active controls. Preset import allows 512 (`toonzqt/seexpreditor.cpp:1248`), more than the FX accepts. | Medium | Read and rechecked. |
| E4 | **One global lock serializes all expression evaluation,** including ordinary OpenToonz expressions. `TExpression` evaluation takes `seExprGraphEvaluationGate()` (`common/expressions/texpression.cpp:249`) before it knows whether the text is SeExpr, so a slow SeExpr curve on one render thread stalls the others and the UI. No deadlock was found. | Low to medium (performance) | Read and rechecked; measure before and after. |
| E5 | **Clamp, Repeat and Mirror fail on some Sources.** With an infinite Source (a Color Card or gradient) they throw "needs a finite Source canvas", and the frame fails. They also request the whole Source box for every tile, so a float Source over 256 MiB exceeds the working-buffer limit. | Low | Read. |
| E6 | **Hidden size fallback.** With no Source, or an infinite one, `$u`, `$v`, `$width` and `$height` use `width` and `height` parameters (1920x1080) that the Expression Color layout does not show. | Low | Read. |

**Order:** E1 and E3 first, each with a test through the full renderer, not
`doCompute`. E2 needs a rotation test as well. Measure E4 before changing it.

## Out of scope

Kept from the earlier decision: GPU rendering and FX preview, OpenFX,
Cryptomatte, USD and other 3D formats, a 3D scene graph, Harmony Server, and
cloud AI. Groundzero's local Cadmium painting covers part of what Harmony's
Ember add-on does.

## Decisions needed

1. **Format policy** for data upstream OpenToonz cannot read, before R1 to
   R3 start: optional tags that older builds ignore, or a declared Groundzero
   format version.
2. **The first large item** after stabilization: R1 (live vector
   deformation) or H3 (controllers in the viewer).
