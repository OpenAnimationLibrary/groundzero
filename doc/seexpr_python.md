# Python expression inspection and Qt authoring SDK

The optional `BUILD_SEEXPR_PYTHON` build produces a separately packaged Python 3 SDK. The application does not embed Python. `BUILD_SEEXPR_PYTHON_UI_TESTS` additionally qualifies the optional Qt widgets and requires the selected PyQt binding in the test interpreter.

The SDK implementation and named acceptance fixtures form the source inventory. Actual passing evidence comes from the interpreter/LLVM jobs and reports of SeExpr qualification, as recorded in `seexpr_parity.md`. The SDK contains independently implemented authoring widgets and the licensed upstream syntax-inspection parser. It does not redistribute the upstream Qt package initializer with its restrictive notice; that file contains no executable functionality.

## Native syntax and evaluation

`SeExprPy.AST`, `ASTHandle`, `ASTType`, `traverseCallback`, `traverseFilter`, `getComment`, `printTree`, and `Edits` follow the upstream inspection interface. `printTree(tree, source)` prints node names, values and ordered tree structure; it does not promise byte-identical legacy spacing. Node ranges are UTF-8 byte offsets. Handles retain their node data after the original tree object is released. Edits preserve Unicode source, reject overlapping ranges, and quote replacement strings. The checked private `prepare_parser.py` step fixes imported polymorphic destruction and call/vector/unary source ranges, bounds AST height during construction and guards parser recursion before a deep tree can form. It preserves the downloaded source and license notices. Flat/malformed operator chains, nested unary/power/ternary source and immediate recovery have focused fixtures.

`SeExprPy.evaluate(source, variables=None, result_type="vector", dimension=3, backend="interpreter", functions=None)` evaluates through the same native adapter used by the application. Scalars return Python floats, vectors return tuples, and strings return Unicode strings. Variables accept numbers, numeric sequences of one through sixteen components, or strings. The result type is explicitly `scalar`, `vector`, or `string`.

`SeExprPy.validate` accepts the same arguments and returns `valid` and native source-location diagnostics without committing document changes. `SeExprPy.functions()` returns the actual registered builtin names, documentation, and argument ranges. An unavailable requested backend reports an error rather than silently substituting the interpreter.

An explicitly registered Python host function uses a signature dictionary:

```python
import SeExprPy

callbacks = {
    "twice": {
        "arguments": ["scalar"],
        "result_type": "scalar",
        "callback": lambda value: value * 2,
    }
}
value = SeExprPy.evaluate("twice($input)", {"input": 3.5},
                         "scalar", functions=callbacks)
```

Function signatures can include `dimension`, `argument_dimensions`, `min_args`, `max_args`, and `documentation`. Declared vectors retain tuple values even at dimension one. Native evaluation releases the Python interpreter lock; Python callbacks acquire it while executing. Exceptions become useful expression errors, and subsequent evaluations remain usable. The existing native instruction, string, type, finite-result, and console-output policies still apply; this SDK evaluation context rejects console output.

## Qt authoring API

Install PyQt5 or PyQt6 into the SDK consumer's interpreter. Set `SEEXPR_QT_API=pyqt5` or `pyqt6` to select one explicitly. Import `expreditor2` or `SeExprPy.editor`.

The frontend provides:

- `ExprEditor`: source editing, syntax highlighting, source completion including parsed local assignments, actual function help, native validation/value preview, literal/annotation controls, error ranges and apply signals.
- `ExprDialog`: a modeless editor with `preview` and `expressionApplied` signals.
- `ExprControlCollection`: numeric, vector, color and string controls, including explicitly selected file/directory dialogs, curve controls, swatches and spectrum parameters.
- `ExprBrowser`: nested search paths, source/JSON preset selection and saving with metadata preserved. Relative folder labels distinguish repeated filenames. Traversal skips links/reparse points and reports limits after 4,096 entries or 32 subdirectory levels.
- `ExprShortEdit` and `ExprShortTextEdit`: compact editing and an expanded editor that retains source and propagates applied changes.
- `ExprCurve` and `ExprColorCurve`: point editing, all five upstream interpolation modes and previews evaluated by the native engine.
- `ExprColorSwatchWidget`: color addition, removal, reordering and selection.
- `ExprDeepWater`: the twelve spectrum parameters and spectrum preview. This is the upstream spectrum-authoring behavior, not a fluid simulation.

Use `ExprEditor.setContext(variables, functions, result_type)` to bind real host inputs. `registerExtraFunction`, `registerExtraVariable`, and `updateCompleter` extend authoring help; documentation registration alone does not implement a function callback. Unknown functions remain validation errors until the host supplies their implementations.

Literal controls update source and emit apply notifications. Numeric RGB dialogs preserve HDR values and untouched component precision, and canceling them does not change source. Display swatches and the optional display color picker are limited to colors from zero to one; they do not replace the numeric values merely by opening and accepting the dialog. A consumer owns the actual document/scene transaction and should validate before applying it. The main application's native C++ editor owns OT undo, keyframes, references and persistence; the standalone Python widgets do not claim ownership of those application objects.

## Qualification

The CI fixtures `seexpr_python_ast`, `seexpr_python_eval`, and optional `seexpr_python_ui` verify syntax ownership/editing, real scalar/vector/string and host-callback evaluation, and real Qt source/controls/presets/dialog behavior. They load the built SDK before the source tree, use disposable preset directories, and report failures rather than skip missing requested bindings. Qt5 qualification does not by itself establish Qt6 runtime qualification.
