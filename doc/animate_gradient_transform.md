# Gradient Transform tool

The **Gradient Transform** tool moves, turns and resizes the gradient of a
filled area in a Toonz Vector level by dragging handles in the viewer, the way
the Gradient Transform tool of Adobe Animate does. Without it the only way is
to type numbers in the Style Editor.

It works on fills that use one of the two gradient styles of the Style Editor
(Special styles): **Linear Gradient** and **Radial Gradient**. Clicking any other
fill shows nothing and changes nothing.

## Where to find it

- In the toolbar, next to the Finger tool (shown when the toolbar is expanded),
  and in **Tools > More Tools > Gradient Transform Tool** in the menu bars that
  have a Tools menu (the Palette and Ink & Paint room menus).
- It has no default shortcut; you can give it one in the shortcut editor
  (**Gradient Transform Tool**).
- It works only on Toonz Vector levels.

## How to use it

1. Choose the tool and click inside a filled area that uses a Linear or Radial
   Gradient style. A dotted box shows the bounding box of that area, with the
   handles inside it. The style becomes the current style, so the Style
   Editor shows the values that the handles change.
2. Drag the handles. The fill updates while you drag, and the Style Editor
   values follow.
3. Release the mouse. Each drag is one step in **Edit > Undo**.

Click outside the handles to pick another area, or an empty spot to hide the
handles.

### Linear Gradient

A line runs through the centre of the gradient, with two dashed lines across
it: the colours blend between those two lines.

| Handle | What dragging it does | Style Editor value |
| --- | --- | --- |
| Round handle in the middle | Moves the gradient | X Position, Y Position |
| Square handle filled with the main colour | Turns the gradient and makes the blend longer or shorter | Angle, Smoothness |
| Square handle filled with the second colour | The same, from the other end | Angle, Smoothness |

Hold **Shift** while dragging an end handle to turn in steps of 15 degrees.

### Radial Gradient

Two circles show where the blend starts (inner circle, second colour inside it)
and where it ends (outer circle, main colour outside it).

| Handle | What dragging it does | Style Editor value |
| --- | --- | --- |
| Round handle in the middle | Moves the centre | X Position, Y Position |
| Square handle on the outer circle | Makes the gradient bigger or smaller | Radius |
| Square handle on the inner circle | Makes the soft edge wider or narrower | Smoothness |

The square handles can be dragged in any direction around the circle; only
their distance from the centre matters.

When a blend or a circle is very small, its square handles are drawn a little
away from the centre so that they can still be grabbed. The handles belong to
the drawing of the current frame: going to another frame hides them until you
click an area again.

## What it edits, and its limits

- **The gradient belongs to the style, not to the area.** The tool changes the
  parameters of the palette style (the same numbers as in the Style Editor).
  Every area filled with the same style, in every drawing that uses the
  palette, changes with it. The label next to the handles ("shared by all its
  regions") is a reminder of this.
- **To change only one area**, give it its own style first: duplicate the
  style in the palette (select its chip, **Copy**, then **Paste Insert**), then
  fill the area with the new style using the Fill tool. Now the tool only
  changes that copy.
- **Positions are relative to each area's box.** The style stores its centre as
  a percentage of the box around the area, and the size of a radial gradient
  as a percentage of the box's longer side. Two areas with the same style but
  different shapes therefore show the "same" gradient at their own places and
  sizes.
- **The values stay inside the ranges of the style**: the centre cannot leave
  the area's box, the angle stays between -180 and 180 degrees, the linear
  Smoothness stays between 1 and 500 drawing units and the radial Radius and
  Smoothness between 0.01 and 100 percent. A handle stops at the limit even if
  the pointer goes further.
- A **locked palette** cannot be changed: the handles are shown but do not
  move, and the label says "palette locked".
- If the palette has style keyframes, the change behaves as an edit in the
  Style Editor: on a keyframe it updates that keyframe.
- The tool does not create anything new in the scene or the level: no file
  format changes. Only the palette is saved with the new values, as after any
  Style Editor edit.

## For developers

- Tool: `toonz/sources/tnztools/gradienttransformtool.cpp` (`T_GradientTransform`).
- The conversions between style parameters and handle positions are in
  `toonz/sources/include/tools/gradienthandles.h`, a header without GUI
  dependencies that repeats the formulas of `TLinGradFillStyle::getRects` and
  `TRadGradFillStyle::drawRegion` (`toonz/sources/colorfx/regionstyles.cpp`).
  If those formulas change, change the header too; the unit test
  `toonz/sources/tnztools/tests/gradienthandles_test.cpp` checks the header
  against a copy of them.
- A drag replaces the palette style by an edited copy and notifies the level
  palette handle, as the Style Editor does; the undo stores the parameter
  values before and after the drag.
