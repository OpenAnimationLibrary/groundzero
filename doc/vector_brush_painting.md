# Constrained vector Brush painting

The vector Brush's **Paint** menu includes **Paint Inside** and **Paint
Selection**. Both produce editable, grouped vector fill contours. They use the
brush's centerline and variable width, then intersect its outline with a vector
mask. Existing fills and other groups keep their own boundaries and styles.
Closed brush loops use separate inner and outer outline cycles, preserving the
hole instead of joining the two sides with a cap seam.

## Paint Inside

Start the stroke in an editable, filled region. Painting stays within that
region, excluding its holes. The region's existing color outside the new paint
remains intact. A start in a hole, an unfilled region, or an excluded Art Layer
does not paint freely. Enter the group that you want to edit when working within
a group.

## Paint Selection

Select strokes or closed shapes with the Selection tool, then switch to Brush.
The selected stroke outlines and closed areas form the paint mask. An unfilled
child region remains a hole even when both its boundary and the surrounding
boundary are selected.

The mask records the selected stroke identities, geometry, outline options,
group depth and filled topology. Reordering unrelated strokes or adding new
paint does not invalidate it. Editing or deleting selected boundaries, changing
their group depth or Art Layer eligibility, leaving their entered group, or replacing the source drawing requires
a new selection. Paint Selection never silently uses an abandoned drawing left
alive by undo.
Mask capture and validation discover regions on detached copies, so merely
switching tools or rejecting a stale selection cannot round the live drawing's
control points.

**Selection Mask: Current Drawing** is the default. It applies the selection
only to the drawing where it was made. To intentionally use that same area in
other drawings, choose **Same Area on All Drawings**. This reuses the selected
area in drawing coordinates; it does not guess correspondences between stroke
indices. The setting belongs to the current tool session, not to saved brush
presets.

## Frame Range and Guided Drawing

Range painting interpolates brush centerlines first, then applies the selected
Paint mode to each destination drawing. Paint Inside requires every generated
stroke to start in an editable filled region. Paint Selection requires either
the originating drawing or the explicit same-area setting. All destinations
are checked before a range is changed. A rejected range produces no undo entry
and does not dirty the scene.

Guided painting requires explicitly picked brush centerlines with nonzero width
from the current vector level. Filled contour shapes cannot serve as those
centerline guides. The back and front ranges are both checked before either is
generated. The current gesture is painted once, and existing guide endpoints
are retained. The current gesture and its generated drawings share one undo
step. Undo addresses actual drawing IDs, including drawings exposed at rows
with different numbers.

Use a fill-capable palette style and nonzero brush width. Native gradient fill
styles are accepted. Stroke-level opacity and opacity envelopes cannot be
represented by one filled contour style, so guides using those features are
rejected with an explanation; transparent palette colors remain supported.
Unsupported painting never falls back to Normal mode. Read-only levels and
locked columns are not edited. Error dialogs open after the gesture releases
its image locks.

## Verification

`brushworkflowintegrationtest.cpp` uses the real Selection-to-Brush tool
transition, Brush mouse events, palette styles, image renderer, onion-skin
guide selection, frame range and undo manager. It checks clipped holes and
outside pixels, preserved original fills, stale-mask refusal, Art Layer and
read-only/locked restrictions, explicit cross-drawing masks, atomic range
refusal, gradient input, guide endpoint preservation and repeated undo/redo
across distinct drawing IDs. `contour_outline_test` supplies native geometry
and closed-outline regression coverage and is included in `ui_port_tests`.

These checks are registered for supported CI. They have not been executed
locally, in accordance with the repository's CI-only build and test policy.
