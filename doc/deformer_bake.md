# Bake Deformers into Vector Level

Master plan A6 and the Tier 7 follow-up "move the deformer FX to vector
strokes", parity plan rows "Envelope deformer", "Curve deformer", "Free-Form
deformer", "Bone deformer", "Shape-Aware deformer" and "Weighted
multi-deformer".

The deformer FXs (Distort > Envelope Deformer, Envelope Deformer Plus, Curve
Deformer, Free-Form Deformer, Bone Deformer, Shape Aware Deformer and Deformer
Stack) warp the rendered raster of their Source, so a vector drawing is
resampled rather than deformed as strokes. **Level > Adjust > Bake Deformers
into Vector Level** applies their deformation to the strokes of the drawings
instead, once, as an edit of the level with one undo step.

It replaces the first version of the command, Bake Envelope into Vector Level,
which baked one Envelope Deformer Plus into the current drawing. That command
id (`MI_BakeEnvelope`) still works, for shortcuts and custom menus, and runs
the same code; the menus now show the new command (`MI_BakeDeformers`).

Nothing is saved that was not saved before: the bake changes the strokes of
the level, and optionally the enabled state of the FXs, which scenes already
store.

## Use

1. Make the column of a Toonz Vector level current, or select one of the
   deformer FXs applied to it in the FX Schematic (the column is then the one
   that the FX's Source chain starts from, as for the first envelope bake).
2. Select the cells to bake in the Xsheet, or only go to the frame to bake.
   With a cell selection that includes the column, every vector drawing of
   the column on the selected rows is baked; otherwise only the drawing of the
   current frame.
3. Run the command. A dialog lists the FXs that deform the drawings and asks
   to confirm. **Disable the baked fxs** (on by default) turns the render
   toggle of those FXs off, so that the baked strokes are not deformed a
   second time. Disabling (not deleting) keeps the FXs and their animation;
   turning them back on restores the raster warp on top of the baked strokes.

Each drawing is deformed with the deformation of its own frame (the FX
parameters are animated). A drawing exposed on several of the selected rows
is baked once, with the deformation of the first row. Note that disabling an
FX removes its deformation from every frame, also the ones not baked: bake all
the frames it deforms, or leave the checkbox off.

Undo restores the drawings and the enabled state of the FXs together.

## Which FXs are baked

The command walks the FX chain from the column's FX downstream and collects
every enabled deformer FX on it, in order. The walk follows the same rules as
the colour overrides that a column picks up (`toonzlib/stagevisitor.cpp`):

- it goes on only while the chain has a single output connection and is not
  also connected to the Xsheet node (the drawing would be rendered more than
  once, with different deformations);
- each FX must take the chain in its first input port (the Source of the
  deformers);
- it stops at the Xsheet and Output nodes and at macro FXs.

Other FXs on the chain (a blur, a colour correction) are passed through and
not changed. Disabled FXs are skipped, as the render skips them. FXs that do
not move anything at a drawing's frame (their points on the rest points) are
not applied to it, and are disabled only if they deform at least one of the
baked drawings.

The deformers are applied in the order of the chain, each one to the output of
the previous one, which is what the render does to the raster.

## Space

The render carries the column's placement past every FX that takes the column
in its first port (`FxBuilder::makePFfromGenericFx()` in
`toonzlib/scenefx.cpp`), and a Toonz Vector level has no dpi affine
(`getDpiAffine()`), so the FXs work in the space of the column's drawings:
their parameters are in the units of the strokes, before the column, peg and
camera placement. Each FX builds its deformer for a render with the affine
`ri.m_affine`; the bake builds it with the identity, which is the same
deformer as the render's up to that affine (every deformer FX declares
`canHandle() == false`, so the render affine it sees is a similarity, and each
FX maps its points, and scales its widths and radii, exactly as in
`doCompute()`).

A column deformed by the Plastic skeleton of a mesh parent column is refused:
there the FXs work in the mesh's space, on the Plastic-deformed image
(`FxBuilder::addPlasticDeformerFx()`), which a stroke bake cannot reproduce.

## Code

- `include/toonz/tdeformer.h`: `TDeformerSource`, the interface of the FXs that
  deform their Source with a `TDeformer`:
  `std::shared_ptr<const TDeformer> makeDeformer(double frame, const TAffine
  &placement) const`, which returns the deformer the FX applies at the frame
  (built as for a render whose affine is `placement`), or null when the FX
  leaves the Source unchanged. It is in toonzlib, which stdfx already links
  for the deformers, like `TColourOverrideSource` for the Colour Override FX.
- Each deformer FX in `stdfx` implements it with its existing builder: the
  envelopes of the Envelope and Envelope Plus FXs, `buildDeformer()` of the
  Curve FX, `getDeformer()` of the Free-Form, Bone and Shape Aware FXs, and
  `buildStack()` of the Deformer Stack FX, which makes every layer type
  (Envelope, Curve, Free-Form, Bone and Shape-Aware).
- `toonz/deformerbake.cpp` (was `envelopebake.cpp`): the command, the chain
  walk and the undo. The deformers of a drawing are composed in a
  `TDeformerStack` of full-weight layers, and the strokes are mapped once with
  `deformVectorImage()`.

## Limits

- A bake, not a live deformation: the strokes are edited once. To animate the
  deformation on vector strokes, bake every frame (each drawing takes the
  deformation of its frame), with a drawing per frame.
- Only the control points of the strokes are moved (`deformVectorImage()`), so
  a stroke follows a strong deformation only as closely as its control points
  are dense; the thickness is kept.
- FXs that warp the raster but are not deformer FXs (Free Distort, Warp, Random
  Wave and the like) are not baked, and a geometric FX of that kind between the
  column and a deformer makes the bake differ from the render. Affine changes
  through the column's placement are fine.
- Only the vector drawings of the column are baked (Toonz Vector levels); cells
  of other levels in the selection are skipped.
- A chain that splits, or that is connected to the Xsheet node before the last
  deformer, is baked only up to that point.
