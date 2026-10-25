# Layered PSD export

**File > Export > Export Layered PSD...** renders a range of frames of the
current xsheet and writes one layered Photoshop file for each frame, with one
layer for each top-level column (master plan E2, parity plan Q6). The existing
PSD level writer is unchanged for users: it still writes one flattened file
per frame. Nothing is added to scenes.

## Use

The command opens a small dialog:

- **Save In**: the output folder (created if missing). It is remembered.
- **Name**: the files are named `<name>_0001.psd`, `<name>_0002.psd`, ...,
  numbered by xsheet frame (frame 1 is `_0001`, whatever the first exported
  frame is). The default is the scene name.
- **Frames**: the first and the last frame to export. They default to the whole
  xsheet.
- **Include Hidden Columns**: also export the columns whose render (eye)
  toggle is off. They are rendered anyway and stored as hidden layers. Off by
  default, which skips them.

If some of the files exist, the export asks before overwriting them. A progress
dialog with a Cancel button is shown; the files already written are kept.

## What a file contains

- The **layers**, bottom to top in column order (column 1 is the bottom
  layer). Each layer is the column rendered alone with the scene camera and the
  size of the camera (not the output settings' shrink), so the fxs connected to
  the column, its transforms and the xsheet-level fxs apply, and all layers are
  registered with each other. A column which has no drawing in a frame gives a
  transparent layer, so every file has the same layers.
- The layer **name** is the column name (the stage object name, `Col1` ...).
- A layer is **hidden** (Photoshop's eye off) when the column's render toggle is
  off, which is only possible with Include Hidden Columns.
- The **merged image** of the file (what viewers which ignore layers show) is
  the whole scene rendered as the Render command does, so it also has the
  effects which are not part of a column, and does not have the hidden
  columns.
- The resolution of the scene camera, in pixels per inch.

The render uses the output settings for the quality, the bit depth and the
number of threads (the Dedicated CPUs choice). 8 bit output gives 8 bit layers
(RLE compressed); an output of 64 bit or more gives 16 bit layers (stored raw,
so the files are large).

## Compatibility

- Opt-in: a new command; no existing behaviour, scene or file format changes.
  The columns' render toggles are switched during the render and restored
  afterwards, as the script Renderer does.
- The PSD files use the same structure as those of the single layer writer
  (version 1, RGB with alpha, normal blend mode, layer names in ASCII and
  Unicode); only the layer count differs. They are ordinary PSD files, which
  PSD readers (including OpenToonz as a PSD level) can load.

## Code

- `include/tpsdlayerwriter.h`, `image/psd/tiio_psd.cpp`: `writeLayeredPsd(path,
  merged, layers, dpiX, dpiY)` with `TLayeredPsdLayer` (name, raster, opacity,
  visibility). It is exported by the image library, so the application can call
  it. `TLevelWriterPsd::save()` goes through the same file writer, with one
  layer which is also the merged image, and writes the same bytes as before.
  Checked by `tiio_psd_test` (`BUILD_PSD_TESTS`), which parses the layered
  files independently of the reader: layer records, names, opacity, visibility
  flag, and the exact channel data of the layers and of the merged image, for 8
  bit (RLE) and 16 bit (raw) files.
- `toonz/exportlayeredpsdpopup.cpp`: the dialog and the render. Frames are
  rendered a few at a time (as many as the render threads), once for the whole
  scene and once for each column, with a `TRenderer` and a `TRenderPort` as the
  animated SVG export and the script Renderer do, then written, so only a few
  frames of rasters are in memory.
- The command is `MI_ExportLayeredPSD`, in the File > Export menu and in the
  default `menubar_template.xml`. Menu files which were customized in an older
  profile do not get it automatically; add `MI_ExportLayeredPSD` to the Export
  menu in the menu bar customization.

## Limitations

- Top-level columns only: a sub-xsheet column is one layer. Sound, palette and
  mesh columns are not exported.
- The layers follow the column order. Z depth or stacking order changes in the
  scene are not reflected in the layer order, and the merged image is the one
  that has them.
- Column opacity and color filters are in the layer pixels only when the scene
  renders them (Scene Settings: Enable Column Color Filter and Transparency
  for Rendering). The layer opacity in the file is always 100%, so that the
  layers composite to the render.
- A column which is only an input of another fx is rendered alone, as if it
  were connected to the xsheet. Blend modes of Add, Multiply and the like are
  not stored as PSD blend modes; the layers are normal.
- Layers are full size (no cropping to the drawing), which RLE compression
  keeps small for 8 bit files. A PSD file is limited to 30000 pixels per side.
- Vector columns are rasterized with the render settings; there is no vector
  layer.
