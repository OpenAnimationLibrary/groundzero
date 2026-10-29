# SeExpr port parity checklist

This checklist is the completion gate for the full port. A linked evaluator or a successful effect preset does not establish full upstream parity. It records the audited upstream surface and the native host work needed to make it usable in Groundzero.

Upstream snapshot: `8f8c8f2c5e27e96fae70d6b82ac1ff4f4811d6dc`.
Initial Groundzero base: `3a2b9f6dc56a55a9f04f432dc00241020397b6e4`.

Status definitions:

- **Pending**: implementation, native equivalent, packaging or required acceptance evidence is missing. Assigned work remains Pending until concrete code exists.
- **Implemented / CI pending**: concrete code and meaningful acceptance cases exist, but required CI has not passed.
- **Verified**: the named acceptance cases passed on the supported platform matrix. Record the relevant test names when changing a row to Verified.
- **Scope decision required**: the upstream component targets another host or has an unresolved dependency/license question. This is not an automatic exclusion. It must receive an explicit disposition before declaring a full port.

This is an initial source inventory while implementation proceeds. No local builds or tests were run to produce it. Every row below begins Pending unless otherwise stated; feature authors and integration review must update the evidence before readiness.

## Language and evaluation

Source paths in this document are upstream paths unless identified as native.

| ID | Upstream surface and source evidence | Native destination / acceptance contract | Status |
|---|---|---|---|
| L01 | Numeric literals, scalar FP values; `ExprParser.y`, `ExprType.h` | Native evaluator returns scalar values; numeric boundary and fractional cases | Pending |
| L02 | Vector literals and subscripts; `ExprParser.y:305` | Internal arbitrary `float[N]` and external/result dimensions must be distinguished. A three-component color wrapper alone does not cover every upstream vector dimension | Pending |
| L03 | Strings, escape handling and string results; `ExprParserLex.l`, `Expression.h:192` | Owned string results; Unicode/escape, empty and repeated evaluation cases | Pending |
| L04 | Optional `$` prefixes and host variables; `ExprParser.y:132` | Typed external binding, unresolved variables and repeated value updates | Pending |
| L05 | Arithmetic `+ - * / % ^`; `ExprParser.y:322` | Operator precedence, scalar/vector promotion and finite-result policy | Pending |
| L06 | Unary `+ - ! ~`; `ExprParser.y:318` | Native interpreter parity for every unary operator | Pending |
| L07 | Comparisons/equality and logical AND/OR; `ExprParser.y:310` | Truth values, string/numeric distinctions and branch behavior | Pending |
| L08 | Ternary conditions; `ExprParser.y:309` | Scalar/vector/string result typing and selected-branch evaluation | Pending |
| L09 | Assignment and compound assignment; `ExprParser.y:245` | Both name forms, all six compound arithmetic operators and statement order | Pending |
| L10 | If/else/else-if assignment blocks; `ExprParser.y:293` | Branch-local assignments, missing branch and result typing | Pending |
| L11 | Modules, blocks and declaration lists; `ExprParser.y:135` | Multi-statement formulas preserve source and evaluate in order | Pending |
| L12 | Local typed/inferred functions; `ExprParser.y:153` | Arguments, return type inference, nested calls, separate worker stacks and diagnostic parity | Pending |
| L13 | External function prototypes; `ExprParser.y:153` | Resolve supported typed host callbacks; unresolved prototype gives useful error | Pending |
| L14 | Function calls and arrow-call syntax; `ExprParser.y:328` | Fixed and variadic arguments, receiver placement and type errors | Pending |
| L15 | Constant/uniform/varying lifetimes; `ExprParser.y:174`, `ExprType.h` | Native binding lifetime metadata plus invalid lifetime rejection. Varying-only wrappers do not establish full API coverage | Pending |
| L16 | Desired return type/type checking; `Expression.h:113` | Scalar, vector and string modes have explicit output requirements | Pending |
| L17 | Source offsets, comments and syntax/type diagnostics; `Expression.h:87` | UTF-8 byte offsets mapped correctly to native editor positions | Pending |
| L18 | `usesVar`, `usesFunc`, constant detection; `Expression.h:142` | Dependency queries support native preview/cache/reference integration | Pending |
| L19 | Context parameters and custom variable/function resolution; `Expression.h:198` | Typed native context/callback API, correct callback lifetime and error handling | Pending |
| L20 | `evalMultiple`/VarBlock array evaluation; `Expression.h:182`, `VarBlock.h` | A qualified batch API or documented native equivalent with numerical/ownership tests | Pending |
| L21 | Multi-expression dependency graph; `ExprMultiExpr.h` | Native equivalent covers external variables, dependent expressions, invalidation and graph evaluation | Pending |
| L22 | Core curves/noise/vector helper APIs; `Curve.h`, `Noise.h`, `Vec.h` | Required embedded math remains available to native controls/SDK; ownership and dimensional behavior qualified | Pending |

General user `for`/`while` loops and recursive local functions are not supplied by the audited upstream grammar. Their absence is not a port omission. Bounded source and resource policies still require host acceptance tests.

## Builtin function registry

The following table is extracted statically from `ExprBuiltins.cpp::defineBuiltins` at the pinned snapshot: **99 registered names**. Five are guarded by `#ifndef SEEXPR_WIN32` upstream, yielding 94 names in its unmodified Windows branch. Cross-platform port disposition must address those five explicitly.

For each name, acceptance requires registry/help availability, representative numerical or string output, arity/type handling where applicable, and interpreter/backend parity when more than one backend is offered. Runtime host policy must distinguish a function that exists but is explicitly disallowed in a rendering context from a function silently absent from the port. Registry presence alone is not a semantic test.

| Function | Source line in `src/SeExpr2/ExprBuiltins.cpp` | Upstream platform/context note | Native parity status |
|---|---:|---|---|
| `abs` | 1724 | All upstream platforms | Pending |
| `acos` | 1725 | All upstream platforms | Pending |
| `asin` | 1726 | All upstream platforms | Pending |
| `atan` | 1727 | All upstream platforms | Pending |
| `atan2` | 1728 | All upstream platforms | Pending |
| `ceil` | 1729 | All upstream platforms | Pending |
| `cos` | 1730 | All upstream platforms | Pending |
| `cosh` | 1731 | All upstream platforms | Pending |
| `exp` | 1732 | All upstream platforms | Pending |
| `floor` | 1733 | All upstream platforms | Pending |
| `fmod` | 1734 | All upstream platforms | Pending |
| `log` | 1735 | All upstream platforms | Pending |
| `log10` | 1736 | All upstream platforms | Pending |
| `pow` | 1737 | All upstream platforms | Pending |
| `sin` | 1738 | All upstream platforms | Pending |
| `sinh` | 1739 | All upstream platforms | Pending |
| `sqrt` | 1740 | All upstream platforms | Pending |
| `tan` | 1741 | All upstream platforms | Pending |
| `tanh` | 1742 | All upstream platforms | Pending |
| `cbrt` | 1744 | Guarded out on upstream Windows | Pending |
| `asinh` | 1745 | Guarded out on upstream Windows | Pending |
| `acosh` | 1746 | Guarded out on upstream Windows | Pending |
| `atanh` | 1747 | Guarded out on upstream Windows | Pending |
| `trunc` | 1748 | Guarded out on upstream Windows | Pending |
| `deg` | 1759 | All upstream platforms | Pending |
| `rad` | 1760 | All upstream platforms | Pending |
| `cosd` | 1761 | All upstream platforms | Pending |
| `sind` | 1762 | All upstream platforms | Pending |
| `tand` | 1763 | All upstream platforms | Pending |
| `acosd` | 1764 | All upstream platforms | Pending |
| `asind` | 1765 | All upstream platforms | Pending |
| `atand` | 1766 | All upstream platforms | Pending |
| `atan2d` | 1767 | All upstream platforms | Pending |
| `clamp` | 1770 | All upstream platforms | Pending |
| `round` | 1771 | All upstream platforms | Pending |
| `max` | 1772 | All upstream platforms | Pending |
| `min` | 1773 | All upstream platforms | Pending |
| `invert` | 1776 | All upstream platforms | Pending |
| `compress` | 1777 | All upstream platforms | Pending |
| `expand` | 1778 | All upstream platforms | Pending |
| `fit` | 1779 | All upstream platforms | Pending |
| `gamma` | 1780 | All upstream platforms | Pending |
| `bias` | 1781 | All upstream platforms | Pending |
| `contrast` | 1782 | All upstream platforms | Pending |
| `boxstep` | 1783 | All upstream platforms | Pending |
| `linearstep` | 1784 | All upstream platforms | Pending |
| `smoothstep` | 1785 | All upstream platforms | Pending |
| `gaussstep` | 1786 | All upstream platforms | Pending |
| `remap` | 1787 | All upstream platforms | Pending |
| `mix` | 1788 | All upstream platforms | Pending |
| `hsi` | 1789 | All upstream platforms | Pending |
| `midhsi` | 1790 | All upstream platforms | Pending |
| `hsltorgb` | 1791 | All upstream platforms | Pending |
| `rgbtohsl` | 1792 | All upstream platforms | Pending |
| `saturate` | 1793 | All upstream platforms | Pending |
| `hash` | 1796 | All upstream platforms | Pending |
| `noise` | 1797 | All upstream platforms | Pending |
| `snoise` | 1798 | All upstream platforms | Pending |
| `vnoise` | 1799 | All upstream platforms | Pending |
| `cnoise` | 1800 | All upstream platforms | Pending |
| `snoise4` | 1801 | All upstream platforms | Pending |
| `vnoise4` | 1802 | All upstream platforms | Pending |
| `cnoise4` | 1803 | All upstream platforms | Pending |
| `turbulence` | 1804 | All upstream platforms | Pending |
| `vturbulence` | 1805 | All upstream platforms | Pending |
| `cturbulence` | 1806 | All upstream platforms | Pending |
| `fbm` | 1807 | All upstream platforms | Pending |
| `vfbm` | 1808 | All upstream platforms | Pending |
| `cfbm` | 1809 | All upstream platforms | Pending |
| `cellnoise` | 1810 | All upstream platforms | Pending |
| `ccellnoise` | 1811 | All upstream platforms | Pending |
| `pnoise` | 1812 | All upstream platforms | Pending |
| `fbm4` | 1813 | All upstream platforms | Pending |
| `vfbm4` | 1814 | All upstream platforms | Pending |
| `cfbm4` | 1815 | All upstream platforms | Pending |
| `dist` | 1818 | All upstream platforms | Pending |
| `length` | 1819 | All upstream platforms | Pending |
| `hypot` | 1820 | All upstream platforms | Pending |
| `dot` | 1821 | All upstream platforms | Pending |
| `norm` | 1822 | All upstream platforms | Pending |
| `cross` | 1823 | All upstream platforms | Pending |
| `angle` | 1824 | All upstream platforms | Pending |
| `ortho` | 1825 | All upstream platforms | Pending |
| `rotate` | 1826 | All upstream platforms | Pending |
| `up` | 1827 | All upstream platforms | Pending |
| `cycle` | 1830 | All upstream platforms | Pending |
| `pick` | 1831 | All upstream platforms | Pending |
| `choose` | 1832 | All upstream platforms | Pending |
| `wchoose` | 1833 | All upstream platforms | Pending |
| `swatch` | 1834 | All upstream platforms | Pending |
| `spline` | 1835 | All upstream platforms | Pending |
| `voronoi` | 1839 | All upstream platforms | Pending |
| `cvoronoi` | 1840 | All upstream platforms | Pending |
| `pvoronoi` | 1841 | All upstream platforms | Pending |
| `curve` | 1843 | All upstream platforms | Pending |
| `ccurve` | 1844 | All upstream platforms | Pending |
| `getVar` | 1845 | Dynamic variable/default behavior; dependency semantics required | Pending |
| `printf` | 1846 | Console side effect; explicit host-context policy and controlled output test required | Pending |
| `sprintf` | 1849 | String formatting; owned result and format/type tests required | Pending |

The documentation must use this real registry plus native host bindings. Names mentioned by old examples, such as demo-defined `rand`, are not automatically core builtins. Additional OT functions such as image sampling and channel references require their own registered signatures/help and acceptance cases.

## Native authoring and control parity

| ID | Upstream surface / source | Native destination / acceptance contract | Status |
|---|---|---|---|
| U01 | Editor, short editor, popup editor; `UI/ExprEditor.*`, `ExprShortEdit.*`, `ExprDialog.*` | Native multiline editing, keyboard/focus, Apply/Revert, undo, validation and useful preview | Pending |
| U02 | Syntax highlighting and completion; `UI/ExprHighlighter.h`, `ExprCompletionModel.*` | Actual variables/functions, dialect-aware highlighting and accurate documentation | Pending |
| U03 | Error and function help popups; `UI/ExprPopupDoc.*`, `ExprHelp.h` | Error ranges, help access, theme/translation/accessibility integration | Pending |
| U04 | Expression browser; `UI/ExprBrowser.*`, `ExprFileDialog.*` | Built-in/user preset browsing/search, save/import/export and portable scene embedding | Pending |
| U05 | Numeric controls; `UI/Editable.h:60`, `ExprControl.h` | Range/default/step/integer policy, native scalar keyframes and stable IDs | Pending |
| U06 | Vector controls; `UI/Editable.h:101` | Component editing, ranges and native vector/component animation | Pending |
| U07 | Color controls and color linking; `UI/ExprColorSwatch.*`, `ExprControl.h` | Native color selection/link behavior with explicit color/alpha semantics | Pending |
| U08 | String controls; `UI/Editable.h:134` | Literal editing, escaping and portable semantics; no accidental file execution | Pending |
| U09 | Scalar curves; `UI/ExprCurve.*`, `Editable.h:170` | Interactive curve/ramp editing, interpolation modes, save/reopen and undo | Pending |
| U10 | Color curves; `UI/ExprColorCurve.*` | Color-stop editing/interpolation, native persistence and undo | Pending |
| U11 | Color swatches; `UI/Editable.h:274`, `ColorSwatchControl` | Add/remove/reorder/edit palette stops, formula update and undo | Pending |
| U12 | Animation curve callback/preview; `UI/Editable.h:211`, `AnimCurveControl` | Native Function Editor integration and curve lookup with dependency tracking. External animlib-specific integration requires explicit disposition | Pending |
| U13 | Deep-water spectrum control; `UI/ExprDeepWater.*`, `Editable.h:327` | Native parameter/spectrum editor equivalent or explicit upstream-module scope decision | Pending |
| U14 | Control parsing and annotation comments; `UI/ExprSpecParser.y`, `EditableExpression.*`, `ControlSpec.*` | Preserved comment/control metadata, stable source edits and schema migration | Pending |
| U15 | Grapher widgets and context examples; `UI/ExprGrapher2d.*`, `BasicExpression.*` | Formula/value/range preview using the actual host context and cancellation | Pending |

A native editor may replace upstream widgets, but replacement alone is not evidence of equivalent controls. Fixed generic sliders do not cover named schema controls, curves, color ramps, swatches, annotation parsing or migration of animated controls.

## Backends, SDK, bindings and external host adapters

| ID | Upstream component and evidence | Required disposition / acceptance | Status |
|---|---|---|---|
| B01 | Interpreter; `Interpreter.*`, `Expression.cpp` | Qualified native core evaluator; per-worker ownership; full language corpus | Pending |
| B02 | LLVM JIT; `ExprLLVMCodeGeneration.cpp`, `ExprLLVM*.h`, `Evaluator.h` | Real backend selection, selected LLVM version, generated-code/function/context/string/batch behavior, interpreter parity and package startup | Pending |
| B03 | Custom function ABI; `ExprFunc.h`, `ExprFuncX.h`, `ExprFuncStandard.h` | Typed callbacks, variadics, constant preparation, thread-safety declaration and correct cleanup | Pending |
| B04 | Explicit plugin loading; `ExprFunc.cpp:217` | Trusted SDK loading API and cross-platform behavior; source-scene discovery policy explicit | Pending |
| B05 | Environment plugin discovery; `ExprFunc.cpp:158` | Deliberate host policy. Disabling inherited discovery is a context restriction, not native plugin ABI parity | Pending |
| B06 | Platform plugin behavior; `ExprFunc.cpp:255` | Upstream Windows loader is absent; do not claim Windows binary-plugin compatibility without implementation | Pending |
| B07 | Python AST module; `src/py/SeExprPy.cpp` | Python `AST`, `ASTHandle`, `ASTType`, child/value/range ownership and error behavior | Pending |
| B08 | Python helpers; `src/py/__init__.py`, `utils.py` | Unicode constructor, comment extraction, traversal callbacks/filters, source edits and package import tests | Pending |
| B09 | Python/Qt editor bindings; `UI/SeExpr2Editor.sip`, `UI/__init__.py` | Separate dependency/license scope decision; restrictive UI initializer must not be redistributed without authorization | Scope decision required |
| B10 | Animation-library adapter; `SEEXPR_USE_ANIMLIB`, `UI/CE/*` | Native Function Editor equivalent must cover needed artist behavior; proprietary/external animlib ABI is not supplied automatically | Scope decision required |
| B11 | Renderer adapter example; `src/demos/rman2/seop.cpp` | Targets an external renderer and old SeExpr include/API names. Map useful functions into OT or explicitly resolve renderer-specific binary scope | Scope decision required |
| B12 | Image synthesis/image editor demos; `src/demos/imageSynth/*`, `imageEditor/*` | Native Generator/Color/image-sampling FX and editor preserve useful capabilities; standalone executable disposition recorded | Pending |
| B13 | Graphing/calculator demos; `src/demos/asciiGraph.cpp`, `asciiCalculator.cpp`, `segraph*` | Native formula/range preview and SDK examples, or separate standalone utility disposition | Pending |
| B14 | Demo host functions and example formulas; `imageSynthForPaint3d.cpp`, example `.se` files | Inventory host dependencies per example; only qualified/licensed examples advertised as usable | Pending |
| B15 | Documentation/utilities; `src/doc`, `src/utils`, tests | Native supported-function help, build/SDK documentation, usable examples and upstream regression coverage | Pending |

LLVM source qualification is real work: the audited code uses MCJIT/legacy pass managers and several older IRBuilder overloads. Merely finding a system LLVM library or returning interpreter results under a JIT label is insufficient. A developer-only backend option does not establish shipped backend availability.

The upstream Python extension is an AST parser/editing interface, not an expression evaluation binding. It uses a separate parser under `src/SeExpr2/parser`; its `ASTType` inventory includes Module, Prototype, LocalFunction, Block, Node, IfThenElse, Vec, Cond, Compare, UnaryOp, BinaryOp, CompareEq, TernaryOp, Def, Subscript, Var, Num, Call, Assign and String. Native JavaScript/QJSEngine scripting can be useful independently but is not Python import/API parity.

The trusted native plugin SDK needs explicit path/API ownership, callback lifetime, registration/help visibility, version mismatch and unload behavior. The upstream loader's existing `.so` Unix convention and absent Windows implementation must be recorded accurately. Scene expressions must not trigger uncontrolled plugin discovery just to claim SDK parity.

## OT integration completion

| ID | Host feature | Required evidence | Status |
|---|---|---|---|
| O01 | Expression Generator FX | Native registration/editor/finite canvas; 8/16/float; tiled/untiled; transform/shrink; source/schema/control/time cache identity | Pending |
| O02 | Expression Color FX | Source/Mask/Mix, RGB/alpha/color-space/HDR policy, transparency and missing-input tests | Pending |
| O03 | Bounded image sampling | Declared reach, filter/edge modes, transform-aware input requests, tile equivalence and cancellation | Pending |
| O04 | Named animatable controls | Stable ID vs display/variable names, migration, native curves and one-transaction undo | Pending |
| O05 | Scene/preset persistence | Source/schema/engine/function-set versioning, portable values, clone/linked-copy/macro round-trip and unsupported-version failure | Pending |
| O06 | Animation dialect | Explicit engine identity, legacy semantics, units/frame/fps/fractional samples, valid UI edit/apply/revert | Pending |
| O07 | Reference lifecycle | Direct/indirect/mixed cycles, dependency notifications, rename/reorder/delete, clipboard and sub-Xsheet operations | Pending |
| O08 | Batch rendering | Same semantics as preview; invalid committed expressions and unsupported versions produce failed renders/nonzero results | Pending |
| O09 | Native UI lifecycle | Focus/keyboard, themes, translated presentation, source/ID literal preservation, close/cancel/stale preview behavior | Pending |
| O10 | Platform/package qualification | Supported Windows/Linux/macOS core/UI/batch checks; no developer runtime dependency; notices/presets installed | Pending |
| O11 | Performance/resource qualification | Representative cold/warm 1080p/4K, many-node memory, bounded work and cancel responsiveness | Pending |
| O12 | Editable vector style/geometry extensions | Separate host topology/palette/region/undo/persistence work; rendered vector FX alone does not satisfy these extensions | Scope decision required |

## License and source-selection gates

- Root `LICENSE` and most core/UI sources carry Apache2 with an explicit replacement trademark section. Preserve the exact text and required notices; do not describe it as unmodified Apache2.
- `src/py/__init__.py` carries the main modified-Apache terms. It is distinct from `src/SeExpr2/UI/__init__.py`, whose restrictive proprietary/confidential header requires exclusion or authorization before redistribution.
- Generated parser/scanner files have their own generator notices/exceptions. Keep these with the selected files and record generated-source provenance.
- External renderer headers/libraries and optional animation/UI integrations need a selected-file dependency/license inventory; a top-level repository license does not establish every transitive component's disposition.
- The source/provenance manifest and packaged third-party license are maintained by the dependency integration. This checklist must agree with the actual manifest; it must not imply unused/restricted files are bundled.

## Readiness rule

The PR can be called a complete port only after every required row is Verified, or an explicit scope decision is recorded for an upstream component whose host/dependency differs. Pending rows must remain visible in the PR description and release documentation. Disabling a module at build time, importing a header, or adding a sample formula is not equivalent to a completed feature.
