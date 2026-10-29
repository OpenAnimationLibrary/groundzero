# Native SeExpr integration

Implementation status is tracked in [the parity checklist](seexpr_parity.md).
Until its required rows are verified, the integration remains in development.
This guide records the native workflow and compatibility contract; individual
capabilities must not be advertised as complete solely because they appear here.

## Expression language

SeExpr evaluates arithmetic expressions, vectors and strings. Formulas may use
local variables, conditional assignments and local functions. Their final
expression supplies the result. The host specifies the expected result type and
provides external variables and functions.

Native OT expressions retain their existing interpretation. SeExpr animation
uses an explicitly selected language mode. In particular, SeExpr's ordinary
`sin`, `cos` and related functions use radians; its `sind`, `cosd`, `deg` and
`rad` functions provide explicit degree conversion. Reinterpreting an existing
native expression as SeExpr can therefore change its result.

Both `$name` and `name` are accepted by the audited SeExpr grammar. External
variable names, their dimensions and their lifetime are part of the host
context. A formula using a variable absent from that context reports a binding
error rather than receiving an invented value.

Examples using the FX coordinate context:

```text
$scale = 8;
$amount = clamp(noise([$u * $scale, $v * $scale, $time]), 0, 1);
mix([0.1, 0.2, 0.4], [0.8, 0.9, 1], $amount)
```

A scalar formula can express motion with explicit angle conventions:

```text
10 * sind($frame * 10)
```

The examples require their host variables to be provided. The animation
integration defines frame, relative-frame, normalized segment time, seconds,
fps and units explicitly; these conventions must remain distinct from the FX
coordinate context where appropriate. Source and reference IDs stay literal
when the application language changes.

## Native authoring workflow

The native editor supplies validation, function and variable completion, source
error locations, help, presets and temporary previews. Typing edits a draft.
Apply commits one native undo transaction; Revert restores the committed
formula. Invalid drafts do not overwrite valid saved formulas. A preview of the
last valid expression must be identified as such when the current draft is
invalid.

Named controls have stable IDs, variable identifiers and display labels.
Changing a display label preserves its formula bindings and animation.
Renaming a variable is a migration of source references and bindings, not a
cosmetic operation. Removing controls or changing their types must preserve or
explicitly migrate affected animation.

The complete control set includes scalar and vector values, colors, strings,
scalar curves, color ramps and swatches. The parity checklist separately tracks
annotation parsing, animation-curve callbacks and the upstream deep-water
spectrum editor. Fixed generic sliders are not a substitute for these features.

Presets carry formula source, schema and default control values. A scene embeds
the information required to render after the original preset file is moved or
removed. User preset import/export validates the schema and reports unsupported
versions. Formula source remains available for repair after a load error.

## Procedural effects

Expression Generator produces finite-canvas RGB and alpha output from formulas,
coordinates, frame/time, seed and native animated controls. Generator bounds and
pixel centers are defined independently of tile dimensions so the same pixel
has the same meaning in preview, cropped renders and final output.

Expression Color receives Source and optional Mask images. RGB and alpha
evaluation use the documented working color space and straight/premultiplied
conversion policy. The processed result is blended with Source using the
selected mask channel and Mix. Missing Mask means full influence; zero influence
preserves Source. Transparent, HDR and negative float values require explicit
handling rather than premature integer clamping.

Bounded source sampling adds declared sample reach, filtering and border modes.
It must request the required source region and remain consistent across tile
sizes and transforms. It does not imply arbitrary previous-frame sampling or
unlimited image requests.

These effects can process full-color, Toonz raster and vector columns through
the normal renderer. Rendered appearance changes preserve the original vector
drawing. Editable vector styles and geometry operations have separate native
palette, topology, region, persistence and undo requirements recorded in the
parity checklist.

## Runtime and extension contract

The native adapter owns prepared evaluators and their host bindings. Each render
worker uses a separate evaluator; editor preview state is separate from final
render state. Changing formula source or control schema triggers preparation,
while changing ordinary variable values reuses a prepared evaluator when its
lifetime/type contract permits this.

The interpreter is the foundation backend. A qualified LLVM backend must expose
its real backend identity and pass parity tests; an unavailable JIT cannot be
silently labeled as active. Backend, engine and function-policy versions are
part of saved/validated feature data and cache identity.

Custom host functions declare names, signatures, documentation, result types,
ownership and concurrency behavior. Trusted native extension loading is a
separate SDK operation from evaluating a scene formula. Scene loading must not
inherit arbitrary plugin search paths merely to find builtins. Console-output
functions need an explicit host-context policy; string functions and string
results must be tracked independently from that policy.

The upstream Python package exposes AST parsing and source-editing helpers. Its
`AST`, `ASTHandle`, `ASTType`, traversal, comments, ranges and edits are separate
from OT's JavaScript engine. A native JavaScript helper is not an equivalent
Python import/API implementation. Python packaging and its tests remain visible
in the parity checklist until qualified.

## Persistence, diagnostics and rendering

Scenes save formula source, schema, control animation, stable bindings and
engine/function-policy versions. Unknown versions preserve editable source and
produce actionable diagnostics. A committed invalid formula or unavailable
required engine must make batch rendering report failure, rather than report a
successful black frame.

Numeric output policy defines NaN/Inf handling, alpha bounds, HDR behavior and
integer conversion. Source complexity, nesting and memory have documented
bounds. Cancellation occurs at bounded preparation/evaluation boundaries; the
upstream interpreter alone does not provide an interrupt inside every native
function.

Cache identity includes exact formula/schema/version data, relevant control
values, coordinates/dimensions, frame/time/fps when used, seed, color policy and
upstream content. Different worker counts, tile sizes and frame order must not
change deterministic results.

## Build, SDK and acceptance

The core is pinned and built behind the native adapter. The complete port must
qualify interpreter, selected optional backend/binding modules and their package
behavior on the supported Windows, Linux and macOS toolchains. Exact source
provenance and third-party license terms accompany the selected source files.

The principal upstream license modifies Apache2's trademark clause. Selected
generated files retain their own generator notices. The unused Qt/Python editor
initializer has a different restrictive notice and cannot be treated as covered
by a blanket repository-wide license assertion. The source manifest and packaged
notices must describe the actual distributed subset.

Required acceptance covers language/function semantics, typed callbacks,
thread ownership, native editing and undo, control/schema migration, presets,
save/reopen, dependency/reference lifecycle, preview/batch equivalence, image
formats, cancellation, malformed input, packages and representative performance.
Builds and tests run in CI. Manual desktop, tablet and GPU observations are
reported separately from automated expression tests.
