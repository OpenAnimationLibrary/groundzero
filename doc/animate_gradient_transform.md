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
handles. A right click on another gradient area picks it too, so that the
right-click menu acts on it.

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

## Make Unique Style

The gradient belongs to the palette style, so moving the handles changes every
area filled with that style. **Make Unique Style** gives the picked area its
own copy of the style, so that the handles change that area alone, as in
Adobe Animate.

- Pick the area with the Gradient Transform tool, then click **Make Unique**
  in the tool options bar, or right-click the area and choose **Make Unique
  Style**. The command is also in **Tools > More Tools** (next to the tool)
  and in the shortcut editor (**Make Unique Style**, under Right-click Menu
  Commands), without a default shortcut.
- It is offered when other areas of the same drawing use the style. The tool
  options bar and the label next to the handles say how many areas of the
  drawing share it. Only the drawing of the current frame is counted, because
  counting every drawing of the level would mean loading them all.
- What it does:
  - it adds a copy of the style to the palette, in the same page, right after
    the original. The copy has the same colours and gradient values, and the
    original name with " (unique)" added (then " (unique 2)", " (unique 3)",
    and so on; the smallest free number is used). A copy is not linked to a
    studio palette, even when the original was. If the original has style
    keyframes, the copy takes the values of the current frame, without
    keyframes;
  - it fills the picked area with the copy. The other areas keep the
    original style;
  - the copy becomes the current style: the handles and the Style Editor now
    edit it.
- **Edit > Undo** removes both the copy and the area change in one step;
  Redo puts them back.
- Nothing happens, and a message says why, when the palette is locked, when
  the drawing cannot be changed (read-only level or locked frame, or a locked
  column), or when no gradient area is picked.
- The area keeps its own style afterwards. To give the same gradient back to
  several areas, fill them with one style using the Fill tool.
- If the style is used by this area only in this drawing but also in other
  drawings, the command is not offered: duplicate the style in the palette
  (select its chip, **Copy**, then **Paste Insert**) and fill the area with
  the copy using the Fill tool.

## What it edits, and its limits

- **The gradient belongs to the style, not to the area.** The tool changes the
  parameters of the palette style (the same numbers as in the Style Editor).
  Every area filled with the same style, in every drawing that uses the
  palette, changes with it. The label next to the handles says how many areas
  of the current drawing share the style. To change only one area, use
  **Make Unique Style** (above).
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
  Style Editor edit. Make Unique Style adds an ordinary style to the palette
  and changes the style of one area, as the Style Editor and the Fill tool
  do.

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
- Make Unique Style (`MI_MakeUniqueStyle`, handled in
  `gradienttransformtool.cpp`) inserts the copy with `PaletteCmd::addStyles`
  and changes the region's style with an undo of the same kind as the Fill
  tool's, both in one undo block. The choice of the copy's name and the
  "is it shared" decision are in `toonz/sources/include/tools/uniquestyle.h`,
  tested by `toonz/sources/tnztools/tests/uniquestyle_test.cpp`.
