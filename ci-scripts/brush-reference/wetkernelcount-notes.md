# Dulling count and normalization discriminators

The first 185 cases retain their names and order. The next 42 cases are original
two-dab masks, appended to the same bounded acquisition group. They do not change
candidate eligibility, comparison thresholds, or the sampling implementation.

Existing captures identify missing endpoint samples at extent 28 (nominal index
81) and extent 32 (nominal index 100). Both neighboring half-rounding locations
of index 100 are absent. The 80th sample at extent 16 remains present. However,
subtracting one sample globally contradicts captures at extents 40 and 48.
Single-pixel impulses alone have poor denominator resolution in 8-bit output.

The new masks use extents 28, 32, 40 and 48 with the same source diameter/radius
combinations as the earlier evidence. At each extent, three explicit point sets
contain the nominal first quarter, just under half, and three quarters of the
sample coordinates. Each set is paired with its exact complement over the opaque
pickup backdrop. The points are stored explicitly; their construction is a
hypothesis for selecting informative pixels, not a source-engine assertion.

- Broad gray ratios distinguish dividing by the attempted count from dividing by
  the accepted count, where isolated impulses round to the same byte.
- Complement pairs distinguish channel rounding from an omitted endpoint or
  non-unit normalization. They also expose any asymmetric black/white handling.
- Adjacent isolated endpoint coordinates, nominal indices N-2 through N+2,
  distinguish a changed stopping count from a shifted window. Previously acquired
  identical points are reused, rather than duplicated.
- The existing row/column masks constrain geometry independently. A proposed
  rule must explain those masks, both polarities, endpoint probes, and earlier
  scale captures together. Passing one extent does not justify a size exception.

All masks keep alpha opaque in the pickup region and transparent beyond its
existing boundary. Source and candidate render the same explicit pixels. The
source capture remains deterministic and repeated; no random mask generation or
external brush assets are involved. Extents beyond current source eligibility
remain acquisition-only until a general equation is independently qualified.

## Public-API instrumentation for the next acquisition

The 227-case captures rule out treating the remaining discrepancy as just two
sample-count endpoints. The complementary point sets produce substantially
different ratios from the candidate's joint-coordinate model; some paired U8
outputs sum to 248–250 rather than 255. Alpha stays opaque. This is evidence to
investigate spatial sampling and normalization separately, not permission to
correct those outputs with a fitted table.

The public API supports a stronger experiment without accessing or translating
renderer implementation:

- [Krita API](https://api.kde.org/legacy/krita/html/classKrita.html) exposes
  `colorDepths`, `profiles` and `createDocument`. Discover the installed pinned
  runtime's available depths and profiles, rather than assuming a profile is
  compatible with every storage type.
- [Node API](https://api.kde.org/legacy/krita/html/classNode.html) documents raw
  `setPixelData`/`pixelData`, including BGRA integer versus RGBA float channel
  order. Use these raw bytes for measurement; PNG export and projection are not
  the measurement path. `paintLine` uses the active preset; the documented API
  does not expose a callback reporting each internal pickup coordinate.
- [Document API](https://api.kde.org/legacy/krita/html/classDocument.html) exposes
  color-depth/model/profile readback. Verify these after creation. Do not use
  `setColorSpace` to prepare an already painted probe: that performs a color
  conversion, introducing another operation into the experiment.
- The [Color Smudge manual](https://docs.krita.org/en/reference_manual/brushes/brush_engines/color_smudge_engine.html)
  describes Dulling as picking a color and filling the dab before color/opacity
  application, but does not specify coordinates or averaging arithmetic. It is
  useful for separating transport from pickup, not an exact numerical oracle.

### Phase 1: precision and normalization, 58 cases

Create fresh U8 and U16 RGBA documents with an exactly matching compatible
profile name. Record the profile, depth, runtime version and preset digest for
each case. Fail explicitly if that profile intersection is absent; do not choose
a different transfer curve silently. F32 is an optional later discriminator,
only if the same profile is available, and is not necessary for this first pass.

1. At each of the four existing extents, repeat the explicit first-quarter and
   three-quarter masks and their complements at both depths: 32 cases.
2. At extent 32, use uniform opaque gray 0, 64/255, 128/255, 192/255 and 1 at both depths:
   10 cases. These establish constant preservation, offset and gain separately
   from sample coordinates. Construct normalized U16 values directly; do not
   enlarge an exported U8 image.
3. For each extent, repeat its first-quarter mask with a third dab at both
   depths: eight cases. Keep the known surrounding backdrop intact, recording
   the whole pickup halo. This distinguishes first pickup from retained state.
4. Add eight same-U8 first-quarter complements with levels 64/192, retaining
   the existing black/white masks. This distinguishes extreme-value clipping or
   fast paths from depth-related behavior.

The acquisition sidecar uses only two 1024-by-256 documents. Every translated
U8 black/white two-dab control must match its original full-canvas center exactly
before depth results can be interpreted. U16 observations are diagnostic and do
not by themselves establish how the U8 engine computes intermediate values.

Before painting, round-trip the raw initial pickup rectangle and compare every
channel to the authored buffer. Record its digest and exact highlighted point
count. Paint one continuous line per case and capture raw first, second and
third-dab regions as applicable. Repeat from freshly initialized pixels and
fresh stroke state. Record exact component values and alpha, not only a PNG or
a normalized error score. High-depth acquisition is diagnostic; it does not add
high-depth source compatibility to the product contract.

Interpretation criteria:

- If complement defects shrink approximately with the storage quantum and
  uniform controls remain invariant, quantization is implicated. This alone
  does not identify its location in the pipeline.
- If normalized defects persist at U16, a storage-only explanation is rejected;
  inspect state, normalization and color-space behavior before inferring a
  linear spatial kernel.
- If uniform controls drift, solve constant preservation first. A coordinate
  change cannot explain a constant-image discrepancy.
- If only the third dab changes, separate retained-field transport from initial
  pickup. Do not use the visible first center as the hidden pickup state.

### Phase 2: spatial weights, only after calibration

Use bounded rectangular indicator masks to partition the pickup rectangle, with
complements and two amplitudes for each selected region. Three scalar probes
(`A`, `B`, `A+B` on disjoint regions) test additivity before any reconstruction.
If additivity holds within the measured precision envelope, recursively subdivide
only regions containing unexplained weight. A local 2-by-2 impulse neighborhood
around a disputed sample distinguishes nearest-pixel selection from distributed
interpolation weights without assuming a Halton pairing. Stop subdivisions once
pixel support is identified; use a translated held-out mask to check the inferred
geometry. If additivity fails, retain the measurements as nonlinear/state probes
instead of applying a linear reconstruction to them.

All instrumentation is original fixture generation and documented public-API
calls. Public API documentation is not treated as proof that a particular pinned
binary exposes an undocumented method; runtime capability checks remain required.
