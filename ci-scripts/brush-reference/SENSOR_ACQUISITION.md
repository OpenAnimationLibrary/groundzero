# Tablet sensor acquisition integration

`tabletsensors` and `spacingdynamics` are included in the reference workflow
matrix. `prepare.py` dispatches to `sensor_fixtures.prepare`; the normal plugin
copy installs `sensor_probe.py`. For an isolated CI acquisition, select either
group using `BRUSH_REFERENCE_GROUP` in the standard reference workflow.

The source plugin validates effective settings and delivered values, retains
`sensorEvidence` per record, and saves bounded `.sensor.json` diagnostic captures
before no-op/repeat gates. The constant Qt tilt(60,0), identity declination-to-Ratio endpoint now requires
zero paint; all other cases require nonempty output. This endpoint was observed
on pinned build bd6e8d0 after effective-control/delivered-input validation, with
the static counterpart nonempty and tilt30 producing half the mask height. The
exception is derived from settings/events, not case number, and asserts exact
unchanged output rather than skipping a gate. Geometry and repeated-state checks
remain active. New empty configurations must be investigated independently.

Comparison requires all 33 authored contact events and matching source evidence.
Native replay consumes the **delivered** image/pressure/tilt/rotation fields and
millisecond-quantized source-relative clock. Tilt degrees are divided by60 and
barrel degrees converted to radians, matching the native application's existing
input convention; no source normalization equivalence is asserted. Results mark
`sensorAcquisitionInputsValidated` separately from `sensorSourceParityQualified`
(false). Both groups remain exploratory native-conversion metrics and are not
added to source-candidate gates.

The initial `tabletsensors` matrix has 76 cases: seven constant and one swept barrel-rotation
traces, signed X/Y tilt endpoints and ramps, seven tilt-direction configurations,
five tilt-elevation configurations, and six drawing-direction paths. Every
configuration has an otherwise identical sensor-disabled control. The generated
rectangular 24px tip has ratio0.3 so angle changes are visible. All other dynamics
are disabled, pressure/opacity/flow fixed, brush smoothing disabled by the shared
Qt harness. Existing pressure-size/transfer groups are not duplicated.

The initial `spacingdynamics` matrix has 16 cases: pressure quarter/half/full/ramp, active/disabled
spacing sensor and isotropic spacing off/on. Coordinates and timing are identical
within each pair. The common curve is identity; native/source spacing equations
are not presumed equivalent. The rendered bounding region, including alpha,
retains discontinuities, corners and reversals. Unexpected empty output is captured for diagnosis and fails acquisition.

Every contact event must survive Qt5 injection with matching image coordinates,
pressure, signed tilt and rotation, and matching relative timestamps. Absolute
source event time remains monotonic. This proves injected values, not physical
tablet-driver behavior or internal dab timing. Independent repeat hashes remain
enforced by the existing baseline path. Static controls and active cases should
be inspected together before admitting any native or source-profile mapping.

## Primary facts and uncertainty

- [Krita sensor manual](https://docs.krita.org/en/reference_manual/brushes/brush_settings/tablet_sensors.html)
  identifies tilt axes/direction/elevation, rotation and drawing angle.
- [Krita brush options](https://docs.krita.org/en/reference_manual/brushes/brush_settings/options.html)
  documents rotation anchoring differences and isotropic spacing. The fixtures
  measure those distinctions rather than import a normalization equation.
- Official KDE RGBA bundle Git blob
  `318d2861adfe3a5eff9f2d4ab5064e944fd8a2ff` supplies independently inspected
  serialized `RotationSensor` drawingangle/ascension, `RatioSensor` declination,
  `SpacingSensor` pressure, enable/curve/value fields and `Spacing/Isotropic`.
- Official KDE default bundle Git blob
  `337d0e4168ac4eb82727adfbec97099b09518490` supplies serialized xtilt/ytilt
  children and ascension/declination. Only KPP XML metadata was inspected; no
  engine implementation or preset pixels were copied.

The literal barrel `rotation` sensor identity is provisional: it follows the
public sensor name and existing adapter vocabulary, but was not observed in the
inspected bundled presets. Effective XML preservation alone does not prove that
its renderer consumes rotation. Enabled-versus-disabled output differences and
additional original source UI export remain necessary before claiming that
mapping. No runtime normalization or parity is implemented by this acquisition.


Controls, exact delivered contact events and lossless bounded alpha-capture chunks
are emitted as CI notices before no-op gates, including failing cases. This keeps
evidence reviewable when artifact downloads are unavailable. The original case059
sensor artifact could not be materialized (HTTP403); its unseen contents are not
claimed as evidence. The inference uses the recorded no-change error after the
validated capture stage and the adjacent captured half-height/static controls.

## Appended discriminators (existing IDs retained)

The complete matrices now contain **138 tablet-sensor cases** and **112 spacing
cases**. Original tablet IDs000–075 and spacing IDs000–015 remain in their
original order and retain their authored traces/settings.

- Tablet076–091: X/Y tilt at signed10/20 degrees with spacing4. These break the
  180-degree rectangle symmetry that made the original signed30/60 controls
  ambiguous.
- Tablet092–107: barrel angles0/45/90/135/180/225/270/315 using an original
  asymmetric notched-L alpha GBR, each with its disabled control.
- Tablet108–125: the same asymmetric tip with neutral, cardinal and diagonal
  tilt-direction inputs; Tablet126–137 applies it to the six drawing paths.
  The static view rotation stays0, avoiding dependence on the separate bitmap
  static-view-rotation investigation. Source file/registry/effective-brush
  validation and actual resource readback are reused; candidate comparison loads
  a generated bundle containing exactly the same original GBR and preset.
- Spacing016–111: spacing4/8, horizontal/vertical/diagonal traces, four pressure
  inputs, sensor enabled/disabled and isotropic off/on. Sparse separated stamps
  expose placement that the overlapping original spacing1 traces could hide.

All new cases retain effective-control/injected-event checks and independent
repeat gates. No renderer normalization is inferred or source gate relaxed.
The source captures establish that barrel rotation is consumed; its absolute
orientation for asymmetric tips and full sensor equations remain to be measured.
