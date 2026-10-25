# Pencil opacity

A vector stroke can have its own opacity. The Pump tool gets an **Opacity
Brush** mode that paints it, and the selection tool has an **Opacity** field
for the selected strokes. This is the first part of master plan B2 (parity plan
D1): opacity **per stroke**. Opacity that changes **along** a stroke (per
control point) is not delivered, see Limits.

Nothing changes for a drawing that does not use it: a stroke has opacity 1, is
drawn as before and is saved byte for byte as before.

## Decision: a stroke attribute, not a point attribute

Opacity along a stroke would be a new field of `TThickPoint`
(`x, y, thick, opacity`). That type is the control point of every stroke,
chunk, deformer and outline computation, and the PLI file stores the chunks
as raw `x, y, thick` triples with no room for a fourth value that older builds
would skip. A reader that finds extra data in a chunk tag misparses the whole
stroke.

The PLI format does have a clean way to add an optional, ignorable value to a
**stroke**: the outline options tag (`OUTLINE_OPTIONS_GOBJ`), which is already
written only for strokes with non-default cap, join, miter or pattern frame
data, is read by older builds up to the fields they know and its tail is
skipped (this is how the pattern frame fields were added). So the opacity is a
new trailing field of that tag.

## Model

`TStroke::OutlineOptions::m_opacity` (0 to 1, default 1), with
`TStroke::getOpacity()` and `TStroke::setOpacity()` (which clamps). Being part
of the outline options, it is copied with the stroke by every operation that
already copies them (copy and paste, split, join, tape, brush strokes, the
undo of the selection tool's outline changes).

## Rendering

`TSimpleStrokeProp::draw` and `OutlineStrokeProp::draw`
(`tvrender/tstrokeprop.cpp`) give the style a `TStrokeOpacityFunction`
(`tcolorfunctions.h`) in place of the render data color function when the
opacity is less than 1. It applies the color function that was there, then
multiplies the alpha. The solid, outline and center line stroke styles
therefore draw with the reduced alpha, in the viewers and in the renders. At
opacity 0 the stroke is not drawn. The color overlapping of a semi-transparent
stroke is the one of a style with an alpha.

The styles that do not take the color function in `drawStroke` ignore the
opacity: the pattern styles (raster and vector image patterns), the vector
brush styles, the MyPaint brush and the optimized generator styles of
`colorfx`. This is a limit of this version.

## File format

The outline options tag is written for a stroke with an opacity other than 1,
and the default one is written after it to reset the options for the next
strokes, as usual. Its layout, in dynamic-width integers, is:

| field | size | notes |
|---|---|---|
| cap, join | 1 byte each | as before |
| miter lower, upper | 1 value each | thousandths, as before |
| pattern frame offset, step | 1 value each | present if either is not 0 and 1, or if the opacity is written |
| opacity | 1 value | thousandths (0 to 1000), present only if not 1000 |

A drawing without opacity saves exactly as before. Older builds read the first
fields they know and ignore the rest, so **they open a drawing that uses
opacity, and show it at full opacity** (and lose the opacity if they save it).
No saved data of any other kind is changed. The PLI readers of this build read
the files of older builds unchanged (the fields are checked by the length of
the tag).

SVG export and the other vector formats do not write the opacity.

## Tools

**Pump tool, Opacity Brush.** The mode adds the **Opacity** slider (the
target, in percent) and uses **Size** and **Strength**. Dragging over strokes
moves their opacity toward the target, more at the center of the brush, so a
few passes build up the fade. A stroke is touched when any part of its width is
under the brush. Strokes of other groups and hidden zero thickness strokes are
not touched. One undo covers the whole drag.

**Selection tool, Opacity.** The **Opacity** field of the vector selection
sets the opacity (percent) of the selected strokes, with undo. The field
only sets: it does not show the opacity of the current selection.

## Limits

- Opacity is per stroke, not per point. A pencil line that fades from one end
  to the other has to be drawn as several strokes. Per point opacity needs a
  point type and a chunk tag that carry it, and a PLI chunk record older builds
  can skip; both are changes to the core data model and the format, left for a
  later step.
- The styles listed under Rendering ignore the opacity.
- A fill region under a semi-transparent stroke is not changed.
- The animated texture offset of B2 is not part of this change.
