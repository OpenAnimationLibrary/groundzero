# Smarter morphing

Opt-in additions to the vector Inbetween command of the Filmstrip (master plan
B4, parity plan D3), on top of the stroke-pairing hints. Without them the
Inbetween result is exactly the same as before, and no scene data is added:
nothing here is saved with the level or the scene (the pairs and the zone hints
are kept in memory, by stroke id, like the existing pairs).

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
applying when the selected stroke it was recorded on is deleted.
The two steps are told apart by the drawing (not by its stroke ids), so a key
drawing duplicated from the other one, which keeps the same stroke ids, works
too; the hint then also remembers which drawing each zone was recorded on.

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
- Zone hints use the bounding box of the selection and stroke centroids; they
  are lost when the application closes.
- The other tools that use `TInbetween` (the morph based erasers and the fill
  tool preview) keep the index order matching.
