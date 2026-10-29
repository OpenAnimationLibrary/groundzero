# SeExpr developer SDK

The SDK uses the same pinned SeExpr source, generated grammar and core patches
as the application. Its C++ headers and static library preserve the upstream
`SeExpr2` API. Native OT rendering uses the separate ownership-safe adapter.
The implementation and named fixtures described here form the source inventory. Actual passing evidence comes from the SeExpr qualification jobs, clean installed-SDK consumer reports and native platform checks named in `seexpr_parity.md`; source presence alone does not establish readiness. No manual hardware qualification is implied.

## Building and consuming

`OT_SEEXPR_BUILD_SDK=ON` builds the SDK and command-line utilities. The normal
interpreter remains available with no LLVM dependency. Enabling
`OT_SEEXPR_WITH_LLVM=ON` requires the selected LLVM14 development package;
consumers of that SDK also need compatible LLVM14 development libraries.

Installation places headers, the core archive, package configuration, examples
and utilities beneath `sdk`. A separate CMake project can use:

```cmake
find_package(GroundzeroSeExpr 2.0.0 EXACT CONFIG REQUIRED)
add_executable(example host_callback_example.cpp)
target_compile_features(example PRIVATE cxx_std_17)
target_link_libraries(example PRIVATE GroundzeroSeExpr::Core)
```

Set `CMAKE_PREFIX_PATH` to the installed `sdk` directory. Use a compatible
compiler, C++ standard library, architecture and runtime configuration: the
upstream C++ API and plugin registration objects are not a cross-toolchain C
ABI. The source revision is exposed as `OT_SEEXPR_SOURCE_REVISION`; preserve
the installed license notices when distributing the library or examples.
Build plugins against this installation's headers and library. Private-core
corrections change internal layouts, so a binary built against an unmodified
upstream checkout is not automatically compatible even at the same source
revision. The preserved interface is the C++ source/registration API.

The qualification workflow installs the SDK into a clean prefix, configures a
separate consumer project against that installation, builds it and evaluates a
host-variable/custom-function example. Build-tree includes alone therefore do
not count as SDK packaging validation.

## Evaluation and compatibility corrections

The pinned core's parsed local-function/prototype language is completed in private build copies, preserving the original AST/type environment and interpreter/JIT backends. The corrections also retain captured input lifetimes, isolate nested numeric/string calls, preserve whole-vector equality semantics and support the SDK standard callback forms in both backends. Shared source fixtures cover zero-to-six scalar arguments, scalar/vector variadics, vector results, scalar requests, exception recovery and mixed uniform/varying lifetimes.

`syntaxOK()` queries parsing independently of type preparation. Private core preparation validates structural AST depth before recursive type/backend work, disposes rejected/deep trees iteratively and preserves full signed source-byte offsets. Raw SDK ASTs have a default structural depth bound of 512. Native adapter/CLI evaluation uses the stricter configured depth and instruction/string/cancellation limits; raw C++ SDK evaluation does not automatically become the application's bounded render context. Trusted hosts own their callback work and concurrent expression ownership.

The optional LLVM emitter routes native standard/custom callbacks through checked exception boundaries; it does not allow exceptions to unwind through generated code. Returning to the SDK rethrows the captured exception, and later valid evaluation remains available. A matching compiled result shape is required; passing a vector object through an incompatible scalar ABI is not supported.

Callback data keeps the original ownership contract: `ExprFuncNode::Data(false)` is borrowed from its host, while `Data(true)` is destroyed with the owning expression. Internal curve, Voronoi and variable-dispatch caches opt into ownership. `Expressions::reset()` releases graph-owned expressions and values and invalidates their handles; dependency edges and host-provided string buffers remain borrowed. Graph teardown releases evaluators before their input values, and failed preparation removes its dependency registrations.

The standalone qualification includes deterministic generated/malformed expressions with valid-expression recovery, explicit owned/borrowed callback lifetimes, repeated graph reset and failure recovery. A separate Linux ASan/UBSan job instruments the core, adapter, SDK helpers and plugins; its diagnostics are required evidence rather than a substitute for the ordinary platform/installed-consumer checks.

The optional Python inspection SDK has its own checked private parser preparation step and is documented in `seexpr_python.md`. These transformations preserve the downloaded source and notices. The exact selected files and helper paths are recorded in `toonz/cmake/SeExpr-source.json`.

## Calculator and grapher

`seexpr-calc "2+3*4"` evaluates a formula and returns nonzero for a failed
parse, binding or evaluation. Without an argument it reads formulas from
standard input. `_0`, `_1`, and subsequent identifiers refer to successful
earlier results, up to the documented 256-entry history bound. `q` or `quit`
ends the session. Numeric/vector and string results are supported.

`seexpr-graph "sin(x)" -3.14 3.14` samples a scalar formula into an ASCII plot.
The independent variable is `x`; limits must be finite and increasing. The
grapher compiles once and updates the variable for each sample. Both utilities
use the native evaluator's source, instruction and finite-output bounds rather
than unbounded raw evaluation loops.

The installed `host_callback_example.cpp` demonstrates explicit `resolveVar`
and `resolveFunc` bindings. It supplies its own frame and coordinate convention;
those values are examples, not implicit connections to an open OT scene.

## Trusted native plugins

Explicit `SeExpr2::ExprFunc::loadPlugin(path)` uses the upstream entry point:

```cpp
extern "C" void SeExpr2PluginInit(SeExpr2::ExprFunc::Define3 define);
```

Windows plugins export that symbol with `__declspec(dllexport)`; Unix platforms
must export it with default visibility. Register functions through the supplied
callback, especially when the plugin links a static core. Using an independent
private registry inside a plugin does not register a function in the host.

`loadPlugins(paths)` accepts explicit files or directories. Path lists use
semicolons on Windows and colons on Unix. Directory discovery is deterministic
and selects names beginning with `SeExpr` and the platform module extension.
Windows uses its native UTF-16 loader for UTF-8 paths. The loader avoids repeated
registration of the same canonical/equivalent module.

Automatic `SE_EXPR_PLUGINS` discovery stays disabled. A scene expression does
not choose an executable module, and native plugins are not a sandboxed formula
format. Loading plugins is an explicit trusted SDK operation.

Loaded modules remain alive while their function objects can be referenced.
Destroy every expression and stop all evaluation before calling
`ExprFunc::cleanup()`. Cleanup removes registry objects before closing module
handles. The operating system may keep a module mapped and preserve its static
state after those handles are closed; cleanup does not guarantee physical
unmapping, reset module state or provide hot replacement of a binary. A later
explicit load registers its functions into the fresh registry. Individual
unloading while formulas are active is unsupported. An
initializer that throws after registering code keeps its library pinned until
cleanup so published function pointers cannot immediately become dangling.

The fixture tests cover absence before explicit loading, rejection of a module
without the entry point, function/help registration and removal, deduplication
within each registry lifetime, directory re-registration after cleanup and
repeated cleanup. The fixture accepts either fresh or retained module static
state after handle release: it explicitly resets its own counter before cleanup,
then requires exactly one new registration and unchanged counts after duplicate
explicit/directory loads. This reset callback belongs to the test fixture, not
the production loader. Formula failures identify the phase, expression and
expected/actual result. Tests set the environment plugin path deliberately to
verify that initialization still does not discover it.
