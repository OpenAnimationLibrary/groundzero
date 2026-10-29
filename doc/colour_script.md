# Colour Script

The **Colour Script** FX shifts the colours of a whole scene for the colour
script of a film: dusk, night, a flashback, a mood that changes across the
cuts. It is in the Image Adjust category of the Add FX menu (`STD_colourScriptFx`).

Place it between the Xsheet node and the Output so that every column goes
through it, or after only some columns.

## Settings

| Setting | Range | What it does |
|---|---|---|
| Hue | -180..180 degrees | Turns every hue by the angle. |
| Saturation | -100..100 | -100 is grey, 0 unchanged, 100 fully saturated; in between each colour's saturation moves that share of the way. |
| Lightness | -100..100 | The same, from black to white. |
| Keep Styles | text | Styles whose name starts with it keep their colours. Empty keeps none. |

Every numeric value can be animated. Keys at the first frame of each cut give
each cut its own look; keys further apart blend from one look to the next. A
look is kept for other scenes as an ordinary FX preset.

## How the colours change

- **Toonz Vector and Toonz Raster levels**: the shift is made on a copy of the
  palette when the column is rendered, through the same render data as the
  Colour Override FX (`ColourOverrideFxRenderData`, which now carries a shift
  besides its per-style overrides). Every colour of every style changes, the
  level's palette is never modified, and lines and fills stay clean. The
  overrides of a Colour Override in the same chain are applied first.
- **Full-colour levels** (images, raster drawings) have no palette: their pixels
  are shifted when the column is rendered, in 8-bit, 16-bit or float.
- **Keeping colours**: name a character's styles with a prefix (for example
  `KEEP Skin`) and type the prefix in Keep Styles, or right-click a column header
  and choose **Toggle Keep Colours** to keep the whole column. The column flag is
  a bit of the column status, saved only when set.
- **Viewer**: with the preference that shows the Colour Override in the viewer
  on, the viewer also shows a Colour Script after the Xsheet node on palette
  levels. Full-colour levels show the shift in the preview and render only.

## How it works

The math (`toonz/palettetransform.h`) converts each colour to hue, saturation
and lightness, turns and moves them, and converts back. It is tested by
`toonzlib/tests/colourscript_test.cpp` together with the render data that
applies it to a palette.

Not in this version: a LUT mode (the Lut3D Bake FX covers `.cube` and `.3dl`
files) and a From/To/Mix of two presets; animating the three values covers
blending between looks.
