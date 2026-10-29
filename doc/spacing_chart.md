# Spacing Chart

On paper, the key drawings of an animation carry a **spacing chart**: a line
from one key to the next with a tick for each inbetween, placed where the
inbetween falls along the way. Ticks bunched near a key are an ease (slow in
or out), evenly spread ticks a constant speed. The **Spacing Chart** panel
draws that chart for the current column, from what is in the scene.

It only reads the scene: nothing is saved.

## Opening it

**Windows > Spacing Chart**. It is a panel like the others: it can float, or
be docked in a room.

## What it shows

The chart of the current column, along the path of its Motion Arc
(doc/motion_arcs.md): the centre of the column, or the hook or point set in
the viewer's **Motion Arcs > Track** menu.

- **The chart.** A line for the arc between the two frames, from its first
  frame (left) to its last (right), and a tick per frame at the fraction of the
  path covered at that frame. Keys are long ticks, with their frame number; the
  current frame is the red tick. A dashed mark shows the half way.
- **The strip.** Below, a bar per frame, as high as the distance travelled
  since the previous frame, coloured by speed as the Motion Arcs dots (blue
  slow, red fast). An ease shows as bars growing or shrinking.
- **The line below** gives the frames and how many keys there are.

The distances are taken in the table, as the arc is drawn: a column that
does not move between the frames (a hold) puts every tick at the start.

## The frames charted

With **Keys Around the Frame** on (the default), the chart runs from the key at
or before the current frame to the next key, and follows the current frame:
step through a scene to see each segment's spacing. The keys are those of the
column and of the objects it is linked to. Before the first key or after the
last, the chart runs to the start or the end of the arc.

With it off, **From** and **To** set the frames (as the xsheet numbers them).
Only the frames of the column's arc are charted: the range is the one of the
Motion Arcs, keys of the column and of its parents (and its cells for a hook),
up to the **Frames Shown** setting around the current frame.

## How it works

- The samples and the chart come from `toonz/motionarc.h` (`MotionArc::chart`,
  `MotionArc::keysAround`), tested in `motionarc_test`: the tick of a frame is
  the arc length from the first frame divided by the arc length of the whole
  range.
- The panel is `toonz/spacingchartpane.cpp`, registered as the
  `SpacingChart` panel with the command `MI_OpenSpacingChart`. It refreshes
  when the frame, the column, the xsheet or the current object change.
