# Light Shading FX

`Light Shading` (`lightShadingFx`, `stdfx/lightshadingfx.cpp`) is the first
version of the Harmony-style Light Shading, Volume, Glue and Shine nodes
(item B6 in `doc/master_plan.md`). It sits next to Tone / Highlight in the
Light category and works on the same principle: it only changes the colour of
the Source and keeps its alpha, so the shading never leaves the drawing.

## Ports

| Port | Use |
|---|---|
| Source | The coloured drawing. |
| Matte | Optional. Limits the shading region (the "Glue"): its alpha takes the place of the Source's alpha as the shape that the light works on, and it multiplies the shading. Without it, the Source's own alpha is the shape. |

## Parameters

| Parameter | Meaning |
|---|---|
| Mode | **Radial**: the light falls off from the Light Point. **Directional**: a light from the Light Angle shades the shape like a rounded volume. |
| Light Point, Light Radius | The light of the Radial mode. The point and the radius have viewer gadgets. The intensity is 1 at the point and 0 at the radius. |
| Light Angle | The direction towards the light in the Directional mode, in degrees (0 is right, 90 is up). |
| Shade Color, Shade Amount | Where the intensity is below the middle, the Source is multiplied with the colour. The amount (0 to 1) is the strength of the deepest band. |
| Highlight Color, Highlight Amount | Where it is above the middle, the colour is screened over the Source ("Shine"). |
| Bands | 1 to 6 flat cel levels on each side of a neutral middle band. 1 gives one shadow shape and one light shape. |
| Softness | The width of the blur between the bands, 0 for a hard cel edge. In the Directional mode it also rounds the volume. |
| Protect Lines | Dark line pixels are left untouched, as in Tone / Highlight. |

## How it works

For every pixel the light intensity I in [0, 1] is computed. 0.5 is neutral.

* Radial: `I = 1 - clamp(distance / Light Radius)`.
* Directional: the "normal" is minus the gradient of the Source's alpha (or
  the Matte's) blurred by a radius of 8 pixels plus the Softness, and `I` is
  `0.5 + 0.5 * N.L`. The edges that face the light are lit, the ones that face
  away are in shadow, and the inside of a shape stays neutral.

`I` is quantised to the bands, the levels are blurred by the Softness (weighted
by the shape, so the border of the shape does not fade them), and the result
multiplies or screens the colour. The band quantisation, the intensities and
the box blur are plain functions in `stdfx/lightshading.h`, tested by
`stdfx/tests/lightshading_test.cpp` (the build line is in its header).

## Limits

* CPU only, 8 and 16 bit rasters.
* The normals come from the alpha of the image only. There is no vector-level
  depth information, so overlapping parts of a drawing are not shaded
  differently, and a thin shape gets weaker shading than a thick one.
* The shading is not split into paint and lines: the lines are protected by
  luminance (Protect Lines), not by style.
* Shine is a highlight band on the light side, not a separate specular model.
* It needs an isotropic camera transform, like Blur, because of the blurs.

## Compatibility

A new FX with new parameters. Scenes that do not use it save and render as
before. A scene that uses it cannot be opened with builds that lack the FX.
