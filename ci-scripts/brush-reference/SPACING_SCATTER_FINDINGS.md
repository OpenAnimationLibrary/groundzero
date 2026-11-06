# Original spacing/scatter acquisition findings

Research evidence only. These captures do not qualify a source profile, identify
an RNG, establish a general amount/diameter law, or permit native fallback to be
presented as source support. The original RCP1 aggregate gates remain unchanged.

## Reproducible acquisition

[Reference run 42261347410](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/42261347410)
used exact commit `9111a8970a8990616301c474e251edbc06babf4f`, the pinned Krita
5.3.4.1 `bd6e8d0` application, and independent full-document reset per stroke.
Each scatter group contains exactly 100 independently delivered enabled strokes
and two disabled controls. Source execution is randomized and both the planned
and actual order are retained. Enabled strokes do not require an exact replay
of a random raster. The disabled controls retain deterministic repeat gates.

Effective source controls: `paintop=paintbrush`, `PressureScatter=true`,
`ScatterValue=1`, `ScatterUseCurve=false`, `ScatterUseSameCurve=true`,
`ScattercurveMode=0`, `ScattercommonCurve=0,1;1,1;`,
`ScatterSensor=<params id="pressure"><curve>0,1;1,1;</curve></params>`, and the
explicit `Scattering/AxisX` / `Scattering/AxisY` switches. Other dynamics are
explicitly disabled. The generated round default mask has diameter 8, ratio 1,
horizontal/vertical fades 1, antialiasEdges 0, spacing 4, useAutoSpacing 0, and
full opacity/flow. Every source row retains actual brush XML, setting readback,
contact endpoints, crop bytes and hashes. No foreign renderer implementation
was used to derive these measurements.

The east path is `(256,1024) -> (1249,1024)` and the north path is
`(1024,1248) -> (1024,255)`. Each source stroke contains 32 disconnected dabs.
The retained rectangles are the authored fixed regions, with no registration,
translation, image fitting, or substituted renderer output.

## Six completed directional measurements

Every enabled source raster in these six groups is unique (100 of 100).
In that pre-implementation run, every native candidate raster is identical
within each group (1 of 100), despite 100 distinct candidate seeds. The importer
then had no Scatter control conversion. A source admission must therefore remain disabled,
even for groups whose aggregate statistics pass.

| Group | Original aggregate statistical outcome | Later active-axis centroid SD, pixels | First measured principal axis, degrees |
| --- | --- | ---: | ---: |
| scatter-x-east | passed | 4.582209 | 76.002892 |
| scatter-x-north | passed | 4.624499 | 50.579735 |
| scatter-y-east | failed meanWidth in bands 1 and 2 | 4.700625 | -14.102733 |
| scatter-y-north | failed meanWidth in bands 1 and 2 | 4.604035 | -39.412749 |
| scatter-xy-east | passed | parallel 4.693858; perpendicular 4.612571 | two-dimensional |
| scatter-xy-north | passed | parallel 4.679530; perpendicular 4.614188 | two-dimensional |

These are descriptive measurements of quantized rendered masks, not raw RNG
samples. Later centroid displacement on the active axis spans approximately
`[-8,+8]` pixels. Orthogonal displacement is zero for three configurations; the
north Y configuration has a maximum 0.085107-pixel centroid artifact from mask
rasterization. A symmetric bounded law is consistent with the observations;
its exact distribution and scaling have not been established.

The two Y-only groups fail two of the original 24 aggregate metrics. For
example east band 1 meanWidth is source 0.855467 versus candidate 0.8125, with
95% mean-difference interval `[-0.0441484,-0.0417857]` and tolerance 0.0427734.
The X-only and XY groups pass those aggregates while having no candidate displacement.
Existing variance-floor exemptions and area/width/energy aggregates cannot by
themselves demonstrate that an active stochastic process was preserved.

Disabled source controls have 48-pixel dabs. Native controls have 52-pixel dabs.
That separate baseline mask geometry discrepancy must be resolved as well as
adding scatter dynamics; seed handling alone cannot reproduce these images.

## First dab differs from later dabs

The first X-only displacement axis closely matches the vector from the global
origin to the authored start point: `atan2(1024,256)=75.963757` degrees east and
`atan2(1248,1024)=50.630683` degrees north. The first Y-only axis matches the
perpendicular: -14.036243 and -39.369317 degrees modulo 180, respectively.
The measured axes in the table differ by less than 0.07 degree from those
hypotheses. Later dabs follow the explicit stroke direction instead.

This is an observed input-path hypothesis for Node.paintLine, not a claim about
physical tablet strokes or an approved engine equation. Further authored
origins and direction holdouts must distinguish source delivery initialization
from brush semantics before runtime integration. The helper records the
hypothesis separately and explicitly leaves its qualification false.

## Independent process diagnostic

`scatter_capture_analysis.py` reads complete retained acquisitions and emits
alpha-weighted connected-component centroids, source/candidate raster and
centroid-sequence uniqueness, first-versus-later displacement moments, per-stroke
parallel/perpendicular RMS, and mask areas. Disabled controls from the same
application provide descriptive coordinates. Component matching uses authored
stroke order, and fails if 32 disconnected dabs cannot be resolved. It does not
change any image or the comparator's fidelity metrics.

Run it only on captured artifacts, for example:

```text
python ci-scripts/brush-reference/scatter_capture_analysis.py <artifact>/brush-reference-run
```

The JSON includes SHA-256 hashes of both input reports and all 100 per-stroke
measurements. It is read-only and has no pass/fail admission decision. Its
separate validation script belongs in CI; no local tests or builds were run.
Before source admission, a measured candidate should additionally demonstrate
meaningful displacement and process moments against independent source
holdouts. The statistical law and appropriate thresholds must be chosen from
those measurements. Requiring exactly 100 unique masks is not assumed to be a
universal RNG acceptance rule.

The two XY captures also have observed displacements bounded near [-8,+8] on
each canvas axis. Later X/Y covariance is 0.134434 square pixels east and
-0.661726 square pixels north, with axis SDs approximately 4.6 pixels. This is
consistent with separate symmetric axis draws at this configuration; it does
not establish independence, an exact uniform law, scaling, or source RNG identity.

## Retained report identities

All artifacts have the name
`krita-reference-<group>-9111a8970a8990616301c474e251edbc06babf4f` in the run above.
The evidence report digests below bind these measurements to the original data.

| Group | oracle-status.json SHA-256 | comparison.json SHA-256 |
| --- | --- | --- |
| scatter-x-east | 000cbf8b40af35a276c4fcd7bd47fd137175174d643ea7e06beb41a2be8def2b | 948751eecb41b93c8d5608c63b91190f6308119bfa3668c85dd88627f3891f99 |
| scatter-x-north | 1802e95138a2b81760fcf876380ed687dd153e1051f0aeb9d16206c24e449888 | d7b3c096845293805614ed85335d0ea1f796032a0b705551b173df2eeaff5702 |
| scatter-y-east | dd48721f130ed5d0a7d6046d631f01d6242e34dfd7681e8c697ea9cebce181fc | a0de3fd06cc88b89023aabed13b114f35252ca5b969c51b79e61fca8f19a888d |
| scatter-y-north | 7c21b2e531d8ef169c7921962a8fb90b7fda0af154795c92d20a1e8877688024 | 43fca6250e8487b5701f55045e877497150c8863808e56f5830a39d4848662bf |
| scatter-xy-east | 7cefde23437e824dfe68332f867867da616fd2480d2641c2736d298c7cad2756 | 6dbe2e933c7294ab152f4f5ab5542be747ff4bcc393e9a88621b7ecff5ee12d9 |
| scatter-xy-north | ec63633b624d58f42c6cbbc9598860774daf65f98fd6fa4dadc6e29ea053ccd4 | 571a52e715c74257102cbd7f7993e663aa753d5564581a6dea637f8707ce4660 |

## Implemented bounded source scatter component

`krita-5.3.4.1-scatter-candidate-1` now retains an explicit
`Preset.sourceScatterV1` with amount and independent X/Y switches. This state is
serialized strictly through native, portable and worker packages and cannot be
activated under the native profile by changing a profile string. Original
source parameter declarations remain in private/native provenance; portable
exports retain only a validated technical projection, excluding arbitrary XML
and private strings.

Static source import preserves `ScatterValue`, `PressureScatter` and axis
selection instead of silently discarding them. Disabled controls retain their
metadata and select the existing independently measured hard-Pixel geometry.
The explicit inactive `autoSpacingCoeff=1` is accepted with auto spacing off;
this avoids the former unrelated unknown-attribute native fallback.

The process uses independent uniform draws for both canvas axes. For one-axis
scatter, one signed draw is projected along or perpendicular to the direction
between consecutive nominal dabs. The first previous nominal point is the
global origin, reproducing the observed initial basis; displaced positions do
not feed back into direction. The dimensionless offset amplitude is
`amount*effectiveDiameter`. This is one general state equation, not a fixture
lookup, per-pixel fit, conditional stay probability, or source-seed imitation.

The current admission guard remains exactly the acquired static component:
diameter8, amount1, spacing4, unit aspect, zero angle, AA off, opacity/flow one,
Normal build-up, and no coupled dynamics, texture, mirror, masks or wet engine.
The source curve switch must explicitly be false. Unknown active settings,
curve-enabled scatter, new sizes/amounts or unsupported combinations produce a
clear import error rather than a source-labeled native approximation. Broader
valid RCP1 settings are still required work, not clamped into this component.

The existing native RNG algorithm, native scatter interpretation and native
call order remain unchanged. Imported opaque-source candidates default to the
existing host policy that resolves a different seed per logical stroke. The
raw renderer still uses only its explicit resolved seed, and source/native
seed identity is not asserted. Reference comparisons retain their 100 distinct
explicit candidate streams and all original stochastic gates. Independent
engine regressions exercise seed reproducibility, source mask controls,
nondegenerate displacement, square support, first/later direction and rejected
unmeasured state; import tests cover native/portable/worker preservation and
strict malformed-state handling. These regressions have not been run locally.

The existing captures further discriminate the proposed distribution. Across
6,200 later XY dabs, measured X/Y population SDs are 4.653570/4.645847 pixels and
central kurtoses are 1.779109/1.792807. A uniform [-8,+8] draw has SD4.618802 and
kurtosis1.8. Measured correlation is -0.012091. There are 1,426 centroids outside
a radius-eight disk and 578 in corners with both absolute coordinates above
8/sqrt(2), ruling out a disk-only proposal. Single-axis later-dab kurtoses range
1.766967..1.820526. First-dab residual RMS orthogonal to the global-start basis
is 0.069591..0.082020 pixels over four independent 100-stroke groups. These are
quantized-mask observations, not access to the source's private random values.
They motivate the bounded independent uniform candidate, while final statistical
acceptance remains mandatory.

## Authored final-acquisition holdouts

`scatter_holdout_fixtures.py` contains ten original configurations, each with
100 independent enabled strokes and two disabled deterministic controls:

- X-only and Y-only strokes shifted to a new origin, plus westward XY strokes.
- Half/double scatter amounts, with spacing chosen to separate nominal cells.
- Diameter4/16 controls around the acquired diameter8 component.
- Half/full/ramping pressure delivered through actual Qt tablet events, with
  an explicit identity Scatter curve. No Node.paintLine pressure is fabricated.

`scatter_holdout_fixtures_test.py` checks complete replicate identities, bounded
authored geometry, explicit unverified flags, actual pressure-event traces and
unadmitted scaling/dynamic scope. Existing six group identities and metrics are
unchanged. Holdout preparation does not widen source eligibility or reinterpret
an unsupported active setting as a successful comparison.

The manual `scatter-holdouts` focus now prepares and acquires all ten groups;
these development groups remain absent from the default source workload. A pure
shared contract validates every authored case and the complete 102-case planned
permutation before allowing one capture for each independent enabled stroke.
The final report and reusable cache must bind the actual execution permutation,
canonical record ordering, ordinal identities, repeat evidence, retained actual
effective XML, delivered endpoints or 37 tablet events, and bounded BGRA crop.
The two disabled controls retain two independent exact canonical repeats.
Synthetic contract regressions cover missing cases, altered inputs, coordinated
XML/evidence tampering, event changes, ordering, repetition and cache restoration;
they validate storage/input contracts without claiming renderer observations.

Only shifted-origin X/Y and westward XY groups replay the measured static source
candidate. They retain the ordinary hard-Pixel full-image gates for disabled
controls and each independently enforce the unchanged 24 three-band metrics and
10,000-bootstrap stochastic gates over exactly100 distinct candidate streams.
Westward normalization is the fixed authored 180-degree transform for both
rasters, with no rendered registration or fitted translation. The seven
amount/size/pressure groups instead write `open-scatter-domain.json`, explicitly
marking source-only acquisition, implementation pending and no comparison or
qualification. They are routed before any native importer or renderer and reject
a stale comparison file. Actual observations remain required before broadening
the component. No new source capture, build, test or CI check has been run for
these holdouts; final validation follows implementation.

## Automatic spacing capture remains open

The same run's autospacing acquisition stopped at case 000, the diameter-one,
ratio-one, manual-spacing control (`autoSpacingCoeff=0.1`, useAutoSpacing 0).
The source effective XML agrees with those authored controls, but the helper
reported `Spacing/scatter source unexpectedly painted no coverage` before
saving a source PNG or completed record. This establishes an acquisition
failure, not a qualified no-op. No `expectUnchanged` value or nonempty guard was
changed based on this single unretained observation. Failure-only pixel and
input retention in the subsequent acquisition is required to classify it.
The other 191 automatic-spacing cases were not acquired in this attempt.
