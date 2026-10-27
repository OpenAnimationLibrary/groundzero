# Deformer Stack FX

Distort > Deformer Stack (`stdfx/deformerstackfx.cpp`, id `deformerStackFx`)
bends its Source with up to three deformers, layers **A**, **B** and **C**,
applied in that order. Each layer works on the result of the one before and
is blended by its own weight (`TDeformerStack`, `include/toonz/tdeformerstack.h`;
master plan A1). It also closes the stack follow-up of master plan A5, A7, A8
and C5: each layer can now be any of the deformers that have their own FX.

## Layers

Each layer page in the FX Settings has:

- **Blend**: Weight (0 leaves the Source as it is, 1 applies the whole
  deformer), and an optional distance Falloff: the weight holds within the
  Inner Radius of the Falloff Center and falls smoothly to 0 at the Outer
  Radius.
- **Type**: Envelope (the default), Curve, Free-Form, Bone or Shape-Aware.
  Only the parameters of the chosen type are shown, and only they are used.

| Type | Same deformer as | In a stack layer | Parameter names (layer A) |
|---|---|---|---|
| Envelope | Envelope Deformer FX | 8 points and their rest points | `a_point1`..`a_point8`, `a_rest1`..`a_rest8` (unchanged) |
| Curve | Curve Deformer FX (`TCurveDeformer`) | 3 anchors with In and Out handles, Width, Falloff | `a_curve_point1`, `a_curve_in1`, `a_curve_out1`, `a_curve_restPoint1`, ..., `a_curve_width`, `a_curve_falloff` |
| Free-Form | Free-Form Deformer FX (`TFreeFormDeformer`) | 3 x 3 lattice over a rest rectangle | `a_freeForm_center`, `a_freeForm_width`, `a_freeForm_height`, `a_freeForm_point1`..`a_freeForm_point9` |
| Bone | Bone Deformer FX (`TBoneDeformer`) | chain of 2 bones (3 joints), inner and outer radius | `a_bone_point1`..`a_bone_point3`, `a_bone_rest1`..`a_bone_rest3`, `a_bone_inner`, `a_bone_outer` |
| Shape-Aware | Shape Aware Deformer FX (`TShapeAwareDeformer`) | 2 pins, 1 bone, a cage of 4 corners, Resolution, Margin, Stretch | `a_shapeAware_pinOn1`, `a_shapeAware_pin1`, `a_shapeAware_pinRest1`, ..., `a_shapeAware_cageOn`, `a_shapeAware_cage1`, ... |

Layers B and C use `b_` and `c_` in the same way. The point counts are smaller
than those of the single FXs to keep the stack's parameter list short. Every
type starts on its rest pose, so a layer of any type leaves the Source
unchanged until its points move. The default shapes are nested (half sizes
200, 160 and 120 for A, B and C), so the gadgets of the layers do not lie on
top of each other.

## One builder for each type

The parameters of each type, and the code that turns them into a `TDeformer`
at a frame, are in `stdfx/deformerparams.h` (`deformerfx::CurveDeformerParams`,
`FreeFormParams`, `BoneChainParams`, `ShapeAwareParams`, and
`buildEnvelopeDeformer` for the envelope). The Curve, Free-Form, Bone and
Shape Aware Deformer FXs were refactored onto the same classes, with an empty
name prefix and their own point counts, so a stack layer deforms exactly as
the FX of its type, and the math is in one place. The single FXs keep their
parameter names, defaults and order, and render as before.

The Shape-Aware layer keeps its own cache of the weights, as the single FX
does, so animating the moved handles does not recompute them.

All layer types go through `DeformerStackFx::buildStack()` (signature
unchanged), so anything that builds the stack, such as a vector bake, gets
the new types too.

## Viewer gadgets

The viewer shows, for each layer, the gadgets of its type's points and those
of its falloff. The gadget list is built when the FX becomes current, as for
the Envelope Deformer Plus, so after changing a layer's Type select the FX
again (or another one and back) to see the gadgets of the new type.

## Compatibility

- A scene whose Deformer Stack layers are all Envelope layers (every scene
  made before this change) loads, renders and saves exactly as before. The
  existing parameters keep their names and defaults, and the new ones are not
  written: `DeformerStackFx::isParamSaved()` (the `TFx::isParamSaved` hook
  added for the time remap) writes a layer's `*_type` only when it is not
  Envelope, and the parameters of a type only for the layers of that type.
- A layer's Envelope parameters, weight and falloff are always written, as
  before.
- Older builds skip the unknown parameters of a scene that uses the new
  types, so they read every layer as an Envelope layer. The deformation of a
  Curve, Free-Form, Bone or Shape-Aware layer is lost there.
- The parameters of a type that a layer does not use are not saved: switching
  a layer from, say, Curve back to Envelope and saving drops the curve's
  edits.

## Limitations

- The warp is made on the rendered Source raster, so vector art is resampled
  rather than deformed as strokes (as with every deformer FX).
- The stack uses the sampled bounds of `TDeformer` for all its layers. A
  single Curve Deformer FX uses the curve's own conservative bounds instead,
  because a thin band of influence can fall between the samples; a Curve
  layer with a very small Width and Falloff may be clipped at tile borders.
- Weights are uniform or distance based; there are no painted weights, and
  the stack is an FX on the column's output, not a per-column stack.
- The point counts of the stack layers are fixed (see the table).

## Tests

`toonzlib/tests/deformer_test.cpp`: a stack of a Curve and a Bone layer at
weight 1 deforms, and inverts, as the two deformers applied in order
(`testStackCurveAndBone`), and Curve, Free-Form, Bone (with a distance
falloff) and Shape-Aware layers on their rest pose leave points unchanged at
any weight (`testStackIdentityLayers`). The FX code itself is not unit
tested.
