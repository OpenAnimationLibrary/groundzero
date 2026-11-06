# Stroke opacity envelopes

Select vector strokes and choose **Edit Stroke Opacity Envelope** from the
Selection context menu or Level menu. The curve uses normalized distance along
each stroke: 0 is the start and 1 is the end. Drag knots or edit their exact
length and opacity in the table. **Add Step** creates two values at the same
length for a discontinuous change. **Clear Envelope** restores a uniform envelope;
**Fade In** and **Fade Out** provide simple starting points.

The native drawing preview edits detached copies. Cancel leaves the drawing,
scene dirty state and undo history unchanged. When selected strokes have
different envelopes, Apply becomes available after an explicit edit or preset.
Apply replaces the envelope on all selected strokes in the current drawing as
one undoable operation. Each stroke retains its own overall opacity, which
multiplies its envelope. Frame, level, column, scene and tool switches cancel the editor.

Profiles retain exact binary64 knot positions and values in PLI 150.1. This
version is written only when a drawing contains an envelope; a level keeps the
version if subsequent drawings are uniform. Earlier editions reject these new
files instead of silently discarding the envelope. Drawings without envelopes
retain the existing PLI version-selection behavior. Knots must be finite and
within 0–1, start at 0, end at 1, and be ordered with at most two knots at any
length. The file reader bounds the native profile at 65,536 knots and rejects
malformed, excessive, invalid or truncated data. The editor adds up to 256 knots
while retaining denser profiles created by native joins without truncation.
For a step exactly at an endpoint, the outermost knot controls the cap and the
inner knot controls the adjacent segment. Reversing direction preserves those
cap values; morphing retains both one-sided values.

Morphing interpolates overall opacity and the envelope independently. The union
of both profiles' knot positions retains left/right values at discontinuities.
Hold/shrink/grow unmatched strokes preserve their source opacity envelope.
Because scalar and envelope are separate controls, equal effective endpoint
opacities can have a different intermediate product; this is not flattened into
an opaque combined control.

Cutter and Eraser retain the original opacity at each surviving position.
Tape joins preserve both strokes' overall opacity and envelopes, including
reversed endpoints. A new connecting segment interpolates between the two
endpoint opacities. Straight extensions hold the adjoining endpoint's opacity;
smooth extensions keep the envelope along the reshaped stroke. Pump width edits
retain the original full envelope. Joined strokes may fold their overall
opacities into the envelope so different source values remain visible.

Envelope capacity is checked before native splits or joins change the drawing.
Rectangle Tape validates the complete sequence on a detached drawing, and
Eraser validates all strokes in a sample (or all drawings in a segment erase
range) before applying the operation. A rejected operation reports the reason
without adding an empty undo entry.

Prepared tests include native editor mixed selection, endpoint-removal recovery, exact step editing,
read-only preview/Cancel, one-step undo/redo and real PLI reopen, plus PLI format
round trips, close neighboring knots, discontinuities, malformed input and
version gates. Tests run in CI; no local execution is claimed.
The native editing fixture also checks real Cutter, Pump and Eraser gestures,
their undo/redo, Tape endpoint directions and scalar preservation, and rejected
dense-envelope splits/joins leaving the original drawings intact.

## Rendering and SVG

The renderer captures the complete native stroke style and multiplies its
premultiplied result once by overall opacity and the envelope. This retains
brush spacing, decorative overlap and translucent texture alpha. At each pixel,
the envelope uses the closest centerline point's normalized arc length; exact
crossing ties use the earlier position. Adaptive flattening targets one eighth
of an output pixel, with an explicit sampling limit for pathological geometry.
Scalar-only opacity stays on the GPU. Envelopes require readback and spatial
lookup, and unbounded procedural styles capture the full viewport. CI measures
the cost; no performance improvement is claimed.

SVG exports rasterize only profiled strokes with verified finite bounds (solid,
raster/vector Trail and Circlets), retaining other paths as vectors. Set
**Opacity Raster Scale** from 1 to 16 pixels per drawing unit; the embedded
image's longest dimension is capped at 8192 pixels. The SVG is a visual export.
The native vector importer rejects this tagged mixed output and directs you to
the original PLI 150.1 for editing. Unbounded procedural profile exports fail
before replacing the destination; use the scene export's rendered full-camera
SVG option. Normal centerline/outline SVG strokes preserve scalar × palette
opacity without rasterization.

Prepared native rendering checks cover solid/procedural/vector/raster styles,
scalar and envelope identity, crop versus full-viewport output, 3D viewer depth,
allocation/capability failure cleanup, context nesting/sharing/background use,
and actual command-line PLI conversion. These require the supported CI runners;
source completion does not establish a passing platform result.
