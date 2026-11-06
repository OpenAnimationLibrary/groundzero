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
Every native candidate raster is identical within each group (1 of 100), despite
100 distinct candidate seeds. This agrees with the current importer having no
Scatter control conversion. A source admission must therefore remain disabled,
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
