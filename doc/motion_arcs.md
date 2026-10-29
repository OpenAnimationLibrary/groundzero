# Motion Arcs

Animators check the arcs of a movement: the path a point of a character
follows from frame to frame, and how the drawings are spaced along it. The
Animate tool's **Show Trajectory** shows the path of the centre of the current
object while the tool is active. **Motion Arcs** is a viewer overlay that shows
it with any tool, for one or several columns, for the centre, a hook or any
point of the drawing, with the spacing made visible.

It is a viewer setting, off by default. Nothing is saved in the scene.

## Showing the arcs

**View > Motion Arcs** (also in the viewer's show/hide menu and in the
**Motion Arcs** submenu of the viewer's right-click menu) turns the overlay on.
The arc of the current column is drawn, and of every column kept with **Keep
the Arc of ...**.

| Mark | Meaning |
|---|---|
| The line | The path of the tracked point. |
| A dot per frame | Where the point is at that frame. Close dots are slow, far apart dots fast. |
| A square | A key of the column, or of a pegbar (or other object) it is linked to. |
| A white ring | The current frame. |
| A number | The frame number (as the xsheet shows it), every N frames and on the keys. |

The dots are coloured by speed (blue for slow, green, red for fast) or sized by
speed, as set in the settings. The speed at a frame is the mean distance to the
frames either side of it; the colours and sizes are relative to the fastest
frame of the arc.

The arc covers the frames where the column moves: from the first to the last
key of the column and of the pegbars it is linked to, and, when a hook is
tracked, every cell of the column (the hooks move with the drawings, even with
no keys). Long ranges are cut to a window around the current frame.

The positions are those of the table, seen through the camera at the current
frame with the perspective of the Z (as the motion path spline). The arcs are
not drawn in the 3D view or while editing a level.

## What is tracked

The **Track** submenu of the right-click menu sets the point of the current
column that its arc follows:

- **Center**: the centre of the column, as the Animate tool's trajectory.
- **Hook N**: a hook of the column's drawings. The hook is followed from
  drawing to drawing, so a drawn walk on a still column shows its arcs. A
  drawing without the hook puts the point at the drawing's origin.
- **This Point**: the point that was right-clicked, fixed in the column's own
  space: it moves, turns and scales with the column.

Each column keeps its own tracked point. **Clear All Motion Arcs** forgets the
kept columns and the tracked points. They are kept while the scene is open,
not in the scene file.

## Settings

**View > Motion Arcs Settings...**:

| Setting | What it does | Default |
|---|---|---|
| Show the Current Column | Draws the current column's arc besides the kept ones. | On |
| Spacing | Color by Speed, Size by Speed, both, or plain dots. | Color by Speed |
| Frame Numbers Every | Writes the frame number every N frames and on the keys; None for no numbers. | 4 |
| Frames Shown | The longest arc, in frames around the current frame. | 240 |
| Color | The colour of the line, the plain dots and the numbers. | Light blue |

They are the user's settings (`MotionArcs*` in the settings file), as the
viewer's other guides.

## How it works

- `toonz/motionarc.h` in toonzlib samples the stage: the range from the keys
  of the column and its parents (and the cells, for a hook), the tracked point
  through the placement of the column at each frame (hooks through
  `TStageObjectTree::getHandlePos`, as linked columns use them), and the
  camera perspective as the stage computes it for the column. The stage
  objects cache their placement for one frame only, so after sampling the
  cache is put back to the current frame. The rest of the header is the
  plain math on the samples: spacing, speed, labels, the colour ramp and the
  ticks of the Spacing Chart (doc/spacing_chart.md). `motionarc_test` covers
  both parts.
- The Animate tool's trajectory now gets its range and positions from the
  same functions (`keyRange`, `worldPos`), unchanged.
- `toonz/motionarcs.cpp` holds the runtime state of the columns, the settings
  and the drawing, called by `SceneViewer::drawScene` next to the motion path
  spline; `MI_MotionArcs` and `MI_MotionArcsSettings` follow the Pegbar Guide
  commands.
