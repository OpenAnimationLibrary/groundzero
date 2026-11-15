# Comic and watercolour FX plan

A plan for five new 2D FX: motion lines, burst lines, a dot screen, rough
strokes along vector paths, and a watercolour look. Paths are under
`toonz/sources` unless they start with `doc/` or `stuff/`.

The five:

1. Motion Lines (parallel speed lines, rain, hatching)
2. Burst Lines (radial focus lines, impact flashes, zoom)
3. Dot Screen (a dot lattice driven by control images)
4. Rough Stroke (hand-drawn strokes along vector paths)
5. Watercolour (edge bleed, pigment pooling, paper)

These are our own designs of generic comic and paint techniques. Names,
parameters, presets and panel layouts are designed here from the technique.

## Scope rules

- **2D only.** Nothing here touches the GLB Model FX or adds 3D shading.
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
because it is the only one that reaches outside `stdfx`, and its size depends
on the Phase 0 spike.

---

## Menu, names and help

- Add FX menu (`stuff/profiles/layouts/fxs/fxs.lst`): Motion Lines, Burst
  Lines and Dot Screen under Render; Rough Stroke and Watercolour under
  Stylize.
- Each FX gets a parameter page (`STD_<id>.xml`), names in
  `stuff/config/current.txt`, and a help page in `stuff/doc/`.

## Format changes

None planned. All five are ordinary FX saved through the existing FX
serialization. If the Phase 0 spike finds that Rough Stroke needs a column
flag or a scene tag, that becomes a decision named in its PR.

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

## Risks

| # | Risk | Mitigation |
|---|---|---|
| R1 | Rough Stroke has no existing route to vector strokes; the right route may touch the column renderer. | Phase 0 spike before any F4 code; F4 is last. |
| R2 | Line rasterisation cost with thousands of lines at 4K. | Lines are sorted across the layout and each pixel visits only those within its reach. Measured: the densest field (20,000 thick wavy lines) samples a 4K frame in about 6 s on one thread at -O2. M1.1 has a cost bound. |
| R3 | Watercolour needs a halo as wide as the bleed, which grows tile memory. | `getMemoryRequirement` and the halo are set from the bleed distance; M3.3 checks a 4K frame. |
| R4 | Noise that changes with tile or zoom would shimmer. | Noise is sampled in camera space on the native grid, as the other cel FX do; the tile-split tests cover it. |

## Out of scope

- 3D shading, feature lines on models and render passes for the GLB Model FX.
- Flat-colour blur, edge lines, colour keying, extension, gradient mapping,
  glow and anti-aliasing: already in the tree.
- CMYK separation: Print Process already does it.
