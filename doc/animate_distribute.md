# Distribute to Layers, Distribute to Frames, Convert Selection to Symbol

Three commands that act on strokes selected in the viewer, like the commands
of the same names in Adobe Animate. They split a drawing that holds several
shapes into separate layers (columns), into successive frames, or into a
symbol.

They only create ordinary Toonz Vector levels, columns and sub-xsheet levels,
so scenes and levels are saved exactly as before and open in older builds.

## Where they are

- **Edit > Distribute to Layers**, **Edit > Distribute to Frames** and
  **Edit > Convert Selection to Symbol...**
- The same three commands in the viewer's right-click menu, when strokes are
  selected.
- The shortcut editor, under Edit. They have no default shortcut.

They are enabled only when all of this is true; otherwise they stay greyed out
and do nothing:

- the Selection tool is on a Toonz Vector level, in its standard selection
  mode (the "Selected Frames", "Whole Level" and "Same Style" modes select
  across drawings, and are not supported);
- at least one stroke is selected;
- the drawing is edited from a column of the Xsheet or Timeline, not from the
  Level Strip, where there are no columns;
- the level, the drawing and the column are not locked or read only.

Toonz Raster, Raster and other kinds of level are not supported.

## What goes together: groups

The selected strokes are split into parts, and each command moves one part at
a time:

- the strokes of a **group** are one part and stay together (inside the new
  drawing they are still a group);
- a stroke that is **not in a group** is a part of its own;
- if you have **entered a group** (Edit > Group > Enter Group), the groups
  inside it are the parts, as for the other commands of the selection;
- if only some strokes of a group are selected, only those are moved, as one
  part.

The parts follow the **stacking order** of the drawing. A part is placed by its
lowest stroke, so the part that was at the bottom comes first (the leftmost
new column, the earliest new frame) and the one on top comes last, and inside
a part the strokes keep their order.

## Distribute to Layers

Each part goes to its own new column.

- The new columns are inserted right after the column of the drawing, in
  stacking order, so the columns on the right are on top, as the strokes were.
- Each part is a new Toonz Vector level named after the level of the drawing
  plus a number: `Hero` gives `Hero_1`, `Hero_2` and so on, using the first
  numbers that are free in the scene. Each level has one drawing.
- The palette of each new level is a copy of the palette of the source level,
  with the same styles, so the strokes keep their colours. It is not linked to
  the source palette: editing one later does not change the other.
- A new column looks and moves like the source column: it has the same parent
  (a pegbar, another column or the table), the same position, rotation, scale
  and other values with their animation keys, the same visibility, opacity and
  colour filter. Its name is the name of its level.
- A new column shows its drawing in every row where the source column shows the
  drawing the strokes came from, so what is on screen does not change, apart
  from the stacking: the new columns are above the source column.
- The strokes are removed from the source drawing. Strokes that were not
  selected stay there.

## Distribute to Frames

Each part goes to its own drawing, in successive frames of the same column.

- The **first part stays** in the drawing. Every other part gets a **new
  drawing of the same level**, numbered after the current drawing with the
  first numbers that are free (the drawing `3` of a level that has `1` to `5`
  gets `6`, `7` and so on).
- The new drawings are exposed in the rows right after the current frame, one
  row each, in stacking order. The cells below are moved down to make room, so
  nothing in the column is overwritten.
- The palette is the one of the level, since the drawings are in the same
  level.
- Strokes that were not selected stay in the first drawing.
- With a single part there is nothing to distribute, and a message says so.

## Convert Selection to Symbol

All the selected strokes become a **symbol**: a sub-xsheet level that can be
exposed again and again, as the Convert to Symbol command of the column
selection does.

- The command asks for the name of the symbol first; cancel it to do nothing.
  The name is made unique when it is already used.
- The strokes, with their groups, go to one new Toonz Vector level named like
  the ones of Distribute to Layers (`Hero_1`), with a copy of the palette of the
  source level. The level is the only content of the symbol.
- The symbol is exposed in a new column right after the source column. That
  column has the position, parent and look of the source column, and the
  symbol holds the strokes as they were drawn, so they stay where they were.
- The symbol shows the drawing in the same rows as the source column shows the
  source drawing. If the drawing is first shown at row 10, the first nine
  frames of the symbol are empty.
- The symbol is made without asking questions in the middle of the command. The
  question about pegbars that Collapse asks is not needed, since the new
  column in the symbol is connected to the table.

## Undo

Each command is **one undo step**. Undoing it:

- puts the strokes back in the source drawing, at their old places in the
  stacking order, with their groups;
- removes the new columns, the new cells, the new drawings, and the new levels
  and the symbol from the scene cast.

Redo does it all again.

## Limits

- Only Toonz Vector levels. Raster and Toonz Raster drawings are not
  supported; the commands are disabled on them.
- Fills (the colour of a closed area) belong to the strokes around the area. A
  fill whose strokes are in several parts may be lost: a new level or drawing
  keeps only the areas that the strokes of its own part close, and the source
  drawing loses an area when the strokes that closed it are moved.
- The strokes are removed from the drawing itself. If the same drawing is
  exposed in other columns, they lose the strokes too, and only the column of
  the current frame gets the new columns.
- The new columns are connected to the xsheet node. Effects applied to the
  source column are not copied.
- Column settings that are not listed above, such as a lock or a mask, are not
  copied to the new columns.
- A menu bar loaded from a saved menu file does not have the commands until
  they are added with the menu customisation; the shortcut editor and the viewer
  menu always have them.

## Tests

The part that has no dependency on the application, in
`toonz/sources/toonz/distributeutil.h`, is covered by `distributeutil_test`
(`toonz/sources/toonz/tests/distributeutil_test.cpp`): the split of strokes into
parts (groups kept together, one part for each ungrouped stroke, stacking order,
no strokes), the choice of the next free `name_N` for the new levels, and the
numbers of the new drawings.
