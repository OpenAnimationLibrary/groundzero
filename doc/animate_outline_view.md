# Outline View of the columns

A column can be switched to **Outline View**, as in the layer outline mode of
other animation programs. In the viewer its drawings are shown as thin coloured
outlines instead of filled art, so that the shapes of several overlapping
columns can be seen at once. Each outlined column gets its own colour.

It is a viewer setting only. The Flipbook, the preview and the render are not
changed, and the levels and palettes are never modified.

## Using it

Right-click a column header and choose **Toggle Outline View**. The command
works on the selected columns, or on the current column when none is selected.
If every one of them is already an outline they go back to normal, otherwise
all of them become outlines. It can be undone.

The command is `MI_ToggleColumnOutline`, so it appears in the shortcut editor
(Right-click Menu Commands) and can be given a shortcut. There is no header icon
in this version.

## What it does

| Column content | In Outline View |
|---|---|
| Vector levels | Only the strokes are drawn, in the outline colour. The filled regions are not drawn. |
| Toonz Raster levels | Only the inks are drawn, in the outline colour. The paints are not drawn. |
| Sub-xsheet columns | The drawings inside the sub-xsheet are shown as outlines, all in the colour of the column. |
| Full-colour raster levels, mesh columns, plastic-deformed drawings, other columns | Unchanged, they are still drawn as usual. |

The outline colour comes from a fixed palette of eight colours (red, blue,
green, orange, purple, teal, pink, olive), picked from the index of the column:
the first column is red, the second blue, and so on, starting again after the
eighth. The colours are mid tones that can be read on light and dark viewer
backgrounds. The colour is not saved. Moving a column changes its colour.

Other details:

- The column opacity and the light table fade still apply. The column colour
  filter does not: the outline colour is used as it is.
- The onion skins of an outlined column are also drawn as strokes or inks only,
  but keep the usual onion skin colours.
- The Toonz Raster palette used for the outline is a copy made for drawing; with
  the GPU compositing of raster levels on, the outline version of a drawing is
  cached apart from the normal one.
- The viewer shows the outlines while playing as well.

## Limitation

Full-colour raster (bitmap) levels cannot be shown as outlines: they have no
strokes or inks to keep. Columns of these types, and mesh columns, can still
be toggled, but nothing changes on screen. The 3D camera view of the viewer does
not use Outline View either.

## File format

Outline View is one bit (0x100) of the column status word, which is saved with
every column already (it holds the lock, the camstand and preview toggles, the
mask, the selection lock and the light table exclusion). The word is the same
as before for a column that is not an outline, so a scene that does not use the
feature is saved exactly as before. Older builds ignore the unknown bit: they
open the scene and show the column as usual. Nothing else is added to the scene
or level files.

## Code

- `TXshColumn::isOutlineView()` and `setOutlineView()` (`toonzlib/txshcolumn.cpp`).
- `include/toonz/outlinecolors.h` has the palette and the index to colour
  function, with no GUI dependency.
- `Stage::VisitArgs::m_outlineView` (set by the viewer only) makes
  `toonzlib/stage.cpp` fill `Stage::Player::m_outlineView` and `m_outlineColor`
  from the column. `toonzlib/stagevisitor.cpp` uses them in the vector drawing
  (strokes only, a colour function with the outline colour) and for Toonz
  Raster images (inks only, on a copy of the palette).
- The command is in `toonz/columncommand.cpp`, with the menu entry in
  `toonz/xshcolumnviewer.cpp`.
- The test is `toonzlib/tests/outlineview_test.cpp`: the palette (stable, in
  range, eight distinct colours, negative and large indices) and the status bit
  (other bits kept, clone and reload).
