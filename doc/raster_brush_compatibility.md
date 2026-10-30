# Regular-raster brush compatibility contract

Status: implementation in progress against the scoped independent compatibility contract.
The existing ABR importer extracts sampled tips only. None of the new foreign
behavior profiles below is implemented or reference-verified by publishing this
contract. Keep this work separate from vector/touch usability and neural styling.

The implementation preserves legacy MyPaint and TLV/vector behavior. A native
compatibility renderer, bounded format adapters, portable resource graph and
raster transactions are required for completion; tip extraction alone does not
satisfy the contract. Build and test through CI per CLAUDE.md.

## Acceptance ledger

| Requirement | Implementation | Validation / release gate |
|---|---|---|
| Existing ABR input and allocation budgets, exact subversion guards | Implemented | CI parser fixtures at the PR head |
| Collection installation without overwriting or partial writes | Legacy ABR staging and native atomic collection staging implemented | Invalid-member, cancellation, reimport and conflict tests; packaged application fault-injection review pending |
| Versioned native IR and decoded/supported/verified status separation | Native v1 implemented; foreign semantics remain separate | Package round-trip, missing resource, checksum, enum and version tests in CI |
| ABR descriptor/computed-tip/pattern behavioral import | Planned | Every declared version and active setting |
| KPP/bundle/GBR/GIH import | GBR 1/2 and one-dimensional GIH native conversions; KPP native conversion with bounded bundle dependency resolution | Original generated fixtures; conversions explicitly do not claim source semantics |
| Independent dry renderer and defined dynamics/combinations | Native CPU renderer implemented; foreign profiles planned | Numeric native tests; source-reference gates pending |
| Four Krita smudge profiles and native wet | Native wet/smear implemented; four source models still planned | Native deterministic pickup tests; foreign models must not alias native wet |
| Real tilt/rotation capture and normalization | Additive Qt tablet fields and track interpolation implemented | Full app CI and device qualification pending |
| Actual-engine preview, editor, portable preset export/relink | Native scratchpad, sensor/curve/texture/dual/color editor and embedded-asset export implemented | Cross-platform package CI passes; application workflows pending |
| Undo/cancel/frame lifecycle/save/reopen | Native path uses raster undo; cancellation rolls back tiles and created frame | Actual-tool undo, redo, cancellation, frame creation and PNG reopen workload added; full application CI pending |
| Parser isolation/fuzzing/cancel and complete resource budgets | Bounded readers and cancellable worker implemented; libFuzzer target and original corpus added; worker-process crash containment implemented; privilege sandboxing and sustained qualification pending | Sanitizers, malformed corpus, cancellation and limits |
| Packaged Windows/macOS/Linux QA and performance | Planned | Exact-head artifacts and hardware results |
| Optional named-pack parity including any wet/Mixer/bristle/TPL expansion | No named pack required for the chosen RCP1 target | A future expansion needs pack revision/preset identity and authorized reference access; it does not block the original-fixture implementation |

No planned or unresolved row can be counted as completed by passing the current
ABR tests. Keep every row visible until the full outcome is fulfilled. There is
no general Kyle compatibility claim. Original redistributable fixtures are the
public test corpus; proprietary packs are not bundled.

## Normative RCP1 contract — revision 2

This section governs whenever an earlier overview uses “supported,” “specified,” “subset,” or a provisional metric. RCP1 is the chosen practical engineering target under the long-term maintainability direction, not an assertion that compatibility already exists. Original fixtures establish the target without waiting for optional named packs. Touch/UI usability work is separate; only the brush import/editor workflow is included here.

### Three independent status axes

Store these per preset, per importer version and per profile version; display them separately:

* **Decoded:** the container and all referenced resources were read with validation. Values may still describe an unsupported engine. “Partially decoded” lists missing/unknown records; it is not successful preset import.
* **Semantically supported:** every active setting, dependency, numeric range and combination belongs to the selected profile and has an implemented mapping. Parsing an unknown field into an opaque blob does not qualify. Inactive settings may be preserved only when their disabled state is provable. An unrecognized enable flag is conservatively active/unknown. A preset needing conversion is labeled **converted approximation**, with source and replacement semantics shown.
* **Reference-verified:** a specific preset or explicitly bounded equivalence class passed the comparison suite against the stated source application build, document settings and asset hashes. Record date, evidence and confidence scope. A supported preset can remain unverified; a verified tip decoder does not verify dynamics. Verification does not automatically cover another pack revision or app patch.

The repository now supplies native rendering and package statuses. Foreign behavior profiles below remain planned, and reference verification is pending. Commercial/readme language must say “imports the listed brush settings” until reference evidence exists. Never label RCP1 generically “Kyle-compatible,” “Photoshop engine compatible,” or “all Krita brushes.”

### Source-version × engine × active-setting matrix

Use **K5341** = Krita 5.3.4.1, **P2711** = Photoshop desktop 27.11, as fixed behavioral baselines. These are the current stable application versions identified by the primary release sources on 30 October 2026. Krita 6.0.4 shares the 5.3.4 source but remains a separate Qt6 tablet/platform verification target; behavioral verification does not transfer automatically. K5341 has an official release record; Adobe release notes identify September 2026 version 27.11; older 27.0 sections are historical. Installer availability and authorized reference execution remain execution prerequisites; do not replace either version silently. Record executable hash/platform and retain reference images legally. A different available version requires a new profile ID and explicit baseline revision. [Krita download](https://krita.org/en/download/), [Adobe release notes](https://helpx.adobe.com/photoshop/desktop/whats-new/photoshop-on-desktop-release-notes.html).

ABR container version does not identify the Photoshop build that last painted a stroke. The ABR rows target files as interpreted by P2711, not historical Photoshop parity. Likewise KPP without producer metadata is not proof of a source version: load it in K5341 and record that interpretation for verification.

| Container/version | Engine/profile | Required active setting set | Not included in that profile |
|---|---|---|---|
| ABR 1 and 2, separately | P2711 dry legacy | Sampled 8/16-bit raw/PackBits tips; computed ellipse diameter, hardness, angle, aspect and spacing; any absent setting retains explicit legacy default | Descriptor dynamics not present in these records must not be inferred from a name. |
| ABR 6/1, 6/2 | P2711 dry modern | D set below; samp/desc/patt association | Wet/Mixer, bristle, erodible and unsupported descriptor types |
| ABR 7/1, 7/2 | P2711 dry modern | Same D set; separate version fixtures | Same exclusions; do not assume v6 byte layout without validation |
| ABR 8/1, 8/2 | P2711 dry modern | Same D set; separate version fixtures | Same |
| ABR 9/1, 9/2 | P2711 dry modern | Same D set; separate version fixtures | Same |
| ABR 10/1, 10/2 | P2711 dry modern | Same D set; separate version fixtures | Same |
| ABR 3–5, >10, or v6–10 subversion other than 1/2 | None | Header identification/error only | No preset installation, and no “try nearest version” fallback |
| KPP interpreted by K5341; standalone or bundle member | K5341 Pixel | K set below; build-up and wash as separate modes | Other engine IDs; inactive/unknown engine selection cannot default to Pixel |
| KPP interpreted by K5341; new-smudge flag off, Smear | K5341 Smudge Legacy Smear | S set, old-model parameter relationships | Not a claim of fidelity to pre-5.0 application versions |
| KPP interpreted by K5341; new-smudge flag off, Dulling | K5341 Smudge Legacy Dulling | S set, old-model parameter relationships | Same |
| KPP interpreted by K5341; new-smudge flag on, Smear | K5341 Smudge New Smear | S set, new-model parameter relationships | Lightness/height-map and gradient tips excluded |
| KPP interpreted by K5341; new-smudge flag on, Dulling | K5341 Smudge New Dulling | S set, new-model parameter relationships | Same |
| KPP lacking sufficient model information | None until resolved | Decode metadata and request re-export via K5341 or known version manifest | Never choose old/new model by visual guess |
| KPP created by other Krita versions | Unverified import candidate | Decode known fields, validate K/S set, preserve producer metadata | No source-version equivalence claim; verification requires K5341 interpretation or a new profile |
| Bundle exported by K5341 | Resource container, not engine | Manifest, tags and dependencies of the above K5341 profiles | No automatic support for every member engine; archive version alone does not imply brush support |
| GIH consumed by K5341 | Resource of Pixel profile | One selection dimension; ordered/incremental or random selection; finite grayscale/RGBA sequence | Multidimensional rank/select, angular/velocity/pressure/tilt selectors excluded from import parity |
| Native RCP1 package | Groundzero dry/native-wet | Versioned settings defined here | Not a portable ABR/KPP exporter or source-app fidelity claim |

The existing ABR parse entry accepts versions 6–10 and forwards the second header word; its sample reader distinguishes subversion 1 from a longer-offset branch. New parsing must reject unknown subversions explicitly, even if a permissive legacy path happened to accept them. Do not treat a header accepted by current code as evidence of an independently validated format variant.

**D — Photoshop dry active-setting whitelist:** grayscale sampled or generated ellipse tip; diameter, roundness, hardness, spacing, fixed angle; shape dynamics size/angle/roundness with minimums; pressure, tilt, barrel rotation, direction, initial direction, fade and seeded jitter controls where the source exposes them; one-/two-axis scattering, count and count jitter; transfer opacity and flow; color dynamics foreground/background mix and hue/saturation/brightness jitter; flip X/Y; timed airbrush; texture pattern with scale/depth/invert/contrast/brightness, fixed origin or texture-each-tip; dual sampled/generated mask with its own size/spacing/scatter/count and flip. Global source smoothing, wet edges, build-up variants not represented by these controls, protect-texture cross-preset state, noise extras, brush-pose override and undocumented active descriptor extensions are unsupported. Disabled unknown controls require evidence that they are disabled. Modes are restricted by the combination table below.

**K — Krita Pixel active-setting whitelist:** generated default/soft/Gaussian circle or square, including fade/softness and aspect; PNG/GBR alpha masks or RGBA color images; GIH modes above; diameter/spacing, fixed versus auto spacing, fixed precision 5, antialiasing on/off, angle, ratio, flip/mirror, scatter; size, opacity, flow and rotation dynamics driven by pressure, speed, distance, time, drawing angle, tilt axes/elevation/direction, rotation, fuzzy-dab or fuzzy-stroke; independently mapped sensor curves and multiplication of enabled sensor responses. Source curves use their declared interpolation, not an assumed linear conversion; the IR supports piecewise-linear and cubic curves. Additive/max/min sensor combiners, automatic precision reduction, spikes, lightness/gradient tips, texture height maps and gradient color sources are excluded. Foreground color with HSV dynamics, bounded texture masks, airbrush and wash/build-up are required. Masked brush is Pixel/wash only.

**S — all four Krita smudge active-setting whitelists:** K's single alpha-mask tip/geometry/spacing and applicable pressure/distance/time/tilt/rotation dynamics; opacity/flow, color rate, smudge length/strength, smear-alpha on/off, HSV foreground dynamics and texture multiply/subtract masks. Dulling additionally supports smudge radius 0–300% in K5341 units; Smear records the radius as inactive rather than using it. Both enabled and disabled smudge-length toggles must preserve K5341 meaning; disabling a toggle must not be guessed to mean zero. Overlay sampling must be off (current raster only); no masked/dual brush, color-image/gradient/lightness tip, height map, Paint Thickness, cross-layer sampling or non-normal color blend in S. The four profiles are required, not one representative that stands in for the others. The manual documents different old/new relationships and distinct smear/dulling behavior; the narrowed S contract is our choice. [Smudge behavior](https://docs.krita.org/en/reference_manual/brushes/brush_engines/color_smudge_engine.html).

Numeric envelope for new profiles: diameter 0.25–4,096 document pixels; normalized pressure/opacity/flow and mask depths 0–1; aspect 0.01–1; angles modulo 360 degrees; scatter up to 5 tip diameters; count 1–64; manual spacing 0.01–5 diameters; timed airbrush 1–120 dabs/second; curves within resource limits. Imports outside this envelope are unsupported, never silently clamped. These are semantic acceptance limits, not promises of 60 Hz at every extreme. A source's valid zero spacing is supported only by a documented profile-specific zero-spacing rule measured against the baseline; until implemented/tested that specific preset remains unsupported. Do not substitute 0.01 and report parity.

### Explicit combination table and operation order

| Profile | Color blend modes | Secondary-mask modes | Texture-mask modes | Sequence/color constraints |
|---|---|---|---|---|
| P2711 dry D | Normal, Multiply, Screen, Overlay; Erase and Lock Alpha are Groundzero modifiers verified separately | Off or Multiply only | Multiply or Subtract; invert/depth applied before combination | Single grayscale tip; no GIH; foreground/background mixing uses selected source colors |
| K5341 Pixel build-up | Normal, Multiply, Screen, Overlay | Off; masked brush in build-up is unsupported | Multiply or Subtract | Alpha masks and RGBA color images; GIH ordered/random allowed |
| K5341 Pixel wash | Normal, Multiply, Screen, Overlay | Off, Multiply, Subtract | Multiply or Subtract | Same; secondary tip must be single alpha mask, not nested GIH/dual masks |
| Four K5341 Smudge S models | Normal only | Off | Multiply or Subtract | Single alpha mask; current raster only |
| Groundzero native wet | Normal only, Erase/Lock Alpha separately defined | Off | Multiply or Subtract | Single alpha mask; explicit native formula below |

All Cartesian combinations within a row are required unless that row states a restriction. Source settings outside the row produce a converted/unsupported result, not a nearest mode. The native mask definitions are Multiply(a,b)=a*b and Subtract(a,b)=max(0,a-b), for coverage a,b in [0,1]. Foreign adapters must account for source mask polarity and depth evaluation; identical dropdown labels are not proof of identical math. For source subtraction or depth behavior that differs, implement its profile-specific equation and test it rather than relabel the native operation.

Required conceptual evaluation order: normalize samples → evaluate sensors/curves at the dab position/time → compute spacing/tip index and seeded variation → transform/sample primary tip → evaluate texture → compute/evaluate secondary mask if enabled → combine coverage → evaluate color/flow → blend into either the stroke buffer (wash) or destination (build-up) → apply stroke opacity → apply selection/alpha policy → publish dirty tiles. For smudge, sample the pre-deposit raster and update the model state before deposition; never accidentally sample this dab's partly written tiles. Source-specific departures from this order are recorded as profile rules with tests, not hidden importer tweaks. Krita explicitly limits masked brush to wash; preserving that is a correctness condition, not an arbitrary missing feature. [Masked brush](https://docs.krita.org/en/reference_manual/brushes/brush_settings/masked_brush.html).

GIH native ordered sequence increments once per emitted primary dab, starts at index zero each stroke, wraps modulo frame count; random selection is uniform over frames from a stroke-local deterministic PRNG. Do not increment for preview sampling, skipped display frames or cancelled speculative dabs. These are native rules; K5341 equivalence must be verified on original GIH fixtures because consuming applications may differ. GIMP describes multiple selectors, and Krita notes it does not support all GIH parameters. [GIMP animated brushes](https://docs.gimp.org/3.0/en/gimp-using-animated-brushes.html), [Krita tip modes](https://docs.krita.org/en/reference_manual/brushes/brush_settings/brush_tips.html).

### Color and native wet mathematics

RCP1 chooses **encoded sRGB arithmetic** for all foreign profiles and Groundzero native wet, with an sRGB IEC61966-2.1 RGBA8 document. Source application's linear-light blending options must be off in reference fixtures. No runtime toggle silently changes a verified profile into linear-light mode. A future linear profile has a separate engine ID and fixture set. Load colored tip assets through an explicit sRGB conversion; alpha/gray mask data is coverage, not gamma-transformed color. Reject unsupported document color spaces/depths from this mode without changing the document. Display conversion is presentation only and is excluded from saved-pixel comparison.

Internally store premultiplied color C and alpha A in [0,1]. Unpremultiply only when A>0; define straight RGB as zero at A=0, never divide by epsilon and invent hidden color. For straight source Cs, backdrop Cb, source alpha As, backdrop Ab and separable blend B, use Co = As*(1-Ab)*Cs + As*Ab*B(Cb,Cs) + (1-As)*Ab*Cb and Ao=As+Ab*(1-As). B is source for Normal, Cs*Cb for Multiply, 1-(1-Cs)*(1-Cb) for Screen, and Overlay branches on Cb≤0.5. This is the reference native composition definition; source profiles may require documented rounding variants for parity. [W3C compositing/blending](https://www.w3.org/TR/compositing-1/)

Quantize to RGBA8 only at specified raster commit boundaries: clamp 0–1, round half up using floor(255*x+0.5). Keep stroke buffers at float32 minimum; the deterministic reference path uses float64 scalar arithmetic with fixed operation ordering and no fast-math. Erase multiplies destination premultiplied RGB and alpha by (1-coverage*flow); alpha lock preserves destination alpha and confines color deposition to existing coverage. Store hidden RGB at zero when alpha is zero. The adapter must explicitly reconcile the actual host raster convention; it must not assume that normalization alone establishes premultiplication.

Native wet is an original Groundzero brush, not an assertion of Photoshop Mixer or Krita algorithm parity. State R is a premultiplied RGBA reservoir, initially selected foreground at configured load alpha. At each dab sample premultiplied Q from the pre-deposit raster using the normalized tip weights (transparent samples contribute alpha zero). Update R1=(1-p)*R+p*Q for pickup p∈[0,1]; form paint P=(1-c)*R1+c*F for color rate c; deposit P through mask/flow/opacity using Normal composition; then set R=(1-d)*R1+d*F for reload d. Smear native mode uses a transported previous-dab tile field in place of uniform Q/R; Dulling native mode uses a weighted average reservoir. Native wet defaults are p=0.5,c=0.25,d=0, and all state resets on stroke end/cancel. These settings are editable/native presets only. The four K5341 profiles need their own measured parameter relationships; this formula is not used to conceal a missing emulation.

### Exact parity measurement protocol

Use fixed 2,048×2,048 RGBA8 sRGB canvases for fidelity, document zoom 100%, no view rotation, no export resizing, no image registration/alignment and no post-hoc gamma or contrast matching. Diameter fixtures: 1, 8, 64 and 256 document pixels; spacing 0.01, 0.1, 0.5 and 1 where valid; opacity/flow 0.1, 0.5 and 1. Texture origin, source color, initial pixels, random policy, source smoothing and profile ID are recorded. Separate off-canvas fixtures deliberately cross boundaries; normal strokes start sufficiently far from boundaries to avoid clipping. Performance canvases remain separate.

Let B be the initial raster, R reference output and C candidate. A pixel is changed if any RGBA8 channel differs from B by at least one code value. Define E as the **union** of changed pixel sets in R and C, dilated by a fixed two-pixel radius and clipped to the canvas. All error/coverage summaries use E (including unchanged gaps), never only candidate bounds or intersection. Report bounding-box extents separately, changed-pixel counts, maximum error and the entire full-canvas diff to reveal stray marks. An empty candidate against a nonempty reference is failure, not zero error; both empty is a separate no-op test.

Compare premultiplied RGB and alpha separately. Dry-media mean absolute premultiplied RGB error over E ≤2/255; alpha MAE ≤1/255 and alpha p99 absolute error ≤4/255. Coverage IoU ≥0.98 using alpha coverage ≥1/255 for painting on transparent backgrounds; on opaque backgrounds use the changed-pixel sets instead. Report both versions when applicable. Endpoint/width displacement ≤1 document pixel for 1/8-pixel brushes and ≤2% diameter for larger brushes; spacing error ≤2%. Test alpha-lock/erase against exact alpha invariants. No ΔE comparison on undefined transparent RGB: composite both images over fixed black and white backgrounds and score both; source color accuracy is measured only after the specified conversion.

Canonical input traces store integer microsecond timestamps, document x/y, pressure, tilt and rotation/capabilities at 240 Hz, with exact pen-down/up and taps. The engine test consumes those raw events; dab scheduling resamples by profile spacing/time, using linear interpolation of position/pressure/tilt and shortest-arc interpolation for rotation. Preserve endpoints; no event-rate-dependent random reseeding. Test alternate delivery at 60/120/240 Hz, batches of 1/4/16, duplicate timestamps and a 50 ms pause. Compare identical underlying paths separately from intentionally sparse curves; do not demand identical output after deleting path information. Native scheduler output must be identical under batching when sample content/timestamps are identical.

For external references record delivered events, not merely intended OS injections. Disable source stabilizers/smoothing, capture pressure normalization and tablet driver settings, and use equivalent document transforms. If a source interface cannot deliver tilt or the same timing, mark those fixture dimensions unverified; an image-only comparison cannot validate tablet feel or event semantics. Device-in-the-loop latency tests stay separate from deterministic engine traces. No claim of verified timing behavior follows from a synthetic screenshot.

Smudge numeric drift test: 10,000 dabs over original color strips and alpha ramps, compare checkpoints at dabs 1,10,100,1,000,10,000. Same-platform repeat must hash exactly; cross-platform scalar reference checkpoint RGBA errors ≤1/255 per channel and reservoir component absolute error ≤1e-6 against the saved reference state (native engine only). Foreign source output at every checkpoint must have premultiplied RGB MAE ≤3/255, alpha MAE ≤2/255, mean ΔE00 ≤3 and p95 ΔE00 ≤6 on both black/white composites. No monotonic increase of MAE by more than 1/255 from checkpoint 1,000 to 10,000. Also require pickup/deposit transition position within 5% of stroke length and no inverted pickup direction, dark/bright halo or lost transparent-edge character. Do not claim access to another app's hidden reservoir state; foreign drift is output-based.

For opaque source RNG, draw **100 independent strokes per fixture per application**, fixed path/settings and separate runs, with randomized order; compare three spatial bands (leading/middle/trailing) independently. Measure coverage fraction, integrated alpha, mean stroke width, occupied-tip count and normalized high-frequency texture energy at fixed spatial scales. For each statistic require the 95% bootstrap confidence interval (10,000 resamples, fixed analysis seed) for the difference of means to lie entirely within ±5% of the reference mean, with an absolute floor of 0.01 for normalized near-zero statistics; also require the candidate/reference standard-deviation ratio in [0.8,1.25] unless both variances are below that floor. All statistics/bands must pass; do not average failures away. Additional visually selected fixtures require blinded artist review. This earns **statistical reference verification**, explicitly different from pixel verification. For native known RNG, test exact seed/output repeatability instead.

### Kyle/wet-media scope decision and remaining user choice

The practical complete target is **RCP1 dry-media import plus all four specified K5341 smudge profiles plus native wet painting**, including import reports, presets, durability and QA. It is not a generic Kyle pack emulator. A Kyle preset that is an ordinary sampled dry brush and uses only D can qualify after verification; one relying on Mixer Brush, wet edges, bristles, erodible tips, unsupported dual/texture modes or TPL cannot. Native wet conversion is a useful alternative, never advertised as that original preset working correctly.

If broad Kyle compatibility is the required end state, add a separately scoped **W track before declaring the user's outcome complete**: identify exact wet/Mixer/bristle/TPL records in the chosen pack; specify wet/load/mix/flow and clean/load-after-stroke behavior, brush pickup sampling and reservoir lifecycle; model bristle geometry/stiffness/pose and supported sensors; address TPL decoding if needed; establish original authorized reference fixtures; and apply the same import/save/performance/fidelity gates. W is not presently feasible to estimate or promise for every historical pack from the word “Kyle.” Do not replace it with generic native smudge or quietly drop it after implementation. Pack findings may also expand D texture/dual/blend modes without requiring a whole wet engine.

The only missing product information that cannot be inferred is: **Which specific packs/presets and acquisition/version are must-haves, and is approximate artistic conversion acceptable for those, or must their original wet/bristle behavior match?** Names, pack revision/date, preset names and source app version are sufficient for initial scoping; do not request public redistribution of proprietary files. If none are mandatory, recommend accepting RCP1 under its precise name. Users do not need to choose alpha equations, parser limits, ABR subversion guards, testing statistics or the internal engine split.

M0 now freezes the matrices above and gathers required evidence rather than choosing undefined subversions/modes. M2/M3/M4 exit gates must cover every row and declared combination; missing comparisons remain unverified. The 15–27-week estimate applies only to RCP1 and should be re-estimated once fixture availability and independent source-model measurements are known. W, additional source versions, linear-light profiles and unrelated touch/UI work are excluded from that estimate, with no implication that the broader user goal has been accepted as narrower.


## Native implementation and conversion boundary

The opt-in **Native Brush** control applies only to regular RGBA8 raster drawing.
Legacy MyPaint is the default. Built-in native round, wet mix and smear presets,
a scratchpad using the real renderer, basic settings editing, `.gzbrush` import/
export and a local embedded-resource library are implemented. Pixels are baked
into the existing raster level and use its normal image saving and undo machinery.
Host-adapter tests cover actual raster writes, tile undo, PNG save/reopen, input
rotation and generated pen-up timing. Interactive scene/frame/level lifecycle and
physical tablet qualification remain release gates; these lower-level tests do
not prove the entire application workflow.

Native v1 performs encoded-sRGB premultiplied blending, deterministic seeded
sampling, distance/time dab scheduling, multiplicative linear sensor curves,
bounded sampled/generated tips, image sequences, textures, secondary masks,
wash/build-up, alpha lock, erasing, and original wet reservoirs. A shared lazy float tile cache retains sub-byte deposits across dabs and replicated
tracks while the RGBA8 host receives display pixels. The float cache, wash buffers
and wet reservoirs share a 256 MiB stroke budget; preset image resources and the
host application's existing backup/undo buffers have separate budgets.

Generated native tips have an explicit antialias switch: an 8×8 fixed area
sample grid integrates each destination pixel through the tip's rotation and
aspect transform. Fully covered/outside hard-tip footprints use exact fast
paths. The same rule applies independently to secondary masks; bitmap tips
retain bilinear sampling. Missing switches in older native packages mean off,
preserving their rendering. The editor, package round trip and generated KPP
conversion carry the choice. This is a native coverage definition, not a claim
of Krita's antialias kernel. CI fixtures check a one-pixel square split across
four pixels, a quarter-pixel dab and rotated anisotropic coverage.

Foreign resource adapter v1 is an explicit **converted approximation** path:
GBR 1/2 mask/RGBA resources, GIH single-dimension incremental/random sequences,
ABR sampled tips and legacy v1/v2 computed ellipses, and KPP 2.2/5.0 Pixel/Color Smudge containers. Native wet pickup, paint mix and radius can use
sensor curves; mapped KPP curves are explicitly converted to linear native curves.
KPP source model identifiers are retained, but native wet never stands in for
implemented Krita smudge semantics. Unmapped KPP fields are listed in the import
report; foreign installation requires accepting that conversion. Standalone KPP with missing external resources, ABR descriptor behavior and
unsupported selectors remain unimplemented. Bundle manifest v1.2 resolves declared
PNG/GBR/GIH tip assets by explicit path/name or checksum; archive parsing never
extracts files. Missing, ambiguous or malformed dependencies reject the import.
These conversions do not satisfy the foreign-profile acceptance rows above.

Legacy computed ABR records use the fixed 14-byte geometry layout described in
Adobe's [Photoshop 6 format specification](https://oldschoolprg.x10.mx/downloads/ps6ffspecsv2.pdf).
The reader retains signed spacing, diameter, roundness, angle and hardness
separately from sampled pixels. Native conversion uses an antialiased ellipse;
its edge falloff and angle/spacing behavior remain unverified against Photoshop.
Zero spacing and geometry outside the native envelope reject conversion rather
than silently clamping. Original v1/v2 fixtures cover field order, signed angles,
every short record length, extra bytes, portable preset reopening and fuzz seeds.
The legacy MyPaint sampled-tip importer still reports computed records as skipped.

PNG chunk CRC/framing and metadata decompression are bounded before Qt decoding.
Native assets are embedded by content hash; no asset path is extracted from a
package. Package input is limited to 64 MiB, individual decoded images to 2 million
pixels and aggregate image memory to 256 MiB. No bundled proprietary brush packs
are included. Original synthetic fixtures exercise resources and errors.

Format references: [GIMP GBR specification](https://developer.gimp.org/core/standards/gbr/),
[GIMP GIH specification](https://developer.gimp.org/core/standards/gih/), and
[Krita resource documentation](https://docs.krita.org/en/reference_manual/resource_management/resource_brushtips.html).
KPP field layout was inspected in the public KDE preset data, including
`libs/ui/tests/data/testing_200px_colorsmudge_default_dulling_new_sa.kpp`;
no third-party implementation source or brush asset is included in these changes.

## Baseline correction — revision 3

The current stable Krita baseline is **5.3.4.1 (K5341)**, released October 7, 2026,
not 5.3.4. The earlier release post did not include the subsequent display-profile
hotfix. This is an explicit profile-baseline revision; no verification transfers
from 5.3.4 or the separate Qt6 target. See the [current download page](https://krita.org/en/download/)
and [October report](https://krita.org/en/posts/2026/monthly-report-2610/).

A CI reference-acquisition workflow now attempts to run the official Linux 5.3.4.1
AppImage against five original generated presets using the public `Node.paintLine`
API. It records application version, binary hash, source presets, pixel hashes and
output images. Successful fixture generation would establish source execution,
not behavioral parity, and does not qualify hardware input or timed airbrush.
Source-reference availability must be reported from the actual workflow result;
it is no longer treated as an assumed permanent blocker. No paid packs or source
application implementation code are included.

The Photoshop baseline is explicitly revised to **27.11 (P2711)**, the latest
release section in the primary Adobe notes checked on October 30, 2026. The
earlier 27.0 citation came from a historical section of the same page. No
Photoshop execution or parity is established by this correction.


## Native workflow additions

The editor exposes sensor mappings, linear and natural-cubic curves, embedded
image sequences, texture settings, dual masks, HSV foreground variation and
premultiplied foreground/background mixing. Natural-cubic curves have zero
second derivative at the endpoints, clamp their output to [0,1], and reject
knots separated by less than 1e-9. Their compiled coefficients are included in
the stroke budget. This interpolation name is an explicit native rule, not a
claim that every foreign curve uses the same rule.

Collection imports let the user select multiple presets, stage all selected
packages, and publish a complete content-identified directory. Invalid members,
conflicting installed content and cancellation before publication do not install
a subset. Parsing runs in a separate process with a 120-second wall-time limit and hard
cancellation. The application accepts only bounded native packages and a bounded
result manifest from that worker. Unix workers also have a 60-second CPU limit
and disabled core dumps. Staging runs on a thread and checks cancellation between
bounded resources. The worker has the current user's filesystem/network
privileges: this contains crashes and hangs, but is not a privilege sandbox.

The CI native benchmark records 60-second 240 Hz dry and 120 Hz textured/dual
and smudge traces, event-to-surface CPU percentiles, tracked active-stroke memory,
and separate backing-raster memory. Hosted-runner results are measurements, not
hardware/display latency qualification. Foreign reference qualification remains
pending; the expanded native workflow does not satisfy those unfinished rows.

## Native qualification progress

The application CI workload now drives the real raster brush tool and undo
manager in a disposable profile. It checks one-transaction strokes, exact
undo/redo, cancellation without history entries, new-frame undo/redo and
cancellation, and PNG reopening. Passing lower-level tests is not a substitute
for this workload, which remains pending until its exact-head CI run succeeds.
The native library also provides filename/collection search and no longer
silently truncates the selector at 2,000 installed packages.

The import fuzz job uses original generated native, KPP, GBR, GIH, bundle and
ABR inputs and bounded libFuzzer mutations with address/undefined sanitizers.
A short CI fuzz run is regression evidence, not proof of parser safety or a
substitute for a privilege sandbox. Long native wet traces check repeatability,
premultiplied color invariants and memory release through 10,000 dabs; foreign
smudge drift and cross-platform scalar-reference tolerances remain separate.

Hosted Ubuntu measurements at `b784908cb703c638c83cc11193e274f7ba69deb9`:

| Original 60-second workload | Event CPU p95 / p99 | Peak tracked stroke bytes |
|---|---|---|
| Dry, 2048², 64 px, 240 Hz | 0.413 / 0.468 ms | 42,995,072 |
| Texture + dual, 4096², 256 px, 120 Hz | 7.326 / 8.226 ms | 202,672,256 |
| Smear, 4096², 256 px, 120 Hz | 10.740 / 12.234 ms | 247,838,880 |

All three released tracked allocations after stroke completion. Backing rasters
are accounted separately (128 MiB dry; 512 MiB heavy). These are CPU measurements
on a hosted runner, not physical tablet-to-display latency. The native-metrics
check passed at that commit; subsequent-head results must be checked separately.

The source-reference failure at `22e202a45` is diagnosed as the official
AppImage's embedded Python failing to locate its `encodings` standard-library
module, before the reference plugin loads. The harness now locates the bundled
standard library and supplies its prefix/search path only to the reference
application. Successful execution and all source-profile comparison gates remain
pending; no source fidelity claim follows from this harness fix.

## Resource preflight and retained conversion notes

Decoders receive the remaining transaction budget before allocating the next
image, including when the parent reads multiple worker-produced packages and
when a bundle resolves multiple presets. PNG preflight reserves retained float
pixels plus a conservative per-pixel codec/conversion allowance before Qt
allocates its decoded raster. Encoded file, JSON/XML metadata and application
memory remain separately bounded/accounted; these checks are not an OS memory
or privilege sandbox. Fresh PNG-to-mask conversion happens before publication
without duplicating the full float image.

A dedicated non-sanitized Linux CI process consumes hostile worker output whose
two individually valid packages would retain 320 MiB of float image pixels.
It must reject the excess image before allocation, return no partial records,
and remain below a separately reported 320 MiB total-parent-RSS qualification
envelope. The transaction image limit remains 256 MiB. This fixture qualifies
that failure path only; it is not a universal RSS guarantee. Validation is pending
until the new CI workload runs.

Conversion notes are preserved in native packages and exposed by the editor's
Brush details control, including older KPP conversion reports. The optional
notes field accepts at most 1,024 bounded strings and 256 KiB of serialized notes;
older packages without it still load. Verification is never conferred by imported
metadata. Windows import workers use Qt's Unicode command-line arguments.

The five-preset source comparison is exploratory measurement only. Its green
result means the workload and report completed; it does not enforce the RCP1
fidelity thresholds. Every current conversion remains source-semantically
unsupported and source-reference-unverified until its actual behavioral profile
and required comparison suite are implemented and pass.


At `60140f4ff670607af866afcae006fe80fa6be339`, the hostile multi-package
qualification passed with parent peak RSS **223,744 KiB**. All five initial Krita
fixtures rendered, but the reference job failed during application teardown;
those artifacts do not constitute a passing parity gate. The harness now closes
views/windows through the normal application lifecycle and expands to 68
original parameter cases. Expected source IDs are checked exactly against the
generated manifest; duplicate or missing cases fail. Native conversion
measurements remain separate from foreign semantic support and reference parity.

The real Linux application integration workload passed at
`c2014651285a44deee6f8195861fc413972ccffe`, including native strokes, exact undo/redo,
frame/level cancellation and save/reopen. Windows/macOS application runs and
physical tablet-to-display qualification remain separate release gates.

## Typed ABR metadata decoding

The independent ABR reader now decodes bounded version-16 Action Descriptor
objects/lists and scalar types from Adobe's published data-layout specification.
It preserves class/key IDs, unit codes and signed 64-bit integers; sampled tips
retain their explicit IDs. Native conversions retain sample IDs and decoded
object counts as provenance. Objects are **not** associated by positional guess,
and descriptor decoding does not mark names/dynamics/patterns as implemented.
Unknown types and duplicate keys reject their descriptor section with a report;
node/depth/allocation-limit failures reject the complete import. Limits are
16,384 nodes, depth 16 and 4 MiB of accounted descriptor allocations, independent
of image-resource budgets. Alias bytes remain opaque and are never opened.
Original byte fixtures cover typed values, sample IDs, every truncated prefix,
duplicate keys and aggregate limits. Behavioral adapters and source-parity
qualification remain required before this ledger's ABR behavioral row closes.
[Adobe descriptor data layout](https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/).
