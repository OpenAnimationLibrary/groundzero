# Envelope: bend a vector selection with handles on its outline

The vector Selection tool has a new **Envelope** option, the counterpart of
Adobe Animate's Modify > Transform > Envelope. With it on, the box around the
selected strokes becomes an envelope: a closed outline with eight handles, one
on each corner and one in the middle of each side. Dragging a handle bends the
selected artwork smoothly, and whatever lies on the outline follows the outline
exactly.

## How to use it

1. Pick the **Selection** tool on a vector level and select some strokes.
2. Turn on **Envelope** in the tool options (next to Preserve Thickness). The
   box now shows its eight handles; the choice is remembered between sessions.
3. Drag any handle. The handle follows the mouse and the strokes bend with it
   while you drag. Release the mouse to finish: fills are updated, and the
   change is a single undo step.
4. Drag again to keep shaping. Each drag starts from a fresh envelope around
   what the strokes look like now.

The option can be given a keyboard shortcut: **Selection Tool - Envelope** in
the shortcut editor.

While Envelope is on:

- the corners and the side middles bend instead of scaling, and the sides
  themselves do nothing;
- dragging a selected stroke still moves the selection, the round areas just
  outside the corners still rotate it, and the centre can still be moved;
- **Preserve Thickness** works as for Free Deform. When it is off, a line gets
  thicker where the artwork is stretched and thinner where it is squeezed (by
  the square root of how much a small piece of surface grows, so a 2x stretch
  both ways doubles the width). The factor is kept between 0.1x and 10x so a
  sharp corner of the envelope cannot make a line vanish or explode.

Turn Envelope off to get the usual scale handles and the Ctrl Free Deform back.

## Several frames at once

The **Mode** option works as it does for Free Deform: in **Selected Frames**,
**Onion Skin Frames** and **Whole Level** every frame keeps its own box and its
own envelope. The handle you drag moves by the same distance in every frame's
envelope; the current frame bends while you drag, and the other frames are
bent when you release the mouse, each from its own strokes to its own envelope.
Undo restores all of them together. A frame with nothing selected is left
alone.

## How it differs from Free Deform

Free Deform (Ctrl and a corner, without Envelope) moves the four corners of
the box and spreads the change over the inside with a straight blend between
the corners: straight lines stay straight along the box's rows and columns,
and an edge handle drags a whole side.

The Envelope has eight handles and moves each one on its own. The artwork is
mapped with mean value coordinates of the envelope, the same maths as the
Envelope Deformer effect: the outline is carried exactly onto the moved
outline (a point a third of the way from one handle to the next stays a third
of the way along the moved side), and the inside follows smoothly, so pulling
the middle of a side bows it into a curve instead of moving the whole side.
Moving, rotating or scaling all eight handles together gives exactly that
move, rotation or scale.

So that curves bend smoothly, strokes are cut into shorter pieces before they
are bent (a piece is about a sixteenth of the size of the envelope, and no
original piece is cut into more than sixteen). Their shape does not change
when they are cut, but a bent stroke keeps the extra control points.

## Limits

- A handle cannot be dragged to where the envelope would cross itself, turn
  inside out, or become almost flat (less than a thousandth of its first
  area). It stops at the last good position along the way, so you can slide it
  back.
- A selection whose box is flat (a single straight horizontal or vertical
  line with no thickness) shows the usual handles: there is no envelope to
  bend.
- Art far outside the box is bent by the same formula, but the result there
  is less predictable; everything selected is inside or very close to the box.
- Motion paths (the Selection tool on a spline) keep the usual handles.
- The envelope is not kept after a drag: the next drag starts from a new
  rectangle around the bent strokes. There is no way to edit an earlier
  envelope again, other than undo.

## Files and scenes

Nothing new is saved in scenes or levels: a bent stroke is an ordinary stroke
with more control points. The only new setting is the Envelope checkbox,
remembered in the user settings.
