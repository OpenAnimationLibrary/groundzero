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
