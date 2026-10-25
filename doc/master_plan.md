# groundzero master plan (after Tier 6)

This plan turns the Harmony parity plan (`doc/harmony_parity_plan.md`) into
work tiers, in a fixed order:

1. **Partial features first.** Finish what already exists, so nothing stays
   half-built.
2. **Missing features next.**
3. **Everything else last**: foundations that are not tied to one feature,
   quality, pipeline and polish.

## Scope rules

- **Out of scope: GPU and 3D.** This covers GPU deformation and FX preview,
  OpenFX hosting, Cryptomatte and multi-pass EXR, more 3D formats, 3D scene
  objects, a true 3D camera, USD and the Blender bridge. The existing CPU
  paths stay as they are: GLB model FX, Transform3D, and the experimental GPU
  viewer compositing.
- **Not attempted: Harmony features that cannot be matched.** These are
  Harmony Server / WebCC, Storyboard Pro conform and Ember AI (see section
  3.4 of the parity plan). The Contour Editor and the Shape-Aware deformer
  are research items, at the very end.
- **One PR per tier.** Each item is one commit in that PR. A tier is merged
  only when the Linux Build check is green.
- **New behaviour is opt-in.** Scenes that do not use a new feature save
  exactly as before. A new format that older builds or upstream OpenToonz
  cannot read needs an explicit decision.

Sizes: S is a few days, M is one to two weeks, L is several weeks, XL is a
month or more.

---

## Part A: finish the partial features

### Tier 7: rigging and deformation

Most items here depend on A1 to A3, which the parity plan lists as
foundations (F3, F4, F5). They are done here because finishing these
partials needs them.

| # | Item | What is missing today | Approach | Size | Depends on |
|---|---|---|---|---|---|
| A1 | **Deformer stack with weights** (Weighted multi-deformer) | Deformers cannot be stacked on one drawing. There are no per-point weights. | Extend the Tier 6 `TDeformer` layer with a `DeformerStack`: an ordered list per column, with blend weights painted or derived from distance. Apply it in the column render path, before FX. | XL | Tier 6 deformer layer |
| A2 | **Rig node view** (Rigs built in a node view, Nested groups) | Pegs, deformers, drawings and composites live in separate schematics. There is no group node with published ports. | A Rig view on the Stage Schematic model that also shows column deformers and their parameters. Group nodes publish chosen channels as ports. Includes port filtering. | L | A1 |
| A3 | **Offset layers** (Attribute Controller) | Poses and controllers are baked into keys. | Per-channel offset layers: base animation plus additive layers kept separately, each with a weight. They are stored as extra curves, and only when used. Add Setup Mode, and "train from existing keys". | L | none |
| A4 | **Master Controller widgets** | No viewer widgets, no 2D pad, no pose interpolation, and controllers cannot drive drawing swaps. | A viewer gadget for each `ctrl_` controller: a slider, a 2D pad or a grid. Poses are interpolated by controller value, using the Pose library on top of A3. A controller channel can drive a drawing-substitution index. | L | A3 |
| A5 | **Curve deformer** | No Bézier handles of its own, no handle length, needs a mesh. | A Curve deformer on `TDeformer`: a Bézier chain with tangent handles and animatable lengths. It works on any column, with no mesh. | M | A1 |
| A6 | **Envelope deformer** | Straight edges, exactly 8 points, deforms the rendered image instead of vector strokes. | Bézier edges, a variable point count, and an option to deform vector strokes (map control points through `deform()`) instead of the raster. | M | A1 |
| A7 | **Free-Form deformer** | No cage or lattice of free points. | A lattice or cage deformer on `TDeformer`, with animatable control points and an N×M grid option. | M | A1 |
| A8 | **Bone deformer** | Needs a mesh first, and has no region of influence. | Bones on `TDeformer` that work without a mesh, with regions of influence (capsules) and weights. Plastic stays as the mesh path. | L | A1 |
| A9 | **Game bones and game export** | Mesh and Plastic deformation are not exported. No Unity or Godot path. | Spine export of mesh attachments and their deformation (weighted meshes from Plastic or A8). Godot and Unity read Spine through their runtimes. | M | A8 |
| A10 | **Drawing substitution** | No substitution slider in the viewer. Templates have no thumbnail browser. | A viewer gadget for the current column's drawings, plus a template browser with thumbnails. | S | none |
| A11 | **Cut-out character library** | No visual library of rigs and poses. | A Library panel with thumbnails of rig templates and poses, drag-and-drop into the Xsheet, and poses applied on drop. | M | none |
| A12 | **Auto-Patch** (built in Tier 6, partial) | Every line of Top over Bottom's paint counts as a joint line. Disabling the FX hides Bottom. | An automatic joint region from Plastic or the Kinematic Output parent vertex, used as the mask when none is connected. Optionally pass through Top over Bottom when disabled. | S | none |

### Tier 8: drawing, timing and pipeline

| # | Item | What is missing today | Approach | Size | Depends on |
|---|---|---|---|---|---|
| B1 | **Line Art / Colour Art** | No Overlay or Underlay layers, no Art Layer Manager, no "Create Colour Art from Line Art". | Four art layers per drawing, stored as stroke roles with a role tag written only when used. A manager panel. "Create Colour Art" copies the line strokes as 0-thickness colour-art strokes. | L | none |
| B2 | **Pencil lines** | No opacity along a stroke, no Retouch for opacity, no animated texture. | Per-point opacity in the stroke's thick points, saved only when not 1. The Pump/Retouch tool edits opacity. The texture offset can be animated per frame. | L | none |
| B3 | **Brushes** | No import of ABR (Photoshop brush) files. | An ABR parser for sampled brushes, mapped to the raster brush engine. | M | none |
| B4 | **Morphing** | No automatic stroke matching, no zone or vanishing-point hints, no velocity curve. | Automatic matching by shape and position (on top of the Tier 5 pairing hints), zone hints, and an ease curve for the in-betweens. | L | none |
| B5 | **Colour Override** | Only main colours, no viewer display, no palette-swatch picker. | A swatch picker in FX Settings, an option to override style parameters and textures, and viewer display through the column palette override. | M | none |
| B6 | **Light Shading, Volume, Glue, Shine** | No shading driven by a light inside a shape. | CPU FX on the Tone/Highlight model. A light position (a point gadget) drives tone and highlight shapes clipped to the colour-art regions. Volume and Shine are variants. | L | B1 |
| B7 | **Retime / Hold / Loop** | No time-remap curve per column. | A per-column time-remap channel (frame in to frame out) in the Function Editor. Hold and Loop are presets on top. | M | none |
| B8 | **Storyboard** (built in Tier 6, light) | Cards cannot be reordered, no notes per panel, Play does not stop at the panel's end, thumbnails show one drawing. | Drag to reorder (moves cells), a notes field, play bounded to the panel, and composite camera thumbnails. | M | none |
| B9 | **Lasso Cutter** (built in Tier 6) | Regions the cut passes through lose their fill. It only copies to the clipboard. | Close the cut edge with an autoclose stroke so fills survive, and paste into a new drawing or column in one step. | S | none |

---

### Tier 7 status

All twelve items are built (not yet confirmed by a CI build on Linux when
this was written). What each one delivered and what is still partial:

| # | Delivered | Still partial |
|---|---|---|
| A1 | `TDeformerStack` with uniform and distance weights; Deformer Stack FX (3 envelope layers) | Raster warp only; weights are not painted; not a per-column stack |
| A2 | Rig view in the Stage Schematic: badges, filter, published channels with ports, clickable badges | Deformers are not graph nodes; ports have no links |
| A3 | Offset layers on stage objects, panel, Function Editor channels, Train from Keys, pose "apply as layer" | No Plastic / Path / sound channels; no Setup Mode |
| A4 | 2D pad, pose blend through offset layers, drawing-swap bake in Master Controllers | No viewer gadgets; widget modes are not saved |
| A5 | `TCurveDeformer` and Curve Deformer FX | Fixed 4 anchors; raster warp |
| A6 | Envelope Deformer Plus (3 to 16 points, smooth edges) and a vector bake command | Stroke deformation is a bake, not live |
| A7 | `TFreeFormDeformer` and Free-Form Deformer FX (4x4 lattice) | No free cage; not a stack layer yet |
| A8 | `TBoneDeformer` and Bone Deformer FX (4-bone chain, no mesh) | Raster warp; not a stack layer yet |
| A9 | Spine mesh attachments and Plastic deform timelines | Unweighted meshes; no stacking order |
| A10 | Substitution Slider panel; template thumbnails already existed | No separate template browser |
| A11 | Rig Library panel (rigs and poses) | Pose thumbnails are name badges |
| A12 | Auto-Patch joint region and Enable Patch switch | Circle only; no automatic joint detection |

Follow-ups that Tier 7 opened: add the Curve, Free-Form and Bone deformers
as Deformer Stack layers, and move the deformer FX to vector strokes where
the stroke data is available.

---

## Part B: build the missing features

### Tier 9: missing features

| # | Item | Approach | Size | Depends on |
|---|---|---|---|---|
| C1 | **Lock Timeline Ruler** | A toggle that keeps the frame ruler from scrolling with the cells. | S | none |
| C2 | **Port filtering in the node view** | A filter field in the FX and Rig schematic port lists. | S | A2 |
| C3 | **Compact file format** | An optional compressed scene (zstd over the existing XML), with fast load through lazy level loading. Plain `.tnz` stays the default and is always readable. | M | none |
| C4 | **Contour Editor** (research) | An outline (brush-shape) stroke model for vector strokes, so the outline can be edited directly. This needs a new stroke representation and is high risk. | XL | none |
| C5 | **Shape-Aware deformer** (research) | Points, bones and a cage together, on the deformer stack. Harmony's algorithm is proprietary; start from published methods (bounded biharmonic weights). | XL | A1, A7, A8 |

Missing items that stay out: OpenFX, Cryptomatte and performance
anti-aliasing (GPU and 3D scope), and Ember AI (not realistic).

---

## Part C: the rest

### Tier 10: foundations and quality

| # | Item | Approach | Size |
|---|---|---|---|
| D1 | **Finish Qt 6** | Make the manual Qt 6 build green: the Windows-only tablet and window code, qsizetype narrowing, QFlags zero literals. Then build Qt 6 in CI next to Qt 5, and make it the default once it is stable. | L |
| D2 | **Fix what the loader tests find** | Run the Tier 6 damaged-file tests once with `BUILD_TESTS`. Fix the crashes they report (the PSD reader is expected to have some), and record any known issues. | M |
| D3 | **FX render references** | Add the reference images for the FX render test from a CI run, so render changes are caught. | S |
| D4 | **Script API depth** | Images, levels and frames, rendering, and the current UI scene, in both JavaScript and Python. Python in default builds where packaging allows. | M |
| D5 | **Translations** | Add the strings from Tiers 2 to 6 to the `.ts` files. | S |

### Tier 11: workflow polish (the parity plan's quick wins not covered above)

| # | Item | Size |
|---|---|---|
| E1 | Whole-rig breakdown and Quick Ease in the Timeline and Xsheet (today they are only in the Function spreadsheet). **Built in `t10/rig-ease`**: Rig Breakdown and Rig Ease commands in the Cells menu and the cell menu, over the selected columns and optionally their child objects; see `doc/rig_breakdown_ease.md` | S |
| E2 | Layered PSD scene export: one layer per column, instead of a flat image per frame | M |
| E3 | Light Table opacity setting and per-column exclusion | S |
| E4 | Tone / Highlight preset macro | S |
| E5 | Reference panel: several images, remembered view state, PSD layers | S |
| E6 | Faster scene save and load (profiling first) | M |

---

## Order at a glance

1. **Tier 7**, rigging partials. Start with A1 (deformer stack) and A3
   (offset layers), since most other items depend on them. A10 to A12 can
   run in parallel.
2. **Tier 8**, drawing, timing and pipeline partials. These can run in
   parallel with Tier 7; only B6 waits for B1.
3. **Tier 9**, missing features. C1 and C3 any time, C2 after A2, and C4 and
   C5 last.
4. **Tier 10 and Tier 11**, foundations, quality and polish. D1 and D2 can
   start as soon as Tier 6 is merged.

Keep the parity table in `doc/harmony_parity_plan.md` up to date as items
land: Partial becomes Y when the item's gaps are closed.
