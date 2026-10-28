# Texture Atlas export

**File > Export > Export Texture Atlas...** packs the drawings of the current
xsheet into one or more atlas images and writes JSON files that describe the
sprites and the animation, in the style of the *Texture Atlas* that Adobe
Animate exports (`spritemap1.png`, `spritemap1.json`, `Animation.json`). Game
engine plugins that read Animate texture atlases can play it. Nothing is added
to scenes.

## Use

The command first checks that the xsheet has something to export, then opens a
small dialog that says how many layers and drawings will be exported, and
whether the scene has sub-xsheet columns or FX (they are left out):

- **Maximum Page Size**: 1024 x 1024, 2048 x 2048 (default) or 4096 x 4096.
  A page is as large as its sprites need, up to this size; when the sprites
  do not fit, more pages are written.
- **Padding (pixels)**: the empty pixels between two sprites of a page (0 to
  32, default 2). It keeps texture filtering from bleeding one sprite into the
  next. Sprites can touch the border of the page.
- **Scale**: 100% or 50%. At 50% the sprites are stored at half size, and the
  transforms in `Animation.json` scale them back up, so they keep their size
  on the stage (and look softer).

**Export...** then opens the standard folder dialog. If the folder already has
`Animation.json` or `spritemap<N>.json` / `spritemap<N>.png` files, the export
asks before overwriting them; pages of an earlier export with a higher number
than this one writes are removed (after that question), so that the folder
holds a single atlas. The settings and the folder are remembered. A progress
dialog with a Cancel button is shown while the drawings are rendered; nothing
is written until all of them are rendered and packed.

## What is exported

- **Frames**: the output range of the Output Settings (or the whole xsheet,
  as the Spine export does, when there is no range or when a sub-xsheet is
  being edited). Frame 0 of the files is the first frame of the range.
- **Layers**: the columns of the current xsheet whose render (eye) toggle is
  on and that show a Toonz Vector, Toonz Raster or Raster drawing in the
  range. The **first layer of `Animation.json` is the top one** (the last
  column), as in Animate. A layer is named after its column (`Col1` ...),
  made unique with `_2`, `_3` ... if needed.
- **Sprites**: each drawing (level and frame number) shown in the range is
  one sprite, even if several columns or frames show it. It is rendered at
  the camera resolution, as the Spine export renders its images (vector
  drawings rasterized, raster drawings resampled from the level DPI to the
  camera DPI, Raster level "White as transparent" and "Premultiply" applied),
  and trimmed to its visible pixels: a sprite is the smallest rectangle that
  holds the pixels that are not fully transparent. A blank drawing gives no
  sprite. A sprite is named `<level name>_<frame number>` (for example
  `A_0001`), with any character other than letters, digits, `_` and `-`
  turned into `_`, and made unique (ignoring case) with `_2`, `_3` ...
- **Timeline**: the cells of a column are read through its time remap, as the
  render reads them. A run of frames that shows the same drawing with the same
  transform becomes one frame entry with its duration. An empty cell, or a
  column squashed to nothing (scale 0), is a blank frame. The transform is
  baked frame by frame: when the column moves during a held drawing, every
  frame of the move is its own entry.
- **Transform**: the placement of the column (with its pegbars, hooks and
  everything else that the render follows) relative to the camera of that
  frame, combined with the trim offset of the sprite and with the scale, so
  that the sprite lands exactly where the render shows the drawing.

The **stage** of the files is the camera frame: its size is the camera
resolution in pixels, x goes right and y goes down from the top left corner
of the camera. The camera may move: the transforms are relative to it.

## Files

All files are UTF-8 JSON, compact (on one line), like Animate's. The keys of an
object may come in any order (Qt writes them sorted by name). Numbers are
finite; positions are rounded to 1/1000 pixel and the other matrix values to
1e-6.

### `spritemap<N>.json` (one per page, N from 1)

```json
{
  "ATLAS": {
    "SPRITES": [
      {"SPRITE": {"name": "A_0001", "x": 0, "y": 0, "w": 120, "h": 86, "rotated": false}},
      {"SPRITE": {"name": "A_0002", "x": 122, "y": 0, "w": 64, "h": 64, "rotated": false}}
    ]
  },
  "meta": {
    "app": "<application name>",
    "version": "<application version>",
    "image": "spritemap1.png",
    "format": "RGBA8888",
    "size": {"w": 186, "h": 86},
    "resolution": "1"
  }
}
```

- `SPRITES`: every sprite of the page, each wrapped in an object with the one
  key `SPRITE`. `x`, `y` are the top left corner of the sprite in the page
  image, in pixels, y down; `w`, `h` its size in the page. `rotated` is always
  `false` (the packer can turn sprites by 90 degrees, but the export never
  asks it to; if it did, `true` would mean the sprite is stored turned 90
  degrees clockwise and `w`, `h` would be the turned size).
- `meta.image` is the page image next to the file, `size` its size in pixels.
- `meta.resolution` is the scale of the sprite pixels as a string: `"1"` at
  100%, `"0.5"` at 50%. It is information only: the matrices of
  `Animation.json` already make up for the scale. A plugin that applies the
  resolution itself would draw 50% atlases twice as large.
- A sprite name is unique over all the pages.

### `spritemap<N>.png`

The page image: 8 bit RGBA, transparent where there is no sprite. The sprite
pixels are copied into the page, not blended. OpenToonz works with
premultiplied alpha and the PNG is written with straight (not premultiplied)
alpha, as PNG requires; semi-transparent edges keep their color. An engine
that premultiplies textures on import (most do) gets the right result.

### `Animation.json`

```json
{
  "AN": {
    "N": "<scene name>",
    "SN": "<scene name>",
    "TL": {
      "L": [
        {
          "LN": "Col2",
          "FR": [
            {"I": 0, "DU": 3, "E": [{"ASI": {"N": "A_0001", "M3D": [1,0,0,0, 0,1,0,0, 0,0,1,0, 900,497,0,1]}}]},
            {"I": 3, "DU": 2, "E": []},
            {"I": 5, "DU": 7, "E": [{"ASI": {"N": "A_0002", "M3D": [1,0,0,0, 0,1,0,0, 0,0,1,0, 928,508,0,1]}}]}
          ]
        },
        {"LN": "Col1", "FR": [{"I": 0, "DU": 12, "E": [{"ASI": {"N": "B_0001", "M3D": [...]}}]}]}
      ]
    }
  },
  "MD": {"FRT": 24, "W": 1920, "H": 1080},
  "SD": {"S": []}
}
```

- `AN.N` is the scene name (`untitled` for a new scene) and `AN.SN`, the name
  of the main timeline's symbol, is the scene name too.
- `AN.TL.L` is the list of layers, the top one first. `LN` is the layer name.
- `FR` is the list of frame entries of the layer: `I` is the first frame (0
  based), `DU` the number of frames. The entries follow each other without
  gaps or overlaps, from frame 0 to the end of the range: every layer has the
  same length, the number of frames of the range.
- `E` is the list of elements of the entry: empty for a blank frame, else one
  `ASI` (atlas sprite instance) with `N`, the sprite name, and `M3D`, the
  transform.
- `M3D` is a 4 x 4 matrix as 16 numbers in column-major order:
  `[a, b, 0, 0,  c, d, 0, 0,  0, 0, 1, 0,  tx, ty, 0, 1]`. A sprite pixel at
  (x, y), measured in the stored sprite from its top left corner, y down, is
  drawn on the stage at (a x + c y + tx, b x + d y + ty). With no transform
  (and 100% scale), `a = d = 1`, `b = c = 0` and (`tx`, `ty`) is where the top
  left corner of the trimmed sprite goes on the stage. At 50%, `a` and `d`
  are about 2: each one is the exact ratio of the rendered size to the stored
  size of the sprite.
- `MD.FRT` is the frame rate of the Output Settings. `MD.W` and `MD.H` are the
  stage size (the camera resolution).
- `SD.S`, the symbol dictionary, is empty: no symbol is exported. It is
  written so that readers which look it up find it.

Not written: frame labels, tweens (`TWN`), color effects, filters, blend
modes, the stage instance of the main symbol (`AN.STI`) and nested symbols
(`SI`).

This format is reverse engineered from the files that Animate writes and from
the plugins that read them; it is not specified anywhere. The key names follow
Animate, but plugins differ in what they need: some read only the sprites and
`ASI` elements (which is all that is written), some expect symbols in `SD`, or
the optimized key names of other Animate versions. Try it with the plugin of
your engine.

## How the sprites are packed

`include/toonz/atlaspacker.h` packs rectangles with the MaxRects algorithm
(best short side fit), biggest side first. It is deterministic: the same
scene and settings always give the same pages. When everything left fits in
one page, the smallest square page that holds it is searched for, and every
page is then cropped to the area its sprites use, so a small scene gives a
small page. A drawing larger than the maximum page size stops the export with
a message naming it: choose a larger page size or the 50% scale. The packer
also has a power of two page size option and the 90 degree rotation, which
the export does not use.

## Limits

- **Sub-xsheets and FX are not exported**: a sub-xsheet cell is a blank frame,
  and the drawings are exported without any effect. Column transparency,
  color filters, Z depth and stacking order changes, and Plastic deformations
  are not exported either. Only the current xsheet is exported.
- **Vector drawings are rasterized at the camera resolution.** A column scaled
  up in the scene shows the same pixels scaled up: it blurs, where the render
  draws the vectors at the final size. Draw at the size used, or with the
  camera at the resolution needed.
- A drawing is one sprite at one size, whatever the scale it is shown at.
- Shown drawings only: a frame of a level that no cell of the range shows is
  not exported.
- Pixels are premultiplied while the export works on them and written as
  straight alpha in the PNG (see above).
- Engine plugins differ (see above).

## How it differs from the Spine export

The **Export to Spine JSON** command (File > Export) writes a skeleton: one
bone per pegbar and column, with the motion as animated bones that a Spine
runtime interpolates, one PNG per drawing and a simple `.atlas` file (one page
per image), plus mesh attachments for Plastic deformations. The Texture Atlas
export has no bones: each layer is a flat list of frames with one final
matrix each (pegbars and camera already combined), and the drawings are packed
together into a few pages. It is meant for engines with an Animate texture
atlas player, and for simple sprite playback. Both render the drawings with
the same code; the Spine export is not changed.

## Code and tests

- `include/toonz/atlaspacker.h`: the packer, header only and without OpenToonz
  or Qt. Tested by `toonzlib/tests/atlaspacker_test.cpp`: no overlaps,
  everything inside its page, padding kept, same layout for the same input,
  page splitting, oversize and empty rectangles reported, empty input, many
  small sprites, one big with small ones, power of two pages, rotation.
- `include/toonz/atlasjson.h`: the JSON files from plain structures, header
  only (Qt Core). Tested by `toonzlib/tests/atlasjson_test.cpp`: the files
  parse back, have the keys above, list the sprites of the input, layers cover
  the timeline without gaps or overlaps (unsorted, overlapping and missing
  frames repaired), the 16 matrix numbers in the documented places, finite
  numbers, unique names, and the input checks.
- Both tests are built with `BUILD_ATLAS_TESTS` (on with `BUILD_TESTS`).
- `toonz/textureatlasexport.cpp`: the command `MI_ExportTextureAtlas`, in the
  File > Export menu and in the default `menubar_template.xml` (a menu bar
  customized in an older profile does not get it automatically). It renders
  the drawings with the function of the Spine export, which
  `toonz/spineexport.cpp` now also makes available as
  `renderTrimmedDrawing()` (declared in `toonz/textureatlasexport.h`).
- The dialog settings are stored as user settings
  (`TextureAtlasExportPageSize`, `TextureAtlasExportPadding`,
  `TextureAtlasExportScale`, `TextureAtlasExportFolder`), not in scenes.
