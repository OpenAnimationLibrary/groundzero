# Rig Mapping

Like Adobe Animate's Rig Mapping: you have two characters rigged with the same
kinds of parts (both with columns or pegbars named Head, Arm_L, Arm_R, Leg_L
and so on). **Rig Mapping...** copies the animation of the parts of one
character, the *source rig*, onto the parts of the other one with the same
names, the *target rig*.

It only writes keyframes, with the existing keyframe data, so nothing new is
saved in the scene and scenes stay readable by older builds. The three options
of the dialog are user settings, not scene data. One Undo reverts the whole
command.

## Use

1. Select the roots of the two rigs, the source first:
   - in the Stage Schematic, click the source column or pegbar, then
     Ctrl+click the target one;
   - or, in the Xsheet or the Timeline, click the header of the source
     column, then Ctrl+click the header of the target column;
   - or select cells in exactly two columns: then the left one is the source.

   The selections do not keep the order of the clicks, but a click makes the
   object current, so of two objects selected by clicking them the current
   one (the last clicked) is the target. Otherwise the one on the left is the
   source.

   Each rig is the selected object with every column and pegbar attached to
   it, directly or through other ones. The two rigs must not share an object.
2. Choose **Cells > Rig Mapping...** (also at the end of the cell right-click
   menu, after Rig Ease, and in the shortcut and command bar lists). With
   another selection, a message says what to select.
3. The dialog shows the two rigs (the source is named first) and a row for
   each part of the source rig:
   - **Source Part**, with a tick: only ticked rows are copied;
   - **Target Part**: the part proposed, which you can change in the list, or
     set to (None);
   - **Match**: how the pair was found (see below), or "Chosen by hand";
   - **Animated Curves**: how many curves of the source part have keys (the
     Plastic ones included). A part without keys has nothing to copy.

   **Swap Source and Target** exchanges the two rigs if they came in the wrong
   order. A target part can be chosen in one row only.
4. Choose the options and click **Apply**.

## Matching rules

The names are those of the Xsheet column headers and the Stage Schematic
nodes: the name you gave the column or pegbar or, for a column you did not
name, the name of its level if it has a single one.

The pairs are found in this order. A part is never in two pairs, on either
side.

1. The two roots go together, whatever their names: they are the rigs you
   chose.
2. **Same name**: the same name, and no other part of either rig has it.
3. **Same name (case or separators differ)**: the case does not matter, and
   `_`, `-`, `.` and spaces are all the same, or nothing: "Arm_L", "arm l",
   "ARM-L" and "ArmL" match.
4. **Same name (number ignored)**: as 3, without a number at the end: "Arm_L_1"
   matches "arm l", "Head2" matches "Head". If two parts of a rig give the same
   name ("Leg_1" and "Leg_2"), neither is matched this way.
5. The names are tried again among the children of each pair found so far, so
   two parts with the same name are matched if their parents are: the "Hand"
   of Arm_L goes to the "Hand" of the other Arm_L.
6. **By position**: the children of a pair that are still unmatched go
   together in their order (columns from left to right, then pegbars), but
   only if there are as many on each side. With more on one side nothing says
   which are the extra ones, and they stay unmatched. A pair found this way
   between two parts that both have names of their own, which differ, starts
   unticked.

Names that the application makes up are not names: "Col3", "Peg2", "Camera1",
"Table" and the "Vertex_003" of Plastic vertices are never used to match by
name, since "Col3" of one rig is not the "Col3" of another. Such parts can
still be matched by position.

## Options

- **Copy keys relative to the target's rest values** (default): each target
  curve starts from its own value at the frame of the first source key, and
  gets the change of the source since its first key. For positions, angles,
  depth, stacking order, shear and the Plastic curves the change is added: a
  source arm going from 10 to 40 degrees moves a target arm resting at 5 to
  35. For the scales the change is multiplied: a source scale going from 100%
  to 150% takes a target at 80% to 120%. If the first source scale is 0 the
  change is added instead.
- **Copy absolute values**: the target curves get the values of the source.
- **Replace existing keys of the target channels** (default): the keys of each
  target curve that gets keys are removed first. The curves of the target that
  have no counterpart with keys in the source are not touched.
- **Keep other keys**: the target keys at other frames than the source keys
  are kept; those at the same frames are replaced. Old keys between the new
  ones still shape the curve.
- **Also map Plastic vertex animation (by vertex name)** (default on): when
  both parts of a pair have a Plastic skeleton deformation, the angle, distance
  and stacking order curves of each source vertex go to the target vertex with
  the same name, by the rules 2 to 4 above (default vertex names are not
  used).

The keys keep their frames, interpolation, speed handles, ease values and
step; a cycled source curve makes the target curve cycled too (with Keep
other keys, a cycled target stays cycled).

The channels are those of the Function Editor: Rotation, X, Y, Z, SO, Scale
H, Scale V, Global Scale, Shear H, Shear V, Rotation X and Rotation Y. When a
curve was left out (see below), a message says how many curves were copied and
how many were left out.

## Limits

- A curve with expression, file or similar shape keys, in the source or in the
  target, is left out: such keys refer to other curves or files, and copying
  them would not mean the same thing on the other rig.
- The position on a motion path (the Path channel) and the motion path itself
  are not copied, nor are offset layers, published channels or the active
  skeleton curve of a Plastic deformation.
- Only the same channel is copied to the same channel (the X of the source to
  the X of the target, a vertex angle to a vertex angle). A rig whose arm is
  turned by a Plastic bone cannot take the animation of a rig whose arm is a
  column turned by its Rotation.
- Different rest poses and proportions are only partly handled. The relative
  mode keeps each target part's own starting values, but a position change is
  copied as it is: on a character twice as large, the same move looks half as
  large. Bones of different lengths and pivots in different places give a
  different motion from the same angles.
- The parts are matched by name and place only; check the proposed pairs, in
  particular those found by position, before applying.

## How it is built

- `include/toonz/rigmapping.h`: the pure part, with no dependency on the rest
  of the application: the name rules, the matching (`RigMapping::match`, on
  lists of names and parent indices) and the value rule
  (`RigMapping::retargetMap` and `retargetKeys`). Tested by
  `toonzlib/tests/rigmapping_test.cpp` (the `rigmapping` test).
- `toonz/rigmappingcommand.cpp`: the selection, the rigs (the selected object
  and its descendants, children sorted as in the Xsheet), the dialog, and the
  copy. The copy records the keys and the cycle flag of each changed curve
  before and after, in one undo entry that restores them on Undo and sets
  them again on Redo, and then refreshes the Xsheet, the Timeline and the
  Function Editor.
- The command `MI_RigMapping` is defined in `mainwindow.cpp` and added to the
  Cells menus of `menubar.cpp` and `menubar_template.xml`, and to the cell
  menu in `xshcellviewer.cpp`.
