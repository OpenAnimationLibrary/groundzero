# SeExpr port parity checklist

This checklist is the completion gate for the full port. A linked evaluator or a successful effect preset does not establish full upstream parity. It records the audited upstream surface and the native host work needed to make it usable in Groundzero.

Upstream snapshot: `8f8c8f2c5e27e96fae70d6b82ac1ff4f4811d6dc`.
Initial Groundzero base: `3a2b9f6dc56a55a9f04f432dc00241020397b6e4`.

Status definitions:

- **Implemented / CI pending**: the source implementation and named acceptance fixtures exist. This is not a passing result.
- **Acceptance pending**: the implementation exists, but the listed integration, package or performance evidence still needs completion.
- **Verified**: the required fixtures passed on the stated platform/backend matrix. A row must name its passing evidence before receiving this status.
- **Additional host extension**: the feature is new OT behavior, absent from the audited upstream library. Its absence does not establish a missing upstream port.

This is a source-backed implementation inventory. No local builds or tests were run, and no row below is marked Verified. The application port uses native OT workflows for upstream host examples; the external binary interfaces and optional dependency boundaries are recorded explicitly below.

## Language and evaluation

Source paths in this document are upstream paths unless identified as native.

| ID | Upstream surface and source evidence | Native destination / acceptance contract | Status |
|---|---|---|---|
| L01 | Numeric literals, scalar FP values; `ExprParser.y`, `ExprType.h` | `tseexpr.cpp`, `seexpr_runtime`: scalar and fractional/boundary values with explicit finite-result policy | Implemented / CI pending |
| L02 | Vector literals and subscripts; `ExprParser.y:305` | Native numeric inputs/results support dimensions 1 through 16, matching the interpreter opcode limit; `seexpr_runtime` covers a real four-dimensional local function and external variable | Implemented / CI pending |
| L03 | Strings, escape handling and string results; `ExprParserLex.l`, `Expression.h:192` | Owned string values, Unicode/escapes, repeated evaluation and bounded formatting in `seexpr_runtime`, `seexpr_formatting` | Implemented / CI pending |
| L04 | Optional `$` prefixes and host variables; `ExprParser.y:132` | Both name forms, typed unresolved bindings and value updates in `seexpr_runtime` | Implemented / CI pending |
| L05 | Arithmetic `+ - * / % ^`; `ExprParser.y:322` | Original arithmetic/parser semantics retained; scalar/vector promotion, finite-result errors and recovery in `seexpr_runtime` | Implemented / CI pending |
| L06 | Unary `+ - ! ~`; `ExprParser.y:318` | Original grammar/interpreter retained; operator corpus in `seexpr_runtime` | Implemented / CI pending |
| L07 | Comparisons/equality and logical AND/OR; `ExprParser.y:310` | Whole-vector equality/inequality, scalar promotion and IEEE unordered-value fixtures at dimensions 1/3/4/16; shared interpreter/LLVM equality corpus plus branch/type cases | Implemented / CI pending |
| L08 | Ternary conditions; `ExprParser.y:309` | Original branch semantics retained; numeric/vector/string cases in runtime/LLVM fixtures | Implemented / CI pending |
| L09 | Assignment and compound assignment; `ExprParser.y:245` | Original assignments and statement order retained; native local-scope and captured-variable fixtures | Implemented / CI pending |
| L10 | If/else/else-if assignment blocks; `ExprParser.y:293` | Original branches with local assignment environments; nested/local branch fixtures in `seexpr_runtime` | Implemented / CI pending |
| L11 | Modules, blocks and declaration lists; `ExprParser.y:135` | Original module/block AST and ordered declarations; typed/inferred local-function corpus | Implemented / CI pending |
| L12 | Local typed/inferred functions; `ExprParser.y:153` | `prepare_seexpr_language.py` completes parsed-but-disabled upstream AST support: typed/inferred functions, nested arguments, captures, vector/string results, isolated stacks and error cases | Implemented / CI pending |
| L13 | External function prototypes; `ExprParser.y:153` | Actual prototype/host signature resolution in the core AST; valid callback and unresolved/incorrect-type cases in `seexpr_runtime` and optional LLVM fixtures | Implemented / CI pending |
| L14 | Function calls and arrow-call syntax; `ExprParser.y:328` | Original fixed/variadic/arrow-call grammar plus typed native callbacks; runtime and builtin corpus | Implemented / CI pending |
| L15 | Constant/uniform/varying lifetimes; `ExprParser.y:174`, `ExprType.h` | Native `Lifetime` metadata; constant-update rejection and uniform/varying updates in `seexpr_runtime` | Implemented / CI pending |
| L16 | Desired return type/type checking; `Expression.h:113` | Explicit scalar/vector/string result type and dimension in `TSeExpr::Options`; type mismatch fixtures | Implemented / CI pending |
| L17 | Source offsets, comments and syntax/type diagnostics; `Expression.h:87` | Full signed core byte offsets; UTF-8 native editor mapping and corrected Python call/vector/unary ranges; syntax-only/type-error/deep-tree recovery fixtures | Implemented / CI pending |
| L18 | `usesVar`, `usesFunc`, constant detection; `Expression.h:142` | Actual used-variable/function/constant queries exposed by `TSeExpr::Program`; dependency/cache fixtures | Implemented / CI pending |
| L19 | Context parameters and custom variable/function resolution; `Expression.h:198` | Owned typed/variadic callbacks and SDK standard callback forms; exceptions, lifetime aggregation, reentry/mutation/active-move rejection and distinct nested Program budget fixtures | Implemented / CI pending |
| L20 | `evalMultiple`/VarBlock array evaluation; `Expression.h:182`, `VarBlock.h` | Native `evaluateBatch` plus upstream `VarBlock`/`evalMultiple` SDK fixtures; ownership and numerical cases | Implemented / CI pending |
| L21 | Multi-expression dependency graph; `ExprMultiExpr.h` | Actual upstream `ExprMultiExpr` API retained; resolution isolation/topological-order correction in `prepare_seexpr.py`, `seexpr_sdk` | Implemented / CI pending |
| L22 | Core curves/noise/vector helper APIs; `Curve.h`, `Noise.h`, `Vec.h` | Selected core math headers/implementations remain available in the installed SDK; native curves and all builtin families execute in fixtures | Implemented / CI pending |

General user `for`/`while` loops and recursive local functions are not supplied by the audited upstream grammar. Their absence is not a port omission. `prepare_seexpr_limits.py` bounds structural AST depth before type preparation and tears down rejected trees iteratively, including long flat arithmetic/unary chains; source byte offsets no longer truncate at 16 bits. Native guards aggregate nested Program budgets, reject mutation/reentry of an active evaluator and permit separately owned nested evaluators. These corrections have source acceptance cases and remain CI pending.

## Builtin function registry

The following table is extracted statically from `ExprBuiltins.cpp::defineBuiltins` at the pinned snapshot: **99 registered names**. Five are guarded by `#ifndef SEEXPR_WIN32` upstream, yielding 94 names in its unmodified Windows branch. The native build enables those five standard C++17 functions on every platform. `ci-scripts/seexpr/builtin_test.cpp` supplies a representative executed expression for each of the 99 names, registry/help checks, malformed/type cases and optional interpreter/LLVM comparisons; `seexpr_formatting` separately exercises bounded console/string formatting. All results remain CI pending.

For each name, acceptance requires registry/help availability, representative numerical or string output, arity/type handling where applicable, and interpreter/backend parity when more than one backend is offered. Runtime host policy must distinguish a function that exists but is explicitly disallowed in a rendering context from a function silently absent from the port. Registry presence alone is not a semantic test.

| Function | Source line in `src/SeExpr2/ExprBuiltins.cpp` | Upstream platform/context note | Native parity status |
|---|---:|---|---|
| `abs` | 1724 | All upstream platforms | Implemented / CI pending |
| `acos` | 1725 | All upstream platforms | Implemented / CI pending |
| `asin` | 1726 | All upstream platforms | Implemented / CI pending |
| `atan` | 1727 | All upstream platforms | Implemented / CI pending |
| `atan2` | 1728 | All upstream platforms | Implemented / CI pending |
| `ceil` | 1729 | All upstream platforms | Implemented / CI pending |
| `cos` | 1730 | All upstream platforms | Implemented / CI pending |
| `cosh` | 1731 | All upstream platforms | Implemented / CI pending |
| `exp` | 1732 | All upstream platforms | Implemented / CI pending |
| `floor` | 1733 | All upstream platforms | Implemented / CI pending |
| `fmod` | 1734 | All upstream platforms | Implemented / CI pending |
| `log` | 1735 | All upstream platforms | Implemented / CI pending |
| `log10` | 1736 | All upstream platforms | Implemented / CI pending |
| `pow` | 1737 | All upstream platforms | Implemented / CI pending |
| `sin` | 1738 | All upstream platforms | Implemented / CI pending |
| `sinh` | 1739 | All upstream platforms | Implemented / CI pending |
| `sqrt` | 1740 | All upstream platforms | Implemented / CI pending |
| `tan` | 1741 | All upstream platforms | Implemented / CI pending |
| `tanh` | 1742 | All upstream platforms | Implemented / CI pending |
| `cbrt` | 1744 | Guarded out on upstream Windows | Implemented / CI pending |
| `asinh` | 1745 | Guarded out on upstream Windows | Implemented / CI pending |
| `acosh` | 1746 | Guarded out on upstream Windows | Implemented / CI pending |
| `atanh` | 1747 | Guarded out on upstream Windows | Implemented / CI pending |
| `trunc` | 1748 | Guarded out on upstream Windows | Implemented / CI pending |
| `deg` | 1759 | All upstream platforms | Implemented / CI pending |
| `rad` | 1760 | All upstream platforms | Implemented / CI pending |
| `cosd` | 1761 | All upstream platforms | Implemented / CI pending |
| `sind` | 1762 | All upstream platforms | Implemented / CI pending |
| `tand` | 1763 | All upstream platforms | Implemented / CI pending |
| `acosd` | 1764 | All upstream platforms | Implemented / CI pending |
| `asind` | 1765 | All upstream platforms | Implemented / CI pending |
| `atand` | 1766 | All upstream platforms | Implemented / CI pending |
| `atan2d` | 1767 | All upstream platforms | Implemented / CI pending |
| `clamp` | 1770 | All upstream platforms | Implemented / CI pending |
| `round` | 1771 | All upstream platforms | Implemented / CI pending |
| `max` | 1772 | All upstream platforms | Implemented / CI pending |
| `min` | 1773 | All upstream platforms | Implemented / CI pending |
| `invert` | 1776 | All upstream platforms | Implemented / CI pending |
| `compress` | 1777 | All upstream platforms | Implemented / CI pending |
| `expand` | 1778 | All upstream platforms | Implemented / CI pending |
| `fit` | 1779 | All upstream platforms | Implemented / CI pending |
| `gamma` | 1780 | All upstream platforms | Implemented / CI pending |
| `bias` | 1781 | All upstream platforms | Implemented / CI pending |
| `contrast` | 1782 | All upstream platforms | Implemented / CI pending |
| `boxstep` | 1783 | All upstream platforms | Implemented / CI pending |
| `linearstep` | 1784 | All upstream platforms | Implemented / CI pending |
| `smoothstep` | 1785 | All upstream platforms | Implemented / CI pending |
| `gaussstep` | 1786 | All upstream platforms | Implemented / CI pending |
| `remap` | 1787 | All upstream platforms | Implemented / CI pending |
| `mix` | 1788 | All upstream platforms | Implemented / CI pending |
| `hsi` | 1789 | All upstream platforms | Implemented / CI pending |
| `midhsi` | 1790 | All upstream platforms | Implemented / CI pending |
| `hsltorgb` | 1791 | All upstream platforms | Implemented / CI pending |
| `rgbtohsl` | 1792 | All upstream platforms | Implemented / CI pending |
| `saturate` | 1793 | All upstream platforms | Implemented / CI pending |
| `hash` | 1796 | All upstream platforms | Implemented / CI pending |
| `noise` | 1797 | All upstream platforms | Implemented / CI pending |
| `snoise` | 1798 | All upstream platforms | Implemented / CI pending |
| `vnoise` | 1799 | All upstream platforms | Implemented / CI pending |
| `cnoise` | 1800 | All upstream platforms | Implemented / CI pending |
| `snoise4` | 1801 | All upstream platforms | Implemented / CI pending |
| `vnoise4` | 1802 | All upstream platforms | Implemented / CI pending |
| `cnoise4` | 1803 | All upstream platforms | Implemented / CI pending |
| `turbulence` | 1804 | All upstream platforms | Implemented / CI pending |
| `vturbulence` | 1805 | All upstream platforms | Implemented / CI pending |
| `cturbulence` | 1806 | All upstream platforms | Implemented / CI pending |
| `fbm` | 1807 | All upstream platforms | Implemented / CI pending |
| `vfbm` | 1808 | All upstream platforms | Implemented / CI pending |
| `cfbm` | 1809 | All upstream platforms | Implemented / CI pending |
| `cellnoise` | 1810 | All upstream platforms | Implemented / CI pending |
| `ccellnoise` | 1811 | All upstream platforms | Implemented / CI pending |
| `pnoise` | 1812 | All upstream platforms | Implemented / CI pending |
| `fbm4` | 1813 | All upstream platforms | Implemented / CI pending |
| `vfbm4` | 1814 | All upstream platforms | Implemented / CI pending |
| `cfbm4` | 1815 | All upstream platforms | Implemented / CI pending |
| `dist` | 1818 | All upstream platforms | Implemented / CI pending |
| `length` | 1819 | All upstream platforms | Implemented / CI pending |
| `hypot` | 1820 | All upstream platforms | Implemented / CI pending |
| `dot` | 1821 | All upstream platforms | Implemented / CI pending |
| `norm` | 1822 | All upstream platforms | Implemented / CI pending |
| `cross` | 1823 | All upstream platforms | Implemented / CI pending |
| `angle` | 1824 | All upstream platforms | Implemented / CI pending |
| `ortho` | 1825 | All upstream platforms | Implemented / CI pending |
| `rotate` | 1826 | All upstream platforms | Implemented / CI pending |
| `up` | 1827 | All upstream platforms | Implemented / CI pending |
| `cycle` | 1830 | All upstream platforms | Implemented / CI pending |
| `pick` | 1831 | All upstream platforms | Implemented / CI pending |
| `choose` | 1832 | All upstream platforms | Implemented / CI pending |
| `wchoose` | 1833 | All upstream platforms | Implemented / CI pending |
| `swatch` | 1834 | All upstream platforms | Implemented / CI pending |
| `spline` | 1835 | All upstream platforms | Implemented / CI pending |
| `voronoi` | 1839 | All upstream platforms | Implemented / CI pending |
| `cvoronoi` | 1840 | All upstream platforms | Implemented / CI pending |
| `pvoronoi` | 1841 | All upstream platforms | Implemented / CI pending |
| `curve` | 1843 | All upstream platforms | Implemented / CI pending |
| `ccurve` | 1844 | All upstream platforms | Implemented / CI pending |
| `getVar` | 1845 | Dynamic variable/default behavior; dependency semantics required | Implemented / CI pending |
| `printf` | 1846 | Console side effect; explicit host-context policy and controlled output test required | Implemented / CI pending |
| `sprintf` | 1849 | String formatting; owned result and format/type tests required | Implemented / CI pending |

The documentation must use this real registry plus native host bindings. Names mentioned by old examples, such as demo-defined `rand`, are not automatically core builtins. Additional OT functions such as image sampling and channel references require their own registered signatures/help and acceptance cases.

## Native authoring and control parity

| ID | Upstream surface / source | Native destination / acceptance contract | Status |
|---|---|---|---|
| U01 | Editor, short editor, popup editor; `UI/ExprEditor.*`, `ExprShortEdit.*`, `ExprDialog.*` | Native `SeExprEditor` is used by actual FX/animation fields; Apply/Revert, owner replacement, undo and draft validation in `seexpr_editor` | Implemented / CI pending |
| U02 | Syntax highlighting and completion; `UI/ExprHighlighter.h`, `ExprCompletionModel.*` | Actual core/host catalog and variables drive completion/help; native syntax highlighter and source-mode handling | Implemented / CI pending |
| U03 | Error and function help popups; `UI/ExprPopupDoc.*`, `ExprHelp.h` | UTF-8 error spans, actual function documentation, palette-based widgets and translated presentation; manual accessibility/theme review remains separate | Implemented / CI pending |
| U04 | Expression browser; `UI/ExprBrowser.*`, `ExprFileDialog.*` | Native built-in/user browser, search, bounded JSON import/export and embedded scene documents; preset fixtures in `seexpr_editor`/`seexpr_fx` | Implemented / CI pending |
| U05 | Numeric controls; `UI/Editable.h:60`, `ExprControl.h` | Stable control IDs, integer/range rules and native `TDoubleParam` animation; schema/clone/load fixtures | Implemented / CI pending |
| U06 | Vector controls; `UI/Editable.h:101` | Named three-component controls with native component parameters; values/keyframes survive reorder, clone and schema changes | Implemented / CI pending |
| U07 | Color controls and color linking; `UI/ExprColorSwatch.*`, `ExprControl.h` | Native color picker and `TPixelParam` linking/keyframes; negative/HDR doubles and unchanged-picker precision preserved; source color policy explicit | Implemented / CI pending |
| U08 | String controls; `UI/Editable.h:134` | Literal string editing and annotation metadata retained; native and Python Qt file/directory pickers require explicit action, preserve cancel and remain drafts; no source-triggered file execution | Implemented / CI pending |
| U09 | Scalar curves; `UI/ExprCurve.*`, `Editable.h:170` | Native point editor, five interpolation modes, actual engine preview and native stop position/value parameters; control/FX fixtures | Implemented / CI pending |
| U10 | Color curves; `UI/ExprColorCurve.*` | Native color-stop editor, five interpolation modes, HDR values and native animated stops; control/FX fixtures | Implemented / CI pending |
| U11 | Color swatches; `UI/Editable.h:274`, `ColorSwatchControl` | Native add/remove/reorder/edit swatches, exact uniform selection bins and endpoint; high-precision source/value fixtures | Implemented / CI pending |
| U12 | Animation curve callback/preview; `UI/Editable.h:211`, `AnimCurveControl` | Native Function Editor and `channel("reference"[,frame])` bindings preserve dependency tracking; external animlib binary adapter is replaced by the native host workflow (B10) | Implemented / CI pending |
| U13 | Deep-water spectrum control; `UI/ExprDeepWater.*`, `Editable.h:327` | Native twelve-parameter spectrum editor and exact shared spectrum math (`tseexprspectrum.h`), matching upstream UI energy preview; no upstream fluid renderer exists in the core | Implemented / CI pending |
| U14 | Control parsing and annotation comments; `UI/ExprSpecParser.y`, `EditableExpression.*`, `ControlSpec.*` | Native literal/range/string/curve/color-ramp/swatch annotation import with comments, stable occurrence/source IDs and computed-expression preservation; exact-value fixtures | Implemented / CI pending |
| U15 | Grapher widgets and context examples; `UI/ExprGrapher2d.*`, `BasicExpression.*` | Native `Range Preview` samples an immutable real FX/animation context with scalar/vector graph, per-frame errors, fractional frames, 512-sample bound and cancel/owner-close guards; `seexpr_range`, `seexpr_editor` | Implemented / CI pending |

The native editor has real named schema controls, curves, color ramps, swatches, annotation conversion and native animation migration. Range preview clones FX state once per dialog and freezes its draft; animation uses explicit sample frames, normalized segment time and native units. Numeric range preview does not render a full source image: its image-dependent callbacks report that a native FX preview is required, and Apply makes the normal FX preview available.

## Backends, SDK, bindings and external host adapters

| ID | Upstream component and evidence | Required disposition / acceptance | Status |
|---|---|---|---|
| B01 | Interpreter; `Interpreter.*`, `Expression.cpp` | Actual interpreter, worker-owned FX evaluators and language/builtin/SDK corpus; default backend is explicit and independent of environment | Implemented / CI pending |
| B02 | LLVM JIT; `ExprLLVMCodeGeneration.cpp`, `ExprLLVM*.h`, `Evaluator.h` | Optional real LLVM14 emitter/runtime, explicit backend selection, no interpreter fallback under a JIT label; budget/string/callback/local-function/parity fixtures in `seexpr_llvm` and `seexpr_builtins` | Implemented / CI pending |
| B03 | Custom function ABI; `ExprFunc.h`, `ExprFuncX.h`, `ExprFuncStandard.h` | Original SDK standard callback forms retained across interpreter/JIT, including zero-to-six scalar arguments, variadics, vector/scalar results, exceptions and lifetimes; shared standard callback corpus plus native host example | Implemented / CI pending |
| B04 | Explicit plugin loading; `ExprFunc.cpp:217` | Explicit trusted SDK loader with canonical path deduplication, stable directory discovery and registry-before-module cleanup; `seexpr_sdk_plugins` | Implemented / CI pending |
| B05 | Environment plugin discovery; `ExprFunc.cpp:158` | Inherited `SE_EXPR_PLUGINS` discovery deliberately disabled; explicit SDK loading remains available, and plugin fixture tests non-discovery | Implemented / CI pending |
| B06 | Platform plugin behavior; `ExprFunc.cpp:255` | Native UTF-16 Windows loader and Unix dynamic loading implemented in `prepare_seexpr_sdk.py`; matching-compiler C++ ABI required; three-platform CI configured | Implemented / CI pending |
| B07 | Python AST module; `src/py/SeExprPy.cpp` | Optional Python3 wrapper uses privately prepared licensed inspection parser; virtual node destruction, correct call/vector/unary ranges and retained AST/handle ownership; `seexpr_python_ast` | Implemented / CI pending |
| B08 | Python helpers; `src/py/__init__.py`, `utils.py` | Independent Python3 helpers preserve Unicode comments/traversal/source edits; native evaluation/callback API is additional functionality; AST/evaluation fixtures | Implemented / CI pending |
| B09 | Python/Qt editor bindings; `UI/SeExpr2Editor.sip`, `UI/__init__.py` | Independent `SeExprPy.editor`/`expreditor2` PyQt frontend supplies editor/dialog/browser/short editors and controls. Restricted upstream initializer is excluded. External PyQt required; Qt5 CI configured, Qt6 qualification pending | Implemented / CI pending |
| B10 | Animation-library adapter; `SEEXPR_USE_ANIMLIB`, `UI/CE/*` | Native Function Editor/keyframes/curves/channel callbacks replace the artist workflow. No external animlib runtime or binary compatibility is claimed; no such SDK is needed by OT | Implemented / CI pending |
| B11 | Renderer adapter example; `src/demos/rman2/seop.cpp` | Native worker-owned FX programs, host bindings and batching implement the useful renderer-host behavior. The external RenderMan shadeop ABI is not an OT plugin and is not distributed; typed SDK callback example remains available | Implemented / CI pending |
| B12 | Image synthesis/image editor demos; `src/demos/imageSynth/*`, `imageEditor/*` | Native Generator/Color/image sampling, controls, preset browser and installed examples replace standalone image-synthesis/editor host applications; `seexpr_fx`, `seexpr_native_batch` | Implemented / CI pending |
| B13 | Graphing/calculator demos; `src/demos/asciiGraph.cpp`, `asciiCalculator.cpp`, `segraph*` | Installed `seexpr-calc`/`seexpr-graph` utilities plus native range graph; SDK calculator/grapher tests and `seexpr_range` | Implemented / CI pending |
| B14 | Demo host functions and example formulas; `imageSynthForPaint3d.cpp`, example `.se` files | Selected installed presets use actual OT bindings and are validated. Demo-only rand/map/triplanar and host deepWater are not core builtins; external callback formulas require explicit bindings (see below) | Implemented / CI pending |
| B15 | Documentation/utilities; `src/doc`, `src/utils`, tests | Installed native HTML help/presets/notices, actual function help and C++/Python SDK guides; source-backed language/function/host regression corpus | Implemented / CI pending |

The LLVM option builds the actual pinned emitter with LLVM14 compatibility corrections; it does not relabel interpreter evaluation. It remains optional and requires compatible external development/runtime libraries. Configured Linux LLVM CI does not establish Windows/macOS JIT package qualification or make JIT a default shipping dependency.

The Python extension uses the separate licensed `src/SeExpr2/parser` scanner/template parser. Its AST inventory includes Module, Prototype, LocalFunction, Block, Node, IfThenElse, Vec, Cond, Compare, UnaryOp, BinaryOp, CompareEq, TernaryOp, Def, Subscript, Var, Num, Call, Assign and String. The checked `seexprpython/prepare_parser.py` step retains source notices while correcting virtual destruction and ordinary-call/vector/unary source ranges in private build copies. Its independent wrapper/helpers and optional Qt frontend are packaged separately; Python is not embedded into OT. No restrictive upstream Qt initializer is copied or installed. Qt5 qualification does not certify Qt6.

External-host disposition is explicit: the port supplies the usable authoring/rendering behavior through OT's own host interfaces, and does not distribute RenderMan or external animlib binaries. SDK extensions preserve the C++ source/registration API and must be rebuilt against the supplied headers/library with a compatible toolchain; private-core layout corrections do not establish compatibility with binaries built against unmodified upstream headers. They are not a cross-toolchain C ABI. Scene source cannot select or discover executable modules.

`imageSynthForPaint3d.cpp` defines demo-local `rand`, `map` and `triplanar` placeholders, not a texture engine: its fixed/fallback return values do not implement production randomness or texture loading. OT uses the actual core noise/hash functions and bounded source sampling instead. Formula files requiring those demo callbacks need explicit host bindings; unbound examples are not advertised as ready-to-use presets. Likewise the upstream deep-water widget authors a spectrum and references a host-provided `deepWater` function; the core has no fluid simulation/vector renderer to copy. The native spectrum editor preserves the actual spectrum math and exposes a native spectrum curve without claiming a fluid renderer.

## OT integration completion

| ID | Host feature | Required evidence | Status |
|---|---|---|---|
| O01 | Expression Generator FX | `stdfx/seexprfx.cpp`: native registration, finite canvas and exact alias/cache data; `seexpr_fx` covers 8/16/float, tiling, transforms and controls | Implemented / CI pending |
| O02 | Expression Color FX | Source/optional Mask/Mix, explicit working-space and premultiplied policy, float HDR and missing-input fixtures in `seexpr_fx` | Implemented / CI pending |
| O03 | Bounded image sampling | Declared sampling radius, nearest/bilinear, transparent/clamp/repeat/mirror, transformed input requests and tile/cancel fixtures | Implemented / CI pending |
| O04 | Named animatable controls | Native scalar/vector/color/string/curve parameters, stable IDs, archive/regrow and schema migration; actual editor one-transaction undo fixtures | Implemented / CI pending |
| O05 | Scene/preset persistence | Versioned embedded source/schema, archive/registry, native clone/preset/link/save/reopen fixtures; invalid versions retain source and fail validation | Implemented / CI pending |
| O06 | Animation dialect | Explicit native/SeExpr modes and versioned tags; degree/radian, live scene fps, units/fractional frames, Apply/Revert/persistence fixtures | Implemented / CI pending |
| O07 | Reference lifecycle | Native binding/observers/literal rewrites, direct/mixed/concurrent corrupt-cycle fixtures plus actual application column move/insert/delete, clipboard copy/paste, sub-xsheet collapse/explode and undo/redo; `seexpr_animation`, `seexpr_native_application` | Implemented / CI pending |
| O08 | Batch rendering | Actual saved scene → tcomposer → decoded output fixture, with syntax/runtime/animation/version failures returning nonzero; `seexpr_native_batch` | Implemented / CI pending |
| O09 | Native UI lifecycle | Actual native field/editor interaction, draft isolation, immutable range preview, owner deletion/cancel, text diagnostics and undo fixtures; manual focus/tablet/theme qualification remains | Implemented / CI pending |
| O10 | Platform/package qualification | Windows/Linux/macOS core/SDK/Python and native application/batch qualification configured; optional LLVM14 Linux. Help/presets/notices and installed SDK consumer included. Passing results and manual installed-desktop evidence remain pending | Implemented / CI pending |
| O11 | Performance/resource qualification | Source/AST-depth/instruction/string bounds, nested budgets, cancel and reentry fixtures. `seexpr_fx_test --benchmark` measures actual 1080p/4K Generator with 1/4 workers, cold/warm frames, pixel checksums and peak process RSS; Linux CI configured, results pending | Implemented / CI pending |
| O12 | Editable vector style/geometry extensions | SeExpr supplies formulas, not OT vector topology/palette/region editing. Vector columns render through the native effects; editable geometry/style authoring is a separate new host extension, not an upstream component being silently disabled | Additional host extension |

## License and source-selection gates

- Root `LICENSE` and most core/UI sources carry Apache2 with an explicit replacement trademark section. Preserve the exact text and required notices; do not describe it as unmodified Apache2.
- `src/py/__init__.py` carries the main modified-Apache terms. It is distinct from `src/SeExpr2/UI/__init__.py`, whose restrictive proprietary/confidential header requires exclusion or authorization before redistribution.
- Generated parser/scanner files have their own generator notices/exceptions. Keep these with the selected files and record generated-source provenance.
- External renderer headers/libraries and optional animation/UI integrations need a selected-file dependency/license inventory; a top-level repository license does not establish every transitive component's disposition.
- The source/provenance manifest and packaged third-party license are maintained by the dependency integration. This checklist must agree with the actual manifest; it must not imply unused/restricted files are bundled.

## Qualification evidence and remaining acceptance

These fixtures exist in source and are registered for CI. Their presence is not a passing result:

| Layer | Actual fixtures | Material coverage |
|---|---|---|
| Core | `seexpr_runtime`, `seexpr_builtins`, `seexpr_formatting`, `seexpr_sdk` | Typed language, all 99 names, local functions/prototypes, whole-vector equality, full SDK standard callbacks, batch/graph, reentry, source/AST limits and error recovery |
| Optional JIT | `seexpr_llvm`, LLVM execution in builtin/runtime corpora | Actual emitter identity, numerical parity, strings, callbacks, locals, cancellation/recovery |
| SDK/install | `seexpr_sdk_plugins`, `seexpr_sdk_host_callbacks`, `seexpr_sdk_calculator`, `seexpr_sdk_grapher`, clean installed consumer | Explicit loading/non-discovery, cleanup, utilities, installed headers/archive/CMake package |
| Python | `seexpr_python_ast`, `seexpr_python_eval`, `seexpr_python_ui` | Built-package imports, corrected AST byte ranges/ownership/Unicode edits, native typed callbacks/evaluation, real optional Qt widgets and HDR/precision-preserving source edits |
| Native FX | `seexpr_fx` | 8/16/float pixels, tiles/transforms, source/mask, sampling, controls/HDR, schema/cache/clone/preset/link/load and worker failures |
| Animation | `seexpr_animation` | Legacy semantics, live fps/time, literal channel bindings/observers/cycles, versioned save/reopen/undo, bounded concurrent corrupt-cycle failure |
| Native authoring | `seexpr_controls`, `seexpr_editor`, `seexpr_range` | Actual controls/dialogs, annotations, bound-control editing without false name collisions, unchanged HDR picker precision, owner replacement/undo, immutable numeric ranges, fractional/native channel samples and cancel/owner deletion |
| Native application | `seexpr_native_application` | Actual visible application startup and native column move/insert/delete, clipboard copy/paste, sub-xsheet collapse/explode, reference monitoring and undo/redo using disposable data |
| Batch | `seexpr_native_batch` | Saved native scene, real renderer output pixels, nonzero syntax/runtime/animation/version failures |
| Full-resolution measurements | `seexpr_fx_test --benchmark` | 1920×1080/3840×2160 Generator, one/four workers, cold/warm-frame timings, worker-count image checksum equality and process peak RSS; Linux CI report artifact |

Animation graph evaluation currently uses a recursive graph-wide gate before per-curve locks, retaining cycle detection while avoiding opposing-curve lock deadlocks. It serializes this graph path; FX workers still own separate evaluators. Representative animation contention/performance evidence is required before making speed claims.

Assess all required CI at the completed implementation revision. The native application fixture launches the real window system and native commands, complementing focused offscreen widgets and batch renders. Its test-only startup requires disposable copied data and isolates settings, caches and scene assets; it is not a normal end-user launch mode. Windows qualification supplies a checksum-pinned temporary Mesa desktop renderer and removes it after the fixture; this is not an installed application dependency.

The full-resolution benchmark records cold/warm-frame times and process peak resident memory for one Generator per worker. It measures worker-count determinism, not a many-node production scene, GPU acceleration, per-expression heap usage or a hardware minimum. There are no speed thresholds or results claimed before CI runs. Large scene memory/animation contention and manual installed-desktop/tablet/theme/hardware observations remain separate qualification work.

## Readiness rule

A complete source implementation is not a verified release. Required rows become Verified only after the named acceptance evidence passes at the final revision. Acceptance-pending rows remain visible until resolved. External-host equivalents and optional dependency requirements above describe the actual port; they do not imply external binary interfaces or hardware tests that were never performed.
