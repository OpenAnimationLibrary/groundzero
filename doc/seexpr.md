# Native SeExpr integration

SeExpr adds procedural image effects and a separate animation expression mode to OT. The native implementation includes an editor, named animated controls, curves, color ramps, swatches, spectrum authoring, presets, range graphs and developer SDKs. Source-backed implementation and acceptance status are recorded in [the parity checklist](seexpr_parity.md). “Implemented” in that checklist means source and acceptance fixtures exist; actual passing evidence comes from the named CI jobs/reports at the final revision. Runtime results apply to the platform, backend and Qt version actually tested; this guide does not claim manual hardware qualification.

## Creating an effect

Add **Expression Generator** to create an image, or **Expression Color** to process the connected Source. Open its expression document in the native Formula Editor. Edit the RGB formula and separate scalar Alpha formula, select controls or a preset, validate the draft, then Apply. Typing changes the draft; Apply commits one native undo transaction and Revert restores the committed document. Invalid drafts preserve the saved source and do not replace a valid formula.

Generator has a finite canvas, initially 1920 × 1080. Color uses the Source bounds and accepts an optional Mask. Mix controls the blend with the original image; Mask can use alpha or luminance. A missing Mask means full influence. A Color effect with no Source has no source image to process.

These FX work on full-color raster, Toonz raster and vector columns through the normal renderer. They change rendered pixels and preserve the original drawing. SeExpr itself supplies no OT vector topology, region or palette editing API; editable vector geometry/style tools would be an additional host extension.

Use **Expression Surface** for procedural GLB/Image Plane materials and displacement, and **Expression Attribute** for render-local named values. Their geometry, opacity and attribute workflow is described below.

## Formula language and context

Formulas support numeric values, vectors, strings, assignments, conditionals, typed or inferred nonrecursive local functions, external function prototypes and fixed/variadic calls. The final expression supplies the result. Native numeric inputs/results support dimensions 1 through 16; image RGB and vector controls use three components. Both `$name` and `name` are valid variable forms. Missing bindings and incorrect argument/result types produce diagnostics.

The actual core registry contains 99 builtin names, including noise, curves, color math, vector math and string formatting. The native Functions browser reads that registry and adds real host callback help. Radian functions such as `sin` and `cos` retain SeExpr semantics; use `sind`, `cosd`, `deg` or `rad` when the angle convention needs to be explicit. General user loops and recursive local functions are not part of the audited upstream grammar.

The FX context is:

| Name | Meaning |
|---|---|
| `x`, `y` | Local effect coordinates at the pixel center, independent of tile size |
| `u`, `v` | Coordinates normalized to the finite effect canvas |
| `width`, `height` | Finite local canvas dimensions |
| `frame` | One-based current frame, preserving fractional samples |
| `time` | Seconds: the internal zero-based frame divided by Time Rate |
| `fps` | The effect's explicit Time Rate control, initially 24 |
| `seed` | Explicit effect seed; deterministic formulas must use it explicitly |
| `src` | Straight Source RGB vector; zero for Generator |
| `r`, `g`, `b`, `a` | Straight Source channels; zero for Generator |

FX Time Rate is an explicit effect parameter and does not automatically follow the scene frame rate. Native animation seconds use the live scene rate, as described below.

For example, a procedural RGB formula is:

```text
$scale = 8;
$amount = clamp(noise([$u * $scale, $v * $scale, $time]), 0, 1);
mix([0.1, 0.2, 0.4], [0.8, 0.9, 1], $amount)
```

Color can swap Source channels with `[$b,$g,$r]` and preserve Source opacity with Alpha `$a`.

## Controls, curves and presets

The Controls tab creates named scalar, vector, color, string, scalar-curve and color-curve controls. Each has a stable ID, a formula variable/function name and a display label. Changing a display label preserves its identity and animation. Native numeric/vector/color parameters, curve positions and curve values use OT keyframes. Integer controls round and respect their declared integer range. HDR and negative color values retain double precision in the document/native parameters.

String controls can explicitly select a File or Directory input. Browse opens the native chooser only on request; cancel preserves the literal. Selecting a path changes the draft and does not open or execute that path from formula source.

Removed controls are archived so restoring a compatible stable ID can recover its animation. Changing a variable name or type is a schema migration; stable IDs distinguish it from a newly created control. Scene loading establishes the schema before applying saved parameter values, including archived or reordered controls. Presets embed source, schema and defaults into the scene; rendering does not depend on the original preset file remaining present.

Scalar and color curve editors preserve all five upstream interpolation modes: constant, linear, smooth, spline and monotone spline. Curve controls are called as functions of their input. Swatches support add/remove/reorder/edit and preserve the upstream uniform selection bins, including endpoint behavior. Native swatch color selection supports cancel and retains exact numeric values outside a picker's display range.

**Import Annotations** converts supported literal assignments, numeric ranges, vectors/colors, strings, curves and swatches into native controls. It preserves comments and unsupported/computed expressions. Stable source/occurrence IDs keep repeated assignments and RGB/Alpha bindings separate. Import is an explicit draft operation; it does not silently rewrite arbitrary artist code.

**Water Spectrum** supplies the twelve upstream spectrum-authoring parameters and an exact energy-spectrum preview. The native spectrum curve uses the shared analytic math, rather than interpolating its small display graph. This corresponds to the upstream spectrum editor. The upstream core does not contain the host-specific `deepWater` fluid renderer; a spectrum control does not create a 3D water simulation.

The Presets tab browses installed and user JSON presets, filters by name, and supports bounded import/export. Unknown versions and malformed schemas report errors while preserving repairable source. Formula source, IDs and reference strings remain literal when the application language changes.

## Previewing a draft

Validate reports syntax, binding and type diagnostics at native source locations. The value preview evaluates the draft in its actual host context. It is a numeric sample, rather than a replacement for the full native image preview. Source-image callbacks require the native FX render context; apply the formula and use the normal FX preview for those images.

**Range Preview** freezes the current draft and samples explicit one-based frames, including fractional steps. It draws a scalar or multicomponent graph and lists values and errors per frame. A failed frame does not discard later successful samples. The range is limited to 512 samples and 16 numeric components; Cancel retains completed samples. Closing the owning editor stops sampling at the guarded event boundary. Preview does not mutate the scene, keyframes or undo history.

An FX range dialog clones the FX once and evaluates its named controls and RGB/Alpha at each requested frame. Animation range preview uses the native channel grammar, units and normalized segment time. The graph therefore uses host evaluation rather than inventing stand-in values for unknown variables.

## Source sampling and color policy

Expression Color exposes `sample_rgb(dx,dy)` and `sample_alpha(dx,dy)` for bounded sampling relative to the current local pixel. Sampling Radius declares the maximum offset. Requests outside it fail rather than silently request an unbounded image. Nearest and bilinear filters support transparent, clamp, repeat and mirror borders. Transform-aware input requests include the declared reach, keeping tiled and untiled results consistent.

Incoming rendered pixels are premultiplied. Formulas see straight Source RGB and alpha; zero-alpha Source becomes zero RGB. The RGB formula supplies straight output and Alpha is clamped to [0,1] before premultiplication. Mix and the selected Mask channel blend the processed and original premultiplied pixels. Luminance Mask uses its rendered premultiplied RGB.

Formulas operate in the renderer's working color space. Native color controls follow the render linear-color flag and gamma; negative float values preserve the native floating-raster convention. Float output preserves finite HDR/negative RGB within float range. Integer output clamps premultiplied RGB to the output alpha/range. NaN and infinity are errors, rather than successful black frames.

## Animation expressions

In the Function Editor, select **SeExpr** for the expression segment and use **Open Formula Editor** for the expanded workflow. Native mode retains existing OT interpretation, including degree-based native trigonometry. SeExpr uses its own explicit engine identity and radian math; selecting a new mode does not reinterpret every existing expression in a scene.

Animation bindings are:

| Name | Meaning |
|---|---|
| `frame`, `f` | One-based absolute frame |
| `rframe`, `r` | One-based relative frame |
| `t` | Normalized segment time |
| `time` | Seconds, `(frame-1)/fps` |
| `fps` | Live scene frame rate; a standalone grammar defaults to 24 |
| `pi` | π |

`channel("col1.x")` samples a native channel at the current frame. `channel("col1.x", $frame-1)` supplies an explicit sample frame. The first argument must be a literal reference string so native dependency tracking, change notifications and reference rewrites can see it. Computed reference strings fail validation. Units follow the native channel context, and saved expressions preserve fractional frames.

Native persistence uses versioned SeExpr/SeExpr-shape tags; the text adapter has the `seexpr1:` marker. Existing native tags remain readable. Invalid runtime samples may retain the last good interactive value with an error, but they increment the failure signal used by batch rendering. A failed committed expression must not become a successful render through that fallback.

Native reference monitoring follows column movement/insertion/deletion, Column and Stage Schematic clipboard duplication, and sub-xsheet collapse/explode. Clipboard stage/offset/plastic curves are snapshots, including when pasted before or between originals; copied expressions retain their new owners after the clipboard is cleared. Literal references are mapped once while unrelated strings and original curves remain unchanged. Empty/hidden numeric curves participate, and native undo/redo restores FX links as well as bindings. Intentionally retained links to foreign FX keep their original native source context and its lifetime. Pasting does not transfer ownership of those shared references to the destination monitor. Use an independent, unlinked FX copy to avoid sharing that source context.

Direct/mixed dependency cycles are rejected, and runtime cycle detection also protects malformed graphs. Animation evaluation currently acquires a graph-wide recursive gate before curve locks to prevent concurrent opposing-reference deadlocks. This path is serialized; separate FX render workers still use independently owned evaluators. No speed claim is made without representative qualification.

## Runtime, backends and SDKs

The application uses the interpreter by default. A Program rejects reentrant evaluation, mutation and movement while active; independently owned nested Programs remain supported and count against their callers' budgets. Prepared FX evaluators are owned by each worker; source/schema/backend or relevant control changes invalidate the prepared state. Editor preview state is separate from final rendering. Cache identities include exact source/schema/version data, full-precision control values, frame/time/rate, bounds/transforms, color policy and upstream content.

The optional LLVM14 backend builds the actual core JIT emitter. An unavailable requested backend reports an error instead of pretending the interpreter is JIT. LLVM SDK qualification is configured on Linux, Windows and Intel macOS; actual results must be checked at the implementation revision. This does not establish a shipped JIT runtime in the default application packages. Normal interpreter packages require no LLVM installation.

The optional [C++ SDK](seexpr_sdk.md) provides the pinned core headers/static library, typed callback example, installed CMake package, calculator, grapher and explicit native plugin loading. Plugins require a compatible compiler/runtime/architecture and the SDK registration ABI. Automatic environment plugin discovery stays disabled; evaluating a scene cannot select an executable module. Loaded callbacks and their modules have explicit lifetime/cleanup rules.

The optional [Python SDK](seexpr_python.md) supplies the upstream-compatible AST inspection/editing interface plus actual native numeric/vector/string evaluation, validation, catalog and typed Python callbacks. The independent `SeExprPy.editor`/`expreditor2` frontend provides Qt authoring widgets when PyQt5 or PyQt6 is installed separately. Python is not embedded into OT. Qt5 qualification does not certify Qt6 runtime behavior.

The native port supplies owned `animCurve` evaluation and authoring, render-local namespaced attributes/options, and triangle-mesh surface shading/displacement. These are native equivalents of the visible upstream host workflows; they do not establish external RenderMan or animlib binary compatibility. Demo-local `rand`, `map` and `triplanar` placeholders are not core functions or production texture engines. Example formulas requiring external host callbacks must bind those callbacks explicitly.

### Animation curves

Use **Animation Curve** in Controls to create a named function, or **Insert Animation Curve** / **Edit Inline Curve** for literal `animCurve` calls in any source tab. The editor preserves the lookup expression, comments and surrounding source. The curve stores key times/values, in/out weights and angles in degrees, independent tangent names and weight locks. It supports fixed, auto, linear, flat, step, stepnext and spline tangents, weighted or unweighted evaluation, and independent constant, linear, cycle, cycleRelative or oscillate behavior before/after the keys. Numeric source precision and unknown metadata survive untouched edits.

The artist editor supports up to 64 keys per curve; inline runtime/SDK data supports up to 512. Invalid, duplicated or unsorted times produce a diagnostic. Native stable key IDs preserve animation across reorder/rename, archive/restoration, clone/link, presets and scene persistence. Angles and values remain ordinary native numeric animation parameters.

**Refresh Linked Curve** copies the selected native channel into the draft. Apply commits that snapshot in one undoable edit; Cancel or a failed refresh preserves the previous data. Link text does not create a live dependency. Refresh copies constant, linear and weighted Bezier native segments with current units and native cycle behavior. Offset layers, stepped timing, other segment kinds and unqualified/nonlinear unit converters are rejected explicitly; use `channel("col1.x")` when the complete live native evaluation is needed.

### Surface shading and attributes

Connect **Expression Surface** to GLB Model or Image Plane, optionally through Transform 3D. Its native editor has RGB, Alpha, **Opacity RGB** and scalar **Displacement** sources. Position/normal bindings expose the selected Object, World or Camera space as `$P`/`$N`; `$Po`, `$Pw`, `$Pc`, `$No`, `$Nw`, `$Nc`, `$Cs`, `$Os`, `$u` and `$v` provide explicit geometry/material context. Displacement moves posed vertices along their object-space normals before projection; the renderer recomputes normals and shades covered samples using clipped, perspective-correct attributes. Existing native lighting effects remain available.

Opacity RGB defaults to `[1,1,1]` and multiplies scalar Alpha. Each component is clamped to zero through one; finite HDR/negative surface color remains available in floating output. The renderer tracks independent RGB transmittance between its 3D surfaces, multiplying each color component by its opacity before depth composition. Final OT output uses ordinary premultiplied RGBA with alpha equal to the maximum accumulated channel coverage. That single alpha cannot carry independent RGB transmittance into a later, unrelated 2D background; render the geometry/background together when that transmission matters. `$Os` contains material opacity repeated in three components. Set Alpha to `1` when an imported opacity formula already includes `$Os`. Older surface documents default to white Opacity RGB, and all sources participate in presets, persistence and cache identity. Image hosts reject surface presets instead of presenting inactive geometry sources.

Set **Displacement Limit** to the largest permitted distance in object units. Nonfinite output, excess displacement, missing geometry, cancellation and bounded geometry/coverage exhaustion report errors. One surface material is supported per geometry chain; multiple Expression Surface nodes produce an explicit error. Displacement uses existing triangle topology, without adaptive dicing. Image Plane displacement receives material color and does not read raster pixels during geometry/bounds queries; surface shading receives the actual straight texture sample.

Stack **Expression Attribute** providers to bind animated scalar/vector `namespace::name` values. Scalar provider values are repeated across three expression components, matching the upstream renderer binding; use `$shot::gain[0]` where a scalar is needed. Attribute scope takes precedence over Option, and the nearer provider wins within a scope. Missing namespaced values evaluate to zero. Providers snapshot immutable render settings, participate in cache identities and work for both image effects and the native surface chain. Source text cannot discover scene objects, process settings or external renderer resources.

## Resource bounds, distribution and qualification

Default native limits are 64 KiB source, lexical/structural AST depth 128, 512 external variables, numeric dimensions 1–16, one million interpreter/JIT budget steps per evaluation and 1 MiB owned strings. Structural depth is checked before recursive preparation, rejected trees are destroyed iteratively, and source error byte offsets retain the full allowed range. Nested Program work counts against each active caller's budget. FX documents allow at most 128 controls and use a 256 MiB sampling working-buffer bound. Source/preparation boundaries, VM/JIT budget checks and pixel/frame boundaries support cancellation. A custom native callback remains responsible for bounding its own work; a VM budget cannot interrupt arbitrary external code inside that callback.

Console `printf` is registered but disabled in render contexts; authoring/SDK hosts can explicitly permit it. `sprintf` string results remain supported with bounded, validated formats. Unknown engine/function-policy/document versions preserve source and fail with a diagnostic. Batch rendering checks failed expressions during scene loading and rendering and returns nonzero for invalid committed formulas or unavailable engines. Failed frames advance the ordered save queue without discarding later completed rasters; callbacks identify the actual requested output frames. Early, middle and late failures, selected ranges/steps and multimedia failures cannot produce a successful completion summary.

The source revision/archive checksum, parser tools, selected modules, build-copy corrections and independent replacements are recorded in `toonz/cmake/SeExpr-source.json`. The principal upstream license modifies Apache2's trademark clause; packaged notices preserve the exact terms and selected generated files retain their generator notices. The restrictive upstream Qt/Python initializer is not copied or redistributed.

CI fixtures cover the core/function corpus, SDK installation/plugins, Python/Qt bindings, native controls/editor/range dialog, animation references/persistence, image formats/tiles and actual saved-scene batch rendering. `seexpr_native_application` launches the real application with disposable copied data and exercises native reference/column/clipboard/sub-xsheet commands and undo/redo. This complements focused offscreen widget tests; it is not manual desktop qualification.

`seexpr_fx_test --benchmark` measures native Generator output at 1920×1080 and 3840×2160 using one and four workers. It also keeps 64 independently animated effects resident through eight source revisions and 1,024 rendered frames, checking their parameter identities, keyframes and analytic pixel results during cache eviction. Reports contain render timings, checksums and cumulative process peak resident memory; a peak after release does not measure live retained memory or establish absence of leaks. Linux CI retains the measurement report alongside the revision it assessed. The resident population is not a connected production scene. GPU performance, speed guarantees, minimum computer specifications and representative production scenes remain unqualified. Builds and tests run in CI. Fresh-runner package jobs separately qualify automated installed-application launches; physical desktop, tablet and theme observations remain unperformed acceptance work.

Release readiness requires all required native/core/SDK/backend/platform acceptance checks to pass at the final implementation revision. Source implementation or a passing subset alone does not make the port ready; see the parity checklist for the exact CI jobs and reports.
