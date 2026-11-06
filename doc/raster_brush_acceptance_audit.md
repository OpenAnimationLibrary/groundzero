# PR 115 acceptance audit

The normative RCP1 revision 2 text in raster_brush_compatibility.md governs.
This audit records remaining proof obligations, not a reduced release scope.
Every Cartesian combination required by the profile tables must be accounted
for; candidate subsets and successful native conversions do not close rows.

| Requirement | Authoritative implementation/evidence locations | Remaining proof/work |
|---|---|---|
| Version/status model | rasterbrushpreset.h/.cpp, rasterbrushimport.cpp; preset status regressions | Preserve decoded/supported/converted/verified separation through every UI and persistence path; final-head CI |
| ABR 1/2 and modern 6-10 subversions 1/2 | abrparser.cpp, abrimport.cpp, abrparser_test.cpp; raster_brush_abr_resources.md | Independent version fixtures; required computed/sampled/pattern associations and all D dynamics; authorized P2711 behavior evidence |
| Unknown versions and active settings | adapter eligibility and parser tests | Explicit rejection/diagnostics without silent defaults/clamps; each declared version and range |
| K5341 Pixel tip families | Runtime/admission boundaries below; generated-tip and bitmap fixtures | Bounded Gaussian/Soft and bitmap candidates exist; remaining transforms, numeric envelope and K combinations remain implementation obligations |
| K5341 Pixel dynamics | engine mappings/import admission; sensor_fixtures.py, timed_fixtures.py | Complete pressure/speed/distance/time/drawing-angle/tilt/rotation/fuzzy controls and multiplicative curves, including combinations |
| Spacing, scatter, flip and mirror | Source scatter candidate and guarded mirror path below | General automatic spacing, scatter amounts/sizes/dynamics, transformed combinations and measured zero-spacing rules |
| GIH ordered/random | Versioned proposal/cache implementation; GIH original-resource fixtures and heldouts | Random phase-grid identification, independent resource/geometry holdouts, unchanged statistical gates and exact-head source comparison |
| Pixel transfer/color/blend | engine; HSV/transfer fixtures and compare.cpp | Required joint dynamics and Normal/Multiply/Screen/Overlay across build-up/wash; full operation-order evidence |
| Source masks and textures | sourceMasked, secondary and texture paths; masked/texture fixtures | Required secondary geometry/dynamics/resources; multiply/subtract polarity/depth and combinations; native mask equivalence is insufficient |
| Four K5341 smudge models | separate source wet equations/import guards; wet fixture groups | Complete each model's S settings, adaptive sampling, radius, transfer/HSV/texture interactions including New Dulling texture and disabled toggles |
| Native dry/wet behavior | native engine and host tests | Exact declared blend/reservoir math, alpha/erase policy, state reset, deterministic sampling and bounded operation order |
| Color document contract | host adapter and PNG resource handling | Encoded sRGB RGBA8 guard, explicit colored-asset conversion, premultiplication/quantization and saved pixels on every platform |
| Input and timer semantics | inputmanager/track/viewer and engine | Delivery/partition/pause/duplicate-time/tap/terminal/cancel behavior; source-clock identity and real device capabilities |
| Editor/preview/presets/resources | native brush UI and portability tests | All supported editable controls; actual-engine preview; export modes, rights, hash repair, missing assets and application lifecycle |
| Collection transactionality | archive/import/worker implementation and preset tests | Fault injection, cancellation/conflicts/reimport, no overwrites or partial installations in packaged apps |
| Raster transactions | fullcolorbrushtool and rasterbrushintegrationtest.cpp | Painting, undo/redo, cancellation, new-frame rollback, selection/alpha policy, save/reopen across all three installed platforms |
| Isolation/resource budgets | worker/response/archive/PNG sandbox paths; platform isolation document | Exact-head fuzz, malicious responses, cancellation, no-follow reads, allocation limits and packaged Windows/macOS/Linux confinement |
| Fidelity and repeatability | compare.cpp and source acquisition records | Contract canvases/events, complete union-region thresholds, coverage/endpoints, exact repeats and cross-platform reference tolerances |
| Wet drift | wet_drift_contract.py and native/source fixtures | 1/10/100/1000/10000 checkpoints, unchanged numerical/perceptual limits, transition direction/halos and transparency |
| Opaque RNG | stochastic_gate.py and captured independent stroke sequences | 100 strokes, bands, bootstrap mean-difference confidence intervals and variance-ratio thresholds from the normative contract |
| Platform/packages/performance/tablets | desktop workflows, installed fixtures, benchmark.cpp | Exact final commit fully qualified; real tablet input/latency and release performance acceptance |
| Documentation/review | acceptance ledger, portability/isolation/ABR documents and PR | Refresh final scope/evidence, no unsupported parity claims, completed review and explicit merge decision |

## Local implementation inventory

This section describes code, not executed verification. The authoritative
[K/S whitelist and Cartesian combination table](raster_brush_compatibility.md#explicit-combination-table-and-operation-order)
are unchanged. A setting rejected by today's source-candidate guards remains
work owed when RCP1 requires it. Well-formed unsupported KPP inputs may receive
an explicit native conversion; malformed inputs and invalid strict profiles can
reject import. Neither outcome supplies the missing source implementation.

The native engine already has dry blending, wash/build-up, sampled/generated
native tips, ordered/random image sequences, multiplicative linear/cubic sensor
mappings, size-relative manual spacing and timer scheduling, scatter/count,
HSV/background mixing, textures, independently spaced secondary masks, and
separate native Dulling/Smear state. These are native definitions, not K/S
implementations. See [Preset/Mapping/Secondary](../toonz/sources/include/toonz/rasterbrushengine.h)
and [native scheduling/deposition](../toonz/sources/toonzlib/rasterbrushengine.cpp).
The editor, real-engine preview, portable resource/repair paths and application
transaction fixtures are implemented; their exact-head platform validation is
still separate from this inventory.

The shared [profile descriptor V1](../toonz/sources/include/toonz/rasterbrushprofile.h)
now centralizes exact existing IDs, scheduling families, independent tap behavior,
seed policy and safe source-name recognition across engine, host and codec. It
also names capability/rejection categories for subsequent composition work.
This is classification plumbing: numeric/setting guards, serialized IDs and
source qualification remain unchanged; combined source stages are still open.

The following boundaries are enforced by
[`validate()`](../toonz/sources/toonzlib/rasterbrushengine.cpp) and
[`eligibleFixedSource()` plus its resource/control parsers](../toonz/sources/toonzlib/rasterbrushimport.cpp):

| Source component present in code | Current boundary; required work it does not close |
|---|---|
| Hard/default Pixel masks and pressure transfer | Hard masks use integer diameter and a separate hard-circle/ellipse placement model. Default soft circle/rectangle profiles require fixed unit aspect and zero angle and reject size/transfer dynamics. Hard-mask size/opacity/flow accept bounded pressure curves; barrel/tilt-axis rotation has its own narrow mapping and isotropic-spacing conditions. This is not the complete K sensor/product/geometry domain. |
| Gaussian and curve-Soft generation | `generated-mask-candidate-1` requires diameter 16 or 64, AA on, ratio 1, angle 0 and spacing at least .25. Static transfer/blends use shared Pixel stages; a pressure opacity or flow mapping requires the other control at unity and Normal blend. Texture, HSV, scatter, masks, size/ratio/rotation dynamics, AA-off and the rest of the numeric envelope remain rejected. `generated-geometry` and `generated-sensors` are focused source-only obligations, not implemented candidates. |
| PNG/GBR and ordered GIH sampling | Fixed bitmap profiles require equal frame geometry, scale 1-2, fixed aspect/angle/transfer, Normal build-up, one tip and no texture/secondary/HSV/scatter. PNG-as-mask retains its original RGBA through sampling. Required bitmap/GIH wash, blend, transfer, geometry-dynamic and texture/mask combinations remain open. |
| Random GIH proposal/cache selection | `random-bitmap-candidate-1` uses original equal-geometry RGBA frames and the same restricted bitmap context, with stroke-local proposal/actual-frame state and host-resolved independent stroke seeds. Native GIH remains uniform per dab. The unwrapped 64-bucket phase key is an explicit experimental V1 assumption, not established source granularity; all model/phase/reference qualification stays false. See [mechanism](../toonz/sources/toonzlib/rasterbrushgihselection.h) and [evidence/limits](../ci-scripts/brush-reference/GIH_ACQUISITION_FINDINGS.md). |
| Source scatter and mirror | Scatter is a distinct hard-circle profile: 8px, spacing 4, amount 1, one tip, fixed transfer/geometry and Normal build-up. It rejects texture, HSV, secondary/mask and mirror combinations. Source mirror requires one grayscale bitmap, zero rotation and its supported pressure control. General K scatter 0-5 diameters, transformed/dynamic amounts, flips and resource/transfer combinations remain open. |
| Source secondary mask and texture | `SourceMasked` is a separate accumulated generated-mask field, restricted to hard-circle Pixel Wash/Normal with fixed primary geometry, no texture/HSV and no native secondary. It does not implement arbitrary secondary alpha resources or required dynamics. Source texture accepts an opaque grayscale pattern with fixed scale 1, angle/origin 0, brightness 0, contrast 1 and static depth; random/per-tip origin and transformed/depth-dynamic combinations remain rejected. Native `Secondary`/`Texture` support does not close these K rows. |
| Four separate Smear/Dulling equations | Source wet profiles still require an integer-sized, hard, non-AA round tip, fixed unit aspect/zero angle, Normal blend, one tip and no secondary. Strength/color/opacity pressure mappings are identity-only; size, ratio, rotation, time/distance/tilt and multi-sensor products are not admitted. Smear records radius as inactive; Legacy Dulling currently caps radius at 100%, while New Dulling admits 300%. RCP1 requires the Dulling radius domain and applicable S controls for each model. |
| Wet state, HSV, texture and sampling | Disabled length/color-rate switches and smear-alpha have explicit state paths. HSV is limited to fixed/identity-pressure controls and a hard-tip, zero-radius, smear-alpha-on context; hard Pixel additionally excludes simultaneous transfer/texture. Wet texture excludes nonzero radius, smear-alpha-off and New Dulling. New Dulling texture is still unimplemented. Recursive Smear placement and a bounded Lab-checkpoint adaptive pickup path exist, but the pickup minimum/window rule and attempted-versus-accepted counts for clipped modern masks remain unresolved. See [`sourceWetDab()`](../toonz/sources/toonzlib/rasterbrushengine.cpp) and [sampling primitives](../toonz/sources/toonzlib/rasterbrushsampling.h). |
| Time, distance, automatic spacing and airbrush | Native interval units/periodic curves and editor controls exist. Strict source admission still rejects Time-size mappings and does not admit general K/S time/distance/speed/drawing-angle/tilt/fuzzy products. Automatic spacing and valid zero-spacing rules remain source-model obligations. The opt-in source airbrush runtime is only the fixed 16px/.1-opacity/5-or-20Hz context; it is not general imported K airbrush support. |

These profile names select mutually constrained implementations. In particular,
adding a generated-mask profile and a scatter profile does not implement
Gaussian/Soft with scatter; a bitmap profile does not add bitmap with Wash and texture;
a source-mask profile does not add masked Wash with HSV, texture and blend; and wet
HSV at radius zero does not add HSV with radius, smear-alpha-off and texture for all
four S models. Every allowed Cartesian combination in RCP1 remains required.

## Declaration preservation and current diagnostics

Source declaration preservation is now implemented separately from runtime
behavior. Record-only `SourceSensorsV1` retains scoped controls, raw switches,
curve knots, combiner codes and raw intervals; it does not invent source units
or interpolation. Record-only `SourceMaskDefinitionV1` retains independent mask
geometry, controls, generated definitions or one canonical decoded resource.
The resource participates in worker, portable, rights, repair and memory
accounting. Neither representation activates an absent source evaluator or
substitutes native secondary-mask equations.

The exact-profile descriptor centralizes existing scheduling, tip and random
process classification. It is not a general composition implementation. Required
joint source tip, placement, sensor, transfer, texture, mask and wet behavior
still needs identified equations and its own original reference observations.
Adding another descriptor or candidate name alone cannot complete that work.

The [derived support report](raster_brush_support_diagnostics.md) now exposes
bounded original declaration details and fixed omission reasons through import,
preview, editing and explicit native conversion. It separates preserved
declarations from configured/evaluated controls, derives current state rather
than persisting support verdicts, and retains safe omission categories across
portable reopening without raw XML, private resource names or arbitrary
diagnostics. Raw pressure switches are displayed independently of use-curve
switches; neither alone is treated as proof that a source evaluator is inactive.
Each required K/S combination remains either an implemented constrained path or
a named open implementation obligation; a source-only fixture is not a pass.
This implementation and its authored regression sources remain unexecuted.

Source-equation work still depends on discriminating observations where no
model has been identified. Examples include automatic/zero spacing, the random
GIH phase grid, full generated geometry/sensor ranges, non-generated sensor
normalization/curve products/source clocks, bitmap/RGBA transforms, Pixel
transfer/HSV/blend/texture operation order, general secondary-mask interactions,
Dulling pickup counts/clipping and New Dulling texture-aware pickup/feedback.
This nonexhaustive list does not replace any obligation in the tables above.
The [generated control obligations](../ci-scripts/brush-reference/generated_controls_fixtures.py)
and [GIH fractional heldouts](../ci-scripts/brush-reference/GIH_CORRELATION_ACQUISITION.md)
are authored inputs for that work, not completed source evidence.

## Evidence constraints

The prior implementation's failures and historical passes do not qualify the
recovery branch. All new fixtures execute in CI per CLAUDE.md. Photoshop export
associations/reference execution and physical tablet measurements remain external
prerequisites. Optional named paid packs are not prerequisites. Distance interval
editing and the native wet editor/resource/application lifecycle have now been
reconstructed on the recovery branch. Their implementation and authored
regressions do not replace final-head installed-application, resource-budget,
performance, cross-platform repeatability and complete source qualification.
No current implementation subset establishes that the original plan is fulfilled.
