# SeExpr port parity checklist

This checklist is the completion gate for the full port. A linked evaluator or a successful effect preset does not establish full upstream parity. It records the audited upstream surface and the native host work needed to make it usable in Groundzero.

Upstream snapshot: `8f8c8f2c5e27e96fae70d6b82ac1ff4f4811d6dc`.
Initial Groundzero base: `3a2b9f6dc56a55a9f04f432dc00241020397b6e4`.

Status definitions:

- **Implemented**: the source implementation and named acceptance fixtures exist. This label describes the source inventory; it does not assert a passing runtime, platform or package result.
- **Incomplete**: a source capability remains unported. A useful replacement for part of a workflow does not establish parity for the whole component.
- **Additional host extension**: the feature is new OT behavior, absent from the audited upstream library. Its absence does not establish a missing upstream port.

Runtime acceptance is established by the named CI jobs and their reports at the implementation revision being assessed. This inventory does not embed a transient run status. The application port uses native OT workflows for upstream host examples; external binary interfaces and optional dependency boundaries are recorded explicitly below. Manual desktop/hardware and PyQt6 qualification are not claimed.

## Language and evaluation

Source paths in this document are upstream paths unless identified as native.

| ID | Upstream surface and source evidence | Native destination / acceptance contract | Status |
|---|---|---|---|
| L01 | Numeric literals, scalar FP values; `ExprParser.y`, `ExprType.h` | `tseexpr.cpp`, `seexpr_runtime`: scalar and fractional/boundary values with explicit finite-result policy | Implemented |
| L02 | Vector literals and subscripts; `ExprParser.y:305` | Native numeric inputs/results support dimensions 1 through 16, matching the interpreter opcode limit; `seexpr_runtime` covers a real four-dimensional local function and external variable | Implemented |
| L03 | Strings, escape handling and string results; `ExprParserLex.l`, `Expression.h:192` | Owned string values, Unicode/escapes, repeated evaluation and bounded formatting in `seexpr_runtime`, `seexpr_formatting` | Implemented |
| L04 | Optional `$` prefixes and host variables; `ExprParser.y:132` | Both name forms, typed unresolved bindings and value updates in `seexpr_runtime` | Implemented |
| L05 | Arithmetic `+ - * / % ^`; `ExprParser.y:322` | Original arithmetic/parser semantics retained; scalar/vector promotion, finite-result errors and recovery in `seexpr_runtime` | Implemented |
| L06 | Unary `+ - ! ~`; `ExprParser.y:318` | Original grammar/interpreter retained; operator corpus in `seexpr_runtime` | Implemented |
| L07 | Comparisons/equality and logical AND/OR; `ExprParser.y:310` | Whole-vector equality/inequality, scalar promotion and IEEE unordered-value fixtures at dimensions 1/3/4/16; shared interpreter/LLVM equality corpus plus branch/type cases | Implemented |
| L08 | Ternary conditions; `ExprParser.y:309` | Original branch semantics retained; numeric/vector/string cases in runtime/LLVM fixtures | Implemented |
| L09 | Assignment and compound assignment; `ExprParser.y:245` | Original assignments and statement order retained; native local-scope and captured-variable fixtures | Implemented |
| L10 | If/else/else-if assignment blocks; `ExprParser.y:293` | Original branches with local assignment environments; nested/local branch fixtures in `seexpr_runtime` | Implemented |
| L11 | Modules, blocks and declaration lists; `ExprParser.y:135` | Original module/block AST and ordered declarations; typed/inferred local-function corpus | Implemented |
| L12 | Local typed/inferred functions; `ExprParser.y:153` | `prepare_seexpr_language.py` completes parsed-but-disabled upstream AST support: typed/inferred functions, nested arguments, captures, vector/string results, isolated stacks and error cases | Implemented |
| L13 | External function prototypes; `ExprParser.y:153` | Actual prototype/host signature resolution in the core AST; valid callback and unresolved/incorrect-type cases in `seexpr_runtime` and optional LLVM fixtures | Implemented |
| L14 | Function calls and arrow-call syntax; `ExprParser.y:328` | Original fixed/variadic/arrow-call grammar plus typed native callbacks; runtime and builtin corpus | Implemented |
| L15 | Constant/uniform/varying lifetimes; `ExprParser.y:174`, `ExprType.h` | Native `Lifetime` metadata; constant-update rejection and uniform/varying updates in `seexpr_runtime` | Implemented |
| L16 | Desired return type/type checking; `Expression.h:113` | Explicit scalar/vector/string result type and dimension in `TSeExpr::Options`; type mismatch fixtures | Implemented |
| L17 | Source offsets, comments and syntax/type diagnostics; `Expression.h:87` | Full signed core byte offsets; UTF-8 native editor mapping and corrected Python call/vector/unary ranges; syntax-only/type-error/deep-tree/empty-source recovery and aliased SDK reset fixtures | Implemented |
| L18 | `usesVar`, `usesFunc`, constant detection; `Expression.h:142` | Actual used-variable/function/constant queries exposed by `TSeExpr::Program`; dependency/cache fixtures | Implemented |
| L19 | Context parameters and custom variable/function resolution; `Expression.h:198` | Owned typed/variadic callbacks and SDK standard callback forms; exceptions, lifetime aggregation, reentry/mutation/active-move rejection and distinct nested Program budget fixtures | Implemented |
| L20 | `evalMultiple`/VarBlock array evaluation; `Expression.h:182`, `VarBlock.h` | Native `evaluateBatch` plus upstream `VarBlock`/`evalMultiple` SDK fixtures; ownership and numerical cases | Implemented |
| L21 | Multi-expression dependency graph; `ExprMultiExpr.h` | Actual upstream `ExprMultiExpr` API retained; resolution isolation/topological-order correction in `prepare_seexpr.py`, `seexpr_sdk` | Implemented |
| L22 | Core curves/noise/vector helper APIs; `Curve.h`, `Noise.h`, `Vec.h` | Selected core math headers/implementations remain available in the installed SDK; native curves and all builtin families execute in fixtures | Implemented |

General user `for`/`while` loops and recursive local functions are not supplied by the audited upstream grammar. Their absence is not a port omission. `prepare_seexpr_limits.py` bounds structural AST depth before type preparation and tears down rejected trees iteratively, including long flat arithmetic/unary chains; source byte offsets no longer truncate at 16 bits. Native guards aggregate nested Program budgets, reject mutation/reentry of an active evaluator and permit separately owned nested evaluators. Named source acceptance cases cover these corrections.

## Builtin function registry

The following table is extracted statically from `ExprBuiltins.cpp::defineBuiltins` at the pinned snapshot: **99 registered names**. Five are guarded by `#ifndef SEEXPR_WIN32` upstream, yielding 94 names in its unmodified Windows branch. The native build enables those five standard C++17 functions on every platform. `ci-scripts/seexpr/builtin_test.cpp` supplies a representative executed expression for each of the 99 names, registry/help checks, malformed/type cases and optional interpreter/LLVM comparisons; `seexpr_formatting` separately exercises bounded console/string formatting. Runtime results are recorded by the qualification jobs and reports listed below.

For each name, acceptance requires registry/help availability, representative numerical or string output, arity/type handling where applicable, and interpreter/backend parity when more than one backend is offered. Runtime host policy must distinguish a function that exists but is explicitly disallowed in a rendering context from a function silently absent from the port. Registry presence alone is not a semantic test.

| Function | Source line in `src/SeExpr2/ExprBuiltins.cpp` | Upstream platform/context note | Native parity status |
|---|---:|---|---|
| `abs` | 1724 | All upstream platforms | Implemented |
| `acos` | 1725 | All upstream platforms | Implemented |
| `asin` | 1726 | All upstream platforms | Implemented |
| `atan` | 1727 | All upstream platforms | Implemented |
| `atan2` | 1728 | All upstream platforms | Implemented |
| `ceil` | 1729 | All upstream platforms | Implemented |
| `cos` | 1730 | All upstream platforms | Implemented |
| `cosh` | 1731 | All upstream platforms | Implemented |
| `exp` | 1732 | All upstream platforms | Implemented |
| `floor` | 1733 | All upstream platforms | Implemented |
| `fmod` | 1734 | All upstream platforms | Implemented |
| `log` | 1735 | All upstream platforms | Implemented |
| `log10` | 1736 | All upstream platforms | Implemented |
| `pow` | 1737 | All upstream platforms | Implemented |
| `sin` | 1738 | All upstream platforms | Implemented |
| `sinh` | 1739 | All upstream platforms | Implemented |
| `sqrt` | 1740 | All upstream platforms | Implemented |
| `tan` | 1741 | All upstream platforms | Implemented |
| `tanh` | 1742 | All upstream platforms | Implemented |
| `cbrt` | 1744 | Guarded out on upstream Windows | Implemented |
| `asinh` | 1745 | Guarded out on upstream Windows | Implemented |
| `acosh` | 1746 | Guarded out on upstream Windows | Implemented |
| `atanh` | 1747 | Guarded out on upstream Windows | Implemented |
| `trunc` | 1748 | Guarded out on upstream Windows | Implemented |
| `deg` | 1759 | All upstream platforms | Implemented |
| `rad` | 1760 | All upstream platforms | Implemented |
| `cosd` | 1761 | All upstream platforms | Implemented |
| `sind` | 1762 | All upstream platforms | Implemented |
| `tand` | 1763 | All upstream platforms | Implemented |
| `acosd` | 1764 | All upstream platforms | Implemented |
| `asind` | 1765 | All upstream platforms | Implemented |
| `atand` | 1766 | All upstream platforms | Implemented |
| `atan2d` | 1767 | All upstream platforms | Implemented |
| `clamp` | 1770 | All upstream platforms | Implemented |
| `round` | 1771 | All upstream platforms | Implemented |
| `max` | 1772 | All upstream platforms | Implemented |
| `min` | 1773 | All upstream platforms | Implemented |
| `invert` | 1776 | All upstream platforms | Implemented |
| `compress` | 1777 | All upstream platforms | Implemented |
| `expand` | 1778 | All upstream platforms | Implemented |
| `fit` | 1779 | All upstream platforms | Implemented |
| `gamma` | 1780 | All upstream platforms | Implemented |
| `bias` | 1781 | All upstream platforms | Implemented |
| `contrast` | 1782 | All upstream platforms | Implemented |
| `boxstep` | 1783 | All upstream platforms | Implemented |
| `linearstep` | 1784 | All upstream platforms | Implemented |
| `smoothstep` | 1785 | All upstream platforms | Implemented |
| `gaussstep` | 1786 | All upstream platforms | Implemented |
| `remap` | 1787 | All upstream platforms | Implemented |
| `mix` | 1788 | All upstream platforms | Implemented |
| `hsi` | 1789 | All upstream platforms | Implemented |
| `midhsi` | 1790 | All upstream platforms | Implemented |
| `hsltorgb` | 1791 | All upstream platforms | Implemented |
| `rgbtohsl` | 1792 | All upstream platforms | Implemented |
| `saturate` | 1793 | All upstream platforms | Implemented |
| `hash` | 1796 | All upstream platforms | Implemented |
| `noise` | 1797 | All upstream platforms | Implemented |
| `snoise` | 1798 | All upstream platforms | Implemented |
| `vnoise` | 1799 | All upstream platforms | Implemented |
| `cnoise` | 1800 | All upstream platforms | Implemented |
| `snoise4` | 1801 | All upstream platforms | Implemented |
| `vnoise4` | 1802 | All upstream platforms | Implemented |
| `cnoise4` | 1803 | All upstream platforms | Implemented |
| `turbulence` | 1804 | All upstream platforms | Implemented |
| `vturbulence` | 1805 | All upstream platforms | Implemented |
| `cturbulence` | 1806 | All upstream platforms | Implemented |
| `fbm` | 1807 | All upstream platforms | Implemented |
| `vfbm` | 1808 | All upstream platforms | Implemented |
| `cfbm` | 1809 | All upstream platforms | Implemented |
| `cellnoise` | 1810 | All upstream platforms | Implemented |
| `ccellnoise` | 1811 | All upstream platforms | Implemented |
| `pnoise` | 1812 | All upstream platforms | Implemented |
| `fbm4` | 1813 | All upstream platforms | Implemented |
| `vfbm4` | 1814 | All upstream platforms | Implemented |
| `cfbm4` | 1815 | All upstream platforms | Implemented |
| `dist` | 1818 | All upstream platforms | Implemented |
| `length` | 1819 | All upstream platforms | Implemented |
| `hypot` | 1820 | All upstream platforms | Implemented |
| `dot` | 1821 | All upstream platforms | Implemented |
| `norm` | 1822 | All upstream platforms | Implemented |
| `cross` | 1823 | All upstream platforms | Implemented |
| `angle` | 1824 | All upstream platforms | Implemented |
| `ortho` | 1825 | All upstream platforms | Implemented |
| `rotate` | 1826 | All upstream platforms | Implemented |
| `up` | 1827 | All upstream platforms | Implemented |
| `cycle` | 1830 | All upstream platforms | Implemented |
| `pick` | 1831 | All upstream platforms | Implemented |
| `choose` | 1832 | All upstream platforms | Implemented |
| `wchoose` | 1833 | All upstream platforms | Implemented |
| `swatch` | 1834 | All upstream platforms | Implemented |
| `spline` | 1835 | All upstream platforms | Implemented |
| `voronoi` | 1839 | All upstream platforms | Implemented |
| `cvoronoi` | 1840 | All upstream platforms | Implemented |
| `pvoronoi` | 1841 | All upstream platforms | Implemented |
| `curve` | 1843 | All upstream platforms | Implemented |
| `ccurve` | 1844 | All upstream platforms | Implemented |
| `getVar` | 1845 | Dynamic variable/default behavior; dependency semantics required | Implemented |
| `printf` | 1846 | Console side effect; explicit host-context policy and controlled output test required | Implemented |
| `sprintf` | 1849 | String formatting; owned result and format/type tests required | Implemented |

The documentation must use this real registry plus native host bindings. Names mentioned by old examples, such as demo-defined `rand`, are not automatically core builtins. Additional OT functions such as image sampling and channel references require their own registered signatures/help and acceptance cases.

## Native authoring and control parity

| ID | Upstream surface / source | Native destination / acceptance contract | Status |
|---|---|---|---|
| U01 | Editor, short editor, popup editor; `UI/ExprEditor.*`, `ExprShortEdit.*`, `ExprDialog.*` | Native `SeExprEditor` is used by actual FX/animation fields; Apply/Revert, owner replacement, undo and draft validation in `seexpr_editor` | Implemented |
| U02 | Syntax highlighting and completion; `UI/ExprHighlighter.h`, `ExprCompletionModel.*` | Actual core/host catalog and variables drive completion/help; native syntax highlighter and source-mode handling | Implemented |
| U03 | Error and function help popups; `UI/ExprPopupDoc.*`, `ExprHelp.h` | UTF-8 error spans, actual function documentation, palette-based widgets and translated presentation; manual accessibility/theme review remains separate | Implemented |
| U04 | Expression browser; `UI/ExprBrowser.*`, `ExprFileDialog.*` | Native built-in/user browser, search, bounded JSON import/export and embedded scene documents; preset fixtures in `seexpr_editor`/`seexpr_fx` | Implemented |
| U05 | Numeric controls; `UI/Editable.h:60`, `ExprControl.h` | Stable control IDs, integer/range rules and native `TDoubleParam` animation; schema/clone/load fixtures | Implemented |
| U06 | Vector controls; `UI/Editable.h:101` | Named three-component controls with native component parameters; values/keyframes survive reorder, clone and schema changes | Implemented |
| U07 | Color controls and color linking; `UI/ExprColorSwatch.*`, `ExprControl.h` | Native color picker and `TPixelParam` linking/keyframes; negative/HDR doubles and unchanged-picker precision preserved; source color policy explicit | Implemented |
| U08 | String controls; `UI/Editable.h:134` | Literal string editing and annotation metadata retained; native and Python Qt file/directory pickers require explicit action, preserve cancel and remain drafts; no source-triggered file execution | Implemented |
| U09 | Scalar curves; `UI/ExprCurve.*`, `Editable.h:170` | Native point editor, five interpolation modes, actual engine preview and native stop position/value parameters; control/FX fixtures | Implemented |
| U10 | Color curves; `UI/ExprColorCurve.*` | Native color-stop editor, five interpolation modes, HDR values and native animated stops; control/FX fixtures | Implemented |
| U11 | Color swatches; `UI/Editable.h:274`, `ColorSwatchControl` | Native add/remove/reorder/edit swatches, exact uniform selection bins and endpoint; high-precision source/value fixtures | Implemented |
| U12 | Animation curve callback/preview; `UI/Editable.h:211`, `AnimCurveControl` | Owned inline/named curve model, complete key metadata, independent infinity policies, actual native graph preview, explicit linked draft refresh and Python Qt control; `seexpr_animcurve`, `seexpr_controls`, `seexpr_editor`, `seexpr_fx`, `seexpr_python_ui` | Implemented |
| U13 | Deep-water spectrum control; `UI/ExprDeepWater.*`, `Editable.h:327` | Native twelve-parameter spectrum editor and exact shared spectrum math (`tseexprspectrum.h`), matching upstream UI energy preview; no upstream fluid renderer exists in the core | Implemented |
| U14 | Control parsing and annotation comments; `UI/ExprSpecParser.y`, `EditableExpression.*`, `ControlSpec.*` | Native literal/range/string/curve/color-ramp/swatch annotation import with comments, stable occurrence/source IDs and computed-expression preservation; exact-value fixtures | Implemented |
| U15 | Grapher widgets and context examples; `UI/ExprGrapher2d.*`, `BasicExpression.*` | Native `Range Preview` samples an immutable real FX/animation context with scalar/vector graph, per-frame errors, fractional frames, 512-sample bound and cancel/owner-close guards; `seexpr_range`, `seexpr_editor` | Implemented |

The native editor has real named schema controls, curves, color ramps, swatches, annotation conversion and native animation migration. Range preview clones FX state once per dialog and freezes its draft; animation uses explicit sample frames, normalized segment time and native units. Numeric range preview does not render a full source image: its image-dependent callbacks report that a native FX preview is required, and Apply makes the normal FX preview available.

## Backends, SDK, bindings and external host adapters

| ID | Upstream component and evidence | Required disposition / acceptance | Status |
|---|---|---|---|
| B01 | Interpreter; `Interpreter.*`, `Expression.cpp` | Actual interpreter, worker-owned FX evaluators and language/builtin/SDK corpus; default backend is explicit and independent of environment | Implemented |
| B02 | LLVM JIT; `ExprLLVMCodeGeneration.cpp`, `ExprLLVM*.h`, `Evaluator.h` | Optional real LLVM14 emitter/runtime, explicit backend selection, no interpreter fallback under a JIT label; budget/string/callback/local-function/parity fixtures in `seexpr_llvm` and `seexpr_builtins` | Implemented |
| B03 | Custom function ABI; `ExprFunc.h`, `ExprFuncX.h`, `ExprFuncStandard.h` | Original SDK standard callback forms retained across interpreter/JIT, including zero-to-six scalar arguments, variadics, vector/scalar results, exceptions and lifetimes; shared standard callback corpus plus native host example | Implemented |
| B04 | Explicit plugin loading; `ExprFunc.cpp:217` | Explicit trusted SDK loader with canonical path deduplication, stable directory discovery and registry-before-module cleanup; `seexpr_sdk_plugins` | Implemented |
| B05 | Environment plugin discovery; `ExprFunc.cpp:158` | Inherited `SE_EXPR_PLUGINS` discovery deliberately disabled; explicit SDK loading remains available, and plugin fixture tests non-discovery | Implemented |
| B06 | Platform plugin behavior; `ExprFunc.cpp:255` | Native UTF-16 Windows loader and Unix dynamic loading implemented in `prepare_seexpr_sdk.py`; matching-compiler C++ ABI required; three-platform CI configured | Implemented |
| B07 | Python AST module; `src/py/SeExprPy.cpp` | Optional Python3 wrapper uses privately prepared licensed inspection parser; virtual node destruction, correct call/vector/unary ranges and retained AST/handle ownership; `seexpr_python_ast` | Implemented |
| B08 | Python helpers; `src/py/__init__.py`, `utils.py` | Independent Python3 helpers preserve Unicode comments/traversal/source edits; native evaluation/callback API is additional functionality; AST/evaluation fixtures | Implemented |
| B09 | Python/Qt editor bindings; `UI/SeExpr2Editor.sip`, `UI/__init__.py` | Independent `SeExprPy.editor`/`expreditor2` PyQt frontend supplies editor/dialog/browser/short editors and controls. Restricted upstream initializer is excluded. External PyQt required; Qt5 CI configured, Qt6 qualification pending | Implemented |
| B10 | Animation-library adapter; `SEEXPR_USE_ANIMLIB`, `UI/CE/*` | Native inline/named `animCurve`, all nine key fields, seven tangent/five infinity modes, weighted evaluation, explicit refresh, undo/clone/link/archives/presets/persistence. Historical licensed tangent regeneration and owned model replace the absent external evaluator; external binary/numerical-oracle compatibility remains unqualified | Implemented |
| B11 | Renderer adapter example; `src/demos/rman2/seop.cpp` | Worker-local programs, render-local attribute/option providers, full object/world/camera surface context, posed mesh displacement/normal recomputation, per-sample color/opacity and native lighting. Pure GLB and production native chain fixtures qualify the replacement; RenderMan SDK/ABI/adaptive dicing are not distributed | Implemented |
| B12 | Image synthesis/image editor demos; `src/demos/imageSynth/*`, `imageEditor/*` | Native Generator/Color/image sampling, controls, preset browser and installed examples replace standalone image-synthesis/editor host applications; `seexpr_fx`, `seexpr_native_batch` | Implemented |
| B13 | Graphing/calculator demos; `src/demos/asciiGraph.cpp`, `asciiCalculator.cpp`, `segraph*` | Installed `seexpr-calc`/`seexpr-graph` utilities plus native range graph; SDK calculator/grapher tests and `seexpr_range` | Implemented |
| B14 | Demo host functions and example formulas; `imageSynthForPaint3d.cpp`, example `.se` files | Selected installed presets use actual OT bindings and are validated. Demo-only rand/map/triplanar and host deepWater are not core builtins; external callback formulas require explicit bindings (see below) | Implemented |
| B15 | Documentation/utilities; `src/doc`, `src/utils`, tests | Installed native HTML help/presets/notices, actual function help and C++/Python SDK guides; source-backed language/function/host regression corpus | Implemented |

The LLVM option builds the actual pinned emitter with LLVM14 compatibility corrections; it does not relabel interpreter evaluation. It remains optional and requires compatible external development/runtime libraries. Qualification is configured for Linux, Windows and Intel macOS, including clean installed-SDK consumers. Windows uses a checksum-pinned LLVM source build; macOS uses the pinned official development SDK. A configured job is not a passing result and does not make JIT a default application shipping dependency.

The Python extension uses the separate licensed `src/SeExpr2/parser` scanner/template parser. Its AST inventory includes Module, Prototype, LocalFunction, Block, Node, IfThenElse, Vec, Cond, Compare, UnaryOp, BinaryOp, CompareEq, TernaryOp, Def, Subscript, Var, Num, Call, Assign and String. The checked `seexprpython/prepare_parser.py` step retains source notices while correcting virtual destruction and ordinary-call/vector/unary source ranges in private build copies. Its independent wrapper/helpers and optional Qt frontend are packaged separately; Python is not embedded into OT. No restrictive upstream Qt initializer is copied or installed; that initializer contains no executable functionality. The audited upstream Qt bindings support Qt4/Qt5, so PyQt6 compatibility is an additional configuration, not a missing upstream generation. Qt5 qualification does not certify Qt6.

External binary compatibility and native behavior are separate questions. B10/B11 now have native implementations of their visible animation and renderer workflows; the feature-by-feature mapping below records their exact contract. RenderMan and external animlib binaries are not distributed. SDK extensions preserve the C++ source/registration API and must be rebuilt against the supplied headers/library with a compatible toolchain; private-core layout corrections do not establish compatibility with binaries built against unmodified upstream headers. They are not a cross-toolchain C ABI. Scene source cannot select or discover executable modules.

### External-host feature reconciliation

| Upstream behavior | Native replacement | Remaining difference |
|---|---|---|
| Animation-channel binding/refresh; `UI/ExprControl.cpp:626` | Literal `channel` references update through dependency observers; explicit native/Python callbacks replace inline/named curve draft snapshots | Native snapshot conversion rejects offset layers, stepped timing, unsupported native segments and unqualified unit converters instead of approximating them; live `channel` evaluation retains these native behaviors |
| Animation graph; `UI/ExprControl.cpp:551` | Actual owned-model draft graph and native range preview sample fractional frames, errors and cancellation | No external animlib UI binary is required |
| Weighted keyframe editing | All nine key fields, seven tangent modes, functional weight locks, stable key identity, weighted Bezier evaluation and native animatable numeric parameters | Model has explicit finite-time/overflow bounds; external animlib duplicate-key/numerical-oracle compatibility is unqualified |
| Inline curve persistence; `UI/ExprSpecParser.y:174`, `Editable.h:241` | Literal importer/editor retains lookup/comments/metadata; independent five-mode pre/post policies, archives, clone/link/presets/scene save/load and one-edit Apply | Artist UI permits 64 keys; inline runtime/SDK model permits 512; invalid or unsorted keys report errors |
| Renderer compilation/cache, named variables and batching; `rman2/seop.cpp` | Worker-local FX evaluators, typed SDK bindings, `evaluateBatch` and original SDK `evalMultiple` | RenderMan numeric handles/grid ABI are not supplied |
| Color/opacity and invalid numeric results; `rman2/testse.sl` | Native RGB, scalar Alpha and vector Opacity RGB formulas; component-wise premultiplication and independent RGB transmittance within the 3D geometry renderer; explicit finite-result errors | Final OT RGBA uses maximum accumulated channel coverage as its single alpha, so colored transmission into a later unrelated 2D background is not representable. Include that background in the geometry render. Invalid output is rejected rather than replaced with white |
| Automatic namespaced option/attribute resolution; `rman2/seop.cpp:181` | Animated native Attribute providers snapshot namespaced scalar/vector settings; scalar values promote to three equal components; Attribute overrides Option, nearer providers win within each scope, missing values are zero; identities participate in caches | Scene data is explicit native render data, rather than a query into a vendor renderer |
| Object/world positions, normals and diffuse/ambient lighting; `rman2/testse.sl:26` | Expression Surface receives posed object/world/camera positions/normals, material color/opacity and UV; native lighting integrates with clipped perspective-correct sample shading | Native GLB/Image Plane triangle renderer and existing lighting replace the RenderMan shading context |
| Surface displacement and normal recomputation; `rman2/testdisp.sl:20` | Scalar source displaces posed vertices before projection, bounds/cache use displaced geometry, normals recompute and actual samples shade the result | Existing triangle topology, without adaptive RenderMan dicing; Image Plane displacement uses material color and its shading uses actual texels |

The pinned tree references external animlib and `UI/CE` sources which it does not contain. The native model records the historical licensed tangent-generation source separately and preserves the visible inline metadata. An external evaluator is unavailable as a numerical oracle; exact external alias/duplicate-key behavior and binary compatibility cannot be certified. Direct RenderMan plugin qualification requires vendor headers (`RslPlugin.h`, `rx.h`, `RixInterfaces.h`), a compatible renderer library, shader compiler, runtime and entitlement; these are unavailable in this environment. Native equivalents do not require them, and no vendor shader/plugin execution is claimed.

`imageSynthForPaint3d.cpp` defines demo-local `rand`, `map` and `triplanar` placeholders, not a texture engine: its fixed/fallback return values do not implement production randomness or texture loading. OT uses the actual core noise/hash functions and bounded source sampling instead. Formula files requiring those demo callbacks need explicit host bindings; unbound examples are not advertised as ready-to-use presets. Likewise the upstream deep-water widget authors a spectrum and references a host-provided `deepWater` function; the core has no fluid simulation/vector renderer to copy. The native spectrum editor preserves the actual spectrum math and exposes a native spectrum curve without claiming a fluid renderer.

## OT integration completion

| ID | Host feature | Required evidence | Status |
|---|---|---|---|
| O01 | Expression Generator FX | `stdfx/seexprfx.cpp`: native registration, finite canvas and exact alias/cache data; `seexpr_fx` covers 8/16/float, tiling, transforms and controls | Implemented |
| O02 | Expression Color FX | Source/optional Mask/Mix, explicit working-space and premultiplied policy, float HDR and missing-input fixtures in `seexpr_fx` | Implemented |
| O03 | Bounded image sampling | Declared sampling radius, nearest/bilinear, transparent/clamp/repeat/mirror, transformed input requests and tile/cancel fixtures | Implemented |
| O04 | Named animatable controls | Native scalar/vector/color/string/curve parameters, stable IDs, archive/regrow and schema migration; actual editor one-transaction undo fixtures | Implemented |
| O05 | Scene/preset persistence | Versioned embedded source/schema, archive/registry, native clone/preset/link/save/reopen fixtures; invalid versions retain source and fail validation | Implemented |
| O06 | Animation dialect | Explicit native/SeExpr modes and versioned tags; degree/radian, live scene fps, units/fractional frames, Apply/Revert/persistence fixtures | Implemented |
| O07 | Reference lifecycle | Native bindings/observers/cycles; snapshot-preserving Column/Stage Schematic paste before/between/after originals; empty/hidden curves, copied owners, linked FX undo/redo and foreign-context isolation; actual column/sub-xsheet operations in `seexpr_animation`, `seexpr_native_application` | Implemented |
| O08 | Batch rendering | Saved scene → actual tcomposer → decoded output; exact failed/completed frame callbacks for first/middle/last failures, range/step and multimedia cases; later valid frames retained and failed jobs return nonzero without a success summary; `seexpr_native_batch` | Implemented |
| O09 | Native UI lifecycle | Actual native field/editor interaction, draft isolation, immutable range preview, owner deletion/cancel, text diagnostics and undo fixtures; manual focus/tablet/theme qualification remains | Implemented |
| O10 | Platform/package qualification | Windows/Linux/Intel macOS interpreter/LLVM14/SDK/Python and native application/batch qualification configured; PyQt6 Linux. Help/presets/notices and clean installed SDK/header consumers included. Required results come from the final platform jobs; physical/manual observations are recorded separately | Implemented |
| O11 | Performance/resource qualification | Source/AST-depth/instruction/string bounds, nested budgets, cancel and reentry fixtures. `seexpr_fx_test --benchmark` measures actual 1080p/4K Generator with 1/4 workers, cold/warm frames, pixel checksums and peak process RSS; Linux CI measurement/report configured | Implemented |
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
| Generated parser corpus | `seexpr_parser_fuzz` | Fixed-seed grammar and mutation candidates, independent arithmetic oracles, raw SDK/native parsing, bounded diagnostics and valid-expression recovery through reused evaluators |
| Memory/undefined behavior | Dedicated `Linux ASan and UBSan` qualification job | Instrumented core, parser, adapter, SDK helpers/plugins and deterministic corpus; no instrumented production SDK is published |
| Optional JIT | `seexpr_llvm`, LLVM execution in builtin/runtime corpora | Actual emitter identity, numerical parity, strings, callbacks, locals, cancellation/recovery |
| SDK/install | `seexpr_sdk_plugins`, `seexpr_sdk_host_callbacks`, `seexpr_sdk_calculator`, `seexpr_sdk_grapher`, clean installed consumer | Explicit loading/non-discovery, cleanup, utilities, installed headers/archive/CMake package |
| Python | `seexpr_python_ast`, `seexpr_python_eval`, `seexpr_python_ui` | Built-package imports, corrected AST byte ranges/ownership/Unicode edits, native typed callbacks/evaluation, real optional Qt widgets and HDR/precision-preserving source edits |
| Native FX | `seexpr_fx` | 8/16/float pixels, tiles/transforms, source/mask, sampling, controls/HDR, schema/cache/clone/preset/link/load and worker failures |
| Native animation curves | `seexpr_animcurve`, `seexpr_native_animcurve`, controls/editor/FX/Python fixtures | Seven tangents/five infinity modes, weighted/empty/fractional/boundary cases, inline interpreter/JIT, complete metadata authoring, refresh/cancel, native numeric tracks/key identity/archives and installed model |
| Native surface | `seexpr_glb_surface`, `seexpr_surface_fx` | Actual posed geometry/displacement/normals, clipping/perspective, transparent overlap, tile/worker agreement, native GLB/Transform/ImagePlane/provider chains, caches/persistence/errors/cancellation; pure renderer also runs under Linux sanitizers |
| Animation | `seexpr_animation` | Legacy semantics, live fps/time, literal channel bindings/observers/cycles, versioned save/reopen/undo, copy/assignment owners after source destruction and bounded concurrent corrupt-cycle failure |
| Native authoring | `seexpr_controls`, `seexpr_editor`, `seexpr_range` | Actual controls/dialogs, annotations, bound-control editing without false name collisions, unchanged HDR picker precision, owner replacement/undo, immutable numeric ranges, fractional/native channel samples and cancel/owner deletion |
| Native application | `seexpr_native_application` | Actual visible startup, Column/Stage Schematic paste with snapshot/owner preservation, empty columns, linked FX redo, second-sheet core reference isolation, column/sub-xsheet operations and undo/redo using disposable data |
| Installed application | Reusable installed-package qualification in each native platform workflow | Separate fresh runner downloads that run's shipped artifact, installs no Qt SDK, launches actual packaged AppRun/application/bundle with packaged data, clears development library/plugin paths, runs the native lifecycle, and checks temporary renderer/test-driver cleanup |
| Batch | `seexpr_native_batch` | Saved scene/decoded output; exact completed/failed frame numbers for early/middle/late failures, range/step and multimedia cases; nonzero diagnostics and no false success summary |
| Full-resolution measurements | `seexpr_fx_test --benchmark` | 1920×1080/3840×2160 Generator, one/four workers, cold/warm-frame timings, worker-count image checksum equality and process peak RSS; Linux CI report artifact |

Actual passing evidence comes from **SeExpr qualification** jobs **Linux interpreter**, **Windows interpreter**, **macOS interpreter**, **Linux LLVM 14**, **Windows LLVM 14**, **macOS LLVM 14** and **Linux ASan and UBSan**, including their test logs/JUnit reports and clean installed-SDK consumer `seexpr-sdk-tests.log` for the ordinary backend jobs. Native application/FX/animation/batch results come from **Test SeExpr native integration** in **Linux Build**, **Windows Build** and **MacOS Build**, with `seexpr-tests.xml`, `seexpr-tests-output.txt`, `seexpr-native-output.txt` and `seexpr-native-batch-output.txt`. **Measure SeExpr full-resolution rendering** in **Linux Build** records `seexpr-performance.txt`. A report must correspond to the final revision and requested backend/platform before it counts as acceptance evidence.

Clipboard snapshots deep-copy stage, offset and plastic curves while retaining native undo identities on restore. Mapping applies once to destination-owned curves; original and externally shared curves retain their bindings. Saved stable FX-link representatives restore linked groups after undo without replaying cascading ring edges. Reference-only models include hidden/empty numeric channels and do not queue GUI updates after their lifetime ends.

Intentionally retained foreign FX links use their original native source context and its lifetime. They do not become destination-owned reference-monitor entries when pasted. Independent, unlinked FX copies avoid that shared source context.

Animation graph evaluation currently uses a recursive graph-wide gate before per-curve locks, retaining cycle detection while avoiding opposing-curve lock deadlocks. It serializes this graph path; FX workers still own separate evaluators. Representative animation contention/performance evidence is required before making speed claims.

Assess all required CI at the completed implementation revision. The native application fixture launches the real window system and native commands, complementing focused offscreen widgets and batch renders. Its test-only startup requires disposable copied data and isolates settings, caches and scene assets; it is not a normal end-user launch mode. Windows qualification supplies a checksum-pinned temporary Mesa desktop renderer and removes it after the fixture; this is not an installed application dependency.

The full-resolution benchmark records cold/warm-frame times and process peak resident memory for one Generator per worker. A separate resident-node phase retains 64 independently animated effects through eight source revisions and 1,024 rendered frames, verifying native parameter identity/keyframes and analytic output during cache eviction. Its memory samples are cumulative process peaks, including the sample after effects are released; they are not live allocations or leak proof. Neither workload is a connected production scene, a GPU measurement, per-expression heap accounting or a hardware minimum. There are no speed thresholds or hardware results claimed by the source inventory. Installed-package qualification now configures actual clean-runner application launches; the final passing job results remain required evidence. Production-scene memory/animation contention and physical installed-desktop/tablet/theme/hardware observations require their own evidence and remain unperformed here.

## Readiness rule

The port is ready only after every required native, core, SDK, backend and platform acceptance check passes at the final implementation revision. An Implemented row, a passing subset, or an older run cannot satisfy that gate. Required failures must be diagnosed and fixed before readiness is claimed. Manual desktop/hardware and PyQt6 claims require their own evidence. External-host equivalents and optional dependency boundaries above describe the actual port; they do not imply external binary interfaces or unperformed qualification.

U12/B10/B11 now have concrete native source implementations and acceptance fixtures, including the explicit renderer output boundaries above. Clean-package runtime and every other configured required check must pass at the final revision. The unavailable vendor SDK/runtime, missing external animation numerical oracle and physical GPU/tablet/manual desktop observations must be identified explicitly; automated fixtures do not establish those observations or external binary compatibility.
