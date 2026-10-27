# Contour Editor (brush strokes to outline shapes)

Master plan C4, parity plan D5. An opt-in way to edit the *outline* of a
variable-width brush stroke. Nothing changes unless the command is used.

## What it is, and what it is not

Harmony stores a brush stroke as its outline, so the shape can be pushed and
pulled directly. OpenToonz stores a stroke as a centreline (a chain of
quadratic Beziers whose control points are `TThickPoint`: x, y, thickness),
and the drawn shape is computed from it. Adding an outline stroke type would
change `TStroke`, the PLI format and every reader, so it was **not** done.

Instead the shape is made explicit once, on request: the brush stroke is
replaced by a normal closed stroke of thickness 0 that follows the outline,
filled with the style of the original stroke. Editing "the contour" is then
editing a plain closed stroke with the tools that already exist. This is the
same data the Colour Art workflow uses (`doc/harmony_parity_plan.md`, "Line
Art / Colour Art").

## Use

1. Select variable-width strokes with the Selection tool (vector level).
2. **Level > Optimize > Convert Brush Strokes to Outline Shapes**, or the same
   entry in the Selection tool's right-click menu. It is also a command that
   can be given a shortcut (Preferences > Shortcuts, "Convert Brush Strokes to
   Outline Shapes").
3. Each convertible stroke becomes a closed outline stroke, in place (same
   place in the drawing order, same group, same style). The inside is filled
   with the style of the stroke, so the drawing looks the same.
4. Edit the contour: with the **Control Point Editor** (or the Selection tool), move,
   add and delete the control points of the closed stroke. This is the "Contour Editor" mode; no new tool was built,
   because the Control Point Editor already does the job on a zero-thickness
   closed stroke.
5. Undo restores the brush strokes (control points, thickness and the fills
   that were there before).

A stroke is converted only if it is open, has some thickness, and is not a
centreline-only stroke. Other strokes in the selection are left as they are;
if none can be converted a message says so.

## Design

All geometry is in `toonzlib/contouroutline.cpp` (`include/toonz/contouroutline.h`,
namespace `ContourOutline`) and does not depend on any UI, so it is tested
alone (`toonzlib/tests/contour_outline_test.cpp`).

1. **Outline polygon** (`computeOutlinePolygon`). Every quadratic chunk is
   sampled with a number of steps chosen from its curvature, its thickness
   change and the rotation of its normal, so that the polygon stays within
   `sampleError` (0.05) of the offset curves. Each sample gives a right and a
   left point, `p -/+ normal * thickness`.
   - Cap style (butt, round, projecting) and join style (miter with the
     stroke's limit, round, bevel) are read from the stroke's own
     `outlineOptions()`, so the result matches what the brush draws.
   - At a corner between two chunks the outer side gets the join. On the inner
     side the two offset curves cross: the crossing is found in a window of 32
     segments and the loop is cut; if it is not found the side goes through
     the centreline point.
   - The polygon is counter-clockwise and has no repeated points.
2. **Fit** (`makeOutlineControlPoints`). The closed polygon is fitted with the
   existing stroke fitter (`TStroke::interpolate`: corner detection, cubic
   fit, then quadratic chunks), the code that turns a drawn path into a
   stroke. The fit error starts at 0.1. If the stroke has more than
   `maxControlPoints` (400) control points, the error is doubled and the fit
   redone, up to 8 times. The sequence is fixed, so the output is
   deterministic. The control points get thickness 0 and the stroke is made a
   self loop (first and last control point equal).
3. **Command** (`StrokeSelection::convertToOutlineShapes`, in
   `tnztools/strokeselection.cpp`, command id `MI_ConvertToOutlineShapes`).
   It computes all the outlines first (nothing changes if none can be
   converted), then reshapes each stroke **in place** with `TStroke::reshape`
   and `setSelfLoop`, as the Control Point Editor does, so the stroke index, id,
   style and group are kept. It recomputes the regions, fills the new regions
   with the stroke's style (`TVectorImage::selectFill`, only regions that had no
   style), and records the fill information of the area before and after.
   `ConvertToOutlineShapesUndo` stores the control points before and after,
   and the fills, and restores them on undo and redo.

The unused legacy `computeSweepBoundary` (`tsweepboundary.cpp`) was looked at
and not used: it is not called anywhere, depends on global state and has
failure paths that were never exercised.

## Compatibility

- No new saved data. The result is a normal PLI stroke of thickness 0 whose
  self-loop flag and fill are what any closed stroke has. Scenes and levels
  opened in older builds, and in upstream OpenToonz, show the same drawing.
- Levels that never use the command are saved and rendered exactly as before.
- No change to `TStroke`, `TThickPoint` or the PLI format.

## Limits

- **One way.** The thickness profile cannot be recovered from the outline, so a
  converted stroke cannot become a brush stroke again (undo is the way back).
  The brush style's effect (for example a texture along the stroke) is lost:
  the result is a flat fill.
- **Closed brush strokes (self loops) are skipped**, because their shape is a
  ring and needs two outlines.
- The outline follows the offset of the centreline along its normal; where the
  thickness changes very fast the exact envelope differs a little. Where the
  thickness is larger than the radius of curvature of the line (very thick
  strokes on tight curves) the inner offset curve folds on itself, and the
  closed stroke has small self-overlaps there.
- The control-point count is bounded but not minimal; a long wavy stroke can
  give a few hundred control points.
- The fill uses the style of the stroke as the style of the region, whatever
  kind of style it is.
- **Stacking.** A Toonz Vector drawing paints the fills of a group before its
  strokes, so a converted shape (a fill) is drawn under every other stroke of
  its group, also under strokes that were below the brush stroke. A brush
  stroke painted to cover a line no longer covers it after the conversion.
  Regions inside the outline that already had a fill keep it.
- Not a new tool: there is no live outline editing of a still-brush stroke,
  no width handles, and no "Convert Back".

## Tests

`BUILD_CONTOUR_TESTS` (on with `BUILD_TESTS`) builds `contour_outline_test`. It
checks, for a straight round stroke, a tapered butt stroke, a projecting-cap
stroke, a curved stroke, a stroke with a right-angle corner, and a dot, that
the area (analytic, or compared cell by cell with a brute force union of
discs along the centreline) and the bounding box of the fitted shape agree
with the stroke within a few percent, that the shape is closed, has thickness
0, stays under the control-point bound, is deterministic, and that centreline,
zero-thickness and self-loop strokes are refused.
