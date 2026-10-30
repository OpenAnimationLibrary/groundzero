# Vector selection, width and gradient workflows

The Selection tool's Opacity field reads the selected strokes. Mixed values show
**Mixed**, and an empty selection disables the field. Selecting, changing frames,
or leaving the field without editing does not change artwork or add undo entries.
Editing a value applies it to the selected strokes as one operation.

## Width profiles

Select strokes and open **Manage Width Profiles**. Capture the selected stroke's
widths, start from a built-in profile, rename or delete entries, and import or
export a profile library. The preview shows the resulting silhouette using the
captured stroke, or a uniform reference when none is available.

**Replace Pressure** blends the original pressure toward the profile.
**Multiply Pressure** multiplies the original pressure by the profile. Strength
controls the blend; zero keeps the original widths. **Save Library** only saves
the profile library. **Save and Apply** also applies the selected profile. Cancel
keeps the library and artwork unchanged. Export writes a separate file; it does
not bypass the dialog's library-save action. Invalid or concurrently changed
libraries produce an error instead of overwriting data.

The Selection tool's Selected Frames and Whole Level modes apply profiles to
eligible strokes in each target drawing. Repeated exposures are edited once.
Art-layer filters and each drawing's entered group remain respected. Read-only
targets are rejected before changes begin. Apply, undo and redo preserve group
editing context across the entire operation.

## Alignment across drawings

Selected Frames and Whole Level modes align eligible objects independently in
each drawing. A group is one object. Selection bounds and first/last objects
are calculated for that drawing; a camera reference uses the current scene time
for all drawings. Unselected drawings remain unchanged. The complete operation
uses one undo entry and retains painted regions and entered groups.

## Independent gradients

**Make Unique** duplicates the selected region's gradient style even when it
appears only once in the current drawing: another drawing can still use the
original palette style. Other regions and drawings keep the original style.
Repeated Make Unique actions each create an independent copy with one undo step.

When the style is animated, choose **Copy Animation** to retain its keys and
interpolation, or **Current Appearance** to make a static copy at the current
palette frame. Cancel makes no change. Undo removes the copy and restores the
region's previous style assignment.

For fill review, contour conversion, clipped painting, morphing and along-stroke
opacity, see [Vector Fill Review](vector_fill_review.md),
[Contour Editor](contour_editor.md), [Vector Brush Painting](vector_brush_painting.md),
[Morphing](morphing.md), and [Stroke Opacity Envelopes](stroke_opacity_envelopes.md).
