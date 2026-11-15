# Comic and watercolour FX plan

A plan for five new 2D FX: motion lines, burst lines, a dot screen, rough
strokes along vector paths, and a watercolour look. Then four additions to
the GLB Model FX: toon shading, feature lines, line styles and render passes.
Paths are under `toonz/sources` unless they start with `doc/` or `stuff/`.

The five 2D FX (phases 1 to 4):

1. Motion Lines (parallel speed lines, rain, hatching)
2. Burst Lines (radial focus lines, impact flashes, zoom)
3. Dot Screen (a dot lattice driven by control images)
4. Rough Stroke (hand-drawn strokes along vector paths)
5. Watercolour (edge bleed, pigment pooling, paper)

The GLB Model additions (phases 5 to 8, see "3D additions"):

6. Toon shading (stepped zones, zone offset, shaped highlight, light colour)
7. Feature lines (outline, crease, material, open edge, intersection,
   hidden lines, falloff by distance)
8. Line styles (taper, width variation, wave, dashes and dots, overshoot),
   one stroker shared with Rough Stroke
9. Render passes (lines only, shading zones, depth, normals)

These are our own designs of generic comic and paint techniques. Names,
parameters, presets and panel layouts are designed here from the technique.

## Scope rules

- **2D first.** Phases 1 to 4 do not touch the GLB Model FX. Phases 5 to 8
  change the GLB Model FX and the `glb` library only, under the two rules
  in "3D additions".
- **Build on what exists.** The cel family, the gradients, Glow, Texture,
  Multi Tone and the smoothers already cover flat-colour blur, edge lines,
  colour keying, extension, gradient mapping and anti-aliasing. None of that
  is rebuilt. Each item below names the existing code it stands on.
- **One PR per phase**, one commit per item, merged on green checks.
- **New behaviour is opt-in.** A scene that does not use these FX saves
  exactly as before. No scene format change is planned (see "Format changes").
- **FX follow the cel conventions** from `doc/fx_and_production_tools_plan.md`
  ("How a new FX is added"): pure math in a header without toonz types, a
  `_test` and a `_ui_test` registered as `celfx_*`, the source grid from
  `stdfx/celfxsourcegrid.h`, float support, a complete `getAlias`.
- **Nothing GPU.** CPU raster only.
- **Deterministic.** The same seed, frame and parameters give the same pixels
  at any tile split, preview zoom and thread count.

Sizes: S is a few days, M is one to two weeks, L is several weeks. They are
estimates made before any code.

---

## What exists today

| # | Feature | Already in the tree | Verdict |
|---|---|---|---|
| 1, 2 | Lines | Stateless seeded noise with a plane offset (`stdfx/celnoise.h`, `CelNoise::sample`), hold-N-frames stepping (`stdfx/celboil.h`), distance fields with thin and feather (`stdfx/celdistance.h`), zerary generators (`TStandardZeraryFx`, for example the gradients in `stdfx/stdfx.cpp`), viewer gadgets for points, vectors, radii and angles (`include/tparamuiconcept.h`). | New FX on a new shared line engine. Nothing generates lines today. |
| 3 | Dot screen | Rotated screen lattice, dot shapes, dot gain and grain for CMYK plates (`stdfx/printprocess.h`), cell averaging with square or round dots (`stdfx/mosaicfx.cpp`). | New FX. Print Process separates an image into ink plates; it is not a dot pattern whose size, rotation and colour are driven by maps. The lattice math is shared. |
| 4 | Rough stroke | Stroke geometry (`TStroke`, `TVectorImage`), outline FX that restyle a Toonz vector column through render data (`stdfx/calligraphicfx.cpp`, `SandorFxRenderData`), hand-drawn vector brush styles. | New FX, and a new route: no FX in `stdfx` reads the strokes of its input column. Needs a design spike (R1). |
| 5 | Watercolour | Light wrap and matte choke (`stdfx/lightwrapfx.cpp`, `stdfx/celcomposite.h`), flat-colour boundary detection (`stdfx/celedgelines.h`), distance fields, `CelNoise` for paper grain, `TRop::blur`. | New FX. The pieces exist; no FX combines them into bleed, pooling and granulation. |

---

## The FX

### L0: line engine (shared, no FX of its own)

`stdfx/comiclines.h`, pure math. A line is a centre path, a width profile and
a colour. The engine turns a seed and a set of parameters into a list of
lines, then rasterises them with analytic coverage.

- **Distribution:** density, groups (lines per group, attraction within a
  group, gap between groups), per-line random offset.
- **Shape:** length, thickness, taper towards one end, separate in and out
  shaping (the pen-pressure look), and a wave with amplitude, period and phase.
- **Randomness:** each of length, thickness, position, opacity and colour has
  its own amount; all draw from one seed, so a parameter change does not
  reshuffle the others.
- **Animation without keys:** an offset that moves lines along their own
  direction at a rate per frame, and a re-seed interval of N frames (the
  hold-then-jump of `celboil.h`).
- **Size:** M.

### F1: Motion Lines

`stdfx/motionlinesfx.cpp`, a zerary generator on L0. Lines run parallel to the
direction from a start point to an end point; a tile option repeats the band
over the frame. Start and end points get a viewer gadget. To limit the lines
to a region (hatching over a shadow), use the existing Matte In with the shape
as the matte: the FX has no port of its own. Size: S on top of L0.

### F2: Burst Lines

`stdfx/burstlinesfx.cpp`, a zerary generator on L0. Lines radiate from a
centre point, with an inner radius where nothing is drawn, an outer radius, a
twist that curls lines around the centre, and a falloff that thins the lines
towards the centre. Length and Length Randomness set how far in a line
reaches; a length falloff would repeat them, so there is none. Centre and radii get viewer
gadgets. Size: S on top of L0.

### F3: Dot Screen

`stdfx/dotscreenfx.cpp`, math in `dotscreen.h`, reusing the rotated lattice of
`printprocess.h`.

- **Lattice:** spacing, angle, row stagger, per-cell jitter.
- **Dot:** shape (round, square, diamond, line), size, aspect, rotation,
  opacity, colour, each with a random amount.
- **Modifiers:** up to three. Each takes a control image (a port) or a built-in
  ramp, and sets, scales or offsets one dot property: size, aspect, rotation,
  opacity or colour. With the Source as a control image this is a classic
  halftone; with a gradient it is a decorative screen.
- **Size:** M.

### F4: Rough Stroke

`stdfx/roughstrokefx.cpp`, math in `roughstroke.h`. Draws hand-drawn strokes
along the strokes of a vector level.

- **Stroke:** width, in and out shaping, a colour gradient along the stroke.
- **Rough:** copies of each stroke with random offset and width, and a wave.
- **Reveal:** a start and end position along the stroke, so strokes grow, and
  an offset that travels along the path, with a loop option.
- **Colour:** each stroke's own style colour, or a gradient along it.
- **Output:** the rough strokes alone, or over the source drawing.
- **Size:** M (see the M0.2 result).

### F5: Watercolour

`stdfx/watercolourfx.cpp`, math in `watercolour.h`. Takes flat-colour art and
returns a painted look.

- **Bleed:** colour spreads past region edges by a distance, with a noise
  field bending the edge, so regions overlap softly.
- **Edge darkening:** pigment gathers towards the edge of each region
  (distance field from the region boundary).
- **Granulation and unevenness:** low-frequency noise varies density inside a
  region; a paper grain multiplies the result.
- **Paper:** paper colour, an option without paper so the result composites
  over other art, and a white-to-transparent option for drawings on white.
- **Hold:** the noise holds for N frames, as in Cel Boil, so the paint does
  not shimmer on every frame.
- **Size:** L.

---

## Phases and milestones

A milestone is reached when its exit checks pass in CI, not when the code is
written.

### Phase 0: decisions and spike

| Milestone | Work | Exit check |
|---|---|---|
| M0.1 | This plan reviewed; names and menu categories agreed. | Plan merged. |
| M0.2 | **R1 spike:** how Rough Stroke reads vector strokes. Compare (a) a render-data route like `SandorFxRenderData`, where the column renderer hands stroke geometry to the FX, and (b) a toonzlib hook that gives an FX the `TVectorImage` of its input column at a frame. | A short note in this file with the chosen route and what it changes outside `stdfx`. F4 is re-sized. |

**M0.2 result.** Route (b), without a toonzlib hook. `stdfx` already walks
from an FX's input to its level column (`stdfx/celfxsourcegrid.h` uses
`TLevelColumnFx`). Rough Stroke does the same walk through enabled transforms,
multiplies their placements, takes the column's vector image for the frame
(`TXshLevelColumn::getRemappedCell`, as the column's own render does) and
samples each `TStroke` by length. Nothing changes outside `stdfx`, so F4 is
M, not L. Limits: Source must be a vector level, directly or through
transforms; any other source passes through unchanged, and stroke deformers
above the level are not applied to the rough strokes.

### Phase 1: lines

| Milestone | Work | Exit check |
|---|---|---|
| M1.1 | L0 line engine. | `celfx_comiclines` passes: same seed gives the same lines; changing one random amount leaves the others unchanged; a tile split gives the same pixels as one tile; the offset animation moves a line by the stated distance per frame; the re-seed interval holds for N frames; 300 lines over one pixel are all drawn; the densest field (20,000 lines) samples 480 by 270 pixels within a fixed bound. |
| M1.2 | F1 Motion Lines with gadgets, layout, names, help page. | `celfx_motionlines` and `celfx_motionlines_ui` pass: the line is where the parameters put it, a tile equals the same region of the whole frame, the cache key changes only with what changes the picture. |
| M1.3 | F2 Burst Lines, same deliverables. | `celfx_burstlines` and `celfx_burstlines_ui` pass; the inner radius is empty. |
| M1.4 | Presets in `stuff/fxs/presets/`: speed lines, rain, wind, hatching, focus lines, impact, zoom, spiral. | `celfx_comicfx_presets` loads each preset through the FX registry: it sets every parameter, changes the FX and draws. |

### Phase 2: dot screen

| Milestone | Work | Exit check |
|---|---|---|
| M2.1 | `dotscreen.h` and F3 without modifiers. | `celfx_dotscreen` passes: a flat field renders the expected dot area for a given size and spacing; the angle rotates the lattice about the camera centre. |
| M2.2 | The three modifiers and their control ports. | A horizontal ramp on the size modifier gives monotonically growing dots; a modifier on one property leaves the others unchanged. |
| M2.3 | Layout, names, help, presets (halftone, diagonal lines, mosaic, polka dots). | `celfx_dotscreen_ui` and `celfx_comicfx_presets` pass. |

### Phase 3: watercolour

| Milestone | Work | Exit check |
|---|---|---|
| M3.1 | `watercolour.h`: bleed and edge darkening on a synthetic two-colour image. | `celfx_watercolour` passes: colour reaches the stated bleed distance and no further; density is highest at the region edge. |
| M3.2 | Granulation, paper, hold. | The noise is identical within a hold and differs across holds; the alias changes at the hold step. |
| M3.3 | F5 FX, layout, names, help, presets (wet in wet, dry brush, transparent wash). | `celfx_watercolour_ui` and `celfx_comicfx_presets` pass; `celfx_watercolourfx` checks the memory estimate against what a 4K tile and its halo allocate. |

### Phase 4: rough stroke

| Milestone | Work | Exit check |
|---|---|---|
| M4.1 | The stroke route chosen in M0.2. | `celfx_roughstrokefx` draws the stroke of a real vector level in a real column, and follows a transform between the level and the FX. |
| M4.2 | `roughstroke.h` and F4: width, shaping, copies, wave. | `celfx_roughstroke` passes on a synthetic path. |
| M4.3 | Reveal and travelling offset, layout, names, help, presets (sketch, brush, marching dashes). | A stroke revealed to 50% covers half its length; `celfx_roughstroke_ui` and `celfx_comicfx_presets` pass. |

### Order

Phase 1 first: one engine gives two FX and the most visible result. Phase 2
and Phase 3 are independent of each other and of Phase 1. Phase 4 is last
of the 2D phases because it is the only one that reaches outside `stdfx`,
and its size depends on the Phase 0 spike. Phases 5 to 8 follow.

---

## 3D additions

Phases 5 to 8 add four features to the GLB Model FX. Every scope rule above
stands, and two more apply:

- **Library first.** The maths lives in `glb/` (namespace `otglb`, standard
  library only), tested by the standalone suite that the Linux workflow
  builds and runs with the address and undefined behaviour sanitizers on
  every pull request. `stdfx/glbmodelfx.cpp` only maps parameters into
  `otglb::RenderOptions`.
- **Draw in `otglb::renderTile`.** Three-Point Light and 3D Transformer call
  `renderTile` themselves, so anything drawn only in `stdfx` would vanish
  behind them. Standard shading with the Beauty pass keeps today's code
  path, bit for bit.

### What exists today

| # | Feature | Already in the tree | Verdict |
|---|---|---|---|
| 6 | Toon shading | `prepareRender` sets one colour per face (headlight, or the diffuse of the lighting rig from Three-Point Light). Per-sample lighting and skinned vertex normals exist only on the procedural surface path. | New shading mode. The ordinary path keeps no vertex normals, so it needs a per-vertex payload and a per-sample resolve. |
| 7 | Feature lines | Wireframe draws `RenderTriangle::edges`. No adjacency survives `prepareRender`. | New: welded topology, edge types, intersections and visibility. |
| 8 | Line styles | `stdfx/roughstroke.h` has an arc-length sampler, in and out swell, wave, drift, copies and per-point width and opacity, drawn with max coverage independent of the tile. | Reuse it; add dashes, width variation and overshoot. |
| 9 | Passes | Output is premultiplied sRGB only; float tiles keep values above 1. Open EXR AOV reads passes, and its Raw Data colour-space rule is the convention to copy. | New output selector. |

### T1: Toon shading

`glb/glbtoon.h`, pure maths:

- **Ramp:** 2 to 4 zones, each with a start position, a colour and a
  softness. A zone starts with a smoothstep of width Softness.
- **Ramp input:** `s = clamp(master · (ambient + Σ Iᵢ · max(0, N·Lᵢ)))`
  over the rig's lights; without a rig, `s = N·V` (headlight).
- **Light colour:** the zone colour is multiplied in linear light by
  `mix(white, C, k)`, where `C` is the light colours weighted by their share
  of `s`.
- **Offset:** `s += amount · (2 · noise(rest / size) − 1)`, a seeded 3D value
  noise sampled at the rest-pose position, so it sticks to a skinned or
  moving surface.
- **Highlight:** the Blinn half vector of the first light (the key light or
  the headlight), as angles from `N` in a tangent frame turned by Rotation;
  lit inside an ellipse with radii `size · (1 ± squash)`, softened over
  Softness.

In the renderer: when toon shading or a data pass is on, `prepareRender`
keeps a per-vertex payload (normal and rest position) beside the triangles,
clipped with them. Normals come from `NORMAL` through the skinning code; a
model without normals gets face normals. A deferred resolve keeps the
winning triangle and barycentrics of each coverage sample and shades it with
perspective-correct interpolation, so zones are smooth, not faceted.

Parameters: `lighting` gains value 2, Toon; `toonZoneCount`,
`toonZone1..4Color`, `toonZone2..4Position`, `toonZone2..4Softness`,
`toonZoneMode` (Multiply Material, Replace), `toonLightColor`,
`toonOffsetAmount`, `toonOffsetSize`, `toonOffsetSeed`, `highlight`,
`highlightColor`, `highlightOpacity`, `highlightSize`, `highlightSoftness`,
`highlightSquash`, `highlightRotation`.

### T2: Feature lines

`glb/glblines.h/.cpp`:

- **Topology:** vertices welded by the exact bits of their positions, so UV
  and normal seams are not open edges; edge-to-face maps in sorted vectors,
  so the order never depends on a hash map.
- **Types, drawn by priority:** silhouette (the two faces face opposite ways
  in the image; edge-on faces count as back-facing), open edge (one face;
  three or more faces are drawn as open edges too), crease (dihedral angle
  above Crease Angle), material boundary.
- **Intersections:** a bounding-volume hierarchy over camera-space
  triangles, ties broken by index. Pairs sharing a welded vertex are skipped;
  the Möller triangle test gives the segments. Scope Between Parts (the
  default) or All. Candidate pairs have a budget with an explicit error.
- **Visibility:** each projected line is cut where it crosses another
  contour or pierces a triangle, and each piece is classified by one query
  at its midpoint for a nearer surface. This runs once per scene, so every
  tile, zoom and thread count sees the same visible and hidden runs.
- **Output:** pieces clipped to Near and Far, chained through vertices where
  exactly two edges of one type and visibility meet at a turn below 60°, with
  a camera distance per point and a seed from the chain's first edge. The
  scene bounds grow by the widest line's reach.

Parameters: `lineOutline`, `lineCrease`, `lineCreaseAngle`, `lineMaterial`,
`lineOpen`, `lineIntersection`, `lineIntersectionScope`, `lineColor`,
`outlineColor`, `lineWidth`, a width percent per type, `hiddenLines`,
`hiddenColor`, `hiddenOpacity`, `lineFalloffNear`, `lineFalloffFar`,
`lineFalloffWidth`, `lineFalloffOpacity`.

### T3: Line styles

- `stdfx/roughstroke.h` moves to `glb/glbstroke.h`, keeping
  `namespace RoughStroke`, and its two includers change to the new path.
  One stroker serves Rough Stroke and the 3D lines.
- New settings, each defaulting to today's output: dashes (length, gap,
  phase; a dash of 0 is a round dot), width variation (a smooth seeded
  noise on the half width), overshoot (both ends extended along their
  tangents, with a random amount).
- After the surfaces resolve, `renderTile` maps each chain that meets the
  tile to output pixels, scales widths by `√|det A|`, then strokes and
  composites it over. Per-point thickness and opacity carry the distance
  falloff and the hidden-line opacity.

Parameters: `lineTaperIn`, `lineTaperOut`, `lineWidthVariation`,
`lineVariationSize`, `lineWaveAmplitude`, `lineWavePeriod`,
`lineDashLength`, `lineDashGap`, `hiddenDashLength`, `lineOvershoot`,
`lineOvershootRandom`, `lineCopies`, `lineCopyOffset`, `lineSeed`, and
`lineBoil` (frames each wobble is held for).

### T4: Render passes

`glb/glbpass.h`: `RenderPass {Beauty, LinesOnly, ShadingZones, Depth,
Normals}` and the encoders. The pass lives in `RenderOptions` and the
prepared scene, so the 3D FX downstream keep it.

| Pass | Normalised encoding | Raw encoding (float tiles only) |
|---|---|---|
| Depth | Distance along the view axis, as `(d − Near) / (Far − Near)`, clamped, optionally inverted. | `d` in model units. |
| Normals | Camera-space normal turned toward the camera, as `0.5 + 0.5·n` (+X right, +Y up, +Z toward the camera). | `n`. |
| Shading Zones | Each zone's slot colour, unlit, with the highlight on top. | Same. |
| Lines Only | The line layer over transparent. | Same. |

For Depth and Normals, RGB is the premultiplied mean over the covered
samples and alpha is the coverage, and the FX leaves the tile's colour
space as it is, as Open EXR AOV does for raw data.

Parameters: `outputPass`, `depthNear`, `depthFar`, `depthInvert`,
`passEncoding`.

### Phase 5: toon shading

| Milestone | Work | Exit check |
|---|---|---|
| M5.0 | Scope amended; the GLB FX tests run in CI; the comments that say GLB work is tested in OT3D (`glb/CMakeLists.txt`, `stdfx/CMakeLists.txt`, `CMakeLists.txt`) are corrected. | Plan merged; `celfx_glbmodelfx` runs in the Linux, macOS and Windows checks. |
| M5.1 | `glbtoon.h`. | `glbtoon`: with starts {0, 0.5} and Softness 0, s = 0.49 is zone 1 and 0.51 zone 2; with Softness 0.1 the blend is half at 0.5 and pure at 0.45 and 0.55; zones sort by position whatever their slot; offset +0.2 moves the boundary to s = 0.3; with Squash 0.5 a deviation of 1.4 × size is inside along the axis and outside across it, and Rotation 90° swaps them. |
| M5.2 | Per-vertex payload and deferred resolve. | A quad turned about Y under one +Z light is zone 2 at 59° and zone 1 at 61°. A smooth sphere under the headlight has its boundary at 0.866 R ± 1 px. A red light at influence 1 zeroes green and blue; at 0 it leaves them. One tile equals four tiles under a rotated affine, also from two threads. `glbrenderer`, `glblighting` and `seexpr_glb_surface` pass unchanged. |
| M5.3 | Offset noise. | The animated skin fixture has the same zone at the same surface point at seconds 0 and 1; the same seed gives the same pixels. |
| M5.4 | Parameters, Toon page, names, help. | `celfx_glbmodel_ui` lists every new control. `celfx_glbmodelfx`: Three-Point Light drives the zones; the alias changes with each toon parameter; a default GLB Model renders as before. |

### Phase 6: feature lines

| Milestone | Work | Exit check |
|---|---|---|
| M6.1 | Topology and edge types. | `glblines`: a perspective cube seen face-on gives 4 visible silhouette edges, 8 hidden crease edges and no diagonal; seen corner-on, 6 silhouette, 3 visible crease and 3 hidden crease edges. A 24-vertex cube gives the same edges as an 8-vertex one; a two-material cube gives 4 material edges; a single quad gives 4 open edges. |
| M6.2 | Visibility. | A large quad behind a small one has its edge cut at the crossings (± 1e-6), with the middle hidden. An edge lying on its own faces is visible. Permuting the triangles gives the same runs. |
| M6.3 | Intersections. | Two crossing quads give one segment at the expected endpoints; shared-vertex and coplanar pairs give none; Between Parts ignores a mesh's self-intersection; two 25k-triangle spheres finish within 10 s under ASan; exceeding the pair budget gives an explicit error. |
| M6.4 | Chains, plain drawing, falloff. | A silhouette pixel has the line colour. Tile and thread splits are identical, lines included. The bounds hold the full line. At Falloff Far, the summed coverage across the line is Falloff Width × width (± 2%). |
| M6.5 | Lines page; the line set cached with the geometry. | `celfx_glbmodelfx`: lines are identical through Three-Point Light and 3D Transformer; a colour-only edit reuses the line set. |

### Phase 7: line styles

| Milestone | Work | Exit check |
|---|---|---|
| M7.1 | Move to `glb/glbstroke.h`. | `celfx_roughstroke`, `celfx_roughstrokefx` and `celfx_comicfx_presets` pass unchanged; `glbstroke`: `draw` output equals a checksum recorded before the move. |
| M7.2 | Dashes, dots, variation, overshoot. | A 100 px line with dash 10 and gap 10 gives 5 runs; a dash of 0 gives dots whose diameter is the width (± 0.5 px); overshoot 5 reaches 5 px past each end; variation `a` keeps the half width within h × (1 ± a); default settings reproduce the M7.1 checksum. |
| M7.3 | 3D chains, hidden style, boil. | At affine scale 2, widths and dashes double; the wobble is identical across tiles; Boil N holds for N frames; the hidden dash only touches hidden runs. |
| M7.4 | Presets: Clean Outline, Sketch, Technical, Ink. | `celfx_comicfx_presets` loads each and checks it sets every parameter. |

### Phase 8: passes

| Milestone | Work | Exit check |
|---|---|---|
| M8.1 | `glbpass.h`, Depth and Normals. | `glbpass`: a quad at distance 7 with Near 5 and Far 10 gives 102 at 8 bits, 26214 at 16 bits, 0.4 normalised and 7.0 raw in float, and 0.6 inverted; the background has alpha 0; a facing quad gives (0.5, 0.5, 1.0), and turned 30° about Y its red is 0.75 (0.5 raw). |
| M8.2 | Lines Only, Shading Zones. | Lines Only has alpha 0 away from lines and equals Beauty's line layer; a Shading Zones pixel is the slot colour whatever the material. |
| M8.3 | Output page. | `celfx_glbmodelfx`: data passes are identical with linear colour on and off; Depth through Three-Point Light equals Depth from GLB Model alone. |

### Sizes and order

Phase 5 is L (the deferred resolve is most of it), Phase 6 is L
(visibility and intersections are M each), Phase 7 is M (the move itself
is S) and Phase 8 is M. M8.1 can follow Phase 5 directly; Lines Only waits
for Phase 6, and the 3D line drawing (M7.3) waits for M6.4.

---

## Menu, names and help

- Add FX menu (`stuff/profiles/layouts/fxs/fxs.lst`): Motion Lines, Burst
  Lines and Dot Screen under Render; Rough Stroke and Watercolour under
  Stylize.
- Each FX gets a parameter page (`STD_<id>.xml`), names in
  `stuff/config/current.txt`, and a help page in `stuff/doc/`.
- GLB Model gets Toon, Lines and Output pages in its existing layout, with
  names and help to match; no new menu entry.

## Format changes

None planned. All five are ordinary FX saved through the existing FX
serialization. If the Phase 0 spike finds that Rough Stroke needs a column
flag or a scene tag, that becomes a decision named in its PR.

Phases 5 to 8 add parameters to GLB Model only. An older build skips the
parameters it does not know and resets the unknown `lighting` value 2 to
Unlit. They add no port: every port is saved even when unconnected, and an
older build fails to open a scene with a port it does not know, so an image
offset map for the toon zones is left out (see "Out of scope").

## Testing

- Every FX: a `_test` on the pure header and a `_ui_test` on the layout XML,
  registered as `celfx_*` and added to the `celfx_tests` target.
- Determinism: for L0, F3 and F5, a test renders one frame as a single tile
  and as four tiles and compares the pixels.
- The FX tests assert pixel values at known positions, at 8-bit, 16-bit and
  float, and at a scaled affine.
- Presets: `celfx_comicfx_presets` loads every shipped preset through the FX
  registry and checks it sets every parameter of its FX.
- No render references in `stdfx/tests/fxrender_test.cpp`: a reference is a
  PNG rendered by the application and reviewed by eye, which this work has
  not done.
- Compat: none of these changes a scene that does not use them.
- GLB: library tests `glb/tests/glb{toon,lines,stroke,pass}_test.cpp` beside
  `glbrenderer`, run by the sanitizer step; FX tests `celfx_glbmodelfx` and
  `celfx_glbmodel_ui` under `BUILD_CELFX_TESTS`, since `BUILD_GLB_FX_TESTS`
  runs in no workflow.

## Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Rough Stroke has no existing route to vector strokes; the right route may touch the column renderer. | Phase 0 spike before any F4 code; F4 is last. |
| R2 | Line rasterisation cost with thousands of lines at 4K. | Lines are sorted across the layout and each pixel visits only those within its reach. Measured: the densest field (20,000 thick wavy lines) samples a 4K frame in about 6 s on one thread at -O2. M1.1 has a cost bound. |
| R3 | Watercolour needs a halo as wide as the bleed, which grows tile memory. | `getMemoryRequirement` and the halo are set from the bleed distance; M3.3 checks a 4K frame. |
| R4 | Noise that changes with tile or zoom would shimmer. | Noise is sampled in camera space on the native grid, as the other cel FX do; the tile-split tests cover it. |
| R5 | Large meshes: the payload and four shaded samples per pixel. | Built only when toon shading or a data pass needs it; the existing memory checks stay; M6.3 has a cost bound. |
| R6 | Intersection cost grows with dense self-contact. | Off by default, Between Parts by default, shared-vertex pairs skipped first, a pair budget. |
| R7 | Lines or zones that change with tile, zoom or thread count. | Visibility runs once per scene; sorted topology; ties broken by index; one thread during preparation; tile-split tests. |
| R8 | Memory: the projection cache counts only triangles. | Count the payload and the line set in the cache entry's size. |
| R9 | Silhouettes pop as faces flip, and the wobble re-rolls. | Seeds come from edge ids; Boil holds them. |
| R10 | Each pass of a duplicated GLB Model prepares its own scene. | Sharing prepared scenes by geometry is a follow-up. |

## Out of scope

- An image offset map for the toon zones: it needs a new port (see "Format
  changes"). It can come later through a downstream 3D node that passes the
  image as render data, as Expression Attribute does.
- Flat-colour blur, edge lines, colour keying, extension, gradient mapping,
  glow and anti-aliasing: already in the tree.
- CMYK separation: Print Process already does it.
