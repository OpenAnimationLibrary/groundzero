# Adobe Animate features: audit

What groundzero would need to match the parts of Adobe Animate it lacks,
feature by feature: the existing code each one builds on, what is missing,
the approach, the effect on the file formats, the risks and the size. It is
a read-only survey of `edition` (code read, nothing built); paths are under
`toonz/sources/`. Sizes: S is a small pull request, M a normal one, L
several.

Already in groundzero, so not covered: onion skin, bones and IK, pegbar
parenting, symbols with Once/Loop/Hold timing, auto lip-sync, camera with Z,
Matte In/Out, the FX, vector inbetween with stroke hints, ease presets,
motion paths, gap closing and selective fill, the Align and Distribute
panel, corner Free Deform, and GIF, WebP, video, sprite sheet, animated SVG
and Spine exports.

## Summary

| # | Feature | Builds on | Format impact | Size |
|---|---|---|---|---|
| 1 | Motion Presets panel | Poses, offset layers, Train from Keys, the Rig Library panel | None (presets are files outside the scene) | M |
| 2 | Color Effect per column | `TGenericColorFunction`, the hidden `ColumnColorFilterFx`, time remap's saved-only-when-used fx params | Optional fx params, ignored by older builds | M-L |
| 3 | Shared Library across scenes | The Rig Library panel, Save Selection as Template, `IoCmd::loadChildLevel`, the studio palette's linked styles | Optional link tag, only for linked symbols | L (M without "update uses") |
| 4 | Lottie export | Spine export (hierarchy, baked transforms, drawing swaps), the SVG writer's region and outline paths, `QJsonDocument` | None | L (first version M) |
| 5 | Width tool | The Pump tool's Thicken/Thin, `TStroke::insertControlPoints`, `UndoModifyStroke` | None (PLI stores thickness per point) | M |
| 6 | Gradient Transform tool | `TLinGradFillStyle` / `TRadGradFillStyle` parameters, region picking | None for step 1; a new PLI tag for true per-region gradients | M (step 1) |
| 7 | Edit Multiple Frames | Already there for vector levels (Selection tool Mode: Selected Frames, Whole Level) | None | S-M |
| 8 | Distribute to Layers / Keyframes, Convert to Symbol | `TVectorImage::splitImage`, paste-into-new-level, `SubsceneCmd::collapse` | None | M |
| 9 | Envelope modifier | Free Deform, `TFreeFormDeformer` (B-spline lattice), `UndoChangeStrokes` | None | M |
| 10 | Rig Mapping | Named stage objects and Plastic vertices, the pose library's channel addressing | None (writes keys) | M |
| 11 | Brush modes and Fluid Brush | Draw order Over/Under All, `makeOutlineStroke`, `TTrackPoint` time and length | None | Fluid S, Behind/Fills M, Inside/Selection L |
| 12 | Texture Atlas export | Spine export's drawing collector and trimming, the sprite sheet writer | None | M |
| 13 | Outline view per column | Column status bits, the transparency check's inks-only drawing | A status bit, kept and ignored by older builds | S (vector, Toonz raster), M with raster edges |

Found during the audit: **a bug in Free Deform**. When it scales line
thickness, `VectorFreeDeformer::deform` (`tnztools/vectorselectiontool.cpp:270`)
builds its sample points from `p.x` where `p.y` belongs, and samples around
the deformed point instead of the original one, so deformed lines get the
wrong width. In the Selected Frames and Whole Level modes, Free Deform also
changes only the current frame: it moves the corners of the current frame's
box alone (`tnztools/selectiontool.cpp:525`), while the other frames are
transformed through their own boxes (`vectorselectiontool.cpp:650-715`),
which move, rotate and scale update but Free Deform does not. Both are
worth fixing before the Envelope modifier and Edit Multiple Frames, which
reuse that code.

## 1. Motion Presets panel

A library of saved motions (bounce, fly-in, shake) applied to any column or
pegbar in one click, relative to where it is, and saved from selected keys.

- **Builds on.** `PoseLibrary` (`include/toonz/poselibrary.h:45`) is
  additive: it adds weight × offset to the current values
  (`toonz/posespane.cpp:409-452`) in one undo block. Offset layers
  (`tstageobject.h:188-215`) with **Train from Keys**
  (`tstageobjectcmd.cpp:2142`) already extract "curves minus their value at
  the first key", which is what a preset stores. The Rig Library panel
  (`toonz/riglibrarypane.cpp:48, 299-341`) is the model for a panel over a
  folder on disk.
- **Missing.** Poses are single frames and live only in the scene. There is
  no preset file, no stock presets, no relative paste of keys.
- **Approach.** A `MotionPreset` in toonzlib: per channel, keys with frames
  and values relative to the first key. Apply at the current frame into a new
  offset layer (or as base keys). Capture from the key selection with the
  Train from Keys code. A Motion Presets panel over
  `Library/motionpresets/*.tnzmotion`, with stock presets shipped in the
  stuff folder. Add the tilt channels (`T_RotX`, `T_RotY`) to the pose
  channel table.
- **Format.** None in the scene beyond keys or offset layers, both already
  saved only when used.
- **Risks.** Expression, cycled and speed-handle keys; whether scale and
  rotation are offset or multiplied; Plastic channels cannot go in offset
  layers.

## 2. Color Effect per column

Animatable Brightness, Tint (colour and amount), Alpha and Advanced
(per-channel multiply and offset) per column, set from a panel without
wiring FX.

- **Builds on.** `TGenericColorFunction(m[4], c[4])`
  (`include/tcolorfunctions.h:50`) is exactly Animate's Advanced effect and
  is already used for onion skins. The viewer passes the column's opacity
  and filter colour to the painters (`toonzlib/stage.cpp:475-482`,
  `stagevisitor.cpp:645-649, 1645-1650`). The render wraps the column in the
  hidden `ColumnColorFilterFx` (`scenefx.cpp:976-987`). The time remap
  params on `TLevelColumnFx` (`tcolumnfx.cpp:859-907`) are the model for
  animatable per-column values saved only when used.
- **Missing.** Opacity and colour filter are fixed values
  (`include/toonz/txshcolumn.h:159`); there are no animatable colour
  channels, no colour transform in the painters or the render, no UI.
- **Approach.** Params on `TLevelColumnFx` (brightness, tint colour and
  amount, alpha, the 8 advanced values), turned into m/c per frame. The
  viewer gets it as a `TGenericColorFunction` for vectors and a multiply and
  offset pass for rasters; the render gets a hidden `ColumnColorTransformFx`.
  The panel extends the column transparency popup
  (`xshcolumnviewer.cpp:1983`) and the curves show in the Function Editor.
- **Format.** Fx params, written only when used, which older builds skip.
  Not a column tag: `TXshLevelColumn::loadData` throws on unknown tags
  (`txshlevelcolumn.cpp:246`).
- **Risks.** Many viewer paths to cover (vector, raster, Toonz raster, GPU,
  Plastic mesh, sub-xsheets); premultiplied maths; zerary and mesh columns
  have no level column fx.

## 3. Shared Library across scenes

Symbols, rigs, poses, levels and palettes stored outside any scene and
dragged into any scene, optionally updating their uses when edited.

- **Builds on.** The **Rig Library panel** (`toonz/riglibrarypane.cpp`)
  already lists `Library/templates/*.tnz` with icons and inserts them with
  `IoCmd::loadSubScene`. **Save Selection as Template**
  (`subscenecommand.cpp:2788-2978`) packages columns, pegbars, FX and the
  level files into `Library/templates/<name>/`. `IoCmd::loadChildLevel`
  (`iocommand.cpp:873-968`) brings a saved piece into a scene, reusing
  matching levels or importing them. The studio palette's linked styles
  (`studiopalette.cpp:559-604`) are the model for "update uses".
- **Missing.** Symbols cannot be saved to disk (they are pointers into the
  scene, `symbolspane.cpp:63-106`); a loaded template keeps no link to its
  source; no poses, levels or palettes in the library; no project library
  folder; inserting a template has no undo.
- **Approach.** Grow the Rig Library into a Library panel with Symbols and
  Rigs, Poses, Levels and Palettes tabs, over `stuff/library` (personal) and
  an optional project `+library` folder. Save a symbol through the template
  code; give each item an id and revision; insert with a real undo; "Update
  Uses" swaps the xsheet of every linked child level, as linked styles do
  for palettes.
- **Format.** An optional link (id and revision) for linked symbols only,
  kept where older builds skip it.
- **Risks.** Path resolution and duplicated level files on import, renamed
  duplicate levels after an update, palettes embedded per level, the camera
  prompt on insert, undo of an update across many uses.

## 4. Lottie export

Bodymovin JSON for lottie-web, iOS and Android: vector levels as shape
layers with animated transforms, sub-xsheets as precomps, drawing swaps as
visibility.

- **Builds on.** The **Spine export** (`toonz/spineexport.cpp`) already maps
  the stage hierarchy to bones, bakes each frame's local transform
  (`:909-929`), drops redundant keys (`:339-367`) and turns drawing swaps
  into keys. The SVG writer turns regions into paths (`writeRegion`,
  `image/svg/tiio_svg.cpp:1909`) and strokes into filled outlines
  (`computeOutlines`). `QJsonDocument` is used by every exporter.
- **Missing.** Everything Lottie-specific: shape layers, quadratic to cubic
  curves, precomps, the y-down conversion, and region holes (the SVG writer
  draws subregions as separate paths, so holes are not cut).
- **Approach.** A scene-to-JSON core in toonzlib (no GUI dependency) and a
  File > Export command. Pegbars and columns become null layers with
  parents; each run of one drawing a shape layer with in and out points;
  sub-xsheets precomps with time remap; raster drawings image assets. First
  version: top-level and sub-xsheet columns, solid fills and outline
  strokes, raster levels as PNG assets, baked transforms, static opacity.
- **Format.** None.
- **Test.** A ctest that builds a small scene in memory (as
  `scenebench_test` does), exports it, and checks the JSON: frame rate,
  layer count, parents, key counts, precomp references, path vertices.
- **Risks.** Only the main colour of a style carries over (no gradients or
  textures in the first version); variable-width strokes make large files;
  Z perspective, Plastic deformation, FX and blend modes cannot be expressed.

## 5. Width tool

Drag on a vector line to change its width at that point, and save and apply
width profiles.

- **Builds on.** The Pump tool's Thicken and Thin modes already change
  control point thickness in place with a falloff and one undo per stroke
  (`tnztools/pumptool.cpp:716-770`). `TStroke::getNearestW` and
  `insertControlPoints(w)` (`include/tstroke.h:109, 213`) find and add a
  point without changing the shape. `UndoModifyStroke`
  (`include/tools/toolutils.h:251`).
- **Missing.** A point-precise width drag, and width profiles (nothing
  exists).
- **Approach.** A Width tool (or a Pump mode): hit the stroke, insert a
  point when none is near, drag its thickness with a falloff to the
  neighbours. Profiles as a list of (length, thickness) pairs saved like the
  vector brush presets (`toonzvectorbrushtool.h:49, 104`), applied by
  resampling the stroke.
- **Format.** None: PLI already stores a thickness per control point
  (8 bits, `image/pli/pli_io.cpp:2273-2304`).
- **Risks.** The off-curve control points carry thickness too; the 8-bit
  quantization; inserted points make strokes denser.

## 6. Gradient Transform tool

Move, rotate and scale a gradient fill in the viewer.

- **Builds on.** The linear and radial gradient styles have angle, centre
  and size parameters (`colorfx/regionstyles.h:639, 694`), drawn relative to
  each region's bounding box (`regionstyles.cpp:2862-2900, 3127`).
  `TVectorImage::getRegion(pos)` picks the region.
- **Missing.** A viewer gadget for style parameters, and any data per
  region: a region stores only its style id (`include/tregion.h:212`) and
  the Style Editor edits the palette style, shared by every region using it
  (`toonzqt/styleeditor.cpp:2947-2977`).
- **Approach.** Step 1: a Gradient Transform tool that picks a region and
  edits its style's parameters with handles, with a palette undo (every
  region with the style follows, each in its own box). Step 2, per-region
  gradients: a "Make Unique Style" that clones the style into a new slot (no
  format change), rather than per-region data in the PLI.
- **Format.** None for step 1 and cloned styles; true per-region gradients
  would need a new PLI tag that older readers drop.
- **Risks.** Regions have no stable identity across recomputes; parameters
  are percentages of the bounding box, so they shift when the shape changes.
  There is no multi-stop gradient.

## 7. Edit Multiple Frames

Move, rotate or scale artwork on many frames at once.

- **Builds on.** This **largely exists for vector levels**: the Selection
  tool's Mode has Selected Frames and Whole Level
  (`tnztools/vectorselectiontool.cpp:1344-1352`), and `transformWholeLevel`
  (`:650-715`) applies move, rotate and scale to every frame's bounding box
  in one undo block.
- **Missing.** Free Deform on the other frames (only the current one
  changes, `selectiontool.cpp:525`); taking the frames from the onion skin range or
  the xsheet; several columns at once; raster levels.
- **Approach.** Fix Free Deform for all frames, then add an Onion Skin Range
  frame mode that feeds the same code. Raster levels and several columns are
  a later, larger step.
- **Format.** None.
- **Risks.** Each frame transforms about its own box, not one shared pivot;
  large undo blocks on long levels.

## 8. Distribute to Layers, Distribute to Keyframes, Convert to Symbol

Split selected vector strokes into new columns or successive frames, or wrap
them in a new symbol.

- **Builds on.** `TVectorImage::splitImage`
  (`common/tvectorimage/tvectorimage.cpp:1584`) copies strokes out with
  their palette; paste-into-a-new-level creates the level and cell with
  undo (`toonz/cellselection.cpp:498-640`); `SubsceneCmd::collapse`
  (`subscenecommand.cpp:2309-2366`) makes a symbol from columns, with undo.
- **Missing.** All three commands; splitting a selection by group or by
  stroke; creating a level with a given palette.
- **Approach.** One command file in the app (tnztools cannot reach the
  column and symbol commands): split the selection, create the levels with a
  clone of the source palette, insert the columns after the source with its
  parent and pegbar values, remove the strokes from the source, all in one
  undo block. Keyframes: the same split onto new frames of the same level.
  Convert to Symbol: distribute into one column, then collapse it.
- **Format.** None.
- **Risks.** A palette per new level breaks style sharing; fills that span
  parts are lost; the collapse prompt in the middle of the command.

## 9. Envelope modifier

Warp a vector selection with a grid of handles.

- **Builds on.** Free Deform (`VectorFreeDeformer`,
  `tnztools/vectorselectiontool.cpp:254`) and its undo;
  `TFreeFormDeformer` (`include/toonz/tfreeformdeformer.h`), a B-spline
  lattice with a direct `deform()`, already used by the Free-Form Deformer
  FX; `deformVectorImage` (`toonzlib/tdeformer.cpp:421`) maps control points
  and reshapes the strokes.
- **Missing.** A grid gadget in the Selection tool, and subdividing strokes
  before warping them.
- **Approach.** An Envelope mode of the Selection tool that holds a lattice
  and maps the selected strokes through `TFreeFormDeformer` on each drag,
  subdividing first, with the existing undo.
- **Format.** None.
- **Risks.** B-spline handles do not touch the art (it follows part of a
  handle's move), so the grid should be drawn as the deformed surface;
  sparse strokes need subdivision; the grid can fold over itself.

## 10. Rig Mapping

Copy an animation from one character's rig to another's by matching names.

- **Builds on.** Named stage objects (`tstageobject.h:260-264`) and their
  parent tree; uniquely named Plastic vertices, with deformations keyed by
  vertex name (`plasticskeletondeformation.h:250`); the pose library already
  addresses channels by object and vertex name; the child traversal of the
  Rig Ease commands (`rigeasecommand.cpp:89-158`).
- **Missing.** Any copy by name; plain key copy and paste is by position.
- **Approach.** A command with a match table the user can correct: match
  source and target objects (and Plastic vertices) by name, copy each
  channel's keys and cycle flag, optionally relative to the target's rest
  values, in one undo block.
- **Format.** None (it writes keys).
- **Risks.** Default names ("Peg3", "Vertex_003") give false matches;
  different bone lengths and rest poses; distances need scaling; expressions
  point at the source objects.

## 11. Brush modes and Fluid Brush

Paint Behind, Paint Fills, Paint Inside and Paint Selection for the vector
brush, and a brush whose width follows the drawing speed.

- **Builds on.** The vector brush's draw order: Over All, Under All (about
  Paint Behind) and Palette Order (`tnztools/toonzvectorbrushtool.cpp:591-622`);
  `ContourOutline::makeOutlineStroke` turns a stroke into a closed fill;
  `TRegion::contains` and stroke intersection for clipping; pressure becomes
  thickness in `computeThickness` (`:689`); the input points carry time and
  length (`include/tools/track.h:76-84`), so speed is available.
- **Missing.** Clipping a stroke to a region, a region selection, brush to
  fill inside the brush, speed-based width.
- **Approach.** A paint mode option: Fills converts the new stroke to an
  outline and fills it; Inside keeps the parts of the stroke inside the
  region under the pen-down point; Selection limits that to the selected
  strokes' regions. Fluid Brush: an input modifier that turns speed into
  pressure, smoothed.
- **Format.** None.
- **Risks.** Clipping works on the centreline while the visible edge is the
  thick outline; region lookups while drawing; noisy speed.

## 12. Texture Atlas export

Animate's game engine format: a packed atlas PNG with `spritemap1.json`
and `Animation.json`, read by Unity and other engine plugins.

- **Builds on.** The Spine export's drawing collector (deduplicated by
  level and frame), per-drawing trimmed rendering (`spineexport.cpp:225-327`)
  and per-frame placements; the sprite sheet writer
  (`image/sprite/tiio_sprite.cpp`) for layout and padding ideas.
- **Missing.** A rectangle packer (none in the tree), both JSON files, and
  per-frame trimming (the sprite sheet writer trims all frames to one box and
  writes a `.txt`, not JSON).
- **Approach.** A pure, unit-tested packer, an atlas exporter sharing the
  Spine export's collector (moved into toonzlib), and a File > Export
  command. First version: top-level columns, trimmed sprites on one page,
  baked per-frame matrices.
- **Format.** None.
- **Risks.** The format is reverse-engineered and has two key spellings
  that engine plugins read differently; pages over 4096 or 8192 pixels;
  premultiplied alpha; vectors rasterized at camera size.

## 13. Outline view per column

Show a column's drawings as coloured outlines in the viewer.

- **Builds on.** Column status bits (`include/toonz/txshcolumn.h:76-83`,
  `0x100` is free), saved with the status word and kept by older builds;
  the column commands and header icons of the light table exclusion
  (`columncommand.cpp:1486-1647`, `xshcolumnviewer.cpp:1001-1139`); the
  transparency check's inks-only drawing for vectors and Toonz raster
  (`stagevisitor.cpp:728-779, 1656, 1726`).
- **Missing.** A per-column outline mode (the checks are global), and an
  outline for full-colour rasters.
- **Approach.** An Outline bit with a header toggle, passed to the painters:
  vectors draw strokes only in the outline colour; Toonz raster draws inks
  only; full-colour raster gets an alpha-edge pass later.
- **Format.** A status bit.
- **Risks.** Full-colour rasters and the GPU cache key; viewer only.

## Recommended order

1. **Fix Free Deform** (the thickness bug and the other frames): S, and
   the base of 7 and 9.
2. **Quick wins on existing code:** Edit Multiple Frames (7), Outline view
   (13), Fluid Brush (11): S each.
3. **Everyday Animate tools:** Width tool (5), Motion Presets (1), Gradient
   Transform step 1 (6), Distribute and Convert to Symbol (8), Envelope (9):
   M each, one pull request each, no format change.
4. **Color Effect (2):** M-L, touches the viewer and the render; saved only
   when used.
5. **Rig Mapping (10)** and **Texture Atlas (12):** M each.
6. **The two big ones:** Shared Library (3) and Lottie export (4), several
   pull requests each.
7. **Paint Inside and Paint Selection (11):** L, after the rest.
