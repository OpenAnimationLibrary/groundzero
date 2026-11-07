# PR 115 deferred compatibility roadmap

The current phase is **stabilizing existing native and bounded Krita support**,
as explicitly chosen on 2026-11-06. Use the four-step
[stabilization plan](raster_brush_recovery_plan.md) for active work and the
[acceptance audit](raster_brush_acceptance_audit.md) for its concrete fixes.

The broader D/K/S requirements below are retained for a possible future phase.
They are not current implementation assignments, release promises or reasons to
keep adding code. Photoshop emulation, broader Krita research and R-04 precision
work are deferred. Existing ABR/native conversion and implemented candidates stay
intact. Native workflow fixes and checks of existing protections remain active.
Original full-profile completion and strict macOS memory architecture are still
unfinished; this smaller phase must not be reported as completing full RCP1.

## Original full target: deferred expansion

- ABR 1 and 2 separately; modern 6/1, 6/2, 7/1, 7/2, 8/1, 8/2, 9/1,
  9/2, 10/1 and 10/2, interpreted by Photoshop 27.11. Complete the D setting set
  and required sampled, computed and pattern-resource associations.
- Krita 5.3.4.1 Pixel: the complete K setting set, all required generated,
  alpha-mask, RGBA and ordered/random GIH resources, and build-up/wash behavior.
- All four Krita 5.3.4.1 S models: Legacy Smear, Legacy Dulling, New Smear and
  New Dulling. Each needs its own complete applicable settings and interactions.
- Native dry/wet mathematics, editable presets and real-engine preview, bounded
  import/resources, portable repair, transactional painting and installed apps.
- Full required numeric envelope and categorical Cartesian combinations, then
  source fidelity, repeatability, performance, isolation and real-device gates.

The common numeric envelope is diameter 0.25-4096 document pixels, aspect .01-1,
pressure/opacity/flow/depth 0-1, angles modulo360, scatter 0-5 tip diameters,
manual spacing .01-5 diameters and airbrush 1-120 dabs/s. Count1-64 applies where
that source profile exposes count; do not invent a KPP property. Source-valid
zero spacing needs an identified profile-specific rule. The normative tables
retain all exclusions and combination restrictions, including S Normal-only,
Pixel secondary masks only in Wash, and no nested GIH/dual or color-stamping
secondary applications. RGBA-backed resources used as a single alpha mask
remain in scope when the source alpha rule needs their RGB values.
Optional named paid packs and a generic Mixer/bristle/TPL emulator are outside
this chosen target; they do not replace any D/K/S obligation.

## Execution policy and progress accounting

All CI access is deferred: no statuses, logs, artifacts, dispatches or reruns.
Project builds, tests and generators do not run locally. Implementation is
resumed for the current stabilization list; the broader roadmap remains deferred. Keep checkpoints local on
`codex/pr115-raster-brush-recovery` in its independent clone; protect the other
working checkout. No push is needed for a local checkpoint.

Some source observations and macOS OS-capability observations are prerequisites
to identifying missing implementation, not merely final validation. Neither
exact source application is available; additional discriminating original
records for the listed unknown rules and a physical tablet are unavailable.
Reuse already documented observations for existing components. Do not substitute an older
application, execute project acquisition locally, relax a gate, or invent an
equation to bypass those dependencies. Authorized externally supplied original
records can unblock their matching packages; all CI acquisition remains deferred.

Track implementation and evidence separately. A row is implemented only when
all required settings/ranges/combinations reach actual behavior, with explicit
state/resource lifecycle and authored coverage. Source-reviewed code is not
compiled or verified code. Keep reference qualification false until its own
unchanged gates pass. Report completed deliverables and the current critical
path; do not infer an overall percentage from the number of candidate profiles.

## Written deliverables and conditional additions

| ID | Deliverable | Current state and implementation exit |
|---|---|---|
| N-01 | Match preview and actual-tool lifecycle | Written in the current working tree, source-reviewed and unexecuted. Ownership, moved release, timers, capture/deactivation and rollback are present. Audit FIX-03/FIX-04 are implemented and independently source-reviewed; do not reimplement preview. |
| N-02 | Edit already admitted source controls | Written in the current working tree, source-reviewed and unexecuted. Atomic source controls, generated-state synchronization and native-lookalike restrictions exist. Reuse existing editor and nine-family fixtures; no source-admission expansion is implied. |
| N-03 | Measure complete engine lifecycle costs | Written extension of the existing benchmark and report. Audit FIX-01/FIX-02 are implemented and independently source-reviewed, preserving original workloads and thresholds. Constructor/add/finish/cleanup/resource accounting and added first/repeat pairs already exist; execution is deferred. |
| N-04 | Optional actual-host CPU diagnostic | Conditional method for final performance work, not an additional mandatory pre-CI subsystem. Extend the existing app fixture only for a demonstrated host measurement gap. It cannot establish device-to-display latency. |
| R-01 | Exercise real installed-app collection import | Written actual-GUI/worker/selector helper and integration hook; source review found no concrete defect. Retain complementary direct installer fault tests. Execution is deferred; no new collection harness is planned. |
| S-12 | Independent native wet spatial reference | Spatial oracle written and source-reviewed, unexecuted. Retain existing nonuniform pickup/transport matrix and drift tests. The empty-tip policy and wet alpha-lock proof cases are now written and independently source-reviewed; no native production mismatch was established. This repeated ID is the same deliverable, not another task. |
| R-04 | Deferred source resource precision | Photoshop track deferred; no schema implementation started. Sampled16 support remains required. Determine whether source low bytes affect required RGBA8 output before adding typed precision/transport. Pattern channels are already decoded with full precision; do not mandate all-path archival or duplicate that decoder without an established requirement. |

Primary files are `fullcolorbrushtool.cpp`, its private editor helper,
`rasterbrushintegrationtest.cpp`, private integration helpers,
`ci-scripts/brush-reference/benchmark.cpp`, and native engine regression sources.
Use explicit file/hunk ownership for parallel work. Changes to shared
engine/importer/codec files require deliberate integration, not concurrent
whole-file rewrites. Review each completed implementation batch before saving
its local checkpoint; final execution remains deferred.

## Deferred Krita Pixel expansion

Shared implementation: `rasterbrushengine.cpp`, `rasterbrushimport.cpp`,
`rasterbrushengine.h`, `rasterbrushpreset.cpp` and `compare.cpp`. Reference paths
below are relative to `ci-scripts/brush-reference/`.

| ID | Required work and dependency | Concrete implementation exit and input |
|---|---|---|
| K-01 | Identify actual joint Pixel deltas | The existing dab path already shares tip coverage, texture, secondary, foreground and deposition. Track the precise unmet interaction and observation for each runtime change. No generic extraction/registry project; this feeds K-02 through K-09 and the K-10 completeness gate. |
| K-02 | General generated geometry | Complete Default/Soft/Gaussian circle/rectangle across diameter, aspect, angle, AA, fades/curves and fractional origins. Current Soft/Gaussian guard is16/64px, AA-on, ratio1, angle0. Use `generated_controls_fixtures.py`, `generated_mask_analysis.py` and generated-tip probes; no fitted diameter table. |
| K-03 | PNG/GBR/RGBA and ordered GIH sampling | Complete transforms and the full diameter domain, non-square/partial-alpha resources and applicable sequence geometry. Current bitmap candidates are fixed transfer, Normal build-up and scale1-2. Use `resource_fixtures.py`, resource/bitmap probes and original asymmetric resources. |
| K-04 | Placement, fixed/auto/zero spacing and timing | Identify general source cadence, aspect/size dependence, valid-zero behavior, timers1-120Hz, taps/endpoints/holds, pauses and actual delivered clocks. Existing airbrush is only16px/.1-opacity/5-or-20Hz. Use spacing/scatter and timed fixtures/probes. |
| K-05 | Active source sensor evaluator | After clock/normalization observations, implement independently curved multiplicative inputs for size/opacity/flow/rotation: pressure, speed, distance, time, drawing angle, tilt axes/elevation/direction, barrel and fuzzy dab/stroke. Preserved `SourceSensorsV1` is not evaluation. Use sensor/timed fixtures and actual-delivery records. |
| K-06 | General scatter and flip/mirror | After geometry/cadence/sensor rules, implement amounts0-5, scaling, dynamic products, transformed/resource tips and coupled controls. Current scatter is8px/amount1/spacing4. Use the10 authored scatter holdouts and unchanged stochastic gates; retain native RNG semantics. |
| K-07 | General random GIH state/key law | Establish finer phase boundaries, frame/resource/transform/cache invalidation, reset/cancel and scatter interaction. The64-bucket key remains unproved. Use9 causal and28 fractional-phase groups, new original resources/frame counts and independent100-stroke records. No fitted repeat probability. |
| K-08 | Transfer, HSV and four blends | Implement joint opacity/flow/sensor products, foreground applicability, rounding and order across all tips, Wash/build-up and Normal/Multiply/Screen/Overlay. Use generated-control, HSV and option groups over transparent/opaque backgrounds. Zero flow is inactive in measured build-up; only explicit Wash zero-flow controls may be no-ops. |
| K-09 | Texture and secondary alpha-mask behavior | Implement source texture transforms/depth/invert/order and independent generated/PNG/GBR secondary geometry/dynamics/cadence. Preserve source Multiply/Subtract, Wash-only masking and single-alpha secondary restrictions. Record-only mask resources do not render. Use masked, texture-pickup and option groups. |
| K-10 | Full K implementation exit | Depends on identified K-01 deltas and K-02 through K-09. Account for every required tip/resource, geometry/sensor/process, mode/blend, texture and permitted-secondary tuple using one coverage ledger. This is semantic integration and completeness, not another renderer or new generic test runner. Final execution is Q-01. |

Runtime couplings needing observations include displaced bitmap/GIH geometry
and cache keys; Pixel byte-domain transfer/texture independent of bitmap family;
masked-Wash blend/dynamic opacity; and generated-AA anchoring under transformed
or scattered geometry. Existing native equations cannot be substituted by name.

## Deferred Photoshop D work

There is no active P2711 renderer/profile yet. Current ABR support is bounded
decoding and native conversion. The known modern static binding is6/2 sampled
ID association. Use `abrparser.cpp/.h`, `rasterbrushimport.cpp`, engine/profile
state, codec and original ABR regression builders. Acquisition inputs and
privacy/ownership rules are in
[the Photoshop operator recipe](../ci-scripts/brush-reference/PHOTOSHOP_REFERENCE_INPUTS.md).

| ID | Required work/input | Concrete implementation exit |
|---|---|---|
| D-00 | Original producer and observation ledger | Record exact27.11 build/executable identity, original assets/rights, one-setting pairs, literal choices and enabled/disabled/unavailable states. Bind original input, produced export and readback separately; the input-contract receipt stays unqualified. |
| D-01 | All12 container variants | Cover1/2 computed and sampled8/16-bit raw/PackBits separately; independently justify every6-10/1-2 layout and graph. Represent absent/present/malformed sections explicitly, including generated-only modern graphs. Do not clear sample-completeness guards or qualify versions by editing header bytes. |
| D-02 | Legacy source interpretation | Identify P2711 defaults, sampled16 normalization/low-byte relevance, computed ellipse hardness/AA/angle/aspect/spacing and fractional placement. Existing high-byte native conversion is not source fidelity. These observations decide whether conditional R-04 precision transport is needed. |
| D-03 | Modern resource graph | Establish identity/location joins for shared/reordered/missing/ambiguous samples, patterns and primary/secondary generated resources per variant. No name/ordinal fallback. A computedBrush observed inside a dual is not proof of primary-generated layout. |
| D-04 | Versioned active P2711 state/admission | Only after observed mappings, retain active fields, units, enable relationships, ranges and combinations through worker/private/portable records. Unknown active fields reject or remain explicit conversion; metadata cannot select source behavior. |
| D-05 | Tip geometry and cadence | Implement sampled/generated coverage, hardness/AA, transforms/flips, interpolation/phase and manual/valid-zero spacing throughout the envelope from original asymmetric/fractional controls. |
| D-06 | Shape/sensor dynamics | Implement size/angle/roundness/minima, exposed Off/Fade/pressure/tilt/barrel/direction/initial-direction/jitter, normalization and simultaneous-control order. No guessed descriptor enum or interpolation. |
| D-07 | Scatter/count processes | Identify axes, amounts, count1-64, count jitter/controls, transforms and correlation/RNG consumption. Use actual stochastic policy and independent stroke evidence rather than an invented source seed interface. |
| D-08 | Opacity and flow | Distinguish tool options from transfer controls/jitter/minima, disabled values and effective scalar encoding. Implement accumulation/opacity order across overlaps, taps, pressure and backgrounds. |
| D-09 | Foreground/background and HSV | Implement measured mixing, channel/distribution/order, per-tip/per-stroke effects and all permitted blends, with distinct colors and alpha contexts. |
| D-10 | Pattern and texture | Bind nonempty `patt` resources, producer compression/channel roles, source scale/depth/invert/contrast/brightness and fixed/per-tip origin. Implement source coverage/polarity/order; decoded resource channels alone are insufficient. |
| D-11 | Dual source brush | Implement sampled/generated secondary size/spacing/scatter/count/flips and Off/Multiply scheduling/coverage with primary/texture. Preserve unsupported-mode negative controls rather than relabeling them. |
| D-12 | Source airbrush/timing | Identify source clock, stationary/moving rate, phase, fades/direction restart, taps/terminal/pause/cancel and actual60/120/240Hz delivery/batching. Native timing is not automatic Photoshop equivalence. |
| D-13 | Complete D integration exit | Depends on D-00 through D-12 and any demonstrated R-04 need. Bind completed active behavior through existing import/editor/worker/portable/raster transactions and account for every required D tuple. Share durability machinery with R-03; final execution is Q-01. No second resource or renderer project. |

D requires Normal/Multiply/Screen/Overlay x secondary Off/Multiply x texture
Off/Multiply/Subtract, with all applicable required controls and resources.
Smoothing, wet edges, unsupported build-up/noise/protect-texture/pose/extensions
remain exclusions as specified. Missing original producer records block the corresponding mappings and source
equations. Bounded structural decoding and high-byte native projection exist;
sampled16 source interpretation and modern pattern association remain open.
R-04 is a conditional precision design decision, not an unconditional original-byte
archival requirement. Raw pattern-channel decoding already preserves precision.

## Deferred four-model S expansion

Each package applies separately to Legacy Smear, Legacy Dulling, New Smear and
New Dulling unless it names Dulling or original native wet. Shared code is
`sourceWetDab()` in the engine, `rasterbrushsampling.h`, importer/codec and engine
regressions. Reference paths are under `ci-scripts/brush-reference/`.

| ID | Required work and dependency | Concrete implementation exit/input |
|---|---|---|
| S-01 | Four recurrences and storage | Complete opaque/partial/transparent first/second/third-dab transitions, disabled nonneutral controls, foreground alpha, quantization, feedback and injection order. Preserve declared straight-byte/associated-byte/floating storage. Use wetsteps/state/switches/combined/opacity groups. |
| S-02 | Full alpha-tip geometry | Complete inherited Default/Soft/Gaussian circle/rectangle, PNG/GBR alpha applications (including RGBA-backed alpha resources), and one-dimensional ordered/random alpha-GIH with the full transforms/ranges. Use the correct model pickup/deposition frames for nonuniform masks, fades, AA, aspect, rotation, flips and changing geometry. Color-stamping applications, lightness/gradient and dual masks remain outside S. |
| S-03 | Dulling radius/window/sampling | Identify one general window/minimum, coordinate rounding, clipping/weighting and attempted/accepted-count rule across0-300% for both Dulling models. Preserve calibrated Lab16 and16-attempt convergence. Remove Legacy100% ceiling only after implementation; Smear retains radius as inactive. Use kernel scale/count/single-mark/convergence discriminators. |
| S-04 | New Dulling texture reduction | Identify neighborhood extent, acceptance/weights, threshold, normalization and empty fallback from center/off-axis/holes/ramps/phase/second-third-dab controls. No center-only or fitted average substitute. Use texturestate/texturepickup and `WET_INTERACTION_FINDINGS.md`. |
| S-05 | Applicable sensors/curve products | Implement actual source normalization, units, clocks, interpolation and multiplicative responses for every wet target. Include nonlinear/shared/local curves and disabled retained controls. Preserved declarations are not active evaluation. |
| S-06 | Cadence and delivered input | Implement fixed/auto/valid-zero/isotropic spacing, geometry-dependent sensor positions, corners/reversal/taps/terminals, pauses and batching. Retain measured recursive Smear placement without treating Node partition behavior as universal tablet proof. |
| S-07 | Toggles and wet transfer | Implement model-specific length/strength/color-rate/smear-alpha/radius/opacity/flow meanings and simultaneous dynamics. Disabled does not imply zero; zero flow does not automatically imply no-op. |
| S-08 | Foreground/HSV/injection order | Complete joint fixed/dynamic HSV, foreground alpha, color rate and opacity with length/alpha/radius and transparent-state interactions for all models. Current radius-zero restrictions remain unfinished work. |
| S-09 | Full texture domain | Implement Multiply/Subtract, soft/hard texturing, depth/invert/transforms/origin/contrast/brightness and applicable per-dab behavior through pickup, feedback and deposition. Depends on S-04 for New Dulling. |
| S-10 | Full S implementation exit | Depends on S-01 through S-09, including applicable K geometry/sensor deltas. Account for each required model x alpha-tip/geometry x sensor x radius x toggle x transfer/HSV/texture tuple. Use one shared coverage ledger; this is completeness/integration, not another wet engine. |
| S-11 | Drift/boundary/state lifecycle | Preserve continuous1/10/100/1000/10000-dab prefixes, all edges/corners/exit-reentry, transparent halos, changing controls and consecutive/cancelled strokes. Apply unchanged output-based foreign drift gates, never hidden-reservoir claims. |
| S-12 | Independent native wet spatial reference | Spatial oracle written and source-reviewed, unexecuted. Retain existing nonuniform pickup/transport matrix and drift tests. The empty-tip policy and wet alpha-lock proof cases are now written and independently source-reviewed; no native production mismatch was established. This repeated ID is the same deliverable, not another task. |
| S-13 | Active admission and shared durability | Integrate each completed S behavior through existing import/active IR/worker/portable/editor/tool paths as its implementation lands. Preserve diagnostic distinctions. Share resource, repair, undo and cancellation machinery/evidence with R-03; do not build another descriptor registry. Final execution is Q-01. |

The general Dulling window/count/clipping equation, New Dulling texture-aware
reduction and broader source sensor/cadence/operation-order interactions still
need discriminating original observations. Already implemented recurrences,
Smear geometry and Lab conversion are components, not full S completion.

## Shared requirements: apply to current capabilities; retain future gaps

| ID | Work and dependency | Exit/evidence |
|---|---|---|
| R-02 | Future strict macOS whole-process memory design | Architecture research deferred from stabilization. Existing signed sandbox, relative address-space cap and sampled watchdog remain credited and their regression checks stay active. Required sequence: capability evidence -> admissible architecture/proof -> necessary implementation -> installed qualification. The strict2GiB resident/commit outcome is unresolved; a footprint experiment alone cannot close it. |
| R-03 | Shared resource/durability/isolation exit | Reuse existing worker/archive/PNG budgets, no-follow reads, publication, rights/hash repair and raster transactions. Bind each newly completed active profile to this machinery, correcting concrete gaps only. Qualify its authored faults and installed-app behavior once in Q-01; this is not a second installer/sandbox project. |
| Q-01 | Final checks for stabilization | All CI access deferred until implementation of the current stabilization steps is finished. Apply existing gates to current supported features; do not claim full D/K/S qualification. Run all six OS/Qt parser configurations, fuzzing, app/installed-package/isolation/source/performance gates. Read actual failed logs/annotations first; fix causes before rerun and audit recurring failures. Reconcile concurrent code before qualifying the actual final commit. |
| Q-02 | Unverified physical input/display and artist evidence | Requires real device access, currently unavailable. Record the limitation; do not claim measured tablet behavior or full source qualification. Record actual pressure/tilt/rotation capabilities, delivery and input-to-display latency; preserve required native repeatability and source stochastic/blinded artist review. Synthetic traces do not close tablet feel or hardware latency. |
| Q-03 | Public scope/integration/merge review | Refresh documentation and PR description around the final implementation/evidence, preserve all open limitations, review the current stabilization scope and integrate deliberately with concurrent work. No ready/green/parity statement from historical or partial results. |

Final gates retain the normative full-canvas union-region image thresholds,
endpoints/spacing, black/white-composite color metrics, exact repeats,
100 independent opaque-RNG strokes, three spatial bands,10000-bootstrap bands
and variance limits, long wet drift, resource bounds and release performance.
Refer to the contract for exact values; no gate is weakened by this plan.

## Future compatibility dependency order

Current stabilization follows the separate four-step plan. If broader
compatibility work is resumed in a future scope decision, model-dependent
work follows original reference inputs -> identified general equations ->
production implementation and complete categorical integration -> feature
completion audit -> deferred final qualification/device evidence -> PR review.
The dependency on source input is explicit; it cannot be made to disappear by
calling acquisition final testing, preserving more metadata or converting a
required source preset to native.

After each implementation batch, record what code/deliverable changed, what was
source-reviewed, which regression sources were authored, what remains unknown,
and the next concrete package. While CI is deferred, every progress report must
say that execution/qualification is still pending. When a package is input-gated,
name the missing original observations rather than estimating progress from
unexecuted fixtures. A future full-compatibility effort cannot be called complete
while its required packages/evidence remain missing. This does not restore the
deferred roadmap as a completion gate for current stabilization.

## Future S implementation units, if resumed

S-01/S-07/S-08/S-09 overlap the existing wet foreground/transport/texture path.
Execute concrete units rather than four parallel compositor rewrites:

1. Identify general Dulling pickup (S-03) and radius-zero New Dulling texture
   reduction (S-04) independently, reusing the current probes and Lab controller.
2. Reuse applicable K-02/K-03 geometry/resources for wet alpha-tip frames (S-02),
   preserving wet-specific placement and pickup distinctions.
3. Reuse K-04/K-05 clock/sensor rules for wet evaluation sites (S-05/S-06).
4. Implement only identified joint operator differences for remaining
   S-01/S-07/S-08/S-09 obligations.
5. Close S-10/S-11/S-13 using the existing combination, drift and durability
   infrastructure. Their required outcomes remain; they are not new engines.
