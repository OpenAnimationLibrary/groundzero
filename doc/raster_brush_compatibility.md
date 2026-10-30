# Regular-raster brush compatibility contract

Status: implementation in progress against the scoped independent compatibility contract.
ABR decoding includes sampled tips, legacy computed geometry and typed metadata.
Strict experimental K5341 component profiles are implemented; none of the full
foreign behavior profiles below is complete or reference-verified. Keep this work separate from vector/touch usability and neural styling.

Current platform qualification is incomplete. The signed macOS helper now starts,
but its prior memory admission used adjusted task accounting instead of Darwin's
raw mapping size. The replacement enumerates bounded top-level mappings and keeps
the fixed growth budget and allocation-refusal probes; CI qualification is pending.
Neither this growth limit nor the separate sampled footprint watchdog establishes
an absolute OS-enforced resident-memory ceiling. Windows overlap qualification
also remains pending after correcting an already-exited-child wait race. See
[platform boundaries](raster_brush_platform_isolation.md) for current mechanisms;
historical checkpoints below do not qualify the latest boundary.

Native preset alpha preservation is combined with the toolbar lock instead of
being overwritten by an unchecked modifier. The global eraser explicitly clears
the effective lock without changing the saved preset; erase coverage is
independent of foreground pigment alpha. Actual-tool fixtures cover these choices
through undo, redo, cancellation and scene/level reopening. Experimental source
presets also offer an explicit, cancellable conversion into an editable native
copy. The original and resource rights remain intact, and changed wet equations
and reset controls are disclosed. This artistic conversion does not close any
missing foreign-profile acceptance requirement.

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
| ABR descriptor/computed-tip/pattern behavioral import | Typed descriptors, bounded pattern decoding, legacy 1/2 computed tips and ABR 6/2 sampled-ID/static native binding implemented; other version bindings and source dynamics pending | Every declared version and active setting; decoding is not behavioral support |
| KPP/bundle/GBR/GIH import | GBR 1/2 and one-dimensional GIH native conversions; KPP native conversion plus strict fixed-tip source candidate selection; bounded bundle dependency resolution | Unknown/missing active settings retain native conversion; candidate flags do not claim source verification |
| Independent dry renderer and defined dynamics/combinations | Native CPU renderer and independent fixed hard-circle source candidate implemented | Six isolated and nine long source hard-pixel gates passed; source AA/softness/dynamics remain incomplete |
| Four Krita smudge profiles and native wet | Native wet/smear plus four separate fixed-tip source candidates implemented | All 48 wet-pressure gates passed at 5423354f; 215 enforced full-image kernel gates passed at 99ca200c, including nonzero-radius legacy/new Dulling. Earlier fixed/control/state gates passed; full dynamics/geometry/texture profiles remain incomplete |
| Real tilt/rotation capture and normalization | Additive Qt tablet fields and track interpolation implemented | Linux full application CI passes; physical device qualification pending |
| Actual-engine preview, editor, portable preset export/relink | Native scratchpad, sensor/curve/texture/dual/color editor, provenance-aware manifest/permitted/private packaging and exact pixel-hash relink/repair implemented | Portable protocol regressions and application workflow fixtures implemented; final-head application export/repair qualification remains open |
| Undo/cancel/frame lifecycle/save/reopen | Native path uses raster undo; cancellation rolls back tiles and created frame | Linux actual-tool and installed-package undo, redo, cancellation, frame creation and PNG reopen workloads passed at `31a0bee6`; exact final head and other installed platforms remain gates |
| Parser isolation/fuzzing/cancel and complete resource budgets | Pre-allocation shared budgets, anchored no-follow response reads, Linux confinement, private Windows AppContainer runtime and signed macOS App Sandbox helper implemented | Historical six-platform parser/fuzz passes do not qualify later isolation changes; exact-head sandbox, malicious-response, cancellation and packaging checks remain release gates |
| Packaged Windows/macOS/Linux QA and performance | Installed application brush workloads wired on all three platforms; native numeric/performance CI implemented | Linux package workload passed; final Windows/macOS package results and hardware latency remain pending |
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

### Kyle/wet-media scope decision

The practical complete target is **RCP1 dry-media import plus all four specified K5341 smudge profiles plus native wet painting**, including import reports, presets, durability and QA. It is not a generic Kyle pack emulator. A Kyle preset that is an ordinary sampled dry brush and uses only D can qualify after verification; one relying on Mixer Brush, wet edges, bristles, erodible tips, unsupported dual/texture modes or TPL cannot. Native wet conversion is a useful alternative, never advertised as that original preset working correctly.

If broad Kyle compatibility is the required end state, add a separately scoped **W track before declaring the user's outcome complete**: identify exact wet/Mixer/bristle/TPL records in the chosen pack; specify wet/load/mix/flow and clean/load-after-stroke behavior, brush pickup sampling and reservoir lifecycle; model bristle geometry/stiffness/pose and supported sensors; address TPL decoding if needed; establish original authorized reference fixtures; and apply the same import/save/performance/fidelity gates. W is not presently feasible to estimate or promise for every historical pack from the word “Kyle.” Do not replace it with generic native smudge or quietly drop it after implementation. Pack findings may also expand D texture/dual/blend modes without requiring a whole wet engine.

Pack selection is resolved for this implementation: use original or openly
licensed representative fixtures and the complete RCP1 contract, prioritizing
long-term maintainability. No named paid pack is a prerequisite or an open
product question. Optional future pack qualification needs a specific pack
revision, preset identity, source application baseline and authorized reference
access. It must preserve the distinction between a native artistic conversion
and verified source behavior; it does not reduce any RCP1 requirement.

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
that failure path only; it is not a universal RSS guarantee. The passing result
and measured RSS are recorded below.

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


## Current source measurements and installed-package gate

At `50e067b46725bfba3eefada288d81f81dcecb333`, all 68 original source cases
rendered in Krita 5.3.4.1, effective preset XML was captured, normal application
shutdown completed, and the comparison process exited successfully. This closes
the reference harness execution failure, **not** the behavioral parity gates.
For example, the dry baseline measured premultiplied RGB MAE 0.008499 and alpha
MAE 0.007807; legacy Smear measured 0.123856 and 0.151572 respectively. Both
remain outside the required fidelity limits. Every native conversion still
records source semantic support and source reference verification as false.
All six OS/Qt brush suites, native metrics and importer fuzzing passed on that
head; the complete application matrix was not yet finished when recorded.

The installed-package workflow now also invokes the actual raster-brush workload
from the produced AppImage, macOS application or Windows executable in an isolated
profile. Its result is separate from the existing installed SeExpr workload;
a passing expression test alone does not qualify the brush package. These newly
added package brush checks require their own passing exact-head results.

Native wet pickup uses the tip-local frame, including aspect, rotation and
mirroring, for pre-deposit samples and transported fields. Radius zero samples
the dab center exactly. Original regression fixtures check a narrow tip over a
colored band, its 90-degree rotation, zero radius and an asymmetric mirrored
mask in both native wet modes. This fixes native geometry handling; it does not
implement Krita's four smudge equations. Seven short dry reference strokes add
measurements around the 3.2-pixel spacing boundary, bringing the original source
workload to 79 cases, including stored radius probes at 0, 3, 100 and 300. Those new geometry and short-stroke checks are pending CI
until a head containing them is published and passes.

## Native count, fade and directional primitives

The native editor and portable settings expose a count multiplier with ordinary
sensor curves. Each scheduled dab emits `floor(count * multiplier + 0.5)` stamps,
bounded to 0–64; zero suppresses deposition while scheduled dab index continues.
The `dab_index` sensor starts at zero, increments once per scheduled dab (not
once per replicated stamp), and divides by mapping scale before its curve.
A descending curve therefore supplies an explicit fade over that many intervals.
`initial_direction` retains the first nonzero motion segment for the stroke;
before any motion it is zero, including the initial mark. It is distinct from
the continuously updated direction sensor. Perpendicular-only scatter uses the
normal of the current motion segment; two-axis scatter remains the old default.
Native packages without the new fields retain full stamp count and two-axis
scatter. Original tests cover zero/two stamps, a three-step fade, a direction
change, perpendicular confinement and portable reopening. These are native
primitives for the planned adapters, not verification of Photoshop dynamics.

KPP embedded-resource conversion honors explicit `brushApplication` alpha-mask
and color-image values, with the modern value taking precedence over legacy
`ColorAsMask`. PNG color stamps retain premultiplied sRGB colors and transparency;
explicit masks use native inverse weighted encoded-sRGB gray multiplied by alpha.
GBR/GIH color resources honor the same mask selection. Conversion operates on
newly decoded images in place and preserves the pre-allocation memory limit.
Absent flags retain the prior native conversion rule. Lightness, gradient and
unknown application values reject conversion, matching the stated K/S boundary.
This is native resource interpretation, not verified Krita mask/color mathematics.
The numeric mode names were checked against the public
[brush application enum](https://github.com/KDE/krita/blob/master/libs/brush/kis_brush.h)
and serialized metadata in KDE's original test presets; no paint-engine
implementation was copied. The user-facing modes are described in the
[Krita brush-tip manual](https://docs.krita.org/en/reference_manual/brushes/brush_settings/brush_tips.html).

Native dual masks expose a separate count (1–64) and horizontal/vertical flips.
For each primary stamp, secondary offsets are drawn once from the stroke PRNG;
each secondary coverage includes its opacity, and their union is accumulated
as `b = b + (1-b)*coverage`. Multiply/subtract is applied once to that union.
A count of one with no flips preserves the prior native result and random draw
order. Optional package fields default to count one and no flips. Original
fixtures check two half-opacity stamps, subtraction, asymmetric bitmap mirrors,
limits and reopening. Optional native independent spacing is specified below;
source-specific dual scheduling remains required adapter work. Neither native
mode claims Photoshop or Krita union/scheduling parity.

An experimental `krita-5.3.4.1-hard-pixel-candidate-1` runtime profile implements
independently measured hard-circle lattice placement and defers an exact
spacing-boundary endpoint until further movement. This preserves interior dabs
when a path is split into input events while omitting a terminal boundary dab.
It is a separate strict profile, not a change to native painting. Unsupported
settings reject validation. The general KPP importer now selects this candidate
only when all required settings are explicit and eligible, with unknown fields
and attributes retaining native conversion. The harness verifies selection
through the importer and enforces fidelity gates. Source support/verification
flags remain false. The full K5341 Pixel and all four
smudge profiles remain unfinished; this is an implementation slice toward them.

Four separate experimental source wet profiles now implement legacy/new Smear
and Dulling state equations inferred from original measurements.
They initialize without depositing the first dab, snapshot feedback before writes,
use distinct legacy/new color equations and quantize deposited RGBA8 results.
The first 64-case comparison matched all 16 legacy Dulling cases exactly; Smear
exposed a separate pixel-centered footprint, corrected independently of Dulling.
The 40-case holdout adds third-dab feedback, sizes and unseen strengths. All three
candidates enforce max RGB <=2/255, alpha <=1/255 and changed IoU >=.98. Candidates
remain restricted to hard circles, fixed controls and sampling radius zero;
new Dulling uses radius-zero attenuation measured across three tip sizes;
full S-profile behavior remains incomplete. Runtime support for
an experimental package is not semantic support or verification of its source.


### Arithmetic feedback and application lifecycle continuation

Third-dab probes rule out interpreting smudge strength as source-over opacity.
The next candidate uses arithmetic interpolation of premultiplied feedback and
destination, followed by the independently measured old/new color step. The
32-case `wetstate` workload varies the third destination between transparent and
opaque yellow to distinguish this equation from carried-ink models that coincide
on uniform repeated backgrounds. Do not infer complete state fidelity from the
first/second-dab gates alone.

Strict KPP eligibility is extended to all four fixed-tip wet candidates only
with explicit model selection, smear-alpha enabled, radius zero, static controls,
and no unknown settings. Portable-preset tests preserve model identity and false
source verification flags. The reference harness requires actual importer
selection. Dry eligibility preserves build-up/wash and static opacity/flow, with
fourteen additional gated comparisons. The painting-mode enum and user semantics
are documented by Krita; no paint-engine implementation was copied.

The native host now distinguishes preparing a read/undo region from changing
quantized raster pixels. Initialization-only strokes must not create undo entries,
and newly created unpainted frames are rolled back. Installed-application tests
exercise both cases. Candidate CPU/memory traces cover hard wash and all four wet
candidates, including release of retained snapshots at stroke end. These added
checks are pending exact-head CI and are not completed acceptance claims.

## Third-dab qualification and application transaction checkpoint

At `763fb470`, Linux full-application and installed-application workloads pass,
including no-op wet initialization on existing and newly created frames. Native
metrics, fuzzing, tablet repeatability, hard-pixel long strokes, geometry and all
64 first/second-dab wet gates pass. The six Qt/platform parser suites fail a
new hand-authored pressure fixture whose size minimum retained the native 0.01
default despite requesting the strict source linear mapping; the fixture is
corrected to explicit zero, preserving the runtime guard.

All 32 varied-third-destination cases meet max-channel <= 2/255 and max-alpha
<= 1/255. Sixteen transparent-destination cases fail raw changed-set IoU because
source hidden RGB at zero alpha differs while premultiplied visible output is
identical or within those limits. The correction retains raw changed-set IoU and
the full raw-union numerical sampling region, adds visible changed-set IoU, and
uses that visible metric for the unchanged .98 wet coverage threshold. A
mixed-background synthetic metric regression covers the distinction. Independent
third-dab numerical fixtures now cover transparent and opaque-yellow destinations
for all four models, rather than only repeating the same red destination.

The diameter sweep failed before comparison because the integer-only reference
API received an integral-valued JSON float. The harness now validates integral
coordinates and explicitly constructs integer points; fractional inputs are
rejected rather than silently rounded. These corrections await exact-head CI.
Source pressure, opacity/flow, heterogeneous wet sampling, and broader AA/softness
remain separate acceptance items. No universal source-compatibility claim follows
from native application or component successes.

## Measured state corrections and default-circle implementation

Published head `0c514ec2` separates Dulling previous-center feedback from Smear's
transported field and samples pressure at the measured next integral path-distance
position. It also fixes tablet transform column normalization and the Qt6 QMenu
include, and adds real-application original-KPP import/install/select/paint,
undo/redo/cancel/PNG reopen and independent-stroke checks for all four wet profiles.
Exact-head CI is pending. These changes do not qualify broad source compatibility.

At `997ba9b3`, all six Qt/platform parser suites, import fuzzing, CPU/memory gates,
hard-pixel, geometry, tablet, first/second wet dabs, varied third destinations,
second-dab texture and Linux installed-application checks passed. Pressure,
patterned third-dab Dulling, seven-pixel Smear footprints and opacity/flow remained
red. The first two have implementation corrections; exact odd-size BGRA masks and
Qt freehand opacity delivery now distinguish the unresolved source behaviors.
Node.paintLine accumulated opacity and ignored flow in both serialized modes,
so that API alone cannot establish stroke-level wash semantics. No fidelity
threshold was weakened. The observed zero-flow API expectation was corrected.

The new experimental `krita-5.3.4.1-default-circle-candidate-1` implements equal
horizontal/vertical default-circle fade with antialiasing disabled, fixed integral
size, unit aspect and zero rotation. Full captured 32-pixel alpha masks at hardness
0, .25, .5, .75 and 1 exactly match a squared-distance fade with a one-pixel
minimum inner radius. Four captured point regressions per soft setting and twelve
7/64/128-pixel source gates qualify the implementation beyond those original
masks. This implementation is pending CI; AA, unequal fades, soft ellipses and
soft pressure mappings are explicitly rejected by candidate eligibility. Native
conversion remains available with honest provenance. Source verification flags
remain false. No engine source was copied or consulted to derive the fade rule.

The next implementation slice adds explicit, portable source-wet alpha and
Smudge Length switches. Acquired three-dab control fixtures distinguish displayed
paint from source-over feedback when alpha transport is disabled. New Dulling
also applies foreground color before pickup in that state. Disabling Smudge
Length selects full pickup strength while legacy color attenuation still uses
the serialized rate. The 48 individual-control cases now enforce source gates;
24 combined-control cases independently qualify their interaction. Native wet
paint retains its existing equations. These changes await exact-head CI.

Exact odd-size Smear masks establish 3/5/7/7/7/5/3 coverage rows for a 7px tip,
versus Dulling's distinct lattice. The runtime now implements that odd-size
placement with captured footprint regressions. The ellipse reference harness
had reset source rotation to zero after selecting a rotated fixture; it now
applies and verifies the requested rotation rather than compare unlike inputs.

Actual-application qualification caught a PNG round-trip mismatch. The 8-bit
PNG writer demonstrably truncated straight-alpha conversion: channel 130 at
alpha 200 reopened as 129. Its conversion now rounds, and an exhaustive
alpha/channel fixture exercises two real PNG save/reopen cycles. Wet-paint
persistence equality remains exact, with first-pixel and dimension diagnostics
retained to expose any additional cause. The PNG edit is limited to 8-bit
straight-alpha encoding; it does not modify TLV, 16-bit conversion, or shared
pixel utility semantics. Full application and cross-platform CI are pending.

### Default-circle AA continuation

Independent numerical analysis of the complete captured alpha arrays recovered
the pixel-axis AA rule: with pixel distances x/y, squared radius q=x²+y² and
qplus=(|x|+1)²+(|y|+1)², replace the squared inner fade by
max(hardness,1/radius)²*q/qplus before evaluating the squared-distance ramp.
Nearest-byte quantization reproduces every pixel in all five 32px AA hardness
fixtures and the 32/64/128px hard AA fixtures exactly. The implementation uses
this direct expression rather than native supersampling. Source presets require
unit aspect, zero rotation, fixed integral size and equal fades; AA sizes below
16 remain explicitly ineligible pending their distinct subpixel sampling rule.
A 14-size acquisition matrix tests that boundary, existing geometry gates retain
exact-error requirements, and a ten-case CPU suite now includes this renderer.
Large soft-mask diagnostics use a bounded edge window to stay within annotation
limits; all fidelity comparisons still use full rasters. This continuation is
pending publication and exact-head CI, not a reference-verified claim.

A separate `krita-5.3.4.1-default-rect-candidate-1` now implements the measured
fixed default-rectangle mask. Non-AA coverage uses the minimum linear axis fade,
including the source's opaque exact-axis behavior and inclusive hard boundary.
AA uses independent pixel-axis fade factors and removes that exact-axis special
case. Complete 32px masks at five fade settings differ by at most one alpha byte;
most are exact. Captured point regressions and twelve additional size/fade gates
qualify the renderer. Fixed unit aspect/zero rotation and the same small-AA
exclusion remain explicit. These are source-candidate profiles, not silent
changes to native square or round brushes.

### Correct serialized painting action

Public bundled preset metadata and original controlled freehand cases identify
`PaintOpAction`, not `PaintingMode`, as the operative setting. Action 0 has wash
semantics (quarter opacity stays alpha 64; quarter flow accumulates; zero flow
is empty). Action 1 builds opacity per dab and ignores static flow. PressureFlow
true/false is inactive with FlowUseCurve=false in these cases. Action 2 occurs in
bundled presets and is now measured separately; it remains ineligible until its
semantics are established. Strict import now requires the operative field and
rejects a misleading PaintingMode-only preset. The authored fixtures are corrected
to explicit actions. Candidate wash uses the pixel-centered footprint inferred
from its measured bounds, distinct from the build-up lattice; full pixel gates
will qualify that correction. Tablet diagnostics now follow their actual stroke
origin rather than capture an empty window near the API-line origin.

The 18 original ellipse masks at 16/64px identify positive KPP rotation as
counterclockwise in raster coordinates. Negating the imported angle yields
zero differing alpha pixels for all four formerly failing 45-degree ellipse
masks; the next source CI run qualifies this adapter change end to end. Native
angle semantics remain unchanged.

The four wet source candidates now evaluate explicit identity pressure curves
for smudge strength and color rate at each dab, before the model-specific
mixing equation. Import eligibility requires the active flag, an explicit
single pressure sensor, identity endpoints, and a successfully decoded mapping;
unknown/nonlinear/multiple sensors remain native conversions. Portable round-trip
checks preserve both controls. A 48-case source matrix covers each control and
both together at pressure 0/.25/.75/1 over three dabs. This new behavior is pending
reference qualification and does not change source-verified labels.

Nonzero Dulling radius is not inferred from the radius-zero equation. The
heterogeneous three-dab observations distinguish legacy and new sampling areas.
An original 48-case black/white column-impulse acquisition isolates the initial
pickup kernel at radius 0/50/100/300 and offsets 0/4/8/16/24/48, using an opaque
source and transparent second-dab destination. These are measurements, not
parity gates, until the independent kernel is established and implemented.

Linux worker isolation implementation now requires Landlock ABI 3 before
parsing and grants only read access to the selected input and read/write access
to the private result directory. PNG codecs are initialized before the boundary;
a syscall allowlist then denies networking, executable loading, process creation,
ptrace and cross-process memory access. Failure to establish the boundary fails
the import explicitly, rather than silently decoding with ambient privileges.
Original child-process tests attempt unrelated-file read/write, socket creation,
clone and exec, then verify the private output and unchanged unrelated file.
These checks and successful worker imports are pending CI. Other platforms still
need equivalent privilege isolation; CPU/time/allocation limits alone do not
satisfy it. Architecture references: [Landlock](https://docs.kernel.org/userspace-api/landlock.html)
and [seccomp](https://docs.kernel.org/userspace-api/seccomp_filter.html).

### Full-contract integration audit (implementation continues)

| Requirement | Current runtime/import path | Remaining acceptance work |
|---|---|---|
| Native combined resource/dynamics workflows | Embedded primary tip + texture + dry dual mask/wash/multiply, or native Dulling + pressure pickup; pressure/tilt/rotation controls | New real-application export/reload/install/paint/undo/redo/cancel/PNG tests pending CI; this proves native combinations only |
| Four source wet workflows | Strict KPP candidate selection, worker import, portable install, actual tool transactions; fixed controls passed | Pressure-controlled variants now added to the same application transaction suite; nonzero radius and broader geometry/dynamics/texture still need source implementation |
| K5341 Pixel masks and placement | Hard circle/ellipse, default circle and rectangle candidate equations, versioned import | Corrected rotation and AA size qualification; soft-size, rectangle, wash/flow and near-zero pressure failures remain under investigation |
| Source dynamics | Identity size pressure and wet strength/color pressure candidate mappings | Other sensors, declared cubic interpolation, combinations, scatter/spacing/color dynamics and timed airbrush need source-specific adapters/qualification |
| Source resources and blending | PNG/GBR/GIH decoding and native conversion; source strict candidates currently generated tips | Bitmap/color/GIH source sampling, texture/dual masks and required source blend combinations are not completed by native support |
| Modern Photoshop ABR | Bounded typed descriptor decoder, sampled IDs, native tip conversion | Bounded pattern-v1/VM-v3 raw8/raw16 and explicit VM ZIP1 decoding implemented; ABR 6/2 sampled-ID/static native association implemented; other source versions and dynamic behavioral adapters remain; Photoshop runtime reference access unavailable |
| Safe decoder boundary | Shared pre-allocation budgets, bounded package protocol, cancellation/time limits; Linux Landlock/seccomp implementation | Windows zero-capability AppContainer and signed macOS App Sandbox helper implemented; anchored no-follow response validation integrated; exact-head platform qualification pending |
| Tablet portability | Host pressure/tilt/rotation normalization and actual tool tests | Physical-device and platform input qualification still required; synthetic events do not prove hardware behavior |
| Persistence | Exact application undo/cancel and native/wet PNG round trips; scene/level save/reopen | New combined-presets and pressure-wet lifecycle tests pending CI, plus platform packaging qualification |

This audit preserves the full RCP1 target. A native conversion, isolated numerical
fixture, or green decoder job does not complete a missing foreign-profile row.

The source run at `8379dc99` passes full-image Pixel opacity/flow gates for
both API-line and synthetic-tablet delivery after sharing the measured pixel
lattice between wash and build-up. Expanded soft-circle diameters
7/8/9/12/15/16/17/24/64/128, default rectangle sizes, and the complete fade/AA
matrix pass. These results qualify the measured cases; broader source dynamics
and combinations remain unfinished. Six captured action-2 alpha windows equal
wash/action-0 exactly; the strict adapter now maps that action to the wash
candidate and subjects it to full comparison gates.

Wet-pressure acquisition passes all modern-model cases. Six legacy cases
exposed a distinct rule: pressure scales transport, while foreground attenuation
uses the serialized smudge-length value. The corrected equation matches all
captured legacy center samples within one byte; full-image requalification is
pending. The nonzero-radius impulse acquisition succeeded, but comparison
rejected its spacing-10 fixture outside the native IR range. Revised fixtures
use spacing 5 with a separated source region and denser impulse offsets; no
geometry validation limit is relaxed.

Linux confinement probes passed unrelated-file, socket, clone and exec denials
but failed private output publication through QSaveFile. Worker results now use
exclusive creation in the private staging directory. The parent only accepts
files after successful process exit and complete manifest validation; private
staging does not require atomic per-file publication. The sandbox additionally
tests that the selected input cannot be opened for writing or chmodded. Qt's
Linux unnamed-temporary behavior is documented in
[QTemporaryFile](https://doc.qt.io/qt-6.5/qtemporaryfile.html).

Instrumented child probes also produced invalid-vptr diagnostics when their
runtime could no longer inspect process memory under confinement. The decoder
suite remains ASan/UBSan-instrumented; it now invokes a separately built,
uninstrumented worker for production-boundary and process-protocol tests.
No denied operation is allowed for sanitizer convenience. The actual application
workflow independently exercises the shipped worker. Both this separation and
the private-output correction await CI.

Linux worker resource isolation additionally caps total virtual address space at
2 GiB (including loaded libraries and codec scratch), and each staging file at
256 MiB. These process limits are separate from the stricter shared 256 MiB
decoded-image allowance and cannot be raised after seccomp installation. A
child probe reserves the whole address allowance without touching pages and
requires ENOMEM. This is not an aggregate disk quota; successful protocol output
still has its own aggregate encoded-byte limit. New process-limit qualification
is pending CI.

Windows workers now establish an unnamed Job Object before decoding, limiting
committed memory to 2 GiB, user CPU time to 60 seconds, and active processes to
one, with no breakaway permission. The setup handle is closed after assignment;
the job persists for the associated process lifetime. Child probes require an
over-budget commit and process creation to fail, while ordinary worker imports
must still succeed. This is resource/process containment, not a filesystem or
network privilege sandbox; that remaining Windows requirement is unchanged.
Qualification is pending Windows CI. References: [job lifecycle](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects),
[extended limits](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information).

The editor's PNG resource picker now uses the same worker protocol as foreign
brush imports instead of decoding selected PNG files in the application process.
The parent receives a color-preserving embedded tip, then applies the explicitly
chosen alpha/luminance/color interpretation. It supplies the remaining decoded
image allowance before loading worker packages, including resources retained for
editor cancellation. Cancellation installs nothing. Original partial-alpha PNG
worker tests cover color preservation and rejection when the remaining allowance
is too small. This editor-path correction awaits full application CI.

Legacy Dulling now has an independent nonzero-radius candidate for fixed hard
round tips at 0–100% radius. Dense original column impulses at 50% and 100%
identify an 80-sample base-2 sequence with nearest-pixel lookup exactly across
both observed diameters of the sampling region. The implementation pairs that
axis with a base-3 sequence; independent row impulses are mandatory new gates,
not presumed evidence. It samples premultiplied colors before deposition and
retains unpainted corner pixels for subsequent pickup. Existing heterogeneous
third-dab cases now gate this radius path as well. Portable import binds the
explicit radius percent to the candidate; radius above 100% and new-model
nonzero radius remain conversions pending their separate implementation. This
expands the candidate region without changing the full RCP1 target or marking
it source-verified. Captured column regressions and full-image CI qualify it.

Worker return protocol 2 removes the image-codec boundary from the parent
process: private record metadata references SHA-256-addressed straight RGBA8
sidecars, each with a fixed two-word dimension header. The parent checks file
type, dimensions, exact byte length, checksum, and remaining decoded allowance
before allocation. It never dispatches a PNG or other foreign codec for worker
results. Native portable presets retain their existing embedded PNG format;
they are decoded inside the import worker. Roundtrip tests cover shared
resources and partial alpha; hostile cases cover codec substitution, malformed
headers, truncation, checksums, symlinks, and one-byte-under-budget rejection.
The hostile multi-package memory qualification uses this same protocol. These
new protocol tests await exact-head CI; passing tests of the preceding head do
not qualify this change.

At the preceding head 5423354f, import fuzzing, Linux Qt 5, both Windows parser
configurations (including process/memory Job Object probes), and both macOS
parser configurations passed. Linux Qt 6 and full application/source suites
were still pending at this checkpoint. macOS parser success does not establish
a privilege sandbox, and Windows Job Object success establishes resource limits,
not filesystem/network privilege isolation.

The independent row gates at 5423354f caught six legacy radius mismatches.
Their cause is the sampling rectangle: the base-3 vertical sequence spans
from the top edge to one pixel before the bottom edge. Updating that range
matches every captured legacy row at 50/100 percent. The new Dulling sampling
model uses the same sequence, caps its extent at one tip radius, and rejects
samples outside the hard tip's pixel footprint before averaging. Independent
calculation matches all acquired new-model row and column impulse values at
50/100/300 percent, including the unchanged 0.8 transport attenuation. The
runtime and strict KPP adapter now implement this candidate, with 59 captured
orthogonal impulse regressions and mandatory full-image/third-dab gates.
Those new runtime gates are pending CI; this is not a promotion to verified.
Legacy radius above 100 percent still needs its distinct large-area sampling
rule, and the full dynamics/geometry/texture contract remains unchanged.

The installed-application workload additionally combines a 100 percent Dulling
radius with pressure-driven pickup and color rate (and verifies that Smear keeps
its serialized radius inactive). Each variant imports through the worker,
installs a portable preset, paints through the actual tool, and checks exact
undo/redo, cancellation, state reset and PNG reopening. A separate source scale
matrix varies diameter and radius with orthogonal impulses to distinguish the
legacy large-area sampling rule from the new model's tip-limited rule. Existing
candidate regions retain mandatory comparison gates; measurements outside them
remain exploratory until the corresponding runtime is implemented.

The instrumented mutation corpus now includes both worker metadata and raw RGBA8
sidecars, alongside all six existing foreign/native input formats. Worker fuzz
cases use the exact parent decoder with a 1 MiB allocation allowance; malformed
headers, body lengths and mutated resource graphs cannot bypass that boundary.
Original valid wire records and an oversized-header seed exercise both accepted
and rejected paths. This supplements the protocol unit and peak-memory gates.

At 99ca200c, all 215 enforced full-image kernel gates passed (266 acquired cases;
the remaining 51 legacy large-radius cases are exploratory, not passing parity).
All six OS/Qt parser suites and import fuzzing also passed, including the private
raw return protocol and 59 captured orthogonal impulse regressions. This confirms
the measured radius implementation for those fixtures without claiming general
source compatibility or completion of the full RCP1 contract. The next head adds
combined radius/pressure application transactions, broader scale acquisition and
worker-response fuzzing; those additions require their own exact-head CI.

All four source wet adapters now preserve static FlowValue in 0–1 with
FlowUseCurve disabled, instead of rejecting values other than one. Acquired
long-stroke source probes show identical centerline outputs for unit and half
flow in each model; the runtime intentionally leaves that inactive setting out
of the transport equations. A new 64-case short-stroke transfer matrix enforces
32 static-flow candidate gates and separately acquires 32 non-unit-opacity
cases. Opacity remains unimplemented in these candidates until its distinct
transport/deposit equation is measured and implemented; static-flow acceptance
does not silently approximate opacity or enabled flow dynamics.

Independent review identified two requirements now implemented together but
awaiting exact-head qualification. Every worker response uses an anchored,
no-follow regular-file reader, including manifest and diagnostic files; fake
FIFO/link/oversized and directory-swap responses have regression coverage.
Portable export now offers manifest-only (default for unknown or restricted
resource declarations), permitted-resource packaging and explicit private-copy
packaging, with canonical pixel-hash relink/repair before installation. See
`raster_brush_portability.md` and `raster_brush_platform_isolation.md`. Code and
fixtures alone do not complete their acceptance rows. Neither workflow implies
that private export is unlawful or independent implementation needs blanket
permission.

Pixel size candidates now retain explicit two-endpoint pressure curves with
arbitrary bounded endpoint values, including a nonzero low-pressure diameter.
Shared and sensor-local curve serialization are resolved independently; an
inactive shared curve cannot replace an explicitly selected sensor-local curve.
This is affine interpolation only. Multi-knot source splines remain native
conversions, and source wet pickup/color eligibility remains identity-only.
Portable roundtrip and nonzero-minimum regressions accompany twelve original
source endpoint/serialization gates. They require exact-head CI before any
reference qualification is claimed.

Twenty-four focused Pixel precision cases separate fractional-pressure mask
geometry and fractional document placement from the stroke scheduler, and use
short low-spacing half-opacity strokes to measure a possible spacing floor.
Existing ramp captures alone do not distinguish a minimum diameter from a
minimum spacing at spacing 1. No speculative clamp or fixture-specific geometry
correction is introduced; the full-image gates remain unchanged.
### Native wet transport and analytical drift qualification

Native Smear now applies the native reservoir recurrence independently to each
transported field cell: pickup once, foreground mixing once, then reload after
deposition. Earlier native output mixed the global reservoir with raw previous
canvas samples a second time and did not reload the transported field. This is
an intentional native-output correction; the four experimental source profiles
retain their separate equations. Retained cells are bilinearly resampled in the
normalized tip frame when the grid size changes. Dulling retains its uniform
weighted reservoir and no longer allocates an unused transported field.

The native analytical regression follows 10,000 single-pixel dabs across revisited
color strips and alpha ramps, for both wet modes and reload 0, 0.125 and 1. It
compares raster output against an independent affine recurrence, and retained
state against a closed-form geometric pickup-history sum at dabs 1, 10, 100,
1,000 and 10,000. Component errors must remain within 1e-6 and encoded byte errors
within one code value. It also checks fresh-stroke initialization, finished-state
inaccessibility and memory release on finish or active-stroke destruction. The
existing application transaction tests cover cancellation rollback and undo.
These assertions require CI qualification; they do not establish foreign-source
parity or replace the separate source-output drift gates.


### Native independently spaced secondary mask, version 1

`groundzero-native-1` now has an opt-in `secondary.independentSpacing` setting
(default false) and `secondary.spacing` (default 0.1; range 0.01–5). Existing
packages and synchronized-per-primary masks retain their previous behavior.
This is an independently defined native capability, not an ABR/KPP adapter
mapping or a Photoshop/Krita compatibility claim.

With independent spacing enabled, secondary stamps start at the stroke origin
and follow the underlying sampled path with their own accumulated arc length.
Spacing is the evaluated secondary diameter times the spacing setting. Diameter
is primary evaluated diameter times secondary size, with the native 0.25-pixel
minimum; maximum configured diameter is 4096 pixels. Primary aspect/rotation/
flips and secondary aspect/rotation/flips transform the mask at each secondary
event. Pressure and other dynamic sensors are interpolated at that event;
RandomDab and DabIndex use the secondary sequence, while RandomStroke shares
the stroke seed. A separate seeded random stream selects bitmap frames and
scatter, so changing the secondary cadence does not consume primary random
numbers. Count stamps share the event's dynamic values and bitmap frame;
each scatter offset spans ±scatter times the secondary diameter in canvas x/y.

Secondary coverage, including opacity, accumulates by alpha-over into a sparse
canvas-aligned mask for the entire stroke. At a coincident event the secondary
stamp runs first. Each primary dab samples the mask accumulated *so far*, after
texture and before selection/ink composition, applying Multiply or Subtract
once. There is no lookahead or repaint of old primary dabs when the mask later
grows. Stationary airbrush events emit primary dabs only. These choices make
causality, corners, revisited positions and event batching explicit.

Each 32×32 double-coverage tile is charged to the shared stroke memory budget
before allocation, including a conservative node allowance. Finish and
destruction release all mask state; a new stroke starts empty. Masks never
read or modify the host raster directly: ordinary primary transactions govern
undo/redo and cancel. Allocation/work failure returns an explicit stroke error
so the host can roll back the whole transaction, never silently omit stamps.
Optional fields survive portable export/reopen; old packages use synchronized
mode. Source-profile eligibility does not acquire this native scheduling rule.

Native primary spacing now honors diameter × spacing without the previous
absolute 0.01-pixel floor (0.25 × 0.01 legitimately yields 0.0025 pixels).
Measured source-profile floors remain unchanged. Every add is limited to 65,536
scheduler events; independent mode counts both sequences against that limit.
Native deposition preflights at most 256 million estimated tip samples per
input event, including secondary count, bitmap bilinear taps, procedural 8×8
filtering, texture taps and native wet pickup. Independent mask batches
preflight conservative scattered bounds before sampling or allocating. These
are explicit resource-failure limits, not promises of interactive speed at
maximum diameter/count; extreme filtered combinations may require smaller
settings. Event count, memory and work limits are separate protections.

CI assertions cover origin/tie ordering, gaps distinct from primary spacing,
Multiply/Subtract, count/opacity union, stationary airbrush behavior, pressure/
bitmap/random dynamics and event partitioning, deterministic fresh-stroke reset,
bitmap flips, shared-budget cleanup/denial, the native subpixel spacing ratio,
and extreme-setting failure before raster access. Portable fields/editor
integration and host rollback are qualified by their corresponding application
suites; no foreign dual-brush fidelity is inferred from these native tests.

### Integrated native workflow and decoder-boundary qualification

The current implementation combines native per-cell smear transport, independent
secondary-mask scheduling, portable resource declarations and hash repair, and
strict worker response validation in the application workflow. Independent
secondary spacing remains a native rule, not an inferred Photoshop dual-brush
mapping. Original fixtures include long-run native wet state, hostile protocol
files, damaged embedded resources, and shared resource/work limits.

The source-transfer acquisition now separates 128 static/disabled/dynamic control
cases across four wet models. Pixel acquisitions separately exercise fractional
geometry and affine pressure endpoints. New acquisitions run first in the CI
matrix; all existing full-image thresholds remain enforced. Complete job logs
retain the full bounded probe batches; the truncated annotation summary alone
is not sufficient evidence of complete acquisition.

Full RCP1 remains incomplete: broad source dynamics, texture/dual-tip and bitmap
sampling combinations, modern ABR behavioral binding, and physical-device input
qualification are not established by these native improvements. Windows/macOS
isolation, packaging, and the combined application lifecycle require passing
checks at the integrated commit. Do not promote the draft or any preset to a
reference-verified status merely because its parser or native regression passes.

### Source static opacity transport

Original K5341 two/three-dab captures at opacity 0, 0.25, 0.5, 0.75 and 1
distinguish opacity-scaled state transport from fading only the final dab. With
opacity O, the candidate transport strength is O times the existing model
strength. Legacy foreground injection is O times color rate times the existing
serialized-length attenuation; modern injection is O times squared color rate.
The resulting quantized paint feeds the next dab. Static flow remains inactive
when its source curve is disabled. These are source-profile equations; native
opacity multiplication remains separate.

An independent numerical evaluation matches all 40 captured baseline/opacity
second/third-dab center samples within one encoded channel value (39 exactly).
The runtime regression records the original opacity samples; importer and
portable round-trip tests retain fractional opacity and candidate identity.
The source comparison now enforces the existing image thresholds on all 64
original static-opacity/static-flow cases. CI must qualify that implementation
and complete image outputs. Opacity curves and combinations with alpha transport
disabled, length disabled or pressure controls require separate source evidence;
these measurements alone do not verify those combinations or full preset parity.

The dedicated `wetopacitycombo` group adds 88 enforced source-image comparisons:
half opacity with alpha transport off, length off, both off, two alternative
pickup/color pairs, and pickup/color/both pressure controls at 0.25 and 0.75.
Every combination runs two and three dabs for each of the four source models.
These gates test the general candidate recurrence and must fail on a mismatch;
they are not exploratory exceptions and do not set source-verified provenance.
The default-circle candidate now includes hard antialiased integer diameters
1–9 with fixed size, aspect one and angle zero. Independent analysis of the
complete original D1/2/3/4/7/8/9 masks from the
[reference acquisition](https://github.com/OpenAnimationLibrary/groundzero/actions/runs/41083922429/job/124159970622)
recovered a generic forward 3×3 grid at offsets 0, 1/3 and 2/3 on both axes.
Each subsample uses the existing measured squared-default AA equation, rounded
up to an 8-bit coverage; the nine byte coverages are averaged and rounded up
again. The generated integer-diameter stamp bounds clip the result. This
explains the small-mask directional offset without fitted per-size constants or
lookup tables. Independent numerical analysis matches every captured pixel;
engine/import regression assertions retain the complete original masks.

The same source comparison group now enforces full-image gates for this range,
including new held-out D5/6 captures. Those gates still require execution on the
implementation head; imported presets remain source-reference-unverified.
Dynamic/fractional diameters, soft small masks and small rectangles are not
admitted by this evidence. Diameters 10–15 retain their previous unsupported AA
status: D10/12 captures show a distinct non-supersampled regime with one-byte
asymmetries that the small-mask equation does not explain. No threshold was
relaxed and no foreign source implementation was copied.

### GIH required selection lifecycle qualification

The normative GIH scope remains one-dimensional incremental/random selection;
pressure, angular, velocity, tilt and multidimensional selectors remain excluded.
The [GIMP format description](https://developer.gimp.org/core/standards/gih/)
defines a textual parameter header followed by concatenated GBR records. The
existing versioned native adapter retains the selection flag and frame order,
rejects unsupported selectors or incomplete rank/count declarations, and does
not mark source semantics or reference appearance as verified.

Original distinct red/green/blue GIH frames now qualify import, portable reopen
and actual native raster sampling together. Assertions cover incremental wrap,
stroke restart, event partition, seeded random replay and seed distinction, plus
malformed rank/count, duplicate fields, nonfinite spacing and conflicting
selectors. This closes the earlier identical-frame test's inability to detect
a selection regression; CI qualification remains required. These are native
sequence checks, not evidence of Krita sequence parity.

### Dulling sample-count scale correction

The larger-scale acquisition exposed two existing candidate failures at diameter
64 and radius 100 percent: the fixed 80-point kernel missed the top impulse row.
The candidate now uses an area-scaled count, max(80, floor(0.02 × W × H + 20)),
where W is twice the measured pickup extent and H is W minus one. This produces
100 samples at the newly measured diameter instead of 80; accepted new-model
samples are still clipped to the tip before normalization. Captured regression
values now include all 56 diameter-16/64, radius-100 orthogonal impulses, in
addition to the earlier 59 diameter-32 captures.

This is a falsifiable interpolation consistent with the acquired scale data,
not a claim that sparse captures uniquely identify the source formula. Dedicated
count-breakpoint and fractional-bound probes must qualify it. Legacy radius
above 100 percent remains ineligible: its larger horizontal sampling bounds are
not yet explained, and no radius-specific special case was introduced. Full-image
thresholds and source-unverified labels are unchanged.
### Source brush texture acquisition boundary

The older `wettexture` and `wettexture3` fixtures paint onto patterned canvas
while `Texture/Pattern/Enabled=false`; they qualify pickup kernels, not brush
texture masks. The separate `texturemasks` acquisition uses an original embedded
16×16 opaque four-level grayscale PNG. Its 90 cases cover Multiply/Subtract,
soft texturing on/off, inversion, strength 0.5/1 and half-opacity ordering, across
Pixel and all four Color Smudge models. It records exact effective fields, checks
the resolved original pattern and captures compact RGBA regions of the output.

The [source manual](https://docs.krita.org/en/reference_manual/brushes/brush_settings/texture.html)
distinguishes alpha texturing from color-map modes and documents soft texturing
as a strength-dependent behavior introduced in 5.3. Serialized field names and
mode identifiers were checked against public
[option metadata](https://raw.githubusercontent.com/KDE/krita/master/plugins/paintops/libpaintop/KisTextureOptionData.h),
[resource metadata](https://raw.githubusercontent.com/KDE/krita/master/plugins/paintops/libpaintop/KisEmbeddedTextureData.cpp)
and strength option metadata; rendering code was not reused. Effective readback
in the pinned source application remains mandatory because metadata inspection
alone is not behavioral evidence.

Original captures now establish a strict candidate subset: hard round Pixel
build-up and legacy Smear/Dulling plus new Smear, Normal composition, opaque
grayscale PNG, fixed unit scale and zero offsets/rotation, no cutoff or brightness
adjustments. Wet candidates require alpha transport and zero pickup radius.
Bundle dependencies retain their declared manifest resource kind: texture lookup
searches only `patterns`, and tip lookup only `brushes`. Filename and checksum
matches cannot cross these namespaces; multiple matching resources within the
requested kind fail explicitly. This follows the pattern-specific resource
registry in the linked public resource metadata, rather than inferring type from
a file extension or directory name. Original fixtures cover wrong-kind matches,
same-hash cross-kind resources and ambiguous pattern matches.

The importer resolves embedded or uniquely identified bundled PNGs, checks declared
checksums and remaining decoded allocation budget, and preserves grayscale mask
values, soft-texturing state and resource SHA-256 through portable presets.
Multiply/Subtract use independently derived byte-domain alpha equations; soft
texturing attenuates removed coverage, while legacy strength attenuates incoming
coverage. The three admitted wet models carry the masked displayed result into
the next dab. Analysis of 378 original wet centerline samples differs by at most
one encoded RGBA byte. Runtime regression fixtures cover polarity, strength and
third-dab feedback; the reference comparison enforces existing full-region gates
for all 72 admitted texture cases, rather than counting acquisition as parity.

New Dulling texture remains unimplemented: its retained state differs from these
three models. The separate `texturestate` acquisition distinguishes phase and
second/third-dab state before extending eligibility. Transformed/color/alpha
patterns, texture dynamics and other source texture combinations remain outside
this measured subset and are still outstanding against the full feature contract.
All profiles remain candidate/unverified until their complete qualification gates
pass; no generic source-texture or pack parity is claimed.

### Pixel blend candidates and reference gate

Strict K5341 Pixel candidates now admit explicit Normal, Multiply, Screen and
Overlay settings on the existing generated-tip geometry subsets. Color Smudge
candidates still require Normal. The source-Pixel deposition path has its own
blend boundary, so subsequent source arithmetic corrections do not change
native painting or wet behavior. These imports remain candidates with
`sourceReferenceVerified=false`; implementing an equation does not establish
foreign-engine parity.

The initial candidate evaluates separable blending in straight encoded-sRGB,
then combines premultiplied source/backdrop contributions using source-over.
Multiply uses the channel product, Screen the complement product's complement,
and Overlay switches at backdrop 0.5. The mathematical baseline comes from
[W3C Compositing and Blending](https://www.w3.org/TR/compositing-1/#blending),
with Krita's [Multiply](https://docs.krita.org/en/reference_manual/blending_modes/arithmetic.html#multiply)
and [Screen](https://docs.krita.org/en/reference_manual/blending_modes/lighten.html#screen)
descriptions. Krita's [Overlay prose](https://docs.krita.org/en/reference_manual/blending_modes/mix.html#overlay)
mentions the upper color for its threshold, unlike the standard backdrop rule;
this is an explicit evidence ambiguity for the source fixtures to resolve, not
an assumption of agreement. No source paint/composite implementation was copied.

The `pixelblend` acquisition contains 128 original cases: 80 hard-circle cases
cover four blend modes, wash/build-up, single/overlapping dabs, independent
opacity/flow settings and zero cases; 48 additional soft-circle, AA-circle and
rectangle cases cover full and combined reduced opacity/flow. Eight alternating
non-primary RGB swatches span opaque, partial and zero alpha and both sides of
the Overlay threshold. Each source run verifies both the view's blend mode and
the effective preset's CompositeOp. Candidate import must select the strict
profile; the existing full-image RGBA/coverage/bounds gates are enforced for
every case, including no-op cases. No threshold is relaxed for blending.

Analytic regression assertions distinguish the blend channels, partial-alpha
composition, source wash's original-backdrop accumulation, inactive static flow
in build-up, zero-flow wash, and wet-profile rejection. They are mathematical
and lifecycle checks, not reference evidence. Source byte rounding, wash alpha
accumulation and mode semantics remain pending the new source comparison job.
P2711 compatibility still requires independently available authorized reference
evidence; these Pixel candidates do not establish Photoshop blend parity.

### Source wet transfer control qualification

The expanded original transfer acquisition establishes three separate behaviors:
all 32 enabled/disabled identity-flow-curve cases exactly match the corresponding
static baseline; all 16 identity-pressure-opacity cases exactly match the static
opacity 0.25/0.75 captures; and all eight pairs of disabled-color cases are
identical despite serialized color rates 0.25 versus 1. The comparison uses the
captured second/third-dab RGBA samples, independently of the candidate runtime.

The wet candidate now evaluates identity pressure opacity in the established
transport/injection recurrence. A separate portable `sourceColorRateEnabled`
flag suppresses foreground injection without erasing the retained color-rate
setting. Identity pressure Flow mappings are retained but inactive in this
source model; provenance also preserves the inactive curve identity and pressure
checkbox state. Native equations and dry Pixel eligibility are unchanged.

All 128 transfer cases now require candidate selection and existing source-image
thresholds. Original captured pixel regressions and import/portable tests cover
these controls, with rejection checks for unmeasured opacity curves or Flow
sensors. Source-verified provenance remains false and implementation qualification
awaits CI; constant-pressure measurements do not by themselves establish
varying-pressure long-stroke source parity.

### Continuous off-document wet-state qualification

Empty source-dab clips currently advance the previous dab position and count but
retain the earlier field at its old absolute bounds, without pickup/injection.
Reentry lookup therefore commonly returns transparent feedback; this is a
potential lost-feedback boundary behavior, not proof that stale ink is replayed.
The source application's off-document layer storage and transport must be
measured before changing this recurrence.

The `wetboundary` group contains 96 original cases: four models, four document
edges, entry/exit/reentry paths and color rate 0/0.25. Each uses one continuous
Qt tablet press/move/release stream, 8-pixel event steps, 240 Hz timestamps and
32-pixel tips. The public Canvas preferred-center API positions the document
edge inside the visible widget at 100% zoom; out-of-widget input is rejected,
wraparound and instant preview are disabled. Separate line calls are never
represented as one stroke. Exact-repeat checks reset the full raster and a
bounded 128-pixel outside margin between independent strokes.

Whole-canvas candidate image gates remain enforced. Compact inside-edge and
outside-layer RGBA probes distinguish visible clipping from persistent source
paint beyond the document, without claiming access to hidden reservoirs.
Boundary recurrence changes remain pending these source measurements; no
source behavior is inferred from a native-only boundary test.

### Pixel pressure-transfer evidence acquisition

The `pixeltransfer` workload adds 28 original Qt tablet traces: Normal wash and
build-up, pressure-driven opacity and flow, constant pressure 0.25/0.75 with
paired static-value and disabled-pressure controls, plus four pressure ramps.
All use a 32px hard default circle at fixed 0.25 spacing and static control 0.5.
Each enabled identity curve has explicit sensor and shared-curve metadata;
the source must retain the authored settings in its effective preset. Reports
retain every delivered event and effective transfer field. Static controls use
0.5 times pressure to distinguish multiplication from source-specific transfer
or stroke-opacity state. Disabled-pressure controls retain the serialized curve
to distinguish its enable flag from mere curve presence.

This is acquisition, not completion of the K pressure-opacity/flow contract.
The strict Pixel candidate still accepts pressure-size affine curves only;
active opacity/flow curves stay native conversion until these observations
establish their equations and corresponding full-image gates are implemented.
The existing static Pixel gates are unchanged. The older `pixelcurves` evidence
contains 12 cases in three report forms (36 records), not 36 distinct curves;
it establishes two-point pressure-size curves, not nonlinear or transfer curves.

### Multi-knot source curve boundary

The native `NaturalCubic` evaluator solves a natural-boundary spline and clamps
its response; KPP conversion currently produces explicitly diagnosed linear
native mappings. Strict source Pixel admits only two endpoint pressure-size
knots. Neither the native spline implementation nor a serialized `curveMode=0`
establishes the source's interpolation: curveMode describes sensor combination.
The [public sensor-curve documentation](https://docs.krita.org/en/user_manual/loading_saving_brushes.html#using-sensor-curves)
establishes shared versus separate sensor curves and multiplication, but does
not specify spline boundary conditions or sampled response-table resolution.

`pixelcurveknots` acquires 36 original 64px hard-circle cases, each on the fixed
integer path (256,1024) to (257,1024). Two four-knot curves have nonuniform knot
spacing: monotone `(0,.125),(.25,.25),(.75,.75),(1,.875)` and turning
`(0,.25),(.25,.875),(.75,.125),(1,.75)`. Each is sampled at pressure
0,.125,.25,.5,.75,.875,1 in shared and sensor-local mode. The unselected curve
is deliberately different, testing selection rather than two identical copies.
Eight controls separately disable PressureSize or SizeUseCurve at pressure .5.
Reports retain effective curve metadata, exact API endpoints/pressures and full
alpha windows. These are acquisition fixtures, not source-support gates; no
multi-knot candidate is enabled until interpolation and its boundaries are
independently established and checked against held-out observations.

### Default hard-circle AA transition candidate

Independent complete alpha captures at diameters 10 and 12 identify the
single-sample squared-distance AA regime with a row-local quantization tail:
full eight-pixel blocks round to nearest byte, while the remaining pixels
truncate. The stamp's left edge anchors the blocks. This general rule exactly
matches both captured masks (zero differing pixels), and the existing formula
remains exact for captured 16/24/32/64/128 masks. It replaces neither the
forward 3×3 regime below diameter 10 nor native AA.

The candidate now admits fixed integer hard round AA tips in the 10–15 range.
Full captured D10/D12 masks are engine regression fixtures; D11/13/14/15 are
held out for threshold-enforcing source comparison. Fractional/dynamic sizes,
small soft masks, and small AA rectangles remain outside this eligibility.
This identifies behavior of the pinned reference build; it does not establish
all source CPU implementations use identical rounding. Source verification
flags remain false until the complete required qualification passes.

### Measured multi-knot pressure-size candidate

The first 36 `pixelcurveknots` source cases completed. Independent mathematical
analysis matched every captured alpha window exactly: 28 active shared/local
cases with natural-boundary cubic interpolation, and eight disabled controls
with the full fixed diameter. Intermediate responses distinguish linear from
cubic interpolation: the turning curve produces 0.65625 at pressure 0.125 and
0.34375 at 0.875; the monotone curve produces 0.17578125 and 0.82421875.

The strict hard Pixel candidate now selects `NaturalCubic` for a single
multi-knot pressure-size mapping with endpoints at 0 and 1. Existing validated
knot count, separation, finite-range and sensor constraints still apply.
Two-point curves remain affine; wet eligibility remains identity-only. Native
conversion remains explicitly linear unless the strict source candidate passes
all eligibility checks. Portable presets retain the selected interpolation.
Fourteen complete original mask fingerprints independently regress the active
runtime, while source CI enforces the existing full-image gates. New held-out
nonuniform and overshooting curves exercise additional pressure positions,
output clamping, shared/local selection and disabled settings. Those cases
require CI qualification; source verification flags remain false, and this is
not completion of the full K dynamics contract.
