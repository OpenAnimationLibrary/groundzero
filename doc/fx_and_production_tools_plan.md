# FX and production tools plan

An audit of ten requested features against what the tree already has, and a
plan to build them. Paths are under `toonz/sources` unless they start with
`doc/` or `stuff/`.

The ten:

1. Cel-shading edge detector
2. Boil and line-wobble FX
3. Halftone and print-process FX
4. Procedural rain, snow and particle layer
5. Matte-driven displacement for cloth and hair
6. Colour-script and palette-shift tool
7. Multiplane depth of field and atmospheric perspective
8. Light wrap and compositing helpers
9. Onion-skin and arc visualizer (viewer overlay)
10. Scene statistics and continuity checker

## Scope rules

- **One PR per tier**, one commit per item, merged on a green Linux Build
  and a green full test run on a `ci-tests/**` branch, as for the earlier
  tiers.
- **New behaviour is opt-in.** Scenes that do not use a feature save exactly
  as before. A new scene tag or channel is written only when used, and is
  named in the PR so the format change is a decision, not a side effect.
- **FX follow the cel conventions** (section "How a new FX is added"): pure
  math in a header without toonz types, a `_test` and a `_ui_test`, the
  source grid from `stdfx/celfxsourcegrid.h`, float support.
- **Nothing GPU.** Everything here is CPU raster or viewer GL overlay, as
  the master plan's scope rule says.

Sizes: S is a few days, M is one to two weeks, L is several weeks.

---

## What exists today, in one table

| # | Feature | Already in the tree | Verdict |
|---|---|---|---|
| 1 | Edge detector | Flat-colour matching with tolerance (`stdfx/celcolorkeymath.h`, `CelKey::Matcher`), distance fields with thin and feather (`stdfx/celdistance.h`), run scanning between differing colours (`stdfx/celsmoothing.h`), border tracing (`common/trop/trop_borders.cpp`). Outline FX exist only for Toonz levels (`calligraphicfx.cpp`, `artcontourfx.cpp`). | New FX. All the pieces exist; nothing joins them. |
| 2 | Boil | Warps: `warpfx`, `randomwavefx`, `ripplefx`, `ino_warp_hv`, `iwa_gradientwarpfx`, `perlinnoisefx`. Noise: `stdfx/celnoise.h` (stateless, seed + plane offset), `iwa_simplexnoise.h`. Frame-seeded noise in `noisefx.cpp:91`, `iwa_flowpaintbrushfx.cpp:524`. | New FX. No hold-N-frames stepping anywhere. |
| 3 | Halftone | `mosaicfx.cpp` (cell average, square or circle dot, scale-only affine), `posterizefx`, `channelmixerfx`, grain from `noisefx`/`iwa_fractalnoisefx`, blur `TRop::blur`, per-plate offsets `TRop::over(out, up, TAffine)`. | New FX. No CMYK, screen angles, misregistration or paper anywhere. |
| 4 | Particles | `particlesfx` and `iwa_particlesfx` (zerary; gravity, wind, swing, control images, frame-roll cache in `particlesmanager`), presets `stuff/fxs/presets/STD_particlesFx/Rain.fx` and `Falling snow.fx`, depth of field in `iwa_bokehfx` / `iwa_bokehreffx`. | Extend. Matte collision, depth layers with parallax, and turbulence are the gaps. |
| 5 | Displacement | `ino_warp_hv` (two grey maps, channel choice), `iwa_gradientwarpfx`, the deformer family on `toonz/tdeformer.h` (`warpRaster`, `deformerfxcommon.h`), the RG flow-image convention of `iwa_motionflowfx` / `iwa_flowblurfx`. | New FX, small: a `TDeformer` driven by a flow image or a grey map, with matte weight and animated noise. |
| 6 | Colour script | Per-column Color Effect (diagonal, not style-aware), Colour Override FX (style → colour render data applied on a palette clone at render time, `stdfx/colouroverridefx.cpp`), `Lut3D` and `Lut3DBakeFx` (`.cube`, `.3dl`), Palette Gizmo (destructive H/S/L shifters, `toonz/pltgizmopopup.cpp`), `TSceneProperties::m_colorFilters`, FX presets, animated palette styles. | New FX on the Colour Override route, plus the gizmo math moved to toonzlib. |
| 7 | Depth of field | Stage Z and camera Z: `TStageObject::perspective()` gives the distance from the camera as `1000 + cameraZ - objectZ`. The multiplane is the z-sorted Over cascade in `toonzlib/scenefx.cpp` (`FxBuilder::makePF(TXsheetFx*)`). Per-column hidden FX precedent: the Color Effect inserted in `makePF(TLevelColumnFx*)`. Bokeh FX exist but take hand-typed distances. No focus distance on the camera. | New: a camera focus channel plus hidden per-column blur and haze FX inserted from the column's distance. |
| 8 | Light wrap | `backlitfx` (halo outside), `glowfx`, `premultiplyfx`, `unmultiplyfx` (8-bit, not listed), `erodilatefx` (8/16-bit), `TRop::expandColor`, `celcolorexpandfx` (edge extend by distance field), `CelDistance::thin`/`feather`. | Four small FX, mostly wrappers over what exists. |
| 9 | Arcs and ghosts | Animate tool "Show Trajectory" (`tnztools/edittool.cpp:1403-1483`: centre point only, keyframed range only, drawn only while the tool is active), onion skin mask and fade (`toonzlib/onionskinmask.cpp`, paper-thickness fade), hooks per drawing (`toonz/hook.h`, `TStageObjectTree::getHandlePos`), overlay templates `toonz/pegbarguides.*` and `toonz/aspectguides.*`, world-space spline drawing `SceneViewer::drawSpline`. | New overlay built from the trajectory code; a fade control; no spacing chart exists. |
| 10 | Scene checker | Red cells for missing drawings (`toonz/xshcellviewer.cpp:2115`), Cast "missing" status, Remove Unused Levels (`getUsedLevels`), Erase Unused Styles (`isStyleUsed`), the `SceneResources` visitor, DPI and frame-rate accessors. Missing files are swallowed at load (`toonzscene.cpp:477-485`). | New: a toonzlib `SceneChecker` and a report panel; no duplicate-style or fps/DPI check exists. |

---

## Part A: FX nodes

### Tier 13: lines, print and motion

| # | Item | What is missing today | Approach | Size | Depends on |
|---|---|---|---|---|---|
| F1 | **Cel Edge Lines** (edge detector) | No FX draws lines between flat colour regions. | `stdfx/celedgelinesfx.cpp` with the math in `celedgelines.h`. Classify each pixel to a colour slot with `CelKey::Matcher` (tolerance, so antialiased ink between regions still classifies). A boundary pixel is one whose 4-neighbour has a different slot. The pair list is an `indexedGroups` layout of up to 25 entries, each two colours, a line weight, a line colour and an Off/On switch; an "all other pairs" default entry. Boundary pixels are seeds for `CelDistance::transform`, then `thin` to the pair's weight and `feather` for antialiasing. Output modes: lines over source, lines only, source without lines. Noise port as in Cel Radial Blur for a hand-drawn edge. Float path, native grid via `celfxsourcegrid.h`. | M | none |
| F2 | **Cel Boil** (line wobble) | No boil FX, no hold-N-frames stepping. | `stdfx/celboilfx.cpp`, math in `celboil.h`. A displacement field from `CelNoise` sampled at a low frequency: amplitude (pixels), scale (noise size), cycle length N (frames per step), seed, and an optional second octave. The plane offset is `floor(frame / N)`, so the field holds for N frames, then jumps; an "ease" option blends the two planes over the last K frames of a cycle. Apply with `TDeformer::warpRaster` through `deformerfxcommon.h` (`getSourceRect` enlarges the bbox by the amplitude). An "ink only" mode displaces only pixels darker than a threshold (or the Toonz level's ink, through the level's palette), leaving paint. `getAlias` must include `floor(frame / N)` and the seed; it does not otherwise. | M | none |
| F3 | **Print Process** (halftone) | No CMYK, screens, misregistration or paper. | `stdfx/printprocessfx.cpp`, math in `printprocess.h`. Pipeline per tile on the native grid: RGB → CMYK (naive with a black-generation amount); per plate a rotated screen (default 15°, 75°, 0°, 45°) with a dot size and a dot shape (round, line, diamond, euclidean); per plate a misregistration offset (dx, dy per plate, animatable); paper grain from `CelNoise` as a multiply; ink bleed as `TRop::blur` of each plate before recomposition; subtractive recomposition `1 - sum(plate × ink colour)`. Options: plates on/off, "Riso" preset (two or three spot inks instead of CMYK, with chosen ink colours), output as the print or a single plate. Screen geometry follows `mosaicfx.cpp`'s cell logic but on a rotated lattice. | L | none |

### Tier 14: particles, displacement and compositing

| # | Item | What is missing today | Approach | Size | Depends on |
|---|---|---|---|---|---|
| F4 | **Weather Particles** | The particle FX have no collision, no depth layers with parallax in one node, no turbulence without a control image. | Extend `particlesfx` rather than write a third engine. Add: a **Matte** port (`addInputPort("Matte", …)`, not port 0); on each step, a particle whose next position lands on an opaque matte pixel either dies, stops (snow settles), or splashes (spawns K short-lived particles with no gravity); **turbulence** from `iwa_simplexnoise` sampled at the particle position and time, with strength and scale; **depth layers**: a layer count, a near/far speed and size ratio, each particle born into a layer, rendered back to front, blurred per layer with `CelBlur` by a depth-of-field amount and a focus layer; **streaks** for rain: a stroked line of length `speed × shutter` instead of a sprite. Presets "Rain", "Snow", "Dust" updated. The frame-roll cache in `particlesmanager` must store the layer and the splash state. | L | none |
| F5 | **Flow Displace** (matte-driven displacement) | `ino_warp_hv` takes two grey maps and no matte; nothing takes a flow image. | `stdfx/flowdisplacefx.cpp` on a new `TFlowDeformer : TDeformer`. Ports: Source, **Flow** (an RG image in the `iwa_motionflowfx` convention, `v = 2 × c − 1`), optional **Map** (a grey map used as a scalar field, displaced along a direction parameter), optional **Matte** (weight of the displacement, feathered). Parameters: strength (pixels), direction (for the Map mode), time: a noise modulation from `CelNoise` with amplitude, speed and seed, so a static map gives secondary motion; invert; edge handling (clamp or transparent). Apply with `warpSourceTile`. The user draws the flow or map as a level in another column, which is the "user-drawn vector field". | M | none |
| F6 | **Light Wrap** | No light-wrap FX. | `stdfx/lightwrapfx.cpp`. Ports: Source (the character) and **Background**. Blur the background by the wrap width, mask it by the source's inner edge (the source alpha minus its choke by the width, from `CelDistance::thin`), multiply by intensity and screen it over the source. Options: width, intensity, blend (screen, add, over), "keep source alpha". | S | none |
| F7 | **Edge Extend** | Only `celcolorexpandfx` (Toonz-style) and `TRop::expandColor`. | `stdfx/edgeextendfx.cpp`: push the colour of the last opaque pixel outward by N pixels through the distance field of `celcolorexpandfx.cpp:46-73`, keeping the original alpha (or extending it, as an option). Used before a blur or a keyer. | S | none |
| F8 | **Matte Choke / Spread** | `erodilatefx` is 8/16-bit, rectangular or circular, alpha and colour together. | `stdfx/mattechokefx.cpp`: a signed amount (negative chokes, positive spreads) on alpha only, through `CelDistance::transform` + `thin` + `feather`, float path, with a feather radius. Colour stays; alpha changes. | S | none |
| F9 | **Premultiply / Unpremultiply** | `unmultiplyfx` is 8-bit only and not in `fxs.lst`. | Extend `unmultiplyfx.cpp` to 16-bit and float with `TRop::depremultiply`, list both FX under Matte in `fxs.lst`, add names to `current.txt` and a help page. | S | none |

### Tier 15: scene-wide colour and depth

| # | Item | What is missing today | Approach | Size | Depends on |
|---|---|---|---|---|---|
| F10 | **Colour Script** (palette shift) | No scene-wide grade that knows palette styles; LUTs cannot be animated; presets cannot be keyed between. | A **post-xsheet FX** (placed between the XSheet node and Output) on the Colour Override route: it pushes a `PaletteTransformRenderData` through the settings, applied to a clone of each level's palette at render time (`toonzlib/tcolumnfx.cpp:409-433` and `:701-760`), after palette keyframes. The transform is **H/S/L curves** (hue rotate, saturation and lightness as `TToneCurveParam`s, keyable) or a **LUT** (`Lut3D`, a `.cube` path). Move the Palette Gizmo's shifters into `toonzlib/palettetransform.h` and use them in both. Non-palette levels (full-colour raster, images) get a pixel fallback in the same FX, as `Lut3DBakeFx::applyLut`. **Exclusions**: a per-column "keep colours" flag (a free column status bit, written only when set) tested in `TLevelColumnFx` before applying the data, plus a style-name rule (styles whose name starts with a chosen prefix are skipped), so characters stay consistent. **Presets**: saved as ordinary FX presets in `stuff/fxs/presets/STD_colourScriptFx/`; **keying between presets** is a "From / To / Mix" trio: two preset names and an animatable mix, interpolating the curve parameters (and blending the two LUT outputs when both are LUTs). Snapping a mix key to each `NavigationTags` frame gives per-cut looks. Viewer: extend `overriddenPalette()` in `toonzlib/stagevisitor.cpp:190-230` to also find a Colour Script FX after the xsheet node, so the viewer shows the shift. | L | none |
| F11 | **Camera Focus** channel | No focus distance on the camera. | A camera-only channel `T_FocusDistance` ("focus"), in Z units, added as `T_RotX`/`T_RotY` were (`toonzlib/tstageobject.cpp:98-111`, save/load, Function Editor tree), default 0 meaning "at the table". Written only when keyed or non-default. Plus `T_Aperture` ("aperture") for the blur strength. | S | none |
| F12 | **Multiplane Depth** (depth of field and atmospheric perspective) | Nothing maps column Z to blur or haze. | Scene-level settings in `TSceneProperties` (written only when enabled): on/off, blur per unit of distance from focus, maximum blur, haze colour, haze density per unit of distance, desaturation per unit. In `FxBuilder::makePF(TLevelColumnFx*)` (`toonzlib/scenefx.cpp:~995`, after the Color Effect), and in the sub-xsheet and zerary cases: compute `dz = 1000 + cameraZ − columnZ`, `|dz − focus|`, and insert a hidden `ColumnDepthFx` (a new unary FX next to `ColumnColorEffectFx` in `common/tfx/unaryFx.cpp`): a lens blur with the radius divided by the perspective scale `(1000 + cameraZ) / dz`, so it is constant in screen space, then a 3×3 colour matrix for desaturation and a tint toward the haze colour. The alias carries the values. A per-column "exclude from depth" bit (free status bit). The rack focus is a key on the camera's focus channel. Viewer: the haze tint through `Stage::Player::m_colorEffect` is cheap and done; viewer blur is not attempted. `ColumnColorEffectFx` gets a float path on the way, which it lacks today. | L | F11 |

---

## Part B: viewer and scene tools

### Tier 16: the animation itself

| # | Item | What is missing today | Approach | Size | Depends on |
|---|---|---|---|---|---|
| T1 | **Motion arcs** overlay | Arcs exist only inside the Animate tool, for the centre of one keyframed object, with no spacing display. | Move `EditTool::getTrajectoryRange` / `getTrajectoryPos` into `toonz/motionarc.h` in toonzlib (pure, unit-tested like `pegbarguide_test`), with the placement cache restored after sampling (the gotcha at `edittool.cpp:1454`). The tracked point is the centre, a hook `H<n>` (through `TStageObjectTree::getHandlePos`), or a point picked in the viewer and stored in the column's local space. Draw in `SceneViewer::drawScene` next to `drawSpline`, in world space with the camera perspective, for every column that has the overlay on: a polyline, a dot per frame, squares on keys, and **spacing** shown by dot size or a colour ramp by speed, with frame numbers every K frames. Toggle `MI_MotionArcs` on the `MI_PegbarGuide` pattern (`toonz/menubarcommandids.h`, `mainwindow.cpp:2876`, `menubar.cpp:1689`, viewer menus), with settings in TEnv. Drawing-based motion: when the column has no keys, sample the hook per cell across drawings. | M | none |
| T2 | **Spacing chart** | Nothing. | A small dockable panel (`TPanelFactory` on the `ArtLayerManagerPanelFactory` pattern, `toonz/tpanels.cpp:2002`) that draws the classic chart for the current column: one tick per frame along the arc length between two chosen frames, keys as long ticks, and a 1D strip of the distances. Reads `motionarc.h`. | S | T1 |
| T3 | **Ghost fade** control | Fade is a preference (paper thickness) plus per-marker popups. | A fade slider on the viewer's onion-skin menu (and in the Onion Skin panel if one is added later), as a global multiplier applied in `getOnionSkinOpacity` (`toonzlib/stagevisitor.cpp:106`) and `getOnionSkinFade`, saved in TEnv; a "no tint" option that skips the front/back colouring; and "ghost every Nth frame" for long holds. The onion mask stays runtime state, as today. | S | none |

### Tier 17: scene health

| # | Item | What is missing today | Approach | Size | Depends on |
|---|---|---|---|---|---|
| T4 | **SceneChecker** library | No checker, missing files swallowed at load, no duplicate-style or fps/DPI check. | `toonzlib/scenechecker.{h,cpp}`: a pure function from a `ToonzScene` to a list of `Issue{severity, kind, column, row, levelName, styleId, text}`. Checks: cells whose `!sl->isFid(fid)` (missing drawing), levels whose decoded path does not exist (through the `SceneResources` visitor), empty columns, cast levels not used in any xsheet, unused styles (`isStyleUsed`; slow, so it is an option), **duplicate styles** (same main colour, type and parameters, new), level DPI policy against the camera DPI, sound and movie level fps against the output fps, output and preview resolution against the camera. Sub-xsheets are walked. Unit tests in `toonzlib/tests/scenechecker_test.cpp` on scenes built in memory, as `scenebench_test` does. | M | none |
| T5 | **Check Scene** command and report panel | No report UI. | `MI_CheckScene` in the Render menu next to Output Settings (`menubar.cpp:1668-1677`, `menubar_template.xml` Render menu, an `ensure...Action` helper so saved user menus get it). A dockable **Scene Check** panel listing issues grouped by kind, with a filter, counts in the title, and "go to" on double-click (sets the frame, column and level handles). A **statistics** header: drawings, levels, columns, frames, palette styles, scene file size. "Check before render" preference: run T4 in `RenderCommand::init` (`toonz/rendercommand.cpp:257`) and show the panel when there are errors, with Continue / Cancel. | M | T4 |
| T6 | **Script access** | No palette bindings in the script API. | `Scene.check()` returning the issue list, and `Level.palette` with styles (id, name, colour, used), in JS and Python, documented in `doc/script_api.md`. | S | T4 |

---

## How a new FX is added (checklist)

From the Cel Radial Blur (#88) and Star Glow (#83) pull requests.

1. **Source** `stdfx/<name>fx.cpp`, ending in `FX_PLUGIN_IDENTIFIER(Class, "<id>")`; the id becomes `STD_<id>`. Pure math in `stdfx/<name>.h` with no toonz types. Port 0 is the Source: a disabled FX passes port 0 through (`tnzbase/trasterfx.cpp:845`), and a column connected only to another port counts as a control column (`toonzlib/fxdag.cpp:206`).
2. **CMake** `stdfx/CMakeLists.txt`: the source in `SOURCES`; the tests under `if(BUILD_CELFX_TESTS)` (`_test` and `_ui_test`, both `#include "../<name>fx.cpp"`), registered as `celfx_<name>` so the Linux Build runs them (`.github/workflows/workflow_linux.yml:81-89`), with `QT_QPA_PLATFORM=offscreen` and a timeout, added to the `celfx_tests` target.
3. **Add FX menu** `stuff/profiles/layouts/fxs/fxs.lst`: `STD_<id>` under its category (Blur, Distort, Light, Matte, Noise, Render, Stylize).
4. **Parameter page** `stuff/profiles/layouts/fxs/STD_<id>.xml` (`help_file`, pages, `indexedGroups`, `vbox modeSensitive`).
5. **Names** `stuff/config/current.txt`: the FX and every parameter.
6. **Help** `stuff/doc/<Name>.html`.
7. **Optional**: presets in `stuff/fxs/presets/STD_<id>/`, viewer gadgets through `getParamUIs` (and `tnztools/edittoolgadgets.cpp` for a new gadget type), a render reference in `stdfx/tests/fxrender_test.cpp`.
8. **Shared conventions** used by every cel FX: `celfxsourcegrid.h` (native grid, camera box for infinite sources), `enableComputeInFloat(true)`, `requiresFullPrecisionSerialization()`, `getMemoryRequirement` in MB, `getAlias` with `std::locale::classic()` and every value that changes the output (for F2: the frame step).

Categories for the new FX: F1 and F3 under Stylize, F2 and F5 under Distort, F4 under Render, F6 under Light, F7 to F9 under Matte, F10 under a new "Scene" category.

---

## Order and tiers

| Tier | Items | Why this order |
|---|---|---|
| 13 | F1, F2, F3 | Self-contained FX on the cel conventions; nothing depends on them, and they are what a "lineless" or print look needs first. |
| 14 | F6, F7, F8, F9, F5, F4 | The four compositing helpers are small and unblock the others (F5 and F4 use the choke and blur). F4 last: it is the largest and touches the particle cache. |
| 15 | F11, F10, F12 | F11 is a format addition (a camera channel) and goes in on its own commit. F10 and F12 are the two scene-wide features; both need a per-column status bit, chosen once in this tier. |
| 16 | T3, T1, T2 | T3 is a day of work. T1 is the arc overlay; T2 reads from it. |
| 17 | T4, T5, T6 | The checker library first, with its tests; then the command and panel; then the script bindings. |

## Format changes to decide

- F11: two camera channels ("focus", "aperture"), written only when keyed or non-default. Older builds ignore unknown channels.
- F10 and F12: one free column status bit each ("keep colours", "exclude from depth"), written only when set; older builds keep and ignore them (the Outline View bit from #93 is the precedent).
- F12 and F10: scene properties, written only when enabled.
- F2: none (the boil is an ordinary FX).
- T1: the tracked point per column is runtime state in TEnv, not in the scene, unless users ask for it to be saved.

## Testing

- Every FX: a `_test` on the pure header (F1: pair classification and line weight on a synthetic three-colour image; F2: the field is identical within a cycle and different across cycles, the alias changes at the step; F3: a flat grey renders the expected dot area per plate, and a misregistration offset moves one plate only; F4: a particle dies on the matte, the frame-roll cache gives the same frame twice; F5: a constant flow moves the image by `strength`; F6 to F9: the alpha and colour invariants) and a `_ui_test` on the layout XML, both registered as `celfx_*`.
- F10 and F12: tests in `toonzlib/tests` on an in-memory scene: the render data reaches a level's palette clone; a column with the "keep" bit is untouched; `dz` and the inserted blur radius for a column at a given Z.
- T1, T4: pure unit tests (`motionarc_test`, `scenechecker_test`).
- Compat: the Compat job renders existing scenes; none of the above changes a scene that does not opt in.
