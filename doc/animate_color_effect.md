# Column Color Effect (Brightness, Tint, Alpha, Advanced)

Each level column has a **Color Effect**, like the Color Effect of a symbol in
Adobe Animate: Brightness, Tint, Alpha and Advanced (a multiply and an offset
per channel). It needs no FX node, it is animatable, and it shows in the viewer
and in the render.

Nothing changes for a scene that does not use it: such a scene draws, renders
and saves exactly as before.

## Setting it

Open the column settings of a column header (its config button, where the
Opacity and the color Filter of the column are) and press **Color
Effect...** (enabled for level columns). The dialog edits the column it was
opened from:

| Field | Range | No effect | What it does |
|---|---|---|---|
| Brightness | -100..100 % | 0 | Positive values blend the colors towards white, negative values scale them towards black. The alpha is not changed. |
| Tint | a color | (any, with amount 0) | The color the drawing is tinted with. |
| Tint Amount | 0..100 % | 0 | Blends the colors towards the tint color: 100 % gives the tint color itself. The alpha is not changed. |
| Alpha | 0..100 % | 100 | Scales the opacity. 0 makes the column fully transparent. |
| Advanced: Multiplier (red, green, blue, alpha) | -100..100 % | 100 | Multiplies the channel. |
| Advanced: Offset (red, green, blue, alpha) | -255..255 | 0 | Adds to the channel. |

The Advanced section is shown with **Advanced...** (it opens by itself when it
is used). **Reset** removes the whole effect of the column (one undo).

- Without keys, a value applies to the whole column.
- With **Set Key at the Current Frame** on, a change sets a key at the current
  frame. A value that already has keys is always keyed at the current frame.
- Every change can be undone; a slider drag is one undo.
- The values can also be animated in the **Function Editor**: once the effect
  is used, the column fx is listed there by the name of its column (for
  example `Col1`), with Brightness, Tint Color, Tint Amount, Alpha and the
  eight Advanced channels (and the Time Remap curve, which belongs to the same
  fx). Any interpolation, expression or cycle can be used.

## The math

The values at a frame make one multiply `m` and one offset `c` per channel,
applied to the straight (not premultiplied) colors in the 0..255 range:

    out = clamp(m * in + c, 0, 255)

The effects are combined in this order, each one working on the result of the
previous one, and clamped once at the end:

1. **Advanced**: `m = multiplier / 100`, `c = offset`, for red, green, blue and
   alpha.
2. **Tint**: for red, green and blue, `m = 1 - amount`, `c = tint * amount`.
3. **Brightness** `b`: when positive, `m = 1 - b` and `c = 255 * b` (towards
   white); when negative, `m = 1 + b` (towards black).
4. **Alpha**: the alpha multiply and offset are scaled by `alpha`, so Alpha 0
   is fully transparent whatever comes before.

(`amount`, `b` and `alpha` are the percentages divided by 100.) So a full tint
covers an Advanced offset, and the brightness lightens or darkens the tinted
color. The math is in `toonz/sources/include/toonz/columncoloreffect.h`,
tested by `toonzlib/tests/columncoloreffect_test.cpp`.

On premultiplied pixels (raster drawings in the viewer, and the render), each
pixel is unpremultiplied, transformed and premultiplied again. A fully
transparent pixel stays fully transparent: an alpha offset makes the drawn
pixels more or less opaque, it does not fill the empty areas (as on vector
drawings, where only the drawn colors change).

The effect comes after what the column already does: in the viewer after its
opacity and its color filter, in the render after the color filter (when the
scene setting that renders it is on).

## Where it shows

- **Viewer**, for Toonz Vector drawings (the colors of the strokes and fills
  are transformed), Toonz Raster drawings and raster (full color) drawings
  (each drawing is put on a layer of its own, which gets the effect and is
  then composited). The current frame of the current column shows it too,
  also while onion skin is on.
- **Sub-xsheets**: the effect of a sub-xsheet column applies to all its
  contents, after the effects of the columns inside it. The viewer applies it
  to each drawing of the sub-xsheet, the render to the composited sub-xsheet:
  where drawings of a sub-xsheet overlap with partial transparency, the viewer
  is an approximation (as for the opacity of a sub-xsheet column).
- **Render and Preview**: always, whatever the "column color filter in render"
  scene setting. The render adds a hidden `ColumnColorEffectFx` after the
  column (after its dpi and Plastic deformation), with the values of the
  frame, so that each frame is computed and cached with its own values. It
  works in the color space of the colors (not in linear light), on 8 and 16 bit
  tiles.
- **Scene icons** are drawn as the viewer draws.

Not shown (unchanged):

- the onion skins and the Shift and Trace ghosts of the column (they keep the
  onion skin colors, as they do with the color filter);
- Plastic deformed columns in the viewer, the 3D view, and the Flipbook of a
  level (a level has no column there; a preview or a rendered output shows the
  effect);
- with "GPU Compositing in the Viewer", the raster drawings that have an
  effect are composited by the CPU path (the other ones keep the GPU path), so
  no texture is ever cached with an effect;
- with the darken blended view, a drawing with an effect is composited with a
  normal "over".

## Saved data

The values are twelve parameters of the column fx of the level column
(`TLevelColumnFx`): `brightness`, `tintColor`, `tintAmount`, `alpha`,
`redMul`, `greenMul`, `blueMul`, `alphaMul`, `redAdd`, `greenAdd`, `blueAdd`,
`alphaAdd`, written in the `<params>` of the column fx, like the time remap.
Each one is written **only when it is not at its default** (it has keys, or a
value other than the one without effect); the others are not written at all.
A column that does not use the effect is saved byte for byte as before, and
no new tag is added to the column itself (the column loader rejects unknown
tags).

Older builds can open a scene that uses the effect: when the fx loader
(`TFx::loadData`) meets a parameter name the fx does not have, it skips the
whole parameter (`skipChild`) instead of failing. This has been so since the
first version of the code, so every build reads such scenes; they just ignore
the effect and show the column without it.

## Limitations

- One effect per column; the effect of a column is not inherited by the
  columns parented to it (only by the contents of a sub-xsheet column).
- The tint color is a plain color (its alpha is not used).
- The multipliers stop at -100..100 %, the offsets at -255..255, as in
  Animate.
- Palette, sound, mesh, camera and FX columns have no Color Effect.
