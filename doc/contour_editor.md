# Contour Editor: brush strokes to outline shapes

Select vector brush strokes and choose **Level > Optimize > Convert Brush
Strokes to Outline Shapes**, or the Selection tool's context-menu command.
The result is ordinary editable PLI geometry: closed, zero-thickness boundary
strokes and native filled regions. Control Point Editor edits those boundaries.

Both open strokes and closed brush loops are supported. An open stroke becomes
one boundary with its original cap and join treatment. A closed brush loop
becomes two independent boundary cycles around the painted band. Its center
remains a hole; an existing fill behind that hole remains visible.

## Paint order and existing fills

Conversion preserves the order in which a native group paints its artwork:
existing fills first, then each stroke from bottom to top. The affected group's
original ancestor hierarchy is retained. It gains these editable subgroups:

- A background subgroup, when the group has existing fills, containing
  invisible, zero-thickness copies of the
  original centerlines. These retain the exact boundaries of existing fills,
  including regions formed by several strokes and regions with holes.
- One paint subgroup per original stroke, in its original order. A selected
  brush stroke becomes its filled contour. An unselected stroke retains its
  centerline geometry, width, style, opacity and identity.

An originally ungrouped set receives a parent group so its generated layers can
be selected together. Unaffected groups keep their original hierarchy. This
avoids the native fills-before-strokes rule moving a converted upper stroke
under an unselected lower stroke. It also avoids cutting unselected centerlines
to imitate occlusion. Conversion adds helper geometry and groups; enter the
paint subgroup to edit the desired contour.

## Appearance and undo

Solid-color strokes are supported. Whole-stroke opacity is preserved by a new
palette style whose alpha is multiplied by the original opacity; other drawings
continue to use the original style. Equivalent style/opacity pairs reuse the
same new style during one conversion.

The command checks every selected convertible stroke before changing the
level. Animated styles, texture/procedural styles, region-only styles, and
opacity profiles that vary along a stroke are refused with an explanation.
Those appearances cannot be represented faithfully by a static contour fill.
A selection with no drawable thickness is left unchanged.

One Undo restores the complete original drawing, groups, fills and palette;
Redo restores the complete converted result. The before and after states are
native image/palette snapshots, so repeated undo/redo does not depend on region
IDs remaining stable after a topology change. This is a one-way geometry
conversion; Undo is the way back to the original width profile.

## Geometry and compatibility

`ContourOutline::computeOutlinePolygons` samples offset sides using curvature,
thickness variation and tangent rotation. Corners use the stroke's round,
bevel or limited-miter join. Open strokes use their cap style. For a closed
stroke the parameter seam is placed inside a quadratic, where both tangent
limits agree, and the offset sides close independently. No end cap or bridge is
inserted across the hole.

`makeOutlineContours` fits each boundary using the existing native stroke
fitter. Defaults are a 0.05-unit sampling tolerance and a 0.1-unit fitting
error, relaxed deterministically to limit control points. The fitted contours
classify the native regions with an even-odd rule; the two cycles form the band
around a closed brush loop.

`ContourOutline::convertImage` prepares the entire detached candidate before
the command mutates the document. It preserves existing fill colors through an
explicit source-to-destination stroke map. The command commits that candidate
and its palette together.

No new file-format field or stroke type is used. Converted drawings remain
ordinary PLI strokes, groups, styles and filled regions that older applications
can read. Drawings that do not invoke conversion retain their original data.

The contour follows normal offsets. Very rapid width changes and offsets that
fold on tight curves can differ from the brush renderer's envelope. The native
render regression checks the supported fixtures within a boundary-pixel
tolerance; it does not claim an exact tessellation match for all curves.

## Verification

`contour_outline_test`, registered by `BUILD_CONTOUR_TESTS`, checks the existing
analytic area, fitted bounds, cap/join, determinism and control-point contracts.
It also exercises the real native image conversion: independent closed-loop
cycles, retained hollow center and pre-existing fill, original centerline
identity/geometry, alpha style isolation, stacking groups and safe refusal of
animated styles.

The `BUILD_UI_PORT_TESTS` application fixture invokes the registered command
through `contourworkflowintegrationtest.cpp`. It renders native vectors with
OpenGL before conversion, after conversion, after Undo and after Redo. It checks
lower/upper intersecting strokes, an unaffected group, nested group ancestors,
existing fill, scalar alpha and the ring hole. Broad image comparison allows
only the declared contour-boundary tolerance; interior sample pixels use a
separate two-channel-value tolerance.

Build and test execution belongs to CI under `CLAUDE.md`; these new checks have
been prepared but not run locally.
