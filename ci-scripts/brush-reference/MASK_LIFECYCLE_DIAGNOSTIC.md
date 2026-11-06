# Masked-source lifecycle and tile-boundary diagnosis

## Observed source failure

At c7a7d71cb, the full source maskwash acquisition failed original case 039.
This is the 24px round primary at spacing .25, a linked 12px rectangular
half-aspect Multiply mask at spacing .5, mask opacity .5 and flow 1, Pixel Wash,
transparent RGBA8 canvas and public Node.paintLine from (896,1024) to
(1152,1024). The effective preset XML hash and delivered endpoints/pressure
matched between the independent first and repeated calls.

Recorded pixels provide a sharper diagnosis than the first-difference samples:

| Capture | Alpha-128 strip |
|---|---|
| First stroke | x890..959, y1021..1026 |
| Independent repeat | x890..1154, y1021..1026 |

All 1,170 visible/alpha changes are 0 to 128 over the missing 195x6 tail. The
first result truncates at x960, a multiple of 64 document pixels. No inverse
changes, fractional-error drift or hidden-RGB-only differences explain this.
Both captures and both complete initial-raster resets remained exactly stable
across their fixed readback checkpoints. This does not establish that all
source execution/invalidation state was flushed; it is evidence of a missing
paint tail under this public API/lifecycle sequence.

Evidence: run 42248393484, job 127996742719, artifact
`krita-reference-maskwash-c7a7d71cb9d06da96243e3c78391c011906388c8`, including
repeat-failure.json and both complete source PNGs. No native comparison ran.

## Existing evidence does not cover case 039

Historical run 41155250352 at ae2cb926f completed all four reused/fresh node x
default/one-worker diagnostics. Each of original cases 043,044,070,071 had eight
identical canonical source hashes and seven zero-visible-difference comparisons.
Those jobs establish only those observations. They omit 039 and its original
prehistory, so neither node-state nor concurrency causes of 039 are eliminated.

## Fixed causal controls

The separate `masklife-*` groups keep the original KPP bytes and names. Prefix
profiles retain original cases 000 through 038 in order, with exactly their
original two captures each. Target 039 then has eight fixed independent captures.
Every first/history/repeat capture remains retained and checked. Isolated-target
profiles are separate declared experiments; they do not prewarm or discard a
capture. No decision depends on which pixels happen to appear.

The matrix distinguishes reused/fresh paint nodes and default/one-worker
configuration, explicit preset reselection, isolated execution, translations
x-1/x+1/x+32/y+1, and actual Qt-delivered tablet input paired with Node.paintLine.
The Qt target uses 33 pressure-1 contacts over the same endpoints and the same
constant-full-pressure source modifier as the Node control. Preset/settings,
initial document, view transforms and effective readback remain checked. The
source's delivered image coordinates, sensor values and relative timestamps are
validated; no image equality between the two APIs is presumed.

The ordinary Node-only mask capture helper is not told that tablet input was a
Node call. The lifecycle helper validates the tablet delivery independently and
retains the same effective mask controls and full bounded pixel region.

Strict per-case repetition counts come from the closed group contract: two for
the unchanged original history and eight for 039. Unknown metadata, changed
history/order, an arbitrary repetition override, missing/changed target contacts
or a different endpoint is rejected. Cache admission must apply that same rule.

All groups retain exact visible-RGBA repeat equality. The original maskwash
matrix and fidelity gates remain unchanged. A passing diagnostic is not general
source qualification. If the Qt path is stable while Node truncates, that is
source API/lifecycle evidence requiring a separately verified reference scope;
if both paths truncate, investigate common resource/worker state or a source
bug with the complete retained evidence. No added sleep, favorable-capture
selection, statistical substitution or relaxed threshold resolves the failure.

Only public API plumbing was inspected: pinned Node.cpp paintLine/paintPath and
setPixelData, plus PaintingResources.cpp createHelper. These identify the public
call boundary and active-view resource source; no foreign brush-renderer
implementation was consulted or copied.

- [Pinned public Node API](https://github.com/KDE/krita/blob/bd6e8d0/libs/libkis/Node.cpp)
- [Pinned public painting resource plumbing](https://github.com/KDE/krita/blob/bd6e8d0/libs/libkis/PaintingResources.cpp)
