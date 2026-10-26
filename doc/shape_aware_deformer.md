# Shape Aware Deformer

Master plan C5, parity plan R11 and the "Shape-Aware deformer (H24)" row. A
deformer in which pins, bones and a cage act together, each moving the art
around it with smooth weights that follow the layout of all the handles.
Harmony's Shape-Aware algorithm is proprietary and is not copied; this is a
behavioural equivalent built on a published method, and it will not match
Harmony's results.

It is a new FX (Distort > Shape Aware Deformer) on a new `TDeformer`
(`TShapeAwareDeformer`, `include/toonz/tshapeawaredeformer.h`). Nothing else
changes.

## Use

Put the FX on a column, connect the Source, then turn on handles:

- **Pins** (4): *Pin n On*, *Rest Pin n* (where the pin is in the drawing),
  *Pin n* (where it is moved to), and optionally *Pin n Angle* and *Pin n
  Scale* about the rest position. A pin that is on but not moved holds the art
  still around it.
- **Bones** (2): *Bone n On*, rest head and tail, and head and tail where
  they are moved to. A bone carries the art near its rest segment as the rest
  segment is carried onto the moved one (it turns about the head). *Stretch
  Bones* also scales the art by the ratio of the lengths.
- **Cage** (4 corners): *Cage On*, the rest corners and the moved corners.
  The corners form a closed polygon whose edges move with the corners.
- **Resolution** (16 to 128, 48 by default): cells along the longest side of
  the weight grid. **Margin**: how far the grid reaches beyond the handles (it
  always reaches at least a quarter of their extent).

With no handle on, or every handle on its rest geometry, the Source passes
through unchanged. The viewer shows a gadget for every handle, the rest ones
under the moved ones.

## Algorithm

The deformation is a linear blend of per-handle transforms,

    deform(p) = sum_i w_i(p) T_i p

where `T_i` is the transform of handle `i` (a translation for a cage corner,
a rotation and scale about the rest position for a pin, a rotation about the
head, with an optional stretch, for a bone), and one more implicit handle, the
anchor, has the identity transform and keeps the art in place.

The weights `w_i` follow the bounded biharmonic weights of Jacobson, Baran,
Popovic and Sorkine (SIGGRAPH 2011), simplified:

1. A regular grid covers the bounding box of the rest geometry of the handles
   plus the margin. Its two outer rings of nodes belong to the anchor
   (weight 1), so the deformation dies out smoothly at the grid's border and
   the plane beyond it never moves.
2. The grid nodes around a pin (the four corners of its cell), along a bone
   (the cells its segment crosses) and along the cage edges are fixed. A pin
   or bone has weight 1 there; on a cage edge the weights of the two corners
   are interpolated along the edge, so that the edge moves with its corners.
3. Every handle's weight is the minimizer of the sum of the squared discrete
   Laplacian over the free nodes (the biharmonic energy, which, unlike the
   harmonic one, has no cusp at a point handle). It is found by conjugate
   gradients, on a pyramid of grids from about 8 cells down to the
   Resolution: each level is started from the previous one, the coarsest from
   an inverse squared distance field to the handles (distance to the segment
   for bones) and to the border, and the finest takes 150 iterations at most.
4. The anchor's weight is what is left of 1. All the weights are then clamped
   to [0, 1] and normalised, so they are never negative, never above 1 and sum
   to 1 at every node. This is the "bounded" part, done by clamping after the
   solve instead of as a constraint inside it (the paper solves a quadratic
   program). Between the nodes the weights are interpolated bilinearly.

The weights depend on the rest geometry only. The FX keeps the last ones, so
animating the moved handles, or rendering several tiles or frames, does not
solve again; moving a *rest* gadget does. `TShapeAwareDeformer::computeWeights`
builds them and a deformer can be built on a shared set.

`undeform()`, which the raster warp needs once per pixel, inverts `deform()`
by damped Newton steps with the exact Jacobian of the blend (the gradients of
the bilinear weights), started from the blend of the inverse transforms.

The handles are given in the FX's space and placed in the render's space by
the render affine (`deform = A D A^-1`), as for the Bone Deformer.

## Limits

- Raster warp only: the rendered Source is resampled, vector strokes are not
  deformed (Level > Adjust > Bake Deformers into Vector Level bakes the
  deformation into the strokes once, see `doc/deformer_bake.md`).
- The handle counts of the FX are fixed (4 pins, 2 bones, 4 cage corners).
  The class takes up to 64 handles and any number of cage corners (3 or more
  make a polygon; fewer count as pins).
- Clamping and normalising is an approximation of the bounded solve: the
  weights are bounded and sum to 1, but are not the exact minimizer of the
  constrained problem. Pins and bones are constrained on the grid, so a handle
  is exact to about one cell.
- Handles closer than a few cells compete for the same nodes (pins keep them
  before bones, bones before the cage), and the later one has a weight a
  little below 1 at its position. Raise the Resolution for close handles.
- A linear blend may fold the art where handles move strongly against each
  other, and a strong bend collapses the art, as in any skinning. `undeform`
  then returns the best estimate found.
- A pin moved far, relative to the size of its influence, folds the art
  around it: keep the move small against the Margin, or add pins.
- Cost: one solve is about (handles) x (cells)^2 x (iterations) operations,
  expected to be a fraction of a second for a dozen handles at Resolution 48
  and seconds at 128 (not measured), paid when the rest geometry or the settings
  change. Rendering costs a few
  Newton steps per pixel, each linear in the number of handles.
- Not a layer of the Deformer Stack FX, which has envelope layers only (see
  below).

## Deformer stack

`TShapeAwareDeformer` is a plain `TDeformer`, so it can be added as a layer
of a `TDeformerStack` in code (`stack.addLayer(std::make_shared<...>(...),
weightMap)`), and the unit test does this. The Deformer Stack FX is not
changed: its layers are three fixed envelopes, and a layer of another kind
would need its own parameters. That is left for the stack's generalisation
(master plan A1).

## Compatibility

- A new FX (`shapeAwareDeformerFx`) and a new class in toonzlib. No existing
  FX, class, file format or default changes.
- Nothing is saved unless the FX is used. A scene that uses it stores the FX's
  parameters like any other FX; older builds cannot open scenes that use it
  (the FX is unknown to them). Scenes that do not use it save and render
  exactly as before.

## Tests

`toonzlib/tests/deformer_test.cpp` (`testShapeAware`): the weights are
non-negative, at most 1 and sum to 1 over and beyond the grid; the far plane
is held by the anchor; handles dominate their own place; the weights depend on
the rest geometry only and can be shared; the identity leaves points and a
raster unchanged; a moved pin moves near points more than far ones, leaves
the plane beyond the grid still, and `undeform` inverts `deform`; bone and cage
translations and a bone turn; the placement; the bounds; and use as a stack
layer.
