# Free Deform on several frames, and the Onion Skin Frames mode

Three changes to the vector Selection tool: a fix for line thickness after a
Free Deform, Free Deform that can edit several frames at once, and a new
**Mode** that works on the frames the onion skin shows.

## 1. Line thickness after a Free Deform (bug fix)

Free Deform moves the corners of the selection box and bends everything inside
it to follow. Unless **Preserve Thickness** is on, a line also gets thicker or
thinner where the drawing is stretched or squeezed: the thickness is multiplied
by the square root of how much a small piece of surface around that point
grows (a 2x stretch in both directions means 4x the area, so lines become 2x
thicker; a pure move changes nothing).

The code that measured this was wrong in two ways. It looked at points built
with the x coordinate where the y coordinate belongs, and it looked around the
point *after* the deformation, while the map goes from the point *before* it.
For moves and uniform scales the two mistakes cancelled out, but with a real
distortion (a corner pinched, a skew) the lines came out with the wrong width.

Now the scale is measured at the original point, with the exact derivatives of
the map. The maths lives in `include/tools/bilineardeform.h` (no GUI code), and
`tnztools/tests/bilineardeform_test.cpp` checks it: identity corners, a 2x
scale (area 4, thickness 2), a pure translation (thickness unchanged), a pinched
corner (finite, not 1), a mirror, agreement with a numeric estimate, and
degenerate boxes. A box with no width or no height has no defined area scale,
so the thickness is left alone there instead of dividing by zero.

## 2. Free Deform on several frames

The **Mode** option of the Selection tool lets Move, Rotate and Scale act on
more than the current frame. Free Deform now does the same.

- Hold **Ctrl** and drag a corner (or an edge) of the selection box. In the
  Standard mode nothing changes. In the modes that cover several frames
  (Selected Frames, Onion Skin Frames, Whole Level) Ctrl-dragging used to scale,
  because Free Deform was not available there; it now starts a Free Deform.
- Every frame of the mode keeps its own selection box. The handle you drag is
  moved by the same distance in every box (an edge handle moves the two corners
  of that edge), and when you release the mouse each frame is deformed from
  its own original strokes to its own box. Frames with a different box size are
  fine, and a frame with nothing selected is left alone.
- It is one undo step: undo restores the strokes of every frame together.
- Preserve Thickness is honoured in all frames.

## 3. Onion Skin Frames mode

The new **Onion Skin Frames** entry of **Mode** (right after Selected Frames)
selects the whole drawing of the current frame and of the frames the onion skin
currently shows for the current level, and edits them together.

- When you edit the level (filmstrip), the onion skin marks are drawing
  positions in the level. When you edit the scene, they are rows of the current
  column, and only the cells that hold a drawing of the current level count.
- The current frame is always included, the same way as in the other modes.
- If the onion skin is off, has no frame marked, or Shift and Trace is on (the
  onion skin is not drawn then), only the current frame is edited.
- The frames are taken again whenever the selection is refreshed and when you
  press the mouse button, so changing the current frame or the onion skin marks
  is picked up without switching modes.
- Move, Rotate, Scale and Free Deform all work through the same code as Selected
  Frames. The Adjust Thickness window and Sort with Palette Order also follow
  the onion skin frames when this mode is on.

## Files and scenes

Nothing new is saved. Scenes and levels are written exactly as before.
