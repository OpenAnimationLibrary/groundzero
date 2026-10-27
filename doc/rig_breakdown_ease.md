# Whole-rig breakdown and ease

Master plan E1, parity plan Q2. The Function Spreadsheet already had Set
Breakdown Key and Ease Presets for the selected channels. These commands do
the same for every animated channel of the selected columns, from the Xsheet
and the Timeline, as one undo step.

Nothing is saved in the scene beyond the keyframes the commands create or
change, so scenes stay readable by older builds. Nothing changes until a
command is used.

## Use

Select cells or whole columns in the Xsheet or the Timeline (or just make a
column current) and set the current frame. The commands are in:

- the cell right-click menu: **Rig Breakdown** and **Rig Ease** submenus;
- the **Cells** menu (full menu bar, the Xsheet room menu bar and the default
  menu bar template): the same two submenus;
- the shortcut and toolbar customization lists (Cells commands), where each
  preset is a separate command, so it can have a shortcut or a command bar
  button.

### Rig Breakdown

Inserts a key at the current frame on every animated channel of the target
objects: the new key's value is the chosen percentage of the way from the
previous key's value to the next one's. The presets are the Function
Spreadsheet's: Favor Previous Key 25% and 33%, Halfway 50%, Favor Next Key 66%
and 75%, Anticipation -10%, Overshoot 110%, and **Custom Rig Breakdown...**
(-100% to 200%, the last value is remembered for the session).

A channel is left unchanged when it has a key at the frame, has no key before
or after it, or the segment there is an Expression, Similar Shape or File one
(a new key would take the segment's data and ignore the breakdown value).

### Rig Ease

Sets the segments that contain the current frame to Ease In / Ease Out (%)
interpolation. If the selection is cells that span several rows, every segment
overlapping those rows is eased instead. On the last key of a curve, the
segment before it is used. The presets are the Function Spreadsheet's: Ease In
/ Ease Out 33% and 50%, Ease Out 50% and 100%, Ease In 50% and 100%, and
**Custom Rig Ease...** (Ease Out and Ease In, their sum at most 100%).

Segments that are Constant (holds), Expression, File or Similar Shape are
skipped, so a rig's holds and driven channels are not overwritten. Segments of
the other types (Linear, Speed In/Out, Ease In/Out, Ease % and Exponential)
are changed.

### Which objects

The target stage objects are the columns of the selected cells or columns; if
nothing is selected, the current column. The toggle **Include Child Objects in
Rig Commands** (on by default, in the Cells menu and in the two submenus of
the cell menu) adds every stage object attached to them, directly or through
other objects: pegbars, other columns and cameras. That makes the selected
column the root of a rig, and the commands act on the whole rig. Turn the
toggle off to act on the selected columns only. The toggle is not remembered
between sessions.

The channels are those of the Function Editor: Rotation, X, Y, Z, SO, Scale H,
Scale V, Global Scale, Shear H, Shear V, Rotation X, Rotation Y and Path
position.

## How it is built

- The math is not duplicated: `KeyframeSetter::insertBreakdown` and
  `KeyframeSetter::setSegmentEase` (toonzlib, `doubleparamcmd.h`) were moved
  out of `FunctionSelection::insertBreakdowns` / `setSegmentEase`, which now
  call them, so the Function Spreadsheet behaves as before.
- `toonz/rigeasecommand.h` has the single list of presets;
  `toonz/rigeasecommand.cpp` has the target search and the command handlers
  (`MenuItemHandler`s, one per preset). `mainwindow.cpp` defines the actions
  and `menubar.cpp`, `menubar_template.xml` and the cell menu in
  `xshcellviewer.cpp` use the same table.
- Each command runs inside one `TUndoManager` block, so one Undo reverts it.
  The last entry of the block refreshes the Xsheet and the Timeline when the
  block is undone or redone.

## Compatibility

- No change to the scene format. The keys are ordinary keyframes.
- Existing menu bar files saved by the user do not list the new commands (the
  Customize Menu Bar popup can add them). The default template and the built-in
  menu bars do.
- Other rooms' own `menubar_template.xml` files are unchanged.

## Not done

- Only the current frame is used for the breakdown. There is no "breakdown on
  every selected row" and no viewer or toolbar button; a command bar can take
  the commands as they are separate commands.
- The Plastic skeleton deformation channels, the offset-layer curves and the
  published channels of Master Controllers are not edited; only the stage
  object channels above are.
- The Include Child Objects state is per session.
- The labels of the new commands are not in the `.ts` files yet (translations
  are plan item D5).
