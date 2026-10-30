# Original brush reference fixtures

The `Raster Brush Reference` workflow runs the official pinned Krita 5.3.4.1
Linux application in an isolated CI profile. It generates 79 original parameter cases across five profiles:
Pixel, legacy Smear, legacy Dulling, new Smear and new Dulling. Each includes
diameter, spacing, opacity, flow and pressure-ramp probes. Wet profiles also
probe pickup/color-rate endpoints, smear alpha and the smudge-length toggle;
Dulling adds stored radius values 0, 3, 100 and 300 (not a claim about UI range endpoints).
Seven dry strokes of lengths 1, 2, 3, 4, 7, 16 and 32 pixels isolate initial-dab,
spacing-boundary and final-dab behavior without inferring those rules from long strokes. These isolate observable parameter relationships
for the independent implementation. The generated
presets and input strip images are dedicated to CC0-1.0. No downloaded brush
packs or third-party implementation source are included.

The workflow retains the exact generated presets, application binary hash,
reported version, image pixel hashes, runtime log and PNG results. It fails on
missing presets, version mismatch, unchanged strokes or incomplete output. These
are source execution fixtures, **not passing compatibility comparisons**. The
public `Node.paintLine` API supplies pressure endpoints but does not reproduce
physical tablet delivery, rotation, tilt or timed airbrush. Those require separate
fixtures under the contract in `doc/raster_brush_compatibility.md`.

A separate `tablet` group supplies two original 65-point pressure traces through
the visible canvas using Qt tablet press/move/release and proximity events.
Coordinates use the public View image/flake/canvas transforms at zoom 1; every
point must be inside the unique visible canvas. The public freehand-brush action
is selected explicitly. Requested event timestamps are rounded from 240 Hz to
Qt milliseconds, with an event-loop turn between deliveries. Actual event
acceptance and input values are recorded, and nonempty output is required.
The comparison renderer consumes the same authored trace, including all pressure
points. These cases are synthetic input-path measurements, not physical device,
wall-clock latency, smoothing equivalence, or tilt/rotation qualification.
The baseline is repeated after resetting the canvas and must match exactly.
API sources: [Qt tablet event contract](https://doc.qt.io/archives/qt-5.15/qtabletevent.html),
[Krita View transforms](https://api.kde.org/legacy/krita/html/classView.html), and
[Krita action dictionary](https://scripting.krita.org/action-dictionary).

The comparison executable renders the same strip canvas through the imported
native conversions and retains candidate images, full-canvas differences and
`comparison.json`. Metrics include premultiplied RGB and alpha errors, alpha
p99, changed-pixel IoU and both extents, using the union of changed pixels dilated
by two pixels. Missing output fails the workload. Numerical differences are
reported without a parity pass: the imports remain explicitly converted and
unverified. This parameter workload is initial measurement, not the full
combination, sensor, texture and long-stroke qualification matrix.

The application is fetched from the official versioned HTTPS download location;
its recorded hash is evidence of the executed binary, not a prevalidated hash
allowlist. CI publishes the original fixtures and outputs, not the application.

References:

- [Current official download](https://krita.org/en/download/)
- [Public Node API](https://api.kde.org/legacy/krita/html/classNode.html)
- [Public View API](https://api.kde.org/legacy/krita/html/classView.html)
- [Python plugin registration](https://docs.krita.org/en/user_manual/python_scripting/krita_python_plugin_howto.html)

Run through the repository CI workflow; project instructions prohibit local
builds and tests. A successful workflow only establishes the listed source
fixtures. It never promotes imported presets to reference-verified status.

At `730269e0`, the legacy Smear zero-length probe produced an unchanged raster.
This is a meaningful boundary measurement, consistent with the documented old
algorithm coupling between smudge length and color rate. The two legacy
zero-length cases therefore record whether pixels change instead of requiring
nonempty paint. Every other case retains the nonempty-output guard. These empty
source results still enter the comparison: a nonempty candidate is a difference,
not a pass or an omitted fixture.

Color differences use an independently written CIEDE2000 calculation after
encoded-sRGB compositing over both black and white, followed by sRGB-to-D65-Lab
conversion. Mean and p95 values use the same union region as RGB/alpha errors.
The six-platform package test matrix calibrates the calculation against published
numerical pairs, including neutral colors and hue wrap boundaries, and checks
black/white D65 conversion. This measurement code is confined to CI; it does not
change production color management or neural styling.

CI annotations are compact summaries bounded below the observed 4,096-byte
message truncation and ten-message retention limits. Full-precision metrics,
all preset controls and complete pixel outputs remain in the artifacts. The
compact `wet` array is ordered: smudge strength, color rate, radius, strength
option enabled, new model, mode, smear alpha. A stored value readback alone
never proves that the source engine consumed that option; differing parameter
cases must also be evaluated for observable output changes.

Short-stroke geometry probes use a transparent canvas and retain an exact
80-by-48 alpha window at document coordinate (232,1000), encoded as row-major
alpha8 bytes compressed with zlib/base64. The same manifest background is used
by the candidate renderer. The diagnostic window never replaces the full-canvas
union comparison or permits cropping away stray paint.

A separate `geometry` job renders twelve original one-dab cases: diameters
1, 7, 8, 32, 64 and 128, each with antialiasing disabled and enabled. Spacing
is five diameters and the line length is one pixel, so each case isolates the
initial dab. These size and edge-mode observations complement the 79-case
`parameters` group; they do not replace any parameter case. The geometry alpha
window is sized from the declared diameter. Both jobs retain independent
artifacts and validate requested diameter, opacity, flow and painting modifiers
through the public View API; effective UI values are recorded separately from
requested values, including possible one-byte opacity/flow quantization.

Presets explicitly request fixed precision 5 with automatic precision disabled,
and selected-preset XML must retain both settings. The serialized field names
come from KDE's public `testing_predefined_lod.kpp` and
`testing_200px_colorsmudge_lightness_smearing_new_nsa_maskmode.kpp` fixtures;
only metadata was inspected, and no brush image or engine implementation was
copied. Earlier runs through `a0ef97ea` omitted these fields and therefore
measured source defaults, not the fixed-precision contract. They remain useful
diagnostic evidence but must not be promoted to that contract's reference set.

Each geometry case is rendered three times, and each of the five dry/wet
baselines twice, after a full reset and a return to the Qt event loop. These
fixtures disable random dynamics, so full-canvas bytes must match exactly.
Per-repetition hashes are retained; a mismatch fails the oracle workload and
records the preset and repetition. This checks source repeatability only, not
candidate fidelity or stochastic-brush distribution equivalence.

At integrated head `31a0bee6`, the one-pixel, antialias-off, integer-coordinate
one-dab fixture returned an unchanged transparent raster. Its old nonempty guard
was invalid for this aliasing boundary. The fixture now requires that exact
no-op, records it and compares the candidate normally; it is not omitted or
reported as general tiny-brush parity. The two measured legacy zero-strength
cases also require their observed unchanged result. Cleanup diagnostics preserve
the primary failure instead of replacing it with a shutdown error.

The independently written `krita-5.3.4.1-hard-pixel-candidate-1` runtime profile
now separates measured hard-circle lattice placement and endpoint scheduling
from the native renderer. It rejects antialiasing, nonintegral diameter,
noncircular/soft tips, sensors, angle/aspect changes, opacity/flow changes,
texture, dual masks, wet paint and other unimplemented controls. The profile is
selected by the importer only for explicit eligible KPP settings. Eligibility
requires precision five, disabled auto precision, a hard round default mask,
known static controls, and no unknown brush attributes or settings. Missing
required fields retain native conversion. The harness requires that import
selection itself succeeds, and portable-preset tests preserve the profile.
This selection remains a candidate label, not source verification.
Six hard one-dab cases must match full-canvas premultiplied RGB and alpha exactly. A `hardpixel` job
adds nine diameter/spacing combinations (7/32/128 pixels, spacing .1/.25/1) with
the contract's dry RGB, alpha, p99, IoU and bound-displacement gates enforced.
Failing candidate cases retain metrics/images and fail the job; exploratory
native comparisons still do not grant parity. Passing these cases does not
complete the Pixel profile, qualify arbitrary settings, or set foreign import
verification flags. No source implementation was used to derive this profile.

Tablet fixtures explicitly select the source tool's no-smoothing option via its
Qt control and verify that it remains selected, following the
[documented smoothing choices](https://docs.krita.org/en/reference_manual/tools/freehand_brush.html).
Qt device timestamps remain monotonic across separate strokes; authored trace
times stay relative to the stroke for comparison. The earlier repeated 0-based
device timestamps were not a valid source input stream. Exact repeated-pixel
equality remains required after that correction.

Transparent-canvas coverage gates use alpha >= 1/255, as specified by RCP1;
raw changed-pixel IoU is also retained. K5341 can preserve hidden foreground RGB
in alpha-zero pixels, so raw channel differences are not equivalent to painted
coverage. The comparison retains the union of raw changed sets for error regions
and reports both alpha and changed-pixel bounds. Original numeric tests require
hidden RGB to leave coverage unchanged while still catching stray visible paint.

`wetsteps` isolates state transitions with 64 original cases across all four
smudge models. A hard 32-pixel tip starts on opaque blue; a second dab, when
present, lands on half-transparent red. Foreground is opaque green, spacing is
five diameters, and Dulling sampling radius is explicitly zero. Eight
strength/color-rate combinations each run with one or two dabs. The first-dab
change/no-op outcome is measured because model initialization is not yet
established; known zero-strength legacy no-ops still remain assertions. Both
full rasters and center/edge probes at each dab are retained. This data is for
independently deriving each model's state equations, not validation of the
existing native wet conversion. No source implementation is read or copied.

Four independent experimental wet candidates now separate legacy/new Smear
and Dulling from native wet painting. They initialize without
painting the first dab and retain a feedback snapshot and use arithmetic interpolation of premultiplied
channels; third-dab measurements distinguish this from source-over opacity. Legacy color loading and modern two-stage
compositing are distinct equations; RGBA8 quantization occurs after deposition.
Only fixed hard circles with radius zero and no sensors are accepted. Snapshot
allocation is charged before allocation; the previous snapshot is released after
replacement and the final carried field is released at stroke end. These
models are inferred from original fixtures, not copied source implementation.

`wetsteps` enforces max premultiplied-channel error <= 2/255, max alpha error
<= 1/255, changed-pixel IoU >= .98 and no empty-candidate failure on all four
implemented candidates. New Dulling uses radius-zero attenuation measured at
three tip sizes and independent strengths; nonzero radius remains rejected. `wethold`
adds 40 cases with second/third dabs, diameters 7/32/128 and previously unseen
strength/color-rate values. Its source candidates use the same gates. Passing
two-dab examples alone cannot establish feedback state, general sensor behavior,
nonzero sampling radius, antialiasing, or complete source-engine compatibility.

The initial `wethold` source capture succeeded, but candidate import correctly
rejected the 7-pixel fixture's spacing above the runtime maximum. The fixture now
caps spacing at five diameters and moves its background boundary/probes with the
dab centers. This fixes a fixture outside the declared contract rather than
relaxing runtime validation. Third-dab reference probes reject source-over pickup. A separate varied-third-
destination workload distinguishes arithmetic interpolation from carried-ink
models that coincide on repeated uniform destinations. The original 64-case gates passed before this state expansion.

`pixelopacity` exercises fourteen fixed hard-circle combinations of source
build-up/wash, opacity and flow, including zero-opacity/zero-flow no-ops. The
adapter maps the explicitly serialized painting mode and static opacity/flow;
all source gates remain enforced. The user manual distinguishes stroke opacity
in wash from per-dab opacity in build-up:
https://docs.krita.org/en/reference_manual/brushes/brush_settings/opacity_and_flow.html
The enum order (build-up zero, wash one) is documented in the public GUI data
example, without consulting paint-engine implementation:
https://docs.krita.org/en/untranslatable_pages/brush_editor_gui_with_lager.html

`pixelpressure` adds eighteen hard-tip linear pressure-size traces, including
zero-pressure endpoints, rising/falling ramps, constant pressure and three
spacings. Eligibility accepts only one explicitly serialized linear pressure
mapping; nonlinear curves and other active dynamics remain native conversions.
`wettexture` adds 32 heterogeneous-destination measurements across all four
models and radii 0/50/100/300 percent. Radius-zero candidates retain hard gates;
nonzero-radius conversions remain exploratory until implemented independently.
The reference matrix runs at most two complete application jobs concurrently;
all matrix members and comparison thresholds remain enabled.

`pixelsoft` captures twenty isolated 32-pixel circular/rectangular tips across
five horizontal/vertical fade values and antialiasing on/off. The complete alpha
window is retained as a source measurement for independently deriving edge
behavior. Native comparisons remain explicitly unqualified; those measurements
do not add unsupported softness controls to the strict source candidate.
Reference matrix ordering prioritizes newly introduced state/setting workloads,
while retaining all existing regression members with fail-fast disabled.

The varied-third-destination run at `763fb470` meets the channel/alpha bounds
in all 32 cases. Its transparent-destination coverage failures exposed a metric
bug: raw changes to hidden RGB at zero alpha were counted as painted coverage.
The wet gate now uses visible changed-pixel IoU, preserving the raw union (and
its two-pixel dilation) for numerical error sampling and retaining raw IoU in
reports. A mixed-background metric fixture checks this distinction; no numerical
threshold is relaxed. The diameter sweep also validates integral coordinates
before converting JSON numbers to the integer QPoint required by Node.paintLine.
These corrections require a new CI run before the affected gates can be called
passing.

The first `pixelopacity` source run established that build-up treats static flow
as inactive: quarter/half flow matches full flow, and zero flow still paints.
The independent hard-pixel candidate now applies opacity alone in build-up,
while wash retains separate flow. The source no-op expectation is correspondingly
limited to zero opacity or wash with zero flow. This preserves the measured mode
distinction; the original mistaken zero-flow assumption stopped that capture
before the wash cases and requires corrected reference qualification.

Native CPU qualification now enforces the declared standard/heavy event budgets
(p95/p99 16/33 ms and 33/66 ms respectively) as necessary CPU-only bounds, plus
completion of each simulated 60-second trace in at most 60 seconds. Every workload
is measured and the complete JSON is written before reporting a budget failure.
The workflow publishes measurements even when a CPU gate fails. Hosted CPU gates
do not replace dirty-preview or physical input-to-display qualification.

All eight radius-zero heterogeneous second-dab cases pass exactly at `763fb470`.
Smear now accepts a finite serialized radius of 0–300 as explicitly inactive,
retaining it in provenance while its candidate uses no native radius sampling.
The documented Smear/Dulling distinction and acquired radius probes justify that
adapter rule; nonzero Dulling remains unimplemented. `wettexture3` adds a third
dab after heterogeneous pickup, with hard gates for every Smear radius and
radius-zero Dulling. This distinguishes state sampling from deposition on the
second dab and does not infer general Dulling radius behavior from a uniform
initial pickup.

The first 18 pressure ramps pass nine gates; nine fail, especially wide spacing
with changing pressure. Constant-pressure cases match exactly. The next run
exposes exact packed visible-change masks for the six sparse ramps so spacing
can be inferred from full footprints. The failing fidelity gates stay enabled;
linear sensor decoding alone does not establish source scheduling parity.

The hard Pixel candidate now accepts fixed ellipse aspect and angle through its
independently written analytic geometry. KPP angle radians are converted to the
runtime degree unit; dynamic angle/aspect and wet ellipse geometry remain outside
this candidate. `pixelshape` enforces the existing dry gates on eighteen original
16/64-pixel ellipses at ratios .25/.5/1 and angles 0/45/90 degrees. This is an
experimental extension awaiting measured qualification, not a verified source
profile. Four additional constant fractional-pressure cases separate tip-size
rasterization from varying-pressure scheduling in the pressure workload.

At `997ba9b3`, heterogeneous third-dab Smear passes all sixteen gated cases,
including inactive radii 0/50/100/300. The four radius-zero Dulling cases fail and
establish its different state rule: use the previous center pickup uniformly,
then mix against each current destination pixel. The independent Dulling path
now samples that center instead of translating Smear's field; a two-color
third-dab regression distinguishes both states. Full reference requalification
is required for the correction and all prior wet gates remain enabled.

The six exact sparse-pressure masks at `997ba9b3` expose the sampling rule:
size pressure is evaluated at the next integer path-distance step while dab
positions remain fractional. This predicts rising and falling dab centers within
the half-pixel uncertainty of raster plateau centers; direct continuous pressure
has accumulating displacement. The experimental scheduler now uses accumulated
path distance for this sample, bounded by the current input event, and leaves
native scheduling unchanged. The captured .25-to-.75 ramp has 106 dabs, now a
numeric regression. All eighteen source gates and additional fractional-size
cases remain required; tablet-delivery equivalence is still a separate gate.

`wetswitches` measures 48 first/second/third-dab cases with smear-alpha disabled
or smudge-length disabled, across all four models and two color rates. These
active settings remain native conversions until independently implemented; their
measurements cannot count as parity. Isolated state transitions are needed before
assigning meanings to disabled toggles from long-stroke appearance alone.

The opacity matrix now records both `Node.paintLine` and synthetic Qt freehand
delivery. At the prior measured head, the line API accumulated opacity and ignored
static flow for both serialized painting modes; that evidence does not qualify
stroke-level wash behavior. The zero-flow API expectation was corrected to the
observed behavior, while candidate pixel thresholds remain enforced. The new
freehand group distinguishes delivery from settings semantics. Odd-size Smear
failures retain their gates and now publish exact second-dab BGRA footprints.

Dry changed-set gates now use the same visible-pixel definition as wet gates on
mixed backgrounds. Raw changed-set IoU and the raw-union numerical region remain
in the report. At 0c514ec2, three pressure cases had raw IoU .974–.978 solely due
to invisible RGB changes; visible IoU was .997–.998 and all numeric/bounds gates
passed. Zero-pressure endpoint cases retain real channel/bounds failures and
remain failing. This is a shared metric correction, not a wider tolerance.

At 0c514ec2 the 48 alpha/Smudge Length source measurements completed. The next
head binds both switches to explicit portable candidate state and applies the
existing channel/alpha/visible-coverage gates to those cases and 24 combined
cases. Odd-size Smear is corrected from the full acquired 7px mask. The ellipse
fixture now sets its requested source rotation; the previous harness explicitly
reset it to zero, explaining the unrotated reference bounds. Synthetic freehand
opacity still ignored both serialized PaintingMode variants, so public bundled
setting metadata and isolated PaintOpAction/PressureFlow cases resolve the
setting keys before further mode claims. They are exploratory and not parity.
