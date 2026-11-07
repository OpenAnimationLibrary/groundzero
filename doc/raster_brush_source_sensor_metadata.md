# Preserved source sensor declarations

`Record.sourceSensorsV1` is versioned technical metadata. It never participates
in `Preset` validation, sensor evaluation or source-profile admission. Native
conversion and editing may change active settings while keeping this record of
the original declarations. Native packages, bounded worker responses and all
portable packaging choices retain it through the same strict codec.

The implementation addresses concrete losses in the earlier KPP adapter:

- Disabled or unused controls returned before their sensor data was retained.
- Active conversion understood one sensor and lost `sensorslist` combinations.
- Time conversion recognized only the existing 1,000-unit declaration; other
  durations disappeared. Distance already had a native interval adapter, but
  its source declaration, switches and original curves were not independent.
- Speed/fuzzy IDs and drawing-angle technical attributes could not survive a
  native conversion as source declarations.
- Native mapping interpolation could describe a conversion without retaining
  the separate common and per-sensor source knots.

Version 1 preserves primary and `MaskingBrush/Preset/` scopes for a fixed list of
K/S control targets. Each control stores nullable enable, UseCurve and
UseSameCurve switches; a nullable finite strength and numeric combiner code;
nullable common knots; absent/single/list/unsupported sensor layout; source-field
SHA-256; fixed unsupported-field tags; and ordered source sensor declarations.
Null means absent. A present but unprojectable field has an explicit unsupported
tag, so it cannot be confused with a missing or false declaration.
The `enabled` value is the serialized `Pressure*` declaration, not an inferred
activity decision. For example, the existing Pixel opacity/flow adapter can use
`UseCurve=true` while that retained Pressure flag is false. Preservation occurs
before that adapter's effective-field transformation, retaining both facts.

Sensor IDs use a fixed technical allowlist from KDE serialization, including
pressure, tilt axes, ascension/declination, rotation, drawing angle, time,
distance, speed and fuzzy-dab/fuzzy-stroke. Other recognized serialized IDs are
metadata only and do not expand the K/S active whitelist. Time duration,
Distance/Fade length and periodic declarations remain separate and nullable.
Source duration is tagged in source milliseconds, distance as source length and
fade as source dab count. The projection never converts these values to native
units or infers missing defaults. Known drawing-angle attributes retain their
independent numeric/boolean declarations. Finite technical values remain
metadata even when outside an active evaluator's accepted range.

`curveMode` remains a numeric source combiner declaration. It is not an
interpolation selector. The covered source XML layout has no interpolation
selector, so the preserved representation explicitly says `not-declared` and
retains the source knots. It never copies a native approximation's `linear` or
`natural-cubic` label into source metadata. No missing source equation is
implemented by this representation.

The strict codec rejects missing/unknown fields, wrong types, duplicate
scope/target or sensor identities, unsupported metadata versions, non-finite
numbers, inconsistent interval/unit declarations and malformed curves. Limits
are 32 controls, 16 sensors per control, 1,024 knots per curve, 65,536 knots total
and 8 MiB of encoded metadata. Unknown XML layouts, IDs/attributes or malformed
technical values are represented by explicit unsupported tags and the SHA-256
of the fixed source-field table. Arbitrary source XML, names, paths or unknown
strings are never copied into portable metadata. Hashes identify declarations;
they grant no authenticity, rights or source-reference verification.

Current upstream KDE format references establish serialized shapes only; they
are not proof that another Krita revision behaves like the pinned K5341 baseline:

- [KisKritaSensorPack.cpp](https://raw.githubusercontent.com/KDE/krita/master/plugins/paintops/libpaintop/KisKritaSensorPack.cpp)
  describes scalar switches, common curves and ordered `ChildSensor` lists.
- [KisSensorData.cpp](https://raw.githubusercontent.com/KDE/krita/master/plugins/paintops/libpaintop/KisSensorData.cpp)
  describes optional sensor knots, interval/repeat and drawing-angle attributes.
- [KisDynamicSensorIds.h](https://raw.githubusercontent.com/KDE/krita/master/plugins/paintops/libpaintop/KisDynamicSensorIds.h)
  provides exact serialized sensor identifiers.
- [KisCurveOption.cpp](https://raw.githubusercontent.com/KDE/krita/master/plugins/paintops/libpaintop/KisCurveOption.cpp)
  distinguishes combiner declarations from source curve construction. Its
  rendering equations are not imported by this metadata projection.

Authored regressions cover K/S import and native/worker/portable round trips,
inactive versus missing switches, independent sensor lists/knots, interval/repeat
values, source-only IDs, unsupported/private payload exclusion, strict malformed
metadata rejection and unchanged active native settings. Execution remains
separate from implementation.
