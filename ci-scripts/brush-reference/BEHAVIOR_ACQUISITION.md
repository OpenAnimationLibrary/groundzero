# Remaining source behavior acquisition

These are acquisition boundaries, not completed compatibility claims. Runtime
implementations and candidate gates require the resulting measured evidence.
Only public documentation and serialized KPP metadata were inspected.

## Wired: deterministic Mirror pressure thresholds

`mirrortips` runs 42 cases: horizontal, vertical and both axes, seven pressure
values (.1/.25/.49/.5/.51/.75/1), each with enabled and disabled Mirror controls.
An original asymmetric notched-L alpha GBR makes each flip distinguishable.
Opacity/flow are1; size/rotation/scatter dynamics are disabled. Static view
rotation stays0. Every case uses33 authored Qt contact events and an independent
repeat. The source registry and original resource checksum, effective Mirror
flags/curve/axes, actual delivered input and full320x320BGRA region are checked
and retained. Candidate imports use the same bytes in generated bundles.

Complete counts, nonempty source output, exact delivered-input matching and
Mirror-axis readback are enforced in comparison. Existing bounded sensor notices
retain captures even on failure. Metrics remain exploratory native conversion;
no Mirror threshold, random law or source parity is presumed. Original resources
are8x8, packages contain one tip and one preset, and no external paths resolve.

Observed fields in official default bundle Git blob
`337d0e4168ac4eb82727adfbec97099b09518490`: `PressureMirror`,
`HorizontalMirrorEnabled`, `VerticalMirrorEnabled`, `MirrorSensor`,
`MirrorUseCurve`, `MirrorUseSameCurve`, `MirrorValue`, `MirrorcurveMode`.
The original fixture substitutes the documented pressure sensor and its identity
curve for the bundled fuzzy sensor. Effective source serialization and rendered
paired controls qualify this use; no source implementation was copied.

## Other missing groups: precise next work

| Behavior | Established public/serialized facts | Required discriminating acquisition |
| --- | --- | --- |
| Scatter | PressureScatter, ScatterSensor/Value/UseCurve, Scattering/AxisX/AxisY present in official default metadata | Separate X/Y/both-axis groups;100 strokes per enabled configuration, matched disabled controls, sparse dabs, centroid offsets in drawing coordinates, per-stroke/transition statistics. Do not require random pixel-repeat equality. |
| Speed | SizeSensor id=speed observed | Identical geometry at independently controlled actual wall-clock cadences and event timestamps; raw timestamp changes alone cannot establish which clock drives the source. Record real delivery times and interpolation before qualification. |
| Time | A time sensor with duration and periodic attributes observed | Paired periodic/nonperiodic duration boundaries, stationary and moving traces with actual pacing; validate source effective sensor attributes. Duration units and phase endpoints must be measured. |
| Distance | Public sensor documented; no distance sensor serialization observed in inspected bundles | Acquire a source UI-exported preset recording its distance/periodic controls before authoring exact metadata. Do not invent attribute names from the UI label. |
| Timed airbrush | Public stationary accumulation behavior; legacy AirbrushOption/isAirbrushing/rate and newer PaintOpSettings/isAirbrushing both observed | Establish active namespace/rate units via effective UI export. Add bounded real-time stationary holds with measured monotonic delivery, timer settling, and brush-release cancellation. Existing zero-delay event delivery is insufficient. |
| HSV foreground | Public Pixel Hue/Saturation/Value effects documented; inspected presets expose Darken but no matching HSV option schema | Capture canonical active HSV presets and exact enabled/value/curve keys; do not substitute unrelated ColorOption/useRandomHSV from another engine. Then use nonprimary colors, pressure endpoints/ramps and independently varied H/S/V. |
| Pixel wash secondary mask | Official RGBA metadata includes MaskingBrush/Enabled, MaskingCompositeOp, MasterSizeCoeff, UseMasterSize, and nested MaskingBrush/Preset settings | Original primary/nested tips; enabled controls × wash/build-up × multiply/darken/burn × master-size coefficients. Verify nested effective XML/resources, overlapping versus separated dabs, and full alpha/color regions. Acquisition does not license approximating unknown secondary blend behavior. |

Primary documentation:

- [Krita brush options](https://docs.krita.org/en/reference_manual/brushes/brush_settings/options.html)
- [Krita sensor definitions](https://docs.krita.org/en/reference_manual/brushes/brush_settings/tablet_sensors.html)
- [Pixel engine HSV controls](https://docs.krita.org/en/reference_manual/brushes/brush_engines/pixel_brush_engine.html)
- Official [default bundle](https://github.com/KDE/krita/blob/master/krita/data/bundles/Krita_4_Default_Resources.bundle)
  at the blob above, and [RGBA bundle](https://github.com/KDE/krita/blob/master/krita/data/bundles/RGBA_brushes.bundle)
  Git blob`318d2861adfe3a5eff9f2d4ab5064e944fd8a2ff` for secondary-mask metadata.

All asset pixels/names used by the new acquisition are original. Bundled files
were inspected only for serialized parameters, without copying images or foreign
renderer code. The unresolved groups remain explicit work, not placeholder
fixtures or evidence of support.

### Pinned option metadata follow-up: HSV and distance

The earlier missing-field assessment is superseded by isolated serialization/UI
metadata at Krita `bd6e8d0` (the reference application's 5.3.4.1 build):

- [KisStandardOptionData.h](https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisStandardOptionData.h)
  establishes lowercase option IDs `h`, `s`, `v` for hue, saturation and value.
- [KisKritaSensorPack.cpp](https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisKritaSensorPack.cpp)
  read/write metadata establishes `Pressure` + ID and ID-prefixed `Sensor`,
  `UseCurve`, `UseSameCurve`, `commonCurve`, `Value`, `curveMode` fields.
- [KisSensorData.cpp](https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisSensorData.cpp)
  and [KisSensorData.h](https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisSensorData.h)
  establish sensor XML `periodic` and `length` attributes (time uses `duration`).
- [KisDynamicSensorIds.h](https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisDynamicSensorIds.h)
  establishes literal IDs including `distance`, `time`, `speed`, `rotation`.
- [kis_paintop_settings.h](https://github.com/KDE/krita/blob/bd6e8d0/libs/image/brushengine/kis_paintop_settings.h)
  establishes modern `PaintOpSettings/isAirbrushing`, `rate` (dabs/second),
  `ignoreSpacing`, and `updateSpacingBetweenDabs`. These are format/API facts,
  not acquired timing or rendering equations.

`option_fixtures.py` wires `hsvdynamics` (72 cases: three isolated channels,
two nonprimary foreground colors, six constant sensor curves, enabled/disabled
pairs) and `distancesensors` (32 cases: lengths64/128, periodic0/1,
east/north/diagonal/retrace paths, enabled/disabled pairs). All use pressure1.
Constant HSV curves deliberately avoid treating Qt-delivered pressure as proof
that the source renderer consumes pressure; other acquisition groups have raised
that unresolved issue. Distance curves retain a positive .25 minimum size.
Effective sensor attributes, curves, channel enable flags, source foreground
readback (display conversion tolerance1/255), and all33 delivered contact events
must validate. Full BGRA regions are retained; HSV additionally emits bounded
lossless color notices. Native comparisons remain exploratory. No source behavior
or parity is established merely by successfully serializing these fixtures.

Masked-brush acquisition must include the normative Multiply **and Subtract**
modes described by the [official manual](https://docs.krita.org/en/reference_manual/brushes/brush_settings/masked_brush.html).
Existing observed multiply/darken/burn examples do not substitute for Subtract;
its exact serialized registry identifier still requires metadata evidence.

### Original masked-brush acquisition

`maskwash` and `maskbuildup` each contain108 paired cases: mask off,
Multiply, Subtract; linked master-size coefficients .5/1 and independent-size;
mask opacity/flow (1,1), (.5,1), (1,.5), (.5,.5); and independent spacing
controls (primary,mask) (.25,.5), (.25,2), (2,.5). The24px round primary and
12px rectangular half-aspect secondary are original embedded procedural tips,
with no external resource lookup or asset licensing dependency. Each source
stroke uses the public Node.paintLine API at pressure1. Every case repeats from
an identical transparent canvas. Full-image output and bounded complete320x64
BGRA captures are retained, including nested effective metadata and delivered
line endpoints. Any painted pixels outside the capture fail acquisition.

Pinned metadata sources (only serialization/registry facts used):

- [KisPaintopSettingsIds.cpp](https://github.com/KDE/krita/blob/bd6e8d0/libs/image/brushengine/KisPaintopSettingsIds.cpp)
  defines `MaskingBrush/Enabled`, `MaskingCompositeOp`, `UseMasterSize`,
  `MasterSizeCoeff` and the `MaskingBrush/Preset/` nested prefix.
- [KoCompositeOpRegistry.h](https://github.com/KDE/krita/blob/bd6e8d0/libs/pigment/KoCompositeOpRegistry.h)
  identifies Multiply as `multiply` and Subtract as `subtract`, resolving the
  prior Subtract identifier gap.
- [KisPaintingModeOptionData.cpp](https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisPaintingModeOptionData.cpp)
  serializes Wash as2 and Build-up as1; these canonical values are used.
- [KisMaskingBrushOptionProperties.cpp](https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisMaskingBrushOptionProperties.cpp)
  records independent master-size-link and coefficient fields, with nested
  brush settings. No rendering implementation was used to generate these probes.

All authored nested scalars and procedural geometry attributes must survive
source effective-setting readback. Off controls must paint. Enabled results,
including empty output, are explicitly exploratory observations, never accepted
as parity. Build-up cases diagnose the manual's Wash-only behavior; preserving
requested metadata does not prove the renderer applies masking in that mode.
Mask opacity and flow remain independent dimensions; no source representation
has been collapsed into the native secondary opacity control. These cases cover
procedural resources only; bitmap secondary resources and broader sensor/dynamic
interactions still need their own qualified acquisitions.

### GIH statistical qualification gate

`stochastic_gate.py` consumes100 source strokes and100 distinct native RNG seeds.
Source execution is shuffled with the fixed acquisition-order seed0x47494831;
records retain executionOrdinal and are then restored to canonical preset order
for cache completeness. This order seed does not set or claim knowledge of the
source application's opaque RNG. Each independently initialized native Stroke
retains the existing distinct seed sequence. No pixel-seed correspondence is
assumed.

The fixed authored capture region is split into three equal-width spatial bands.
Both applications must have zero painted alpha outside that region; shifted or
extra output cannot be hidden by cropping. Each band measures:

- coverage: fraction of nonzero-alpha pixels;
- integrated alpha: alpha sum divided by255 and band area;
- mean width: mean nonzero vertical coverage among occupied columns divided by
  authored tip diameter (empty bands have width0);
- occupied tips: fraction of authored separated tip cells with nonzero alpha,
  with cells assigned to bands by their centers (not number of color classes or
  connected components; original tips may have disconnected islands);
- high-frequency alpha energy at fixed1,2,4,8px scales: average squared alpha
  differences over horizontal and vertical pixel pairs within each band.

All metrics are normalized; width is normalized by authored diameter, not canvas
height, and occupied-tip count by the number of authored cells in the band.
For each of the24 band/statistic combinations, independent bootstrap samples
resample100 source and100 candidate strokes10,000 times with fixed analysis seed
0x52435031. Conservative2.5%/97.5% order-statistic endpoints must lie wholly
within ±max(.01,.05×abs(reference mean)). The candidate/reference sample-SD
ratio must also lie within[.8,1.25], except where both sample variances are
below the normalized .01 floor specified by RCP1. A zero reference variance
with nonexempt nonzero candidate variance fails. No failed bands are averaged
away. Reports retain all per-stroke measurements and interval/variance results;
any failed gate makes CI fail. A pass is statistical verification of this original
fixture only, not universal GIH fidelity or pixel verification. Artist-selected
fixture review remains separate.

The stdlib-only decoder caps each region at2048×128, bounds compressed payloads
and decompression, and rejects trailing/incomplete streams. Bootstrap arrays are
bounded to100 strokes and10,000 results per metric. CI runs rejection/geometry
unit checks and the actual source/candidate gate; no local tests were run.

Bitmap source job124326039148 recorded requested90deg as90.0002104591497deg
while all other view modifiers matched. Resource-only view roundtrip tolerance
is therefore .001deg (at16px diameter, tip-edge displacement below .00014px).
Actual readback stays in the report; rendering fidelity tolerances are unchanged.

The GIH gate additionally verifies observed **color-frame selection**, since the
three original frames have identical alpha and alpha-only statistics cannot
qualify the selector. Candidate labels are measured from alpha-weighted RGB in
the same authored separated tip cells used by source `frameSelections`; mixed
nonprimary colors fail identification. Each band adds three frame fractions,
one missing-cell fraction, and nine adjacent transition joint probabilities.
Every value is computed per stroke, then subjected to the same100-stroke,
10,000-resample mean/SD gates. Missing cells remain in denominators and have a
separate gate. Transitions span adjacent slots within each band only; the joint
probabilities preserve both frame marginal and transition behavior without
silently dropping strokes that lack a particular origin frame. These39 gates
supplement all24 alpha gates; all63 must pass. Distinct seed strings alone never
qualify stochastic behavior. A CI negative regression explicitly rejects an
always-first-frame selector even though its alpha statistics are identical.
Verification remains limited to this original three-color fixture, including
observed frame frequencies and adjacent transitions; no universal GIH or source
profile verification is inferred.

Statistics geometry is an exact versioned fixture contract: rect
[232,1000,1041,48], diameter8,32 slots. All dimensions, slot counts and origin
must be integers with those exact values before any per-slot or raster loop;
new geometry requires an intentional schema/test update. Alpha planes must match
the exact area. Reports are read through a32MiB bounded read before JSON parsing.
CI negative cases cover huge slot counts, tiny rasters, zero/nonfinite diameter,
invalid scalar types/origins, oversized decompression and oversized reports.

### Real-time paced source acquisition

`airbrushtimed` adds32 original cases: serialized rates5/20, ignoreSpacing
false/true, stationary versus256px movement,250ms/1s duration, enabled/disabled
pairs. `timedsensors` adds12 paired Size controls for time (serialized duration1000,
periodic false/true) and speed at the two movement speeds. Flow is .1 with
Build-up mode so repeated deposits remain observable. Timing behavior is not
inferred from the serialized rate or duration. The pinned public header describes
rate as dabs/second; the current manual's lower-rate/faster wording differs, so
actual measurements must resolve interpretation for this specific source build.

Serialization provenance is the previously linked pinned
[KisAirbrushOptionData.cpp](https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisAirbrushOptionData.cpp),
[kis_paintop_settings.h](https://github.com/KDE/krita/blob/bd6e8d0/libs/image/brushengine/kis_paintop_settings.h),
[KisSensorData.cpp](https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisSensorData.cpp)
and [KisDynamicSensorIds.h](https://github.com/KDE/krita/blob/bd6e8d0/plugins/paintops/libpaintop/KisDynamicSensorIds.h).
Only field identities/read-write metadata were used, not source renderer equations.

Qt event delivery is paced by actual monotonic deadlines, using bounded Qt timer
callbacks while the GUI event loop continues servicing source timers. A stationary
case sends press, holds with **no fabricated move events**, then releases. Moving
cases deliver33 contacts over the same path at two real elapsed speeds. Every
input event records monotonic microseconds and actual Qt device timestamps.
Pacing is limited to1s nominal per stroke and250ms maximum lateness; stale or
incomplete delivery fails. Total nominal contact time across both groups is27.5s,
plus application setup/delivery processing. Source pressure mode follows the
pinned explicit input contract and remains checked after proximity/before contact.

Effective timer/sensor settings, actual contact/release count, coordinates,
pressure and elapsed times must validate. Complete320x64 BGRA regions, settings,
actual events and release timing are persisted and emitted in bounded notices;
painting outside that region fails. One fresh stroke is acquired per case;
exact-repeat hashes are deliberately not required for wall-clock timer captures.
These are synthetic Qt inputs, not physical-tablet latency measurements.

Native comparisons replay measured contact times and maintain the final contact
position until the measured release time. They retain unsupported-setting
import diagnostics and explicitly remain unqualified native-conversion metrics.
No timer/speed equation or timed source-profile parity is established by these
acquisition hooks. Future source semantics must use the captured timing rather
than treating authored/fabricated timestamps as actual elapsed time.
### Fixed source HSV candidate

The hard round Pixel candidate has a separate versioned fixed-channel color
adapter. It accepts one explicit constant two-knot pressure curve for Hue,
Saturation, or Value, with the other two controls disabled. Native color dynamics
are unchanged. Portable `sourceHsvChannelV1` and `sourceHsvValueV1` must occur
together; older records default to off. Nonconstant curves, simultaneous channels,
soft/image tips, texture, wash, transfer modulation and wet combinations are not
admitted by this adapter. Disabled source controls remain inactive.

Original source captures for two opaque foreground colors and six control values
support hue rotation about neutral .5 and piecewise value scaling/lightening.
Saturation scales below neutral and adds above neutral. The saturation candidate
has a measured one-byte RGB residual for one of twelve original active samples;
this is explicitly not byte-exact reference verification. Engine regressions use
the independently captured RGB values. Reference comparison always rerenders the
current candidate and enforces full union-region maximum RGB error: zero for Hue,
Value and disabled controls, at most one byte for Saturation, with zero alpha
error and the existing hard-mask geometry gate. Source qualification flags remain
false until the broader contract is completed.

`hsvheldout` adds 54 independent source acquisitions: three previously unseen
foreground colors (including gray), three unseen/neutral controls, all three
isolated channels, and enabled/disabled pairs. This also challenges achromatic
saturation behavior. These are qualification gates, not evidence of success
before CI runs. Neither heldout inputs nor expected source output are generated
by the candidate color equations.

`hsvjoint` is a separate 36-case acquisition, not an expanded runtime admission:
for each of the two original foreground colors it captures three channel pairs,
two non-neutral three-channel combinations, a three-channel neutral control, and
three isolated identity-pressure curves, each enabled and disabled. It uses the
public `Node.paintLine` endpoint-pressure interface, records those exact calls,
and validates every channel's flag, shared curve and sensor payload. It does not
infer internal dab pressure from endpoint arguments. Full source raster evidence
and repeat gates remain mandatory. Until those captures identify composition and
dynamic behavior, V1 remains the backward-compatible single fixed-channel
candidate; multiple active channels and nonconstant curves are not admitted.

A subsequent compositional adapter should use a separately versioned ordered
channel representation and explicit curve semantics, migrating V1 as exactly one
fixed channel. Neither an order nor a migration to guessed native HSV dynamics
is implemented by this acquisition change. Joint captures and independent
heldouts must establish those semantics first.

### Prospective GIH label sample-size correction

The preceding39 color-label gates are **moved** to separate `gihlabels` acquisition;
the original `gihstatistics`100 straight strokes retain all24 alpha gates.
The old32-slot strokes have only10–11 labels/band: even correct independent
uniform selectors give frame-fraction difference standard error about .020,
95% interval halfwidth .039, exceeding the unchanged .0167 threshold. They
cannot provide adequate positive evidence. Thresholds are not widened and no
retry-until-pass or seed-selection strategy is used.

The new workload is100 additional independent strokes/application. Each is one
continuous96-vertex serpentine path, with3072 separated8px stamps at32px pitch
on a64×48 grid inside the same2048² canvas. The public pinned
[Node.paintPath API](https://github.com/KDE/krita/blob/bd6e8d0/libs/libkis/Node.h)
receives one QPainterPath, foreground stroke and no fill; no per-dab Qt event
flood or viewport dependence is introduced. Paths are exact bounded original
geometry. Each source raster is read once;3072 actual color labels are extracted
from fixed13×13 cells. Only labels are added to status JSON, not huge embedded
full-raster arrays. Full PNGs remain artifacts under the existing64MiB/file,
512MiB/cache and4096-file admission bounds; limits are not increased. Fixture
status/report reads retain32MiB bounds. The source's600s execution bound remains.

Leading/middle/trailing **path bands** each contain1024 labels (16 consecutive
rows) and1023 adjacent transitions. For independent uniform draws, frame-fraction
standard error of a two-application mean difference is about .00208, giving95%
halfwidth .0041 against threshold .0167. Accounting for overlapping same-label
transition pairs, the largest joint-transition standard error is about .00170,
with95% halfwidth .00334 against threshold .01. These prospective values leave
room for random mean differences; they do not assume actual source draws are
independent or uniform. Bootstrap still resamples whole independent strokes,
retaining within-stroke dependence; all39 unchanged mean/SD gates must pass.

CI includes a positive control generated from two distinct, preselected RNG
streams:100 independently drawn3072-label sequences each, without balanced
counts or retries, tested through all39 full10,000-resample gates. The same
source population must reject an always-first-frame candidate. These analytical
controls qualify the measurement design, not source behavior. Production reports
distinguish intervals wholly outside tolerance (`measured-mismatch`) from
intervals overlapping the boundary (`insufficient-precision`); either fails CI.
Both separate24-alpha and39-label workloads must pass before fixture-level
statistical verification, and neither alone claims overall source parity.

Timed acquisition transfer correction: airbrush/timed fixtures now use
OpacityValue .1 and FlowValue1 with matching view controls. Previously .1 flow
with curves disabled could leave the first Build-up dab opaque, hiding subsequent
timer deposits. All pacing, timing gates, enabled/off controls and effective
readback are preserved; no tolerance was relaxed.

`wethsv` captures 96 original cases: four Color Smudge models, three isolated
constant HSV channels, two foreground colors, controls .2/.8 and disabled pairs.
Node strokes cross a transparent-to-blue boundary with Color Rate .25, smudge
rate .5 and radius zero. Actual model switches, Color Rate enable/curve/scalar,
HSV payloads, foreground and endpoint delivery are checked before acquisition
is accepted. Complete source images, repeats and bounded color notices remain
available even when candidate comparison fails.

The wet HSV gate requires matching source profile/channel/value eligibility and
full union-region max RGB <=2/255, max alpha <=1/255, changed-pixel IoU >=.98,
and nonempty candidate output. Current wet HSV eligibility remains disabled;
native conversion cannot satisfy this gate. Shared foreground plumbing or
synthetic all-model equivalence tests do not establish source interaction parity.
