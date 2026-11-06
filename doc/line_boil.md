# Line Boil

Master plan B2 and parity plan D6, "animated pencil texture".

A Texture style draws its texture along the lines of vector drawings, but the
texture stayed in the same place on every frame. Hand-drawn pencil lines
change from one drawing to the next; Harmony animates the pencil texture for
that.

Two settings of the Texture style, in the Style Editor's settings page:

- **Line Boil** (0 to 1, 0 by default): how far the texture moves on each
  frame, as a fraction of the texture's size, along and across the lines. At
  0 the texture stays put, as before.
- **Boil Hold** (1 to 24 frames, 1 by default): how many frames the texture
  keeps its place, for lines animated on twos or threes.

The move is a pseudo-random function of the scene frame, so a frame always
renders the same way, in the viewer and in the render. The drawings
themselves do not change. Region fills are not moved.

## Saving

The settings are written only when they are not the defaults, where older
versions skip them: as the `textureBoil` and `textureBoilHold` attributes of
the palette's `style` tag, and in PLI files as tagged values after the
style's record (`style-setting-v1:textureBoil`), like the Trail settings.
Older versions load such a palette with a still texture.

## Code

- `TVectorRenderData::m_frame`: the scene frame drawn, set by the viewer
  (`toonzlib/stagevisitor.cpp`) and the render (`TLevelColumnFx`); -1 when
  unknown, as for icons.
- `TOutlineStyle::drawStrokeAtFrame()`: the outline stroke drawing with the
  frame (`OutlineStrokeProp::draw()`); styles that do not change with the
  frame draw as `drawStroke()`.
- `TColorStyle::getExtraSettingNames()`, `getExtraSetting()` and
  `setExtraSetting()`: settings saved outside a style's data, in palettes
  (`tpalette.cpp`) and PLI files (`tiio_pli.cpp`).
- `TTextureStyle::boilShift()`: the move at a frame, applied to the texture
  coordinates of the line's outline.
