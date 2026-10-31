# Smarter morphing

Opt-in additions to the vector Inbetween command of the Filmstrip (master plan
B4, parity plan D3), on top of the stroke-pairing hints. Without them the
Inbetween result is exactly the same as before, and no scene data is added:
manual pairs and zone hints are saved as versioned metadata in the scene.
They belong to a particular level and pair of drawings, not to session-wide
stroke IDs. Older builds ignore this authoring metadata and drop it if they
resave the scene; drawings and rendered animation remain compatible.

Open **Inbetween** from the Filmstrip with two key drawings selected.

## Matching order

Strokes of the two key drawings are matched in this order of priority:

1. the manual stroke pairs (Pair Strokes for Inbetween, "Use Stroke Pairs"),
2. the zone hints ("Use Zone Hints"),
3. the automatic matching ("Match Strokes Automatically", off by default),
4. the index order, as before, for whatever is left.

A stroke is used by one pair only; a conflicting lower priority pair is dropped.

## Automatic matching

`TInbetween::autoCorrespondence(first, last)` minimises the total of a cost per
candidate pair:

```
cost = 1.0  * centroid distance / diagonal of the bounding box of both images
     + 0.5  * (|dwidth| + |dheight|) of the stroke bounding boxes / diagonal
     + 0.5  * |dlength| / max(length)
     + 0.25 * (style ids differ)
```

The centroid is the mean of the control points. The assignment is the
Hungarian method (`include/tinbetweenmatch.h`), so it is optimal and not
greedy: min(stroke counts) pairs are made, the surplus strokes of the drawing
with more strokes are left out, like with the index order matching.

## Zone hints

**Add Inbetween Zone Hint from Selection** (Level menu actions and the context
menu of the Selection tool): select strokes on a key drawing and run it to
record the bounding box of the selection as the source zone; select the
strokes of the matching zone on the other key drawing and run it again to
record the target zone. Strokes of the first drawing whose centroid is in the
source zone are matched with strokes of the other whose centroid is in the
target zone, nearest first (distance measured from the center of each zone).
**Clear Inbetween Stroke Pairs** clears the zone hints too. A hint stops
applying when its anchor stroke is deleted, changed beyond file quantization,
or becomes ambiguous.
The two steps are scoped to the level and frame ID, including frame suffixes.
Renumbering frames remaps the hints. Pairing and clearing hints are undoable.
Save the scene after editing hints; exporting a standalone PLI does not carry
scene authoring metadata.

## Velocity curve

The Interpolation list gets a **Custom** entry (the last one) with the sliders
**Slow Start** and **Slow End** (0-100%). The in-between parameter t goes through
the cubic Bezier (0,0) (a,0) (1-b,1) (1,1), with a = slow start and b = slow
end; 0% / 0% is linear, and the curve is always increasing. The Linear, Ease In,
Ease Out and Ease In / Ease Out entries are unchanged.

The dialog remembers the last values of the sliders and of the automatic
matching checkbox in the user settings.

## Limits

- Vanishing-point hints are not done.
- Zone hints use the bounding box of the selection and stroke centroids.
  Editing an anchor invalidates the corresponding hint rather than guessing
  which new stroke it was intended to identify.
- The other tools that use `TInbetween` (the morph based erasers and the fill
  tool preview) keep the index order matching.

## Correspondence review and unmatched strokes

The Inbetween dialog shows the first drawing, the actual generated vector drawing,
and the last drawing in native vector previews. A position slider evaluates the
same interpolation and correspondence implementation used by the command. The
review lists manual, zone, automatic and index-fallback matches; selecting a row
highlights both source strokes. Appearing/disappearing strokes are listed too.
Cancel does not edit drawings or create an undo entry.

Choose an explicit policy for unmatched strokes:

- **Omit (legacy)** retains the previous behavior.
- **Hold** keeps each unmatched stroke unchanged between the key drawings.
- **Shrink/grow** scales each unmatched stroke around its own bounds center,
  including its thickness: disappearing strokes scale by `1-t`, appearing
  strokes by `t`. It does not pretend that stroke alpha fades a region's fill.
- **Fade** multiplies the unmatched stroke's authored opacity by `1-t` when
  disappearing or `t` when appearing. Geometry, width, group membership and
  the stroke's normalized opacity profile are retained. Repeated previews and
  generation start from the key drawings, so the multiplier does not accumulate.
  Fade is unavailable when an unmatched stroke bounds a painted region,
  including a nested fill or a hole in a painted region. The dialog explains
  the limitation and disables Inbetween; direct generation rejects the same
  case before changing drawings or history. Match those boundaries or select
  another policy. Matched fills and unpainted closed strokes remain supported;
  independent animated region opacity is not implemented.

The key drawings remain unchanged. Generated drawings retain native strokes,
groups, styles and transferred region colors. The Filmstrip command generates
every intermediate drawing before applying any of them, then records one undo
transaction. An invalid or unrepresentable envelope leaves drawings and history
unchanged. Frame suffixes bound both the operation and redo exactly.
Disappearing strokes retain their first-drawing
stack positions. Appearing strokes follow that stack in last-drawing order;
their group identifiers are remapped to avoid collisions between drawings.
Use the actual preview to inspect overlaps before committing.

## Persistence and validation

`morphHintsV1` is an optional attribute of the existing level `info` element.
It contains bounded versioned JSON: frame IDs, pair/zone records and sampled
stroke anchors. No PLI chunk format changes are needed. Coordinates are compared
with a quantization tolerance of 1/128 drawing unit; ambiguous identical strokes
are rejected rather than rebound by index. Unknown versions are retained but
cannot be edited until explicitly cleared. The record limit is 2048 and the
encoded payload limit is 2 MiB.

The `ui_vector_morphworkflow` CTest target exercises the actual native tween engine,
manual/zone/fallback correspondence, birth/death policies, endpoint/source
preservation, closed-region fill transfer, real PLI reload and level metadata
serialization, stale/ambiguous anchors and frame renumbering. It is enabled with
`BUILD_MORPHWORKFLOW_TESTS`, `BUILD_TESTS`, or `BUILD_UI_PORT_TESTS`. The existing
UI CI target also drives the native correspondence dialog, Filmstrip Apply and
undo/redo, lettered drawing bounds, and failure without partial mutation.
Fade coverage includes actual rendered dialog previews at multiple positions,
Cancel, repeated Apply/undo/redo, endpoint identity, source opacity/profile and
group preservation, PLI reload, and rejection of painted unmatched boundaries
in either direction. Native topology fixtures cover nested fills and painted
hole boundaries while retaining supported matched fills.
Source fixtures are not execution results: CI qualification remains required.
