# groundzero vs Toon Boom Harmony Premium 27: parity plan

**What was audited:** `/home/user/gz-mirror` at branch `tier6` (HEAD `1b03738ffd`, "Add a Drawing Substitution panel"). I only read code and git history; nothing was built or run. Paths below are relative to `toonz/sources/`.

**Baseline:** the earlier Harmony catalogue (`harmony-catalogue.md`, Harmony 22 to 27). I checked every feature added since that catalogue against the code; section 1 gives the details. The Colour Override FX was added to `tier6` after this audit (`stdfx/colouroverridefx.cpp`).

**Status codes:** Y = has it, Partial = some of it, N = no. **Tiers:** E/A/P = Essentials/Advanced/Premium.

---

## 1. Current parity table

### 1.1 Rigging and deformation

| Feature | Tier | Status | What exists (file refs) | Gap |
|---|---|---|---|---|
| Rigs built in a node view | P | Partial | Peg hierarchy in the Stage Schematic (`toonzqt/stageschematicscene.cpp`). Compositing in the FX Schematic. A Plastic deformation hangs off a stage object (`TStageObject::getPlasticSkeletonDeformation`, `include/toonz/tstageobject.h:419`) and reaches the render through an internal `PlasticDeformerFx` (`include/toonz/plasticdeformerfx.h`). | Deformers are not nodes. There is no single graph with pegs, deformers, drawings and composites, and no port filtering. |
| Pegs, with X/Y/Z and 3D path | E/A/P | Y | Pegbars, the Animate tool, spline paths (`T_Path`). Two new tilt channels, `T_RotX`/`T_RotY` (`tstageobject.h:134`), which scripts can also reach. | 3D rotation is only card-style tilt. There is no quaternion 3D path. |
| Bone deformer | P | Partial | Plastic mesh and skeleton with rigidity painting (`tnzext/plasticdeformer.cpp`, `plasticdeformerstorage.cpp`). The Plastic tool now has IK (`tnztools/plastictool_animate.cpp:156`, which uses `IKEngine`). | The mesh has to be built first. There is no per-bone region of influence and no bone that works without a mesh. |
| Game Bone | P | Partial | Spine JSON export maps the peg and column hierarchy, the "H0:" vertex links and drawing swaps to bones, slots and attachments (`toonz/spineexport.cpp`). | Mesh and Plastic deformation are not exported (`spineexport.cpp:24`). There is no rigid game-bone deformer type. |
| Curve deformer | P | Partial | Plastic **Curve Bending**: the skeleton's vertex chains bend along a Catmull-Rom spline (`tnzext/plasticdeformerstorage.cpp:202` `buildCurveHandles`, `plasticskeletondeformation.h:283`). | No Bézier tangent handles of its own, no animatable handle length, and no curve that works without a mesh. |
| Envelope deformer | P | N | Nothing. | No closed outline chain. |
| Kinematic Output | P | Y | A column's parent handle can be `H0:<vertex>`, so the column follows a Plastic vertex (`toonzlib/xshhandlemanager.cpp:164`, `tstageobject.cpp:1609`). Renaming the vertex keeps the link. | Only Plastic vertices can drive it, because no other deformer types exist. |
| Free-Form deformer | P | Partial | Plastic mesh vertices. Pinch, Bender and Magnet edit the drawing destructively. | No cage or lattice of free control points that can be animated without a skeleton. |
| Weighted multi-deformer | P | Partial | One Plastic deformation per object, with rigidity weights. | Several deformers cannot be stacked on one drawing with per-point blend weights. |
| Shape-Aware deformer (H24) | P | N | Nothing. | Points, bones and cage together. A proprietary algorithm; see 3.4. |
| Auto-Patch | P | N | Nothing. | No masking of line art at the overlap of joints. |
| Master Controller and wizards | P | Partial | **Poses panel**: additive poses, each with a weight from -100% to 100% (`toonzlib/poselibrary.cpp`, `toonz/posespane.cpp`). It covers stage channels and Plastic vertex angle and distance. | No widgets in the viewer (slider, 2D pad, grid). Poses can't drive drawing swaps. Applying a pose writes keys; it is not a live control. |
| Attribute Controller (H27) | P | Partial | The pose library is already offset-based and stackable. | Poses are baked into keys rather than kept as separate offset layers. No Setup Mode, and no training from existing keys. |
| IK tool | A/P | Y | The Skeleton tool's IK (`tnztools/skeletontool.cpp:60`), and now Plastic IK. | Plastic IK keys angles only. No per-vertex min/max limits in the IK drag UI, beyond the existing angle bounds. |
| Drawing substitution and templates | E/A/P | Partial→Y | **Drawing Substitution panel** with an icon grid (`toonz/drawingsubstitutionpane.cpp`). **Save Selection as Template** (`toonz/subscenecommand.cpp`, `columnselection.cpp`). | No substitution slider in the viewer. Templates have no thumbnail browser. |
| Symbols | E/A/P | Y | Symbols built on `TXshChildLevel`: a Symbols panel, insert by drag, and Once/Loop/Hold timing (`toonz/symbolcommand.h`, `symbolspane.cpp`). | Symbols live only in their own scene; there is no library shared between scenes. |
| Cut-out character library | E/A/P | Partial | Library browser, rig templates, Poses. | No visual library of rigs and poses that you can drag from. |
| Breakdown Pose Assistant (H25) | P | Y (narrow) | Breakdown keys with favour presets, including Anticipation -10% and Overshoot 110%, plus a custom value (`toonzqt/functionsheet.cpp:72`, `functionselection.cpp:713`). | Only in the Function Spreadsheet. No whole-rig breakdown from the Timeline, Xsheet or viewer. |
| Selection Lock (H27) | ? | Y | `TXshColumn::isSelectionLocked`, the command `MI_SelectionLockSelectedColumns` (`toonz/columncommand.cpp:1632`), and the viewer respects it. | Works per column, not per node. |
| Nested groups | P | Partial | Nestable column groups in the Xsheet, built on stage-object groups (`toonz/columncommand.cpp`, `xshcolumnviewer.cpp`, an opt-in strip). | No group node with published ports. |

### 1.2 Drawing

| Feature | Tier | Status | What exists | Gap |
|---|---|---|---|---|
| Line Art / Colour Art | E/A/P | Partial | Toonz Raster keeps ink and paint separate natively. On vector levels, strokes of zero thickness act as Colour Art, with "Art Layer:" and "Show Art:" tool options (`include/tools/artlayer.h`, `tnztools/artlayer.cpp`). The file format is unchanged. | No Overlay or Underlay layers, no Art Layer Manager (H25.2), and no "Create Colour Art from Line Art". |
| Pencil lines: texture, width profile, pressure opacity, Retouch | A/P | Partial | Pressure thickness on vectors. The **Pump** tool gains Thicken and Thin Brush modes (`tnztools/pumptool.cpp:33`). Textured vector styles. | No opacity along a stroke, no Retouch for opacity, and no animated pencil texture. |
| Contour Editor | E/A/P | N | OpenToonz vectors are centreline strokes. | Needs an outline (brush-shape) stroke model. |
| Centerline Editor | E/A/P | Y | Control Point Editor. | n/a |
| Auto gap closing | E/A/P | Y | Autoclose (`filltool.cpp`, `rastertapetool.cpp`). | n/a |
| Paint Unpainted / paint behind | E/A/P | Y | Fill tool's Selective mode; Toonz Raster paints behind the ink. | n/a |
| Brush engines and Photoshop brushes | A/P | Partial | MyPaint and full-colour brushes. | No import of ABR (Photoshop brush) files. |
| Stabilizer | E/A/P | Y | `modifiersmooth.cpp` | n/a |
| Perspective and guides | A/P | Y | Assistants (`assistantperspective.cpp`, and others). | n/a |
| Symmetry | ? | Y (ahead) | Replicators. | n/a |
| Morphing | A/P | Partial | Vector Inbetween now accepts **stroke-pairing hints** (`common/tvrender/tinbetween.cpp`, `tnztools/strokeselection.cpp:694`). The old `morphtool.cpp` was removed. | No automatic matching of strokes, no zone or vanishing-point hints, and no velocity curve. |
| Shift & Trace | A/P | Y | Existing tool. | n/a |
| Onion skin | E/A/P | Y | Existing. | n/a |
| Light Table | E/A/P | Y | View > Light Table: every column except the current one at 30% (`toonzlib/stage.cpp`, `toonz/sceneviewer.cpp`). | Fixed at 30%; there is no per-column setting. |
| Reference view and Mirror (H22/H27) | E/A/P | Y | **Reference panel** with pan, zoom, horizontal and vertical flip, and 90° rotation (`toonz/referencepanel.cpp`). | Shows one image only. 3D models can't be used as references. |

### 1.3 Animation and timing

| Feature | Tier | Status | What exists | Gap |
|---|---|---|---|---|
| Timeline and Xsheet | E/A/P | Y | Existing. | n/a |
| Function curves | E/A/P | Y | Function Editor. | No quaternion rotation. |
| Ease presets | E/A/P | Y | Six presets plus a Custom Ease applied to the selection (`toonzqt/functionsheet.cpp:57-125`). | Not available as Quick Ease buttons in the Timeline. |
| Interpolation types | E/A/P | Y | Existing. | n/a |
| Motion paths in the viewer | E/A/P | Y | Animate tool's "Show Trajectory", with draggable markers for position keys (`tnztools/edittool.cpp:66-84`). | No velocity display, and no editing of the path's tangents in the viewer. |
| Trajectory onion skin | ? | Y | Same feature. | n/a |
| Sub-timing and Retime / Hold / Loop nodes | A/P | Partial | Xsheet reframe and step tools; symbol timing modes. | No time-remap curve per column. |
| Auto lip-sync | A/P | Y | Rhubarb (`toonz/autolipsyncpopup.cpp`). | n/a |
| Sound scrubbing and level meter | E/A/P | Y | Scrubbing, plus a `SoundLevelMeter` (`toonzqt/flipconsole.cpp:422`) and an animatable **Volume** curve (`toonzlib/txshsoundcolumn.cpp:159-187`). | n/a |

### 1.4 Compositing, FX and 3D

| Feature | Tier | Status | What exists | Gap |
|---|---|---|---|---|
| Node compositing | P | Y | FX Schematic with multiple outputs. | No port filtering. |
| Tone / Highlight | A/P | Y | `ToneHighlightFx` (`stdfx/tonehighlightfx.cpp:91`): matte input, softness, and a correct affine for soft edges. | No preset that drives it with a one-click line-art matte. |
| Glow, Blur, Bokeh | A/P | Y | Existing Iwa effects. | n/a |
| Cutter and Matte | A/P | Y | Matte In and Matte Out. | n/a |
| Colour Override | P | Partial | `stdfx/colouroverridefx.cpp`, `ColourOverrideFxRenderData` applied in `tcolumnfx.cpp`: up to 8 style → colour pairs on Toonz Vector / Toonz Raster columns, Preview/Render only. | Only main colours (no textures), no viewer display, no palette-swatch picker. |
| Light Shading, Volume, Glue, Shine | P | Partial | Iwa lighting and gradient effects. | No shading driven by a light inside a shape. |
| Particles | P | Y | Particles Fx. | No baking. |
| Multiplane, Z and depth of field | E/A/P | Y | Z, stacking order (SO), camera Z, Bokeh. | n/a |
| 3D model import | P | Partial | **GLB Model FX** with a CPU rasterizer (`glb/glbloader.cpp`, `glb/glbrenderer.cpp`, `stdfx/glbmodelfx.cpp`), plus **Transform3D** and **ThreePointLight** FX. | GLB only: no FBX, OBJ or USD. No PBR, no shadows, no GPU. Models are FX sources, not scene objects. |
| Full 3D camera | P | Partial | Camera Z plus tilt channels on stage objects. | No real 3D camera or scene graph. |
| OpenFX plugins | P | N | Its own plugin SDK (`toonzqt/pluginhost.cpp`) and GLSL Shader Fx. | No OFX host. |
| GPU real-time preview | A/P | Partial | **Experimental** GPU compositing of raster nodes in the viewer (`toonzlib/stagevisitor.cpp`, `textureutils.cpp`). Frame-strip parallel rendering (`common/tfx/trenderer.cpp`). | Vectors, meshes and FX are still drawn on the CPU or with fixed-function GL. |
| 32-bit float pipeline | A/P | Y | OpenToonz's float and linear rendering, and EXR output. | No DWAA compression. |
| Cryptomatte EXR (H24) | P | N | `OpenExrAovFx` reads EXR layers (`stdfx/openexraovfx.cpp`). | Nothing writes ID mattes. |

### 1.5 Pipeline and IO

| Feature | Tier | Status | What exists | Gap |
|---|---|---|---|---|
| JavaScript scripting | A/P | Y | **QJSEngine** replaced QtScript (no QtScript references are left). The StageObject API gets, sets and keys values, and lists keyframes (`include/toonz/scriptbinding_stageobject.h:62-76`), tilt included. | No API for nodes or FX, and no toolbar scripts. |
| Python | P | Y (opt-in) | The `opentoonz` module and a console panel (`toonzlib/pythonbinding.cpp`, `toonz/pythonconsolepanel.cpp`). Built only with the CMake option `WITH_PYTHON`. | Same coverage as the JS API. Not in default builds. |
| Harmony Server / database | separate product | Partial | SVN version control and the toonzfarm render farm. | Out of scope (see 3.4). |
| Game export | P | Partial | Spine JSON export. | No mesh deformation export; no Unity or Godot path. |
| Export formats | E/A/P | Y | Through an ffmpeg **pipe** (`image/ffmpeg/tiio_ffmpeg.cpp`), EXR, the **PSD writer** (one flattened PSD per frame, `image/psd/tiio_psd.cpp`), OCA, XDTS, xsheet PDF, animated SVG. | No layered PSD of a scene, no USD, no SWF. |
| Storyboard Pro integration | E/A/P | Partial | XDTS and OCA. | Conforming from Storyboard Pro is proprietary. |
| Compact file format | all | N | XML `.tnz`. | No binary or compressed scene, and no fast load. |
| AI (Ember) | add-on | N | Nothing. | Out of scope. |

### 1.6 UI and workflow

| Feature | Status | What exists | Gap |
|---|---|---|---|
| Autosave and backup | Y | Recovery folder plus crash recovery on next launch (`toonz/autosaverecovery.cpp`). | n/a |
| Annotation column | Y | "New Annotation Column" makes a Note Level column (`toonz/xsheetcmd.cpp:1128`). | No drawing directly in the annotation cells. |
| Lock Timeline Ruler (H25) | N | Nothing. | Small. |
| Port filtering in the node view (H24) | N | Nothing. | Small once 2.4 exists. |
| Performance anti-aliasing (H25) | N | Nothing. | Needs the GL renderer (item F2). |
| Rooms and workspaces | Y | Existing. | n/a |

**Overall count** over the 73 rows above: about 38 Y, 23 Partial and 12 N. The N count includes 3 new UI rows the catalogue did not rate. At the time of the catalogue it was about 22 Y, 28 Partial and 25 N. Nearly every remaining Partial is in rigging, GPU and 3D, and each of those depends on one of the foundations in section 3.

---

## 2. Building blocks audit

### 2.1 Subsystems to build on

| Missing or partial feature | Build on | Notes |
|---|---|---|
| Envelope, Bézier curve and free-form cage deformers; weighted stacking | `PlasticDeformerStorage` (cache keyed by object and mesh), `PlasticDeformer` (ARAP-style solve with `PlasticHandle`s), `PlasticSkeletonDeformation` (animated parameters, undo), `PlasticDeformerFx` (render path), `meshbuilder.cpp` | Curve Bending already shows how to turn a higher-level control into sampled `PlasticHandle`s (`buildCurveHandles`). An envelope or curve deformer can compile to handles the same way. |
| Node rig view | `SchematicScene`/`SchematicNode` (`toonzqt/schematicnode.cpp`), the Stage Schematic (pegs and handles), the FX Schematic (ports, groups, macros) | The Stage Schematic already draws parent-handle links, and "H0:" vertex handles appear there (`stageschematicnode.cpp`). |
| Master and Attribute Controllers | `TStageObject` channels (`TDoubleParam`), `txsheetexpr.cpp` expressions, `PoseLibrary`, `KeyframeSetter`, the `XshHandleManager` handle-affine mechanism | Use expressions for "drive B from A", and the pose library for offsets. Widgets can use the edit-tool gadgets (`tnztools/edittoolgadgets.cpp`, which already draws through `tgl::Batch`). |
| Drawing-swap controllers | Drawing Substitution pane, `TXshCell` replacement, the Symbols timing modes | A controller channel can map a value to a frame ID through a lookup table. |
| Symbol library shared between scenes | `TXshChildLevel`, rig templates (`subscenecommand.cpp` Save Selection as Template), the Library browser | A template is effectively a symbol saved to disk. |
| Auto-Patch | `tglregions.cpp` and `stagevisitor.cpp` (column draw order), Art Layer roles (`artlayer.h`), `PlasticDeformerFx` | Draw the Colour Art of the patch column over the parent's Line Art inside the joint mask. |
| Retime / Hold / Loop | `TXshColumn`, the `TXshSoundColumn` volume curve (a template for a per-column `TDoubleParam`), the Symbols timing code | Add a time-remap curve to level columns. |
| Pencil opacity and Retouch | `TThickPoint` (thickness) and the Pump Thicken/Thin brush (`pumptool.cpp`) | Opacity needs a per-point attribute, which changes the file format. |
| Colour Override | `TPaletteHandle`, External Palette Fx, Palette Filter Fx, `TStandardRasterFx` (pattern of `tonehighlightfx.cpp`) | Colour Override on a raster must see style IDs, so it has to sit before flattening (a palette swap at render time, as External Palette does). |
| GPU preview | `Stage::RasterPainter` with the GPU compositing path (`flushRasterImagesGpu`), `textureutils.cpp`, `tgl::Batch` (`include/tglbatch.h`), Shader Fx (`stdfx/shaderfx.cpp`), `ttexturesstorage.cpp`, `meshtexturizer.cpp` | Plastic meshes are already drawn as textured triangles, so they port to shaders naturally. |
| 3D import and camera | `glb/` loader and renderer, `T3DRenderSource` (`stdfx/t3dsource.h`), Transform3D and ThreePointLight FX, the `T_RotX`/`T_RotY` channels | These already form a 3D sub-pipeline in the FX graph. Add more loaders behind the same interface (for example assimp). |
| Cryptomatte | EXR writer (`image/exr`), `OpenExrAovFx` (reader), column IDs in `stagevisitor` | Needs a multi-pass render output (an ID pass for each column). |
| OpenFX host | `TFx`/`TRasterFx`, `pluginhost.cpp` (which already wraps external plugins and their parameters), `TParamContainer` | Natron's OFX host (GPL) is a reference implementation, and the OFX headers are BSD-licensed. |
| Script depth | `scriptbinding_*.cpp`, `pythonbinding.cpp`, which share wrappers | Add Fx, Column, Xsheet-cell and Plastic bindings to both. |

### 2.2 Hard blockers

1. **Fixed-function OpenGL.** `glBegin` appears in 58 files (230 calls), `glPushMatrix` in 51, `glMatrixMode` in 16, and display lists (`glNewList`/`glCallList`) in 5. `QGLWidget` is still used in 5 files. `tgl::Batch` replaces only about 41 call sites, in `edittoolgadgets.cpp` and `toolutils.cpp`. This blocks GPU deformation and FX preview, a core profile (needed for macOS beyond GL 2.1, and for Qt 6 RHI interop) and the Performance anti-aliasing mode.
2. **Qt 5.15.** CMake hard-requires Qt5 (`CMakeLists.txt:292`). The `otdev/qt6-build-abstraction` branch exists but is **not merged** and has drifted (213 files differ from `tier6`). QtScript is already gone, which was the biggest Qt 6 blocker. What remains is `QGLWidget` (5 files), `QRegExp` (7), `QTextCodec` (5) and `QDesktopWidget` (3).
3. **No generic deformer abstraction.** Every deformer is Plastic: one `PlasticSkeletonDeformation` per stage object, with one mesh per level. Stacking, weights and non-skeleton deformers all need a new layer above it.
4. **No rig graph.** Deformers are attributes of a stage object, not nodes. The Stage and FX schematics are separate scenes with separate data models (`TStageObjectTree` and `FxDag`).
5. **No 3D scene graph.** Stage objects make 2D affines plus Z and tilt. The 3D in the GLB FX is confined to one FX subgraph rendered by a CPU rasterizer.
6. **File-format compatibility with upstream OpenToonz.** The project deliberately keeps scenes readable upstream (see the pose-library `<poses>` fix and the Art Layer design that needs no format change). Every per-point attribute, deformer node or offset layer either breaks that or needs a sidecar or optional tags.
7. **Volunteer bandwidth and test coverage.** Rendering and tools have only targeted tests (`stdfx/tests`, `tnztools/tests`), and CI builds only on Linux. Large refactors (GL, Qt 6) risk Windows and macOS regressions that nobody sees.

---

## 3. Phased plan to full parity

**Sizes:** S ≤ 1 week, M 2–4 weeks, L 1–3 months, XL > 3 months (one experienced contributor).

**Realism:** **V** = realistic for a volunteer contributor; **MM** = a multi-month effort that needs a dedicated owner or funding; **X** = not realistic or not worth it.

### Phase 0: Foundations

| # | Item | Size | Depends on | Approach | Risk | Realism |
|---|---|---|---|---|---|---|
| F1 | Qt 6 port | L | none | Rebase `otdev/qt6-build-abstraction` onto `tier6` and keep a dual Qt5/Qt6 build behind `OT_QT_MAJOR_VERSION`. Replace `QRegExp` with `QRegularExpression`, `QTextCodec` with `QStringConverter`, `QDesktopWidget` with `QScreen`, and `QGLWidget` with `QOpenGLWidget`. Add CI jobs for both Qt versions. | Medium: tablet input, high-DPI and docking regressions. | MM |
| F2 | (Dropped with Phase 4.) Only the parts Qt 6 needs (`QGLWidget` → `QOpenGLWidget`, context handling) are kept, as part of F1. | | | | | |
| F3 | Generic deformer framework | XL | none (F2 for GPU) | Add a `TDeformer` interface that returns handle constraints or displacements on a `TTextureMesh`, and a per-column ordered `DeformerStack` that stores the blend weights per vertex. Port Plastic skeleton and Curve Bending first, as the reference implementations. Generalise `PlasticDeformerFx` into a `DeformerStackFx`. Keep Plastic serialisation unchanged and store new stacks in optional tags. | High: performance and caching (`PlasticDeformerStorage` keys); keeping scenes open in upstream. | MM |
| F4 | Rig node view | L | F3 | Add a "Rig" schematic mode on the `SchematicScene` base, with stage objects, deformers and column nodes in one graph, plus port filtering. Initially it edits the existing data (parent, handle and deformer stack), so no new data model is needed. | Medium: two schematic views with overlapping editing. | MM |
| F5 | Offset layers (Attribute Controller runtime) | L | none | Add an optional additive offset channel to each `TStageObject` channel, evaluated as the base value plus the sum of weight × offset. The pose library writes these layers instead of baking keys. This is the core of the H27 model. | Medium: expressions and IK that read the "value" need defined semantics. | V/MM |

### Phase 1: Quick wins (volunteer-sized, high value)

| # | Item | Size | Depends on | Approach | Risk | Realism |
|---|---|---|---|---|---|---|
| Q1 | Colour Override FX | M | none | A raster FX with a `TPaletteHandle`-like list of style ID → colour pairs, applied while a Toonz level renders (the External Palette Fx route), so remaps follow paint zones. Show palette swatches in the parameter editor. | Low to medium: needs the style IDs before flattening. | V |
| Q2 | Whole-rig breakdown and Quick Ease in the Timeline and Xsheet | S | none | Reuse `FunctionSelection::insertBreakdowns(frame, favour)` over every animated channel of the selected columns. Add an Xsheet/Timeline context menu and toolbar buttons. | Low | V |
| Q3 | Create Colour Art from Line Art | M | none | Copy closed Line Art stroke loops as zero-thickness strokes (the Colour Art role in `artlayer.h`), then rebuild regions with autoclose. | Low | V |
| Q4 | Drawing-substitution slider and controller | S | none | A slider in the viewer or Tool Options that steps through the Drawing Substitution pane's frame list for the current cell. | Low | V |
| Q5 | Lock Timeline Ruler; port filter in the FX Schematic | S | none | UI-only changes. | Low | V |
| Q6 | Layered PSD scene export | M | none | One PSD per frame with one layer for each top-level column, written with the existing `TLevelWriterPsd` layer code. | Low | V |
| Q7 | Light Table opacity and per-column exclusion | S | none | Make the 30% value a preference, and let a column be excluded from the Light Table. | Low | V |
| Q8 | Tone/Highlight preset macro | S | none | A macro FX that uses the column's Line Art matte as the matte input. | Low | V |
| Q9 | Retime/Hold/Loop as a time-remap curve | M | none | A per-level-column `TDoubleParam` time map, modelled on the `TXshSoundColumn` volume curve and evaluated in `getCell()`. | Medium: every `getCell` caller | V |
| Q10 | Script API depth | M | none | Fx, column, cell and Plastic bindings in both `scriptbinding_*` and `pythonbinding.cpp`, with toolbar script buttons. | Low | V |

### Phase 2: Rigging parity

| # | Item | Size | Depends on | Approach | Risk | Realism |
|---|---|---|---|---|---|---|
| R1 | Bézier Curve deformer | M | F3 | Bézier chains with animated tangent handles. Sample them into `PlasticHandle`s, as `buildCurveHandles` does, and auto-mesh the drawing. | Medium | V |
| R2 | Envelope deformer | L | F3 | A closed Bézier envelope whose rest and posed shapes drive boundary handles (mean-value or harmonic coordinates on the mesh). | Medium to high: folding artifacts | MM |
| R3 | Free-Form and cage deformer | M | F3 | An animatable set of points and a cage with no skeleton, compiled to handles. | Medium | V |
| R4 | Weighted multi-deformer blending | L | F3 | Per-vertex weight maps painted like rigidity (`plastictool_rigidity.cpp`), blended in `DeformerStack`. | High: UX and performance | MM |
| R5 | Bones without a mesh step and regions of influence | M | F3 | Auto-build the mesh on first deform and compute a region of influence per bone. | Medium | V |
| R6 | Master Controller widgets | L | F5 | Viewer gadgets (slider, 2D pad, grid) bound to a controller channel. The channel drives offset layers by interpolating between poses (bilinear for the pad) and drawing swaps through a lookup table. Store them in the scene, without JS wizards. | Medium | MM |
| R7 | Attribute Controller Setup Mode and training | M | F5, R6 | Setup Mode records which channels a controller owns. Train it by least-squares fitting of offset weights to existing keys. | Medium | V |
| R8 | Auto-Patch | M | Q3 | A patch column type that draws the child's Colour Art over the parent's Line Art, clipped to a joint mask, in `stagevisitor`. | Medium: draw order with GPU compositing | V |
| R9 | Rig library with thumbnails and drag and drop | M | none | A panel over rig templates and poses, with `IconGenerator` thumbnails, that can be shared between scenes. Symbols shared between scenes are a special case. | Low | V |
| R10 | Game export of meshes | M | F3 | Write Spine mesh attachments and deform timelines from the Plastic meshes (the gap noted at `spineexport.cpp:24`). | Medium | V |
| R11 | Shape-Aware-like deformer | XL | F3, R2, R4 | A research item: combine bone, point and cage handles in one bounded-biharmonic or ARAP solve. Do not aim to copy Harmony's behaviour. | High | X/MM |

### Phase 3: Drawing parity

| # | Item | Size | Depends on | Approach | Risk | Realism |
|---|---|---|---|---|---|---|
| D1 | Opacity per point and Pencil Retouch | L | none | Add an optional opacity value to each control point of a `TStroke`, written to the PLI file only when present. Draw it in the stroke styles, and make a Retouch tool by extending Pump's Thicken/Thin brush. | High: the PLI format, and readability by upstream | MM |
| D2 | Overlay/Underlay art layers and Art Layer Manager | L | Q3 | Store the four layers as stroke groups with a role tag, which is a format change. The manager panel moves, swaps and merges them. | High: format | MM |
| D3 | Smarter morphing | L | none | Match strokes automatically (by shape descriptor and position) before the user's hints, and add vanishing-point hints and a velocity curve on top of `TInbetween`. | Medium | V/MM |
| D4 | Importing ABR (Photoshop) brushes | M | none | Read the sampled-tip ABR format into MyPaint or full-colour brush tips (Krita and GIMP readers are references). Dynamic ABR presets are proprietary and stay partial. | Medium | V |
| D5 | Contour Editor | XL | none | Needs an outline vector representation next to centreline strokes. Out of scope, unless vector levels gain a "shape" stroke type. | High | X |
| D6 | Animated pencil texture | M | D1 | A stroke-style parameter that offsets the texture per frame. | Low | V |

### Phase 4: Compositing, GPU and 3D: out of scope

Dropped by decision (2026-10-25): no GPU deformation or FX preview, OpenFX
host, Cryptomatte or multi-pass EXR output, extra 3D formats, 3D scene
objects, true 3D camera, USD or Blender bridge. The existing CPU paths (GLB
model FX, Transform3D, the experimental GPU viewer compositing) stay as
they are.

### Phase 5: Pipeline

| # | Item | Size | Depends on | Approach | Risk | Realism |
|---|---|---|---|---|---|---|
| P1 | Faster, compact scenes | M | none | Optional zstd compression of the `.tnz` file and lazy loading of levels. Do not invent a new binary format. | Medium: upstream compatibility | V |
| P2 | Python in default builds | S | none | Ship `WITH_PYTHON=ON` builds where an embedded Python is available. | Low | V |
| P3 | Asset server, check-out and WebCC | XL | none | Not a goal. Document git-LFS and SVN workflows and the toonzfarm render farm. | n/a | X |
| P4 | Storyboard Pro conform | n/a | none | Proprietary. Keep to OCA, XDTS and OTIO. | n/a | X |
| P5 | Ember AI | n/a | none | Proprietary and cloud-based. A plugin hook for local models is possible at most. | n/a | X |

### 3.4 Where full parity is not realistic

- **Shape-Aware deformer, Ember AI, Harmony Server/WebCC, Storyboard Pro conform and the Harmony Game SDK** are proprietary or separate products. A behavioural equivalent can be built for some of them (R11, Spine and Godot export), but not for the AI tools or the server.
- **A full 3D scene graph and USD (G7, G8)** would effectively rewrite the stage model. A volunteer project should stop at G5 and G6, with 3D models as FX or stage children and tilt-based cards.
- **The Contour Editor (D5)** conflicts with OpenToonz's centreline vector model.
- **Byte-for-byte compatibility with upstream OpenToonz** cannot hold once D1, D2 or F3 land. The project needs a policy decision: optional tags that upstream ignores, or a declared groundzero format version.

---

## 4. First 10 things to build next

1. **Colour Override FX (Q1): done in `tier6`.** Next for it: a palette-swatch picker in the FX settings and display in the viewer.
2. **Rebase and merge the Qt 6 build abstraction (F1)**, dual-build, before the branch drifts further. QtScript is already gone.
3. **Whole-rig breakdown and Quick Ease buttons in the Timeline and Xsheet (Q2).** Small, and it reuses `insertBreakdowns`.
4. **Offset-layer channels (F5).** They turn the Poses panel into a live, stackable Attribute Controller and unblock R6 and R7.
5. **Master Controller widgets (R6)** in the viewer, built on F5 and the `tgl::Batch` gadgets.
6. **The `DeformerStack` interface (F3)**, with Plastic and Curve Bending ported as the first two implementations.
7. **The Bézier Curve deformer (R1)** as the first new deformer, which validates F3.
8. **Create Colour Art from Line Art (Q3) and Auto-Patch (R8)**, which make cut-out joints clean.
9. **Make GPU viewer compositing the default:** extend the path to vector levels and port `stagevisitor.cpp` and `sceneviewer.cpp` to `tgl::Batch` (the first slice of F2).
10. **A rig and pose library panel with thumbnails (R9)**, plus Spine mesh export (R10).
