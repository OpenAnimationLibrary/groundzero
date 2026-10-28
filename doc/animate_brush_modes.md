# Brush paint modes (Paint Fills, Paint Behind)

The vector Brush has a **Paint** option with three values, like the brush
modes of other animation programs:

- **Normal** (the default): the Brush works exactly as before, and the
  **Draw Order** option (Over All, Under All, Palette Order) says where the new
  stroke goes.
- **Paint Fills**: the stroke you draw becomes a filled shape that is put
  **under the lines** of the drawing, so the existing lines stay on top, as if
  you painted the colours under the inks.
- **Paint Behind**: the same filled shape, put **behind everything** in the
  drawing. It only shows where the drawing was empty.

## Where it is

- The Brush tool options bar, next to **Draw Order** (vector levels only).
- The Tool Properties panel, with the other Brush options.

In Paint Fills and Paint Behind the Draw Order option is not used, so it is
greyed out (in the options bar its tooltip says why). The other brushes (Toonz Raster, Raster,
MyPaint) do not have the option.

## What the two modes do

When you lift the pen, the stroke is turned into the shape it draws, the same
shape that **Level > Optimize > Convert Brush Strokes to Outline Shapes** makes
(see `contour_editor.md`): a closed outline that follows the visible edge of
the stroke, with its caps and joins, and that is filled with the current
style. The brush stroke itself is not kept. The shape follows the width of the
stroke at every point, so **Pressure** and **Fluid Width** change its shape as
they change the width of a normal stroke.

- **Paint Fills** puts the shape just below the lowest Line Art stroke (the
  lowest stroke that has some width). The areas of the drawing inside the
  shape take the current style, also those that already had a fill: you paint
  over the fills, never over the lines.
- **Paint Behind** puts the shape at the bottom of the drawing. Inside the
  shape only the areas that had no fill take the current style; areas that
  were already filled keep their colour, because they are in front of it.

Each stroke is one undo step. Undo removes the shape and gives back the fills
that were there before.

## Difference from Under All

**Under All** is a Draw Order of the Normal mode: it puts the brush stroke,
which is still a line, at the bottom of the drawing. The line is drawn, its
width can be edited, and it does not fill anything. Paint Fills and Paint
Behind do not keep a line at all: they leave a filled area, and that area
fills the parts of the drawing it covers.

## How it works with the other options

- **Break**, **Auto Group** and **Auto Fill** are not used: the whole stroke
  becomes one shape, and the shape is filled anyway.
- **Snap** and **Auto Close**: a closed stroke paints a ring, the area the
  stroke covers; its inside is not filled.
- **Shift** straight lines and **Smooth** work as usual; the shape follows the
  final stroke.
- **Assistants** that draw more than one stroke (mirrors and the like): each
  of those strokes becomes its own shape, all in the same undo step.
- **Frame Range**: every drawing of the range gets its own shape, made from the
  in-between brush stroke of that drawing.
- **Guided Drawing with Auto Inbetween**: the in-betweens are made from brush
  strokes, so while that preference is on the Paint modes act like Normal.
- **Group editing**: when you are inside a group, the shape goes inside it,
  under the lines (or at the bottom) of that group. Outside a group, a shape
  that would land inside a group is placed just below that group.

## When nothing or a normal stroke is drawn

- A stroke with no width (Size 0, or the **Art Layer** option set to Colour
  Art) has no area to paint, and a style that cannot fill an area (for example
  a pattern or trail brush style) would make an invisible shape. In both cases
  the stroke is drawn as in Normal mode.
- A single click (a stroke of length zero) paints nothing in these modes, and
  neither does a stroke whose outline cannot be made.

## Limits

- **Paint Inside** and **Paint Selection**, the other brush modes of some
  programs, are not in this version.
- The shape is a Colour Art outline (a closed stroke of width 0), like the
  shapes of Convert Brush Strokes to Outline Shapes. Edit it with the
  **Control Point Editor** or the Selection tool, as any outline shape. It
  cannot be turned back into a brush stroke; undo is the way back.
- The fill is a flat fill with the current style: the effect of a textured or
  generated brush style along the stroke is not kept.
- Fills of other groups are not changed, and a group's own fills are drawn
  over a shape placed below that group.
- As for other outline shapes, a very thick stroke on a tight curve can give a
  shape with small overlaps on the inside of the curve.

## Presets and saved settings

- The mode is part of a Brush preset, like Draw Order and Fluid Width. A
  preset only stores it (as `Paint_Mode`) when it is not Normal, so existing
  presets are not changed, and presets saved before this option existed load
  with Normal. An unknown value loads as Normal.
- The last value is kept in the user settings (`VectorBrushPaintMode`), not in
  scenes.
- The shapes are ordinary closed strokes of width 0 with filled areas, so
  scenes and levels open in older versions unchanged.

## Notes for developers

- The rule for the place of the shape and the saved names are in
  `toonz/sources/include/tools/brushpaintmode.h`, which has no GUI
  dependency; its test is `toonz/sources/tnztools/tests/brushpaintmode_test.cpp`.
- The tool code is `addPaintModeShape` in `tnztools/toonzvectorbrushtool.cpp`:
  it makes the outline with `ContourOutline::makeOutlineStroke`, adds it on top
  and moves it down as Arrange > Send Back does (`TVectorImage::moveStrokes`,
  only to places `canMoveStrokes` allows), recomputes the regions and fills the
  regions of the shape's group whose inside point lies in the outline. The undo
  is the snapshot undo of the Auto Group option (`UndoVectorAutoOptions`).
