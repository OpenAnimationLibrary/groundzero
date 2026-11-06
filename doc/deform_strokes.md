# Deform Strokes

The deformer FXs (Distort > Envelope Deformer, Envelope Deformer Plus, Curve
Deformer, Free-Form Deformer, Bone Deformer, Shape Aware Deformer and Deformer
Stack) warp the rendered raster of their Source. A vector drawing is then
resampled: its lines soften when the deformer stretches them, and a zoom on
the result shows pixels. [Bake Deformers into Vector Level](deformer_bake.md)
moves the strokes instead, but once, as an edit of the drawings.

**Deform Strokes**, an option of each deformer FX, keeps the deformation live
and applies it to the strokes. With it on, when the FX's Source is a Toonz
Vector column, the FX does not warp the raster: it passes its deformer down,
and the column deforms a copy of the current drawing's strokes before drawing
it. The level is never changed. The lines keep their thickness and stay sharp
at any camera zoom, and the deformation follows the FX's animation frame by
frame, as the raster warp does.

The option is off by default. Scenes saved before it load with it off and
render as before; a scene that uses it stores one more FX parameter,
`deformStrokes`, which older versions skip.

## When it applies

The deformer is in the space of the column's drawings, so the column must be
reached without a change of space. The FX deforms the strokes when its Source
is, directly or through the column's own colour settings (colour filter,
Color Effect, multiplane depth haze), a level column showing a Toonz Vector
drawing at that frame, or another deformer FX with Deform Strokes on whose
Source is, in turn, such a column. Chained deformers then apply from the
column up, as their raster warps would.

In every other case the FX warps the raster as before, frame by frame: Toonz
Raster and Raster levels, sub-xsheets, a column deformed by a Plastic
skeleton, a multiplane depth blur, or any other FX between the column and the
deformer.

## Notes

- As in the bake, only the control points of the strokes move
  (`deformVectorImage()`): a stroke follows a strong deformation as closely as
  its control points are dense. The thickness is kept, where the raster warp
  would stretch it.
- The FX hands the whole camera transform to the column (`canHandle()`), so
  the strokes are drawn once at the final resolution.
- The cache tells the deformed drawings apart by the FX's parameters at the
  frame (`TStrokeDeformerRenderData::toString()`).

## Code

- `toonz/strokedeformerdata.h`: `TStrokeDeformerRenderData`, the deformer in
  the render settings; `rendersVectorStrokes()`, the check above;
  `applyStrokeDeformers()` and `strokeDeformedBounds()`, used by
  `TLevelColumnFx::doCompute()` and `doGetBBox()`.
- `TDeformerSource::deformsStrokes()` tells the check that a deformer FX
  passes its deformer down.
- `deformerfx::StrokeMode` (`stdfx/deformerfxcommon.h`): the parameter and the
  render settings, used by the seven FXs in `doGetBBox()`, `transform()`,
  `doCompute()`, `canHandle()` and `getMemoryRequirement()`.
- Test: `testStrokeDeformers()` in `toonzlib/tests/deformer_test.cpp`.
