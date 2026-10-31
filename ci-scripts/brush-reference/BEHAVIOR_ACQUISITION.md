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

`maskwash` and `maskbuildup` each contain 156 paired cases: the original 108
controls plus 48 color/background heldouts. Original controls cover mask off,
Multiply, Subtract; linked master-size coefficients .5/1 and independent-size;
mask opacity/flow (1,1), (.5,1), (1,.5), (.5,.5); and independent spacing
controls (primary,mask) (.25,.5), (.25,2), (2,.5). The24px round primary and
12px rectangular half-aspect secondary are original embedded procedural tips,
with no external resource lookup or asset licensing dependency. Each source
stroke uses the public Node.paintLine API at pressure1. The original controls
repeat from an identical transparent canvas. The 48 appended
heldouts pair two non-primary foreground colors, transparent versus heterogeneous
opaque/translucent backgrounds, primary opacity 1/.5 and secondary opacity/flow
(.5,1)/(1,.5) for all three modes. Foreground readback is checked. Full-image
output and bounded complete320x64
BGRA captures are retained, including nested effective metadata and delivered
line endpoints. Any visible pixel change outside the capture fails acquisition,
compared with
the exact initial raster; transparent RGB is ignored only where alpha is zero.

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
source effective-setting readback. Off controls must paint. The original 108 Wash captures identify independent
primary/secondary spacing with omitted final-boundary dabs, byte-quantized
secondary flow accumulation and opacity applied after accumulation. An
independent model reproduces every alpha pixel of every original 320×64 capture.
Multiply and Subtract outputs sum to the corresponding unmasked alpha. Linked
half-size and independent 12px controls coincide. All 72 enabled Build-up masks
have the same alpha as their paired disabled controls.

The source candidate retains primary paint/base and a separate charged
secondary field. Either scheduler event recomposes dirty pixels, so a later
secondary dab can reveal or remove already retained primary paint. Native
Secondary semantics are unchanged. `sourceMaskedV1` preserves flow, opacity,
master-size linkage and geometry through portable save/reopen. Strict import
requires explicit known nested settings and generated hard rectangular masks;
unknown enabled controls stay native conversions with diagnostics. Build-up
records the mask as inactive. Initial Wash combinations require a fixed hard
round primary, static opacity, primary flow1, Normal blend and no texture, HSV
or dynamic masks. Other combinations, bitmap secondary resources and dynamics
remain unfinished contract work, not silently admitted.

All 156 cases now enforce the existing complete-image source candidate gates,
including the 48 new retained-color/background heldouts. Those heldouts and
C++ implementation tests have not yet executed at this revision. No broad
source parity or reference-verified flag follows from the alpha model alone.

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
### Versioned source HSV candidates

Native color dynamics remain unchanged. V1 retains its single fixed channel and
paired portable fields. V2 stores three explicit enabled/control slots, admitting
joint fixed controls or one identity-pressure H, S or V control. Mixed fixed and
dynamic controls, multiple dynamic controls, other sensors and nonidentity curves
remain excluded. Disabled slots must be neutral and V1 cannot coexist with V2.
The strict hard-round Pixel context remains required; the separately qualified
wet subset admits only V1, not joint or dynamic V2 controls.

Independent original foreground captures identify a chroma-domain transform.
For saturation control u, d=2u-1 scales original chroma by 1+d below neutral and
1+d+2*d*d above neutral. Intermediate chroma is capped at 255 encoded units.
Value scales maximum/chroma together below neutral; above neutral it moves the
maximum toward 255 while scaling chroma toward zero. Final chroma is capped at
the final maximum and hue-preserving reconstruction is quantized once. Hue shifts
by 360*(u-.5) degrees. Achromatic input stays achromatic. No source renderer was
copied and these equations do not change native HSV behavior.

The 24 order-discriminating captures and 30 static joint captures support this
composition. All 66 dynamic center samples match when evaluated at the measured
half-pixel future-pressure sample. These observations do not replace full-image
qualification: every candidate is rebuilt and compared over the union region,
including alpha and missing paint. Existing V1 Saturation allows at most one
RGB byte; all other HSV cases require exact RGB bytes, and all require exact
alpha and the hard-mask geometry gate. Source verification flags remain false.

`hsvheldout` retains 54 original acquisitions including gray. `hsvjoint` retains
36 paired fixed/joint/identity-pressure cases and `hsvorder` retains 24 cases
that distinguish noncommuting composition. `hsvpressure` now has 24 full-region
heldouts: two unseen colors, H/S/V and opposite .125/.875 Node pressure ramps.
`hsvchroma` adds 36 paired cases using two low-chroma colors, positive saturation
.55/.7/.9, isolated S and joint SV/HSV. Neither chroma cap activates for these
inputs, directly challenging the quadratic expansion away from capped training
examples. New heldouts are pending CI, not claimed source parity. All acquisition
controls, input readback, source version and repeats remain mandatory.

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

Repeat protocol `transparent-rgb-zero-v1` retains original BGRA captures and raw
hashes, including each repeated stroke PNG. It separately hashes a copy with RGB
zeroed only at exactly zero alpha. Every raw discrepancy is classified: only
pixels whose two alpha bytes are both zero may differ invisibly. Every alpha
change and every RGB change at nonzero alpha fails exact repeat equality; mixed
hidden/visible differences fail too. Source pixels and internal state are never
rewritten. This corrects the raw-byte gate that rejected observed transparent
black/white storage differences in wet HSV and texture-pickup acquisitions.
Cache schema 2 requires versioned canonical hashes, raw hashes, consistent full
classifications and hashed repeated PNG files; older/partial caches fall back to
fresh source acquisition. Candidate RGBA thresholds remain unchanged.
### Airbrush timer-boundary follow-up

Completed source job124365157119 at bf78a7bc recorded32 acquisitions. Disabled
stationary alpha stayed26. Enabled stationary alpha was49 for5/s over251ms,
105 for5/s over1001ms and20/s over251ms, and227 for20/s over1001ms.
IgnoreSpacing did not change stationary results. Moving ignoreSpacing=true
produced2 separated marks at5/s over.25s and6 at5/s over1s or20/s over.25s.
This supports the pinned header's rate-as-frequency interpretation, but exact
boundary counts depend on source timer firing order, which input timestamps
alone do not expose. No scheduler equation or count-specific patch is justified.

`airbrushtimed` now preserves000–031 and appends24 off-boundary cases032–055:
5/20 rates,375/1125ms stationary holds, three independent enabled/off pairs for
each setting. The20Hz half-period offset is25ms; actual event/release lateness
must remain within20ms (with the existing2ms timestamp-origin tolerance), keeping
accepted input away from the neighboring intended boundary. Older cases retain
their250ms diagnostic bound. Complete actual timing and per-repeat pixels remain
recorded; these repeats are observations, not retry-until-match behavior. Source
and comparison both enforce the same duration-specific deadline bounds. The
maximum nominal hold is now1.125s, and additional nominal hold time is18s.

The first joint captures matched the isolated equations but did not identify a
unique composition order: their saturation/value combinations commute.
`hsvorder` therefore adds 24 original paired cases with both saturation and value
above neutral, including an unsaturated foreground and three-channel hue shifts.
These cases distinguish saturation-before-lightening from the reverse; no order
is assigned merely because the earlier joint acquisition completed successfully.
### Time-period and speed-cadence discriminators

The original12 timed-sensor case identities are preserved. Cases012–019 add
1.25/2.25s paths with time duration1000, periodic/nonperiodic and enabled/off
pairs, ensuring actual painting continues beyond one and two periods.
Cases020–027 use17/65 contacts over the same256px path at.25/1s, with paired
enabled/off speed controls. Together with existing33-contact traces this
separates physical speed from event cadence. New traces enforce20ms lateness,
record every actual elapsed timestamp, and retain the same capture area. Maximum
nominal hold becomes2.25s; the added nominal contact time is19s. No source speed
normalization or periodic wrapping is inferred before these captures execute.

Completed job124365157160 shows enabled time-size starting at4px and approaching
16px over1s for serialized duration1000; disabled controls remain16px. The
independent importer therefore maps only explicit Size/time duration1000,
periodic0 to the existing **native** elapsed-time sensor with one-second scale.
Other durations, missing attributes and periodic1 remain unapplied. This is
portable native unit conversion, explicitly excluded from strict source-profile
eligibility: source time-to-dab scheduling remains unverified. Original import
fixtures verify exact admission, rejection of guessed periods and save/reopen.

The V2 extension and current admission limits are described in the versioned
source HSV section above. Portable snapshots preserve every enabled/control
slot, and explicit conversion to native clears all source HSV state with a
user-visible disclosure. Full-region pressure gates remain mandatory; sampled
center regressions alone cannot qualify a source profile.

### GIH advancement discriminators

The first powered label acquisition had near-uniform color frequencies but 27
failed transition gates. Self-transition joint frequencies were about .18,
versus native independent selection near .11. This is evidence of dependence,
not permission to fit a sticky random selector. The straight-stroke alpha
workload independently found one extra native endpoint tip. Native scheduling
and RNG remain unchanged.

Full original sequences were unavailable through the artifact download (403),
and aggregate transitions cannot identify lag, run-length or row-turn effects.
Each acquired GIH stroke now emits a bounded compressed exact label sequence,
its digest, lag-equality counts at 1/2/3/4/8/16/32/63/64, run-length counts and
separate row-boundary/interior transitions. Null slots remain explicit. The
existing 24 alpha and 39 powered label gates are unchanged.

`gihcadence` adds 144 original cases: incremental and random resources, spacing
2/3/4, six delivery modes and four independently acquired replicates. Modes are
one Node.paintLine, endpoint-only Node.paintPath, path vertices at each dab,
half-spacing vertices, fixed off-grid 17px vertices, and 32 independent short
Node.paintLine strokes. Each case observes the same 32 nonoverlapping stamp
centers, with a final endpoint one pixel beyond the last center. Thus an omitted
spacing-boundary endpoint cannot masquerade as a frame-selection difference.
The 8px tips and original three colored frames are unchanged. Serialized spacing,
resource identity and exact delivered paths/call counts are checked. No more
than 120 vertices or 32 source API calls occur per case, on the existing bounded
2048² canvas; complete images and compact actual labels are retained.

Candidate comparisons replay the identical paths and resets. Discrete native
strokes receive distinct explicit reproducible seeds; no correspondence with
source seeds is claimed. These cadence comparisons are exploratory, not a
replacement for the existing statistical gates or a new source compatibility
claim. They distinguish geometry-distance, input-segment and stroke-reset
advancement hypotheses. A versioned source selector remains blocked on actual
sequence/cadence evidence; no transition-rate fitting is implemented.

### Paced acquisition recovery

A dense 65-contact capture failed its existing 20ms scheduling bound before
comparison. The previous error omitted the failed index and measured lateness;
no numeric cause can be reconstructed for that run. Timing failures now report
phase, contact index, authored deadline, actual monotonic elapsed time, lateness
and unchanged bound. Contact/proximity is cleared before the failure propagates.

Exactly one retry is allowed only for this typed timing-contract failure. It
first drains source work, restores the entire original raster, refreshes and
drains projection, then services the event loop before delivering a new stroke.
Every discarded attempt emits a notice and remains in accepted timing evidence;
a second failure ends acquisition. All actual accepted contacts/release must
still satisfy the original 20ms or 250ms authored bound. Comparison validates
the discard provenance and timing arithmetic. Pixel differences, empty output,
resource/settings mismatches and any other failures are never retried. The
elapsed clocks used by candidate comparison remain actual accepted delivery,
not fabricated timestamps. CI tests exercise exact bounds and reject extra,
within-bound or pixel-driven discarded attempts.

The joint HSV matrix now appends 28 original paired captures (64 total), keeping
its first 36 preset identities unchanged. New cases cover identity-pressure HS,
HV, SV and HSV, plus each isolated pressure channel with two fixed companions.
Two foreground colors include an uncapped low-chroma input; opposing .125/.875
ramps exercise both temporal directions. Every channel's actual curve, enabled
state, source metadata and delivered input are checked. Complete source capture
is saved before candidate comparison. Mixed/multiple pressure admission remains
closed pending these observations: a comparison eligibility failure is expected
until evidence supports extending the current strict adapter. The full-union
exact RGB/alpha gates remain unchanged, including for the newly added cases.

The first retry-enabled dense acquisition still exhausted its one retry: the
65-contact/.25s control was 20,518µs late at contact11 (authored42,969µs,
actual63,487µs). The deadline-driven sender now omits the additional unconditional
Qt turn after each paced contact; the next future deadline already yields to the
event loop. Already-due contacts may catch up in the same bounded input batch,
with original measured timestamps and unchanged lateness checks. Stationary holds
still service timers. Non-paced input delivery is unchanged. Both rejected
attempts remain in the bounded terminal error even when only the runtime log tail
is available. This is acquisition scheduling, not a brush timer equation or a
relaxation of the authored timing contract; successful source execution is pending.
