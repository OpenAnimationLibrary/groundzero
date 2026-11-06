# PR 115 acceptance audit

## Current stabilization evidence

Latest qualification checkpoint, `cfea0b542`: all six parser configurations,
import fuzzing and native metrics passed. Linux build-tree and installed brush
application lifecycles passed in
[run 42307578641](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42307578641).
This confirms the portable-conversion fixture correction below on Linux.

The [Qt6 regression job](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42307606082)
failed: four integration/test sources had Qt6 type ambiguities, three executor/
renderer cases and native batch rendering crashed, and integrated brush workers
reported unavailable isolation. Source review found invalid reverse-iterator
operations at task-queue boundaries. Corrections preserve traversal order and
all assertions. Sandbox failures now report bounded stage/errno details through
the existing error protocol; the cause of isolation failure still needs execution
evidence, and all restrictions remain enforced. These changes require new-head
qualification. Windows/macOS and the remaining full-regression jobs were still
running at this checkpoint.

Implementation and focused source review are complete; runtime qualification is
in progress. The historical source inventory below records what was known at its
named checkpoint, not the current execution status. Do not redo those components.

At commit `697a9212737beb6de0643ec9117fe7eb3e9528df`:

| Required evidence | Observed result |
|---|---|
| Six OS/Qt parser configurations and import fuzzing | All seven jobs passed in [run 42305891378](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42305891378). |
| Native metrics and source-contract/cache checks | Both automatic jobs passed in [run 42305891418](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42305891418). The manual Krita job was skipped, not passed. |
| Linux actual application | Compilation and general native application integration passed. The brush application failed during portable native-conversion reimport in [run 42305891491](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42305891491). Source identity/resource-rights assertions need diagnosis; the combined failure does not yet identify which field differs. |
| Linux installed application | Skipped after the parent failure; no installed qualification. |
| Windows/macOS application and packages | Not yet qualified; application builds were still running at this observation. |
| Full regressions, Qt6 and Compat | [Run 42305900406](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42305900406) was still building at this observation. |

Source diagnosis found a deterministic fixture defect: the random bitmap
conversion has two resources but declared rights for only the first. Portable
export materializes explicit unknown rights for an undeclared resource, so the
whole-object equality was invalid. The fixture now declares all resource rights
and retains every original assertion, with separate conversion/profile-specific
diagnostics. This correction needs CI execution; the original combined log
cannot rule out an earlier failing predicate. Production export policy is unchanged.

The remaining sequence is finite: resolve demonstrated in-scope failures, obtain
all required actual/installed and regression results on the resulting commit,
then complete independent review and the PR evidence summary. Historical passes
must not qualify subsequent changes. No local project execution was performed.

## Stabilization versus reference qualification

The reduced phase keeps existing native and bounded experimental support, without
claiming full Krita or Photoshop parity. Original-Krita acquisition/comparison is
manual qualification; its complete matrix and fidelity thresholds remain intact.
Automatic source-contract, native-metric, parser, engine, application, installed
package and isolation checks remain required for stabilization.

Observed reference failures at `d1c8d3895` in
[run 42302134348](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42302134348):

- Generated-curve comparison failed fidelity gates for 18 of 124 outcomes. These
  are known unresolved fidelity defects; the candidates are not source-verified.
- Automatic-spacing acquisition failed because the original application painted
  no coverage. This is a capture failure, not evidence of candidate parity or a
  demonstrated candidate renderer defect.

This separation follows the reduced scope and does not turn either failure into
a pass. A failure of a capability actually claimed by bounded support still needs
a fix; an experimental label cannot excuse it. No comparison tolerance is relaxed.


The scope was explicitly reduced on 2026-11-06 to stabilizing existing native
brush support and already implemented bounded Krita candidates. Photoshop
emulation and broader K/S compatibility research are future work. See the
[current plan](raster_brush_recovery_plan.md) and the scope amendment in
[raster_brush_compatibility.md](raster_brush_compatibility.md).

The source findings below remain valid. FIX-01 through FIX-04, the two native
proof questions and regressions in existing workflows are current work. Missing
D behavior and broader K/S ranges/combinations are deferred, not new tasks for
this phase. The original target is preserved below for future reference; successful
stabilization does not complete that full target or establish source parity.

## Historical source audit snapshot: 2026-11-06

Audited local HEAD `6064436c2` plus the existing working-tree changes. This is a
source/history audit, not build, runtime or source-reference qualification.
At that checkpoint, all CI access and local project execution were deferred,
and implementation was paused for planning. The current evidence above supersedes
that execution status; the original source findings remain the audit trail.

The original PR already contains the native brush foundation and the full RCP1
target. The main planning error was mixing code already written, missing active
source behavior and missing evidence in the same unchecked rows. The audit did
not establish that the entire implementation must be redone. Existing suites at
different boundaries are often complementary, not duplicates.

| Area | Implementation inventory | Disposition |
|---|---|---|
| Native brush core and transactions | Native dry/wet, curves/dynamics, wash/build-up, textures/secondary masks, undo/cancel/save/reopen, portable resources/repair and isolation machinery exist. See `rasterbrushengine.cpp`, `fullcolorbrushtool.cpp`, `rasterbrushpreset.cpp`, `rasterbrushworker.cpp`. | Preserve; qualify the final head and make only demonstrated corrections. |
| Old cloud-session features | Distance/repeat/Time editor and combined native wet HSV/texture/resource lifecycle were reconstructed in `703b77b82`. | Recovery of the absent private commit objects is not another implementation task. |
| Latest saved components | `fb44b5e1a` adds bounded generated/scatter/random-GIH/wet components, declaration preservation, diagnostics and import/resource fixes; `6064436c2` documents storage modes. | Reuse. Full source domains remain open; neither checkpoint is final-head qualified. |
| N-01/N-02 preview and source editor | Written beyond HEAD: `fullcolorbrushtool.cpp:446`, `rasterbrushsourceeditor.h:37`; regression hooks `rasterbrushintegrationtest.cpp:575,754`. Tablet ownership, capture/deactivation rollback and scoped new timers are present. | Implementation present; two narrow GUI-fixture findings below. No additional preview/editor implementation project. |
| N-03 benchmark | Lifecycle/resource timing extends the existing runner at `benchmark.cpp:108`; report and authored regression modules exist. | Two specific report corrections below; preserve the ten original workloads and five added first/repeat pairs. |
| R-01 collection integration | Real picker/worker/selection/acceptance/install/selector/paint path in `rasterbrushcollectionintegrationtest.h:117`, hooked at `rasterbrushintegrationtest.cpp:1416`. | Authored and source-reviewed; no concrete defect found in this pass. It complements direct installer fault tests rather than replacing them. |
| S-12 native wet spatial oracle | Independent nonuniform pickup/transport cases in `rasterbrushnativewet_spatial_test.h:197,248`, hooked at `rasterbrushengine_test.cpp:5154`. | Authored and source-reviewed, unexecuted. Complements uniform analytical drift and repeatability tests; two focused proof questions below. |
| Krita Pixel / four wet profiles | Real bounded source implementations exist; shared stages and model branches already operate. Current guard inventory follows below. | Stabilize existing bounded behavior. Broader geometry/sensor/cadence/interaction support is deferred. |
| Photoshop source behavior | Bounded ABR decoding/native conversion exists. The exact profile catalog `rasterbrushprofile.h` and engine validation contain no active P2711 profile. | Deferred: source resource bindings, active adapter and D equations. Preserve existing native conversion; no P2711 work in this phase. |
| macOS strict bound | Signed sandbox, baseline-relative map limit and sampled footprint watchdog exist. `raster_brush_platform_isolation.md` states why these do not establish the required whole-process resident/commit limit. | Open future architecture decision. Preserve and check existing containment; do not claim the stronger guarantee or an unqualified platform is ready. |

### Concrete review deltas and implementation disposition

Implementation resumed after the scope decision. FIX-01 through FIX-04 and both
native proof questions below are now implemented and independently source-reviewed.
The benchmark review additionally closed failed-finish counter monotonicity with
a paired-zero negative case. These findings are retained as the audit trail, not
another open implementation list. The target edition revision `2e52fb78b`
merges without conflicts and retains native routing, test entry and build
registration. Scoped source implementation/review is complete; current runtime
qualification remains pending.

These are source findings; no test failure was inferred or reproduced locally.

| ID | Evidence and exact remaining delta |
|---|---|
| FIX-01 | `benchmark.cpp:57,150,154`: dab counts default to zero and are captured only after successful phases. Capture actual progress before cleanup after failed add/finish, distinguish unobserved checkpoints, and amend existing report failure cases. |
| FIX-02 | `benchmark_report.py:100` accepts merely finite workload settings; the synthetic heavy fixture at `benchmark_report_test.py:33` omits the original diameter/resource/curve workload. Pin the emitted original settings/resources and correct existing fixtures/mutation cases without weakening thresholds. |
| FIX-03 | `rasterbrushintegrationtest.cpp:628,690`: the right-button hover helper also sets LeftButton. Represent hover separately from an auxiliary button during tip contact; keep the existing ownership cases. |
| FIX-04 | Older modal-driver callbacks at `rasterbrushintegrationtest.cpp:332,399,409,519,1046,2304` capture stack locals without a QObject context. Bind their lifetime to each fixture scope so failed modal startup cannot leave dangling queued callbacks. New preview/editor callbacks already have contexts. |

The generated zero-flow mistake has already been corrected in the working tree:
`generated_controls_fixtures.py:133` and `compare.cpp:522` retain all 216 cases.
The eight measured build-up FlowValue0 rows must paint; only explicit static
zero-flow Wash controls may be no-ops. Do not apply the earlier blanket no-op
expectation again.

Native proof questions are separate from demonstrated production defects:
document the accepted zero-total-tip-weight behavior (current pickup uses
transparent Q), then add a focused oracle if existing tests do not assert it;
cover wet Normal plus alpha-lock against independent expected pixels. Retain the
written spatial oracle. Direct wet plus Erase is intentionally rejected and is
not a missing foreign Cartesian combination.

### Work to reuse or leave conditional

- The shared `dab()` path already performs tip/coverage, texture, secondary,
  foreground, compositing and wash/build-up (`rasterbrushengine.cpp:2155,2525,2586`).
  No generic stage extraction or descriptor registry is needed as a prerequisite.
- Four source wet branches, calibrated Lab conversion, adaptive convergence,
  measured Smear placement and off-document state already exist. S rows describe
  interacting obligations; they do not request four additional engines.
- Preserve generated transfer/geometry/sensor, auto-spacing, scatter holdout,
  causal/phase GIH, wet kernel/texture and Photoshop input-contract sources.
  Broader acquisition and model research are deferred; reuse these in a future phase.
  Authored acquisition is not acquired evidence; a new equivalent pipeline
  would not resolve the unavailable input.
- N-04 host CPU instrumentation is a conditional diagnostic within final
  performance qualification. The existing contract separately requires engine
  event-to-surface CPU/memory and physical input/display evidence, not this
  particular new instrumentation architecture.
- R-04's proposed exact original-byte transport belongs to the deferred Photoshop
  track and remains conditional if that track resumes. Tip low bytes
  are discarded by `abrparser.cpp:436`; pattern channels already retain 8/16-bit
  decoded bytes (`abrparser.h:110`). The original contract requires 16-bit inputs
  and correct RGBA8 output, not archival of every original byte through every
  protocol. Establish whether discarded precision affects required output; then
  implement the minimum sufficient typed representation/transport if necessary.
  Do not replace the existing pattern decoder or widen source admission.
- Full K-10/D-13/S-10 profile completion is deferred. Current R-03 durability
  work covers the implemented capabilities. Q-01 is one final execution phase
  for this stabilization scope, not multiple new runners.

### Future research inputs and remaining evidence limits

| Input | Work it blocks; existing material to reuse |
|---|---|
| Original exact K5341 geometry/cadence/sensor/process observations | General generated/bitmap transforms; auto/zero spacing; remaining sensor units/products/clocks; scatter scaling/interactions and random-GIH phase law. Reuse generated controls, resource/sensor/timed fixtures and existing scatter/GIH discriminators. |
| Original K5341 joint color/mask/texture observations | Remaining transfer/HSV/blend/secondary operation order; Dulling window/count/clipping; New Dulling texture reduction. Reuse option/masked/HSV/wet state/kernel/texture groups and stored findings. |
| Original P2711 producer/export/behavior records | Separate version/resource associations, control enum/units and all D equations, including whether 16-bit low bytes matter. Reuse `PHOTOSHOP_REFERENCE_INPUTS.md` and the existing input-contract code. |
| macOS capability and architecture evidence | R-02 enforceable whole-process memory design. Reuse the signed helper and existing capability research/probe sources; no experiment is a finished architecture. |
| Exact integrated-head execution | Build, parser/fuzz, actual/installed app, source/statistical/drift, isolation and performance qualification. Deferred until feature implementation is complete; use actual failure logs then. |
| Physical drawing device / artist evidence | Delivered pressure/tilt/rotation, device-to-display latency and required feel/artist review. Synthetic engine/GUI traces do not supply these observations. |

Neither exact reference application nor a tablet is available. Additional
discriminating original records for the listed unknown rules are unavailable;
existing documented observations still support their bounded implementations.
Reuse those findings without redoing acquisition. There is no permission to
obtain or inspect additional CI artifacts while CI access is deferred. After concrete independent work is finished, hold
input-dependent packages rather than invent behavior or add replacement
scaffolding. The scope decision above now defers broader compatibility work; it remains
unfinished in the original RCP1 target. Missing future-research inputs do not
block fixes to the existing supported workflows.

## Original RCP1 requirement index for future reference

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
future compatibility work when the original RCP1 target requires it. Well-formed unsupported KPP inputs may receive
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
source qualification remain unchanged; required joint source behaviors remain open.
The existing shared stages themselves do not need to be recreated.

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
four S models. Every allowed Cartesian combination remains part of the deferred
full RCP1 target; completing that matrix is not this stabilization phase.

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
Each original K/S combination remains either an implemented constrained path or
a named deferred compatibility obligation; a source-only fixture is not a pass.
At the historical source-audit checkpoint, this implementation and its authored
regression sources were unexecuted. See the current evidence above for subsequent
qualification and the remaining application failure.

Deferred source-equation work depends on discriminating observations where no
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
recovery branch. Project fixtures execute in CI per CLAUDE.md after the current
implementation steps finish. Photoshop reference execution is deferred with its
future compatibility track. Physical tablet measurements remain unavailable and
unverified; no measured device or display-latency claim is made. Optional named paid packs are not prerequisites. Distance interval
editing and the native wet editor/resource/application lifecycle have now been
reconstructed on the recovery branch. Their implementation and authored
regressions do not replace final-head installed-application, resource-budget,
performance, cross-platform repeatability and complete source qualification.
No current implementation subset establishes that the original plan is fulfilled.
