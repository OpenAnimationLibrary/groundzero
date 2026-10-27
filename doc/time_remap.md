# Time remap (Retime / Hold / Loop)

A level column can be retimed with an animatable **time remap** curve (master
plan B7, parity plan Q9). The curve tells, for each frame of the output, which
frame of the column is shown there. Hold, Loop and Reverse are presets that
fill the curve.

Nothing changes for a scene that does not use it: a column without a remap
reads, renders and saves exactly as before.

## The curve

- The curve is the parameter **Time Remap** of the column fx of a level column
  (`TLevelColumnFx`), next to a hidden on/off flag. It is listed in the
  Function Editor under the column fx, named by its column (for example
  `Col1`), only for the columns that use the remap.
- The **value** of the curve at a frame is the number of the xsheet frame whose
  cell is shown at that frame, as numbered in the xsheet (1 is the first row).
  The identity is therefore the line that goes through (frame 1, value 1),
  (frame 2, value 2), and so on. The value is rounded to the nearest frame.
- The curve is applied inside the span of its keys only: before the first key,
  and after the last key unless the curve is **cycled** (Cycle in the
  Function Editor), the frames are not remapped. A cycled curve repeats its span
  indefinitely, which is what Loop uses. A remap without keys, or switched off,
  does nothing.
- Any interpolation of the Function Editor can be used (linear, ease, speed,
  expression, similar shape, ...), so a column can be slowed down, accelerated
  or played with a custom timing. Expression and Similar Shape segments use the
  grammar of the xsheet, like the stage object channels: the column gets it
  when it is inserted in an xsheet or loaded with the scene
  (`TXshLevelColumn::setXsheet`).

## Presets

They are in the **Cells > Time Remap** menu, in the column header context menu
(**Time Remap**), and can be given shortcuts. Each preset replaces the curve of
the column with linear keys and switches the remap on. A command is one undo
step, whatever the number of columns. Locked columns and the columns that are
not level columns are skipped.

| Command | With selected cells | With selected columns |
|---|---|---|
| **Time Remap: Hold** | Shows the cell at the first selected row along all the selected rows | Shows the cell at the current frame from the current frame to the end of the cells |
| **Time Remap: Loop** | Repeats the selected rows from the first selected row on (cycled curve) | Repeats all the cells of each column |
| **Time Remap: Reverse** | Plays the selected rows backwards | Plays all the cells of each column backwards |
| **Clear Time Remap** | Switches the remap off and removes the keys | The same |

The rows outside the span of the preset are not remapped. The curve can be
refined afterwards in the Function Editor. The loop frame math and the keys of
the presets are in `include/toonz/timeremap.h` (header only, tested by
`toonzlib/tests/timeremap_test.cpp`).

## Where it is applied

- **Render and preview**: the level column fx looks up the cell through the
  curve in every place it reads the cell of a frame (image, bounding box,
  cache alias, palette, dpi), and so do the sub-xsheet time shuffle and the
  Plastic deformer texture lookups (`TXshLevelColumn::getRemappedCell`).
- **Viewer** (`StageBuilder::addCell`, one place): the other columns are drawn
  through the remap too, so the viewer and flipbook playback match the render.
  The **current column** is always drawn with its xsheet timing, so that what
  is drawn on is what is seen; select another column to see the remap, or
  preview.
- **The Xsheet is not changed**: the cells, the xsheet thumbnails and the cell
  area keep showing the original timing. The remap is not a cell edit, and the
  tools keep working on the xsheet cells.

## Saved data

The two parameters (`timeRemap`, `timeRemapEnabled`) are written in the
`<params>` of the column fx only when the remap is used (switched on, or the
curve has keys or is cycled); otherwise the scene file is identical to what
older builds wrote. This uses the new `TFx::isParamSaved` hook.

Older builds can open a scene that uses the remap (they skip the unknown
parameters) but ignore the remap and show the original timing.

## Limitations

- The scene length is given by the cells. A loop that goes beyond the last
  cell, or a remap that is longer than the cells, needs cells (or an output
  range) to extend the scene.
- The remap is per level column. Sound, palette, Zerary and mesh columns have
  none, and the timing inside a sub-xsheet cell is not remapped (the cell of
  the sub-xsheet column is: the remapped cell then selects the frame of the
  sub-xsheet).
- Presets replace the curve; they do not compose with an existing remap.
- The column fx type of the first cell is used for the render-type decisions
  (`canHandle`, `handledAffine`), so mixing level types in one remapped column
  is as limited as before.
- Envelope bake and the Plastic mesh lookups use the xsheet timing.
