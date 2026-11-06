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
| K5341 Pixel tip families | rasterbrushengine.cpp, rasterbrushimport.cpp; generated_tip_fixtures.py, bitmap fixtures | Gaussian/Soft behavior, required transforms, resources and full supported numeric envelope |
| K5341 Pixel dynamics | engine mappings/import admission; sensor_fixtures.py, timed_fixtures.py | Complete pressure/speed/distance/time/drawing-angle/tilt/rotation/fuzzy controls and multiplicative curves, including combinations |
| Spacing, scatter, flip and mirror | engine/importer; spacing/mirror source fixtures | Auto spacing, scatter distribution, required transformed/dynamic domains and measured zero-spacing rules |
| GIH ordered/random | preset and engine tests; resource_fixtures.py, gih_cadence_fixtures.py, stochastic_gate.py | Ordered final-head source gates; generalized random advancement/statistics and valid resource holdouts |
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

## Evidence constraints

The prior implementation's failures and historical passes do not qualify the
recovery branch. All new fixtures execute in CI per CLAUDE.md. Photoshop export
associations/reference execution and physical tablet measurements remain external
prerequisites. Optional named paid packs are not prerequisites. Distance interval
editing and the native wet editor/resource/application lifecycle have now been
reconstructed on the recovery branch. Their scoped parser evidence does not
replace final-head installed-application and complete source qualification.
