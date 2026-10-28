# Texture Atlas export

**File > Export > Export Texture Atlas...** packs the drawings of the current
xsheet into one or more atlas images and writes JSON files that describe the
sprites and the animation, in the style of the *Texture Atlas* that Adobe
Animate exports (`spritemap1.png`, `spritemap1.json`, `Animation.json`). Game
engine plugins that read Animate texture atlases can play it. Nothing is added
to scenes.

## Use

The command first checks that the xsheet has something to export, then opens a
small dialog that says how many layers, drawings and symbols (sub-xsheets)
will be exported, and how many FX the exported xsheets have, if any (they
are left out):

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
is written until all of them are rendered and packed. The message at the end
gives the number of sprites, pages, layers and symbols written, and names the
sub-xsheets left out because they are nested too deeply (see Limits).

## What is exported

- **Frames**: the output range of the Output Settings (or the whole xsheet,
  as the Spine export does, when there is no range or when a sub-xsheet is
  being edited). Frame 0 of the files is the first frame of the range. A
  symbol has a timeline of its own (see below).
- **Layers**: the columns of the current xsheet whose render (eye) toggle is
  on and that show a Toonz Vector, Toonz Raster or Raster drawing, or a
  sub-xsheet, in the range. The **first layer of `Animation.json` is the top
  one** (the last column), as in Animate. A layer is named after its column
  (`Col1` ...), made unique with `_2`, `_3` ... if needed.
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
- **Symbols (sub-xsheets)**: each sub-xsheet shown in the range is one symbol
  of the symbol dictionary (`SD`), written once however many columns (or
  other sub-xsheets) show it. Its timeline is made the same way as the main
  one, from the columns of the sub-xsheet (render toggle on, the same level
  types, the top one first, cells read through the time remap), over **all
  the rows of the sub-xsheet**, from row 0 to its last non-empty row (not
  the output range). Its drawings are sprites of the same pages: a drawing
  shown both in the main xsheet and in a sub-xsheet, or in two sub-xsheets,
  is one sprite. Sub-xsheets inside sub-xsheets are nested symbols, up to 16
  levels deep. A symbol is named after its sub-xsheet (`sub1` ...), made
  unique with `_2`, `_3` ..., and never named as the main timeline (`AN.SN`).
  A sub-xsheet that shows nothing exportable is not a symbol: its cells are
  blank frames.
- **Symbol instances**: in the layer of a sub-xsheet column, each row shows
  a frame of the symbol, the cell's frame number minus one (the row of the
  sub-xsheet that the viewer and the render show). A run of rows whose
  symbol frame goes up by one per row is one frame entry that plays the
  symbol from its first frame (`"PO"`, play once), or loops (`"LP"`) when the
  run goes from the last frame of the symbol back to the first one; a run of
  rows that hold one symbol frame is one `"SF"` (single frame) entry. A new
  entry starts where the symbol frame jumps, or where the transform of the
  column changes. A row after the end of the sub-xsheet shows nothing and is
  a blank frame, like an empty cell or a column squashed to nothing.
- **Symbol coordinates**: a symbol is drawn in the frame of the current
  camera of its sub-xsheet, in the pixels of that camera (its resolution
  and DPI), y down from its top left corner, as the main timeline is drawn
  on the stage. The motion of the sub-xsheet camera is baked into the
  transforms of the symbol's layers, row by row, and its Z is a scale, as
  the viewer applies it (1 + Z / 1000). The transform of an instance is the
  placement of the sub-xsheet column relative to the camera of the timeline
  that holds it (as for a drawing), with the center of the sub-xsheet camera
  on the origin of the column: with no transform and two cameras of the same
  size and DPI, it is the identity.

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
          "LN": "Col3",
          "FR": [
            {"I": 0, "DU": 8, "E": [{"SI": {"SN": "sub1", "IN": "", "ST": "G", "FF": 0, "LP": "PO",
                                            "TRP": {"x": 0, "y": 0}, "M3D": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]}}]},
            {"I": 8, "DU": 4, "E": [{"SI": {"SN": "sub1", "IN": "", "ST": "G", "FF": 7, "LP": "SF",
                                            "TRP": {"x": 0, "y": 0}, "M3D": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]}}]}
          ]
        },
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
  "SD": {
    "S": [
      {
        "SN": "sub1",
        "TL": {
          "L": [
            {
              "LN": "Col1",
              "FR": [
                {"I": 0, "DU": 4, "E": [{"ASI": {"N": "C_0001", "M3D": [1,0,0,0, 0,1,0,0, 0,0,1,0, 640,300,0,1]}}]},
                {"I": 4, "DU": 4, "E": [{"ASI": {"N": "C_0002", "M3D": [1,0,0,0, 0,1,0,0, 0,0,1,0, 652,300,0,1]}}]}
              ]
            }
          ]
        }
      }
    ]
  }
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
  element: an `ASI` (atlas sprite instance) with `N`, the sprite name, and
  `M3D`, the transform, or an `SI` (symbol instance, see below).
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
- `SD.S`, the symbol dictionary, lists the symbols (the sub-xsheets), each
  one once, a symbol after the symbols it shows; it is an empty list when
  the xsheet shows no sub-xsheet. `SN` is the symbol name, unique, and `TL.L`
  its layers, written exactly as `AN.TL.L` (the top layer first, frames with
  `I`, `DU` and `E`, `ASI` and `SI` elements): their frames cover the
  symbol's timeline, from frame 0 to the number of rows of the sub-xsheet,
  and their `M3D` place the sprites (and the nested symbols) in the symbol
  coordinates, the frame of the sub-xsheet camera (see above). Layer names
  are unique in each timeline; the same name can be in two timelines.
- `SI`, a symbol instance, has these keys:
  - `SN`: the name of the symbol shown, one of `SD.S`. No symbol shows
    itself, directly or through other symbols.
  - `IN`: the instance name, always `""` (a graphic symbol has none).
  - `ST`: the symbol type, always `"G"` (graphic): the instance plays the
    frames of the symbol in step with the timeline that holds it.
  - `FF`: the first frame, the frame of the symbol (0 based) shown at the
    first frame of the entry (`I`). It is always inside the symbol's
    timeline.
  - `LP`: the loop mode. At the n-th frame of the entry (from 0), `"PO"`
    (play once) shows the frame `FF + n` (it never goes past the last frame
    of the symbol in what is written), `"LP"` (loop) shows the frame
    `(FF + n)` modulo the length of the symbol, and `"SF"` (single frame)
    shows the frame `FF`.
  - `TRP`: the transformation point, always `{"x": 0, "y": 0}`, the top
    left corner of the symbol's frame. It is information only (Animate's
    pivot); `M3D` places the symbol.
  - `M3D`: the transform, as for `ASI`, from the symbol coordinates to the
    coordinates of the timeline that holds the layer: the stage for the main
    timeline, the coordinates of the outer symbol for a nested one. A sprite
    of a symbol is drawn on the stage with the product of the instance
    matrices, from the outermost one in, and of its own matrix.

Not written: frame labels, tweens (`TWN`), color effects, filters, blend
modes, the stage instance of the main symbol (`AN.STI`), movie clip and
button symbols (`ST` other than `"G"`), and instance names.

This format is reverse engineered from the files that Animate writes and from
the plugins that read them; it is not specified anywhere. The key names follow
Animate, but plugins differ in what they need: some read only the sprites and
`ASI` elements (a scene without sub-xsheets writes nothing else), some do not
play nested symbols, or the loop modes, or expect the optimized key names of
other Animate versions. Try it with the plugin of your engine.

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

- **FX are not exported**: the drawings are exported without any effect.
  Column transparency, color filters, Z depth and stacking order changes, and
  Plastic deformations are not exported either, in the main xsheet as in the
  sub-xsheets (the opacity and color effect of a sub-xsheet column are not
  given to its symbol instances). The current xsheet is the main timeline:
  when a sub-xsheet is being edited, it is exported with its own
  sub-xsheets as symbols.
- **Sub-xsheets are followed up to 16 levels deep.** A sub-xsheet nested
  deeper, or one that is inside itself (a broken scene), is left out there
  (blank frames), and the message at the end of the export names it. A
  sub-xsheet is made into a symbol where it is first met (column by column
  from the left, row by row, going into each sub-xsheet as soon as it is
  met): if that place is already deep, its own sub-xsheets can reach the
  limit sooner than from another place that shows it.
- A symbol holds all the rows of its sub-xsheet and every drawing they
  show, even rows that no cell of the main xsheet plays.
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
matrix each (pegbars and camera already combined), sub-xsheets are nested
symbols with timelines of their own, and the drawings are packed together
into a few pages. It is meant for engines with an Animate texture
atlas player, and for simple sprite playback. Both render the drawings with
the same code; the Spine export is not changed.

## Code and tests

- `include/toonz/atlaspacker.h`: the packer, header only and without OpenToonz
  or Qt. Tested by `toonzlib/tests/atlaspacker_test.cpp`: no overlaps,
  everything inside its page, padding kept, same layout for the same input,
  page splitting, oversize and empty rectangles reported, empty input, many
  small sprites, one big with small ones, power of two pages, rotation.
- `include/toonz/atlasjson.h`: the JSON files from plain structures, header
  only (Qt Core), and `instanceRuns()`, which makes the symbol instance
  entries (`FF`, `LP`) from the symbol frame of each frame. Tested by
  `toonzlib/tests/atlasjson_test.cpp`: the files parse back, have the keys
  above, list the sprites of the input, layers cover the timeline without
  gaps or overlaps (unsorted, overlapping and missing frames repaired), the 16
  matrix numbers in the documented places, finite numbers, unique names, and
  the input checks; for symbols, a main timeline with symbol instances, a
  symbol nested in a symbol, the content of `SD.S`, the `SI` keys and values,
  the symbol timelines covering their own length, the first frame of an
  instance cut before frame 0, and the refusal of undefined symbols,
  recursive symbols (direct, through one or two other symbols), duplicate
  or main symbol names and first frames outside the symbol; for
  `instanceRuns()`, playing, held, wrapping, jumping and blank runs, and a
  long mixed sequence played back exactly with the loop modes as documented.
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
