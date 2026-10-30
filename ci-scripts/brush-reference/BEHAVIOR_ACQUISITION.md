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
