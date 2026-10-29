"""Prepare pinned core sources and the same grammar for every target platform.

This is a build-time helper. It preserves upstream source notices, writes only
to the binary tree and never executes a fallback generated for another grammar.
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess


def replace_once(text, before, after, description):
    if text.count(before) != 1:
        raise RuntimeError(f"Pinned SeExpr patch no longer matches: {description}")
    return text.replace(before, after, 1)


def tool_version(executable, expected, description):
    result = subprocess.run([executable, "--version"], check=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding="utf-8", errors="replace")
    match = re.search(r"\b(\d+\.\d+\.\d+)\b", result.stdout)
    if match is None or match.group(1) != expected:
        raise RuntimeError(f"SeExpr parser generation requires {description} {expected}")


def normalize_parser(text):
    # Match the upstream parser's namespace transformation. Applying it to
    # scanner, parser and token header together keeps their interfaces equal.
    return text.replace("yy", "SeExpr2").replace("YY", "SeExprYY")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--bison", required=True)
    parser.add_argument("--flex", required=True)
    parser.add_argument("--llvm", action="store_true")
    args = parser.parse_args()
    tool_version(args.bison, "3.8.2", "Bison")
    tool_version(args.flex, "2.6.4", "Flex")

    source = args.source.resolve(strict=True) / "src" / "SeExpr2"
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for path in source.iterdir():
        if path.is_file() and path.suffix in (".h", ".cpp"):
            shutil.copyfile(path, output / path.name)

    functions = output / "ExprFunc.cpp"
    text = functions.read_text(encoding="utf-8")
    text = replace_once(
        text,
        '    const char* path = getenv("SE_EXPR_PLUGINS");\n    if (path) loadPlugins(path);',
        "    // Native function registration is explicitly owned by the host.\n"
        "    // Scene evaluation never discovers process-environment plugins.",
        "automatic plugin discovery")
    functions.write_text(text, encoding="utf-8", newline="\n")

    expression = output / "Expression.cpp"
    text = expression.read_text(encoding="utf-8")
    text = replace_once(text,
        'bool Expression::debugging = getenv("SE_EXPR_DEBUG") != 0;',
        "bool Expression::debugging = false;",
        "host-owned diagnostic policy")
    text = replace_once(text, '''void Expression::setExpr(const std::string& e) {
    if (_expression != "") reset();
    _expression = e;
}''', '''void Expression::setExpr(const std::string& e) {
    // Even an empty source can have cached parse/error state. Preserve an
    // incoming reference before clearing the old evaluator's owned state.
    const std::string source(e);
    reset();
    _expression = source;
}''', "empty-source parser recovery and aliased input")
    text = replace_once(text,
        "    int tempStartPos, tempEndPos;",
        "    int tempStartPos = 0, tempEndPos = 0;",
        "initialized parser error locations")
    text = replace_once(text,
        "    ExprParse(_parseTree, _parseError, tempStartPos, tempEndPos, _comments, this, _expression.c_str(), _wantVec);",
        "    ExprParse(_parseTree, _parseError, tempStartPos, tempEndPos, _comments, this, _expression.c_str(), _wantVec);\n"
        "    // The lexer may have consumed no token for an empty source.\n"
        "    // Keep its cached EOF location inside this expression's bytes.\n"
        "    const int sourceLength = static_cast<int>(_expression.size());\n"
        "    tempStartPos = std::max(0, std::min(sourceLength, tempStartPos));\n"
        "    tempEndPos = std::max(tempStartPos, std::min(sourceLength, tempEndPos));",
        "source-bounded EOF parse locations")
    text = replace_once(text,
        "            int* bound = std::lower_bound(&*lines.begin(), &*lines.end(), _errors[i].startPos);\n"
        "            int line = static_cast<int>(bound - &*lines.begin() + 1);\n"
        "            int lineStart = line == 1 ? 0 : lines[line - 1];\n"
        "            int col = _errors[i].startPos - lineStart;",
        "            const int position = std::max(0, std::min(\n"
        "                static_cast<int>(_expression.size()), _errors[i].startPos));\n"
        "            const auto bound = std::lower_bound(lines.begin(), lines.end(), position);\n"
        "            const int line = static_cast<int>(bound - lines.begin()) + 1;\n"
        "            const int lineStart = line == 1 ? 0 : lines[line - 2] + 1;\n"
        "            const int col = position - lineStart;",
        "iterator-safe multiline and EOF diagnostic formatting")
    default_begin = text.index("static Expression::EvaluationStrategy chooseDefaultEvaluationStrategy() {")
    default_end = text.index("Expression::EvaluationStrategy Expression::defaultEvaluationStrategy", default_begin)
    text = (text[:default_begin] +
            "static Expression::EvaluationStrategy chooseDefaultEvaluationStrategy() {\n"
            "    return Expression::UseInterpreter;\n}\n" + text[default_end:])
    expression.write_text(text, encoding="utf-8", newline="\n")

    # Registry definitions are copied before type preparation. Initialize the
    # complete base/default state instead of copying indeterminate bool/enum
    # members, including the unused standard slot of a custom callback.
    function_base = output / "ExprFuncX.h"
    text = function_base.read_text(encoding="utf-8")
    text = replace_once(text,
        "ExprFuncX(const bool threadSafe) : _threadSafe(threadSafe) {}",
        "ExprFuncX(const bool threadSafe) : _isScalar(false), _threadSafe(threadSafe) {}",
        "initialized extension-function scalar state")
    function_base.write_text(text, encoding="utf-8", newline="\n")

    standard_function = output / "ExprFuncStandard.h"
    text = standard_function.read_text(encoding="utf-8")
    text = replace_once(text,
        "ExprFuncStandard() : ExprFuncX(true) {}",
        "ExprFuncStandard() : ExprFuncX(true), _funcType(NONE), _func(nullptr) {}",
        "initialized empty standard-function state")
    standard_function.write_text(text, encoding="utf-8", newline="\n")

    interpreter = output / "Interpreter.cpp"
    text = interpreter.read_text(encoding="utf-8")
    text = replace_once(text, '#include "Platform.h"',
                        '#include "Platform.h"\n#include "seexprbudget.h"',
                        "native evaluation budget include")
    text = replace_once(text, "void Interpreter::eval(VarBlock* block, bool debug) {",
                        "void Interpreter::eval(VarBlock* block, bool debug) {\n"
                        "    callStack.clear();",
                        "local-function recovery after cancelled evaluation")
    text = replace_once(text, "    while (pc < end) {",
                        "    while (pc < end) {\n        TSeExprInternal::instructionStep();",
                        "native evaluation instruction budget")
    interpreter.write_text(text, encoding="utf-8", newline="\n")

    platform = output / "Platform.h"
    text = platform.read_text(encoding="utf-8")
    text = replace_once(text, "#define snprintf sprintf_s\n", "",
                        "standard C++17 snprintf")
    text = replace_once(text,
        "inline double log2(double x) { return log(x) * 1.4426950408889634; }\n",
        "", "standard C++17 log2")
    platform.write_text(text, encoding="utf-8", newline="\n")

    platform_impl = output / "Platform.cpp"
    text = platform_impl.read_text(encoding="utf-8")
    text = replace_once(text, "    delete _spinlock;",
                        "    delete reinterpret_cast<CRITICAL_SECTION*>(_spinlock);",
                        "typed Windows critical-section deletion")
    platform_impl.write_text(text, encoding="utf-8", newline="\n")

    builtins = output / "ExprBuiltins.cpp"
    text = builtins.read_text(encoding="utf-8")
    text = replace_once(text,
        "#ifndef SEEXPR_WIN32\n    FUNCDOC(cbrt);\n    FUNCDOC(asinh);\n"
        "    FUNCDOC(acosh);\n    FUNCDOC(atanh);\n    FUNCDOC(trunc);\n#endif",
        "    FUNCDOC(cbrt);\n    FUNCDOC(asinh);\n    FUNCDOC(acosh);\n"
        "    FUNCDOC(atanh);\n    FUNCDOC(trunc);",
        "C++17 math registry parity")
    builtins.write_text(text, encoding="utf-8", newline="\n")

    config = (source / "ExprConfig.h.in").read_text(encoding="utf-8")
    (output / "ExprConfig.h").write_text(
        config.replace("@SEEXPR_ENABLE_LLVM_BACKEND@", "1" if args.llvm else "0"),
        encoding="utf-8", newline="\n")

    # The SDK dependency graph's temporary resolution sets belong to one
    # construction, not the previous expression or a different worker.
    graph = output / "ExprMultiExpr.cpp"
    text = graph.read_text(encoding="utf-8")
    text = replace_once(text, '#include <set>',
                        '#include <set>\n#include <functional>\n#include <memory>\n#include <stdexcept>',
                        "graph traversal headers")
    text = replace_once(text,
        "    : Expression(e, type, be), context(context) {\n"
        "    if (type.isFP())\n"
        "        val = new GlobalFP(varName, type.dim());\n"
        "    else if (type.isString())\n"
        "        val = new GlobalStr(varName);\n"
        "    else\n"
        "        assert(false);",
        "    : Expression(e, type, be), context(context), val(nullptr) {\n"
        "    std::unique_ptr<GlobalVal> valueOwner;\n"
        "    if (type.isFP())\n"
        "        valueOwner.reset(new GlobalFP(varName, type.dim()));\n"
        "    else if (type.isString())\n"
        "        valueOwner.reset(new GlobalStr(varName));\n"
        "    else\n"
        "        throw std::invalid_argument(\"Expression graph requires a value type\");\n"
        "    val = valueOwner.get();",
        "graph result ownership during construction")
    text = replace_once(text, "std::set<SeExpr2::DExpression *> tmpOperandExprs;",
                        "thread_local std::set<SeExpr2::DExpression *> tmpOperandExprs;",
                        "graph-local expression resolution")
    text = replace_once(text, "std::set<SeExpr2::GlobalVal *> tmpOperandVars;",
                        "thread_local std::set<SeExpr2::GlobalVal *> tmpOperandVars;",
                        "graph-local variable resolution")
    text = replace_once(text,
        "    prepIfNeeded();\n    operandExprs = tmpOperandExprs;\n    operandVars = tmpOperandVars;",
        "    struct ResolutionScope {\n"
        "        ResolutionScope() { tmpOperandExprs.clear(); tmpOperandVars.clear(); }\n"
        "        ~ResolutionScope() { tmpOperandExprs.clear(); tmpOperandVars.clear(); }\n"
        "    } resolution;\n"
        "    try {\n"
        "        prepIfNeeded();\n"
        "        operandExprs = tmpOperandExprs;\n"
        "        operandVars = tmpOperandVars;\n"
        "    } catch (...) {\n"
        "        for (DExpression *operand : tmpOperandExprs) operand->val->users.erase(this);\n"
        "        for (GlobalVal *operand : tmpOperandVars) operand->users.erase(this);\n"
        "        throw;\n"
        "    }\n"
        "    valueOwner.release();",
        "graph resolution lifecycle")
    text = replace_once(text,
        "const std::string &DExpression::name() const { return val->varName; }",
        "DExpression::~DExpression() {\n"
        "    // Prepared data may borrow graph inputs. Release it before this\n"
        "    // expression's own result; dependency sets never own their peers.\n"
        "    reset();\n"
        "    delete val;\n"
        "}\n\n"
        "const std::string &DExpression::name() const { return val->varName; }",
        "graph expression result destruction")
    text = replace_once(text, '''Expressions::~Expressions() {
    for (std::set<DExpression *>::iterator I = AllExprs.begin(), E = AllExprs.end(); I != E; ++I) delete *I;

    for (std::set<GlobalVal *>::iterator I = AllExternalVars.begin(), E = AllExternalVars.end(); I != E; ++I) delete *I;
}''', '''Expressions::~Expressions() { reset(); }

void Expressions::reset() {
    resetEval();
    // First release every evaluator while all borrowed values still exist.
    // Pointer-set order is unrelated to dependency order.
    for (DExpression *expression : AllExprs) expression->reset();
    for (DExpression *expression : AllExprs) {
        expression->operandExprs.clear();
        expression->operandVars.clear();
        expression->val->users.clear();
    }
    for (GlobalVal *variable : AllExternalVars) variable->users.clear();
    // addExpression/addExternalVariable create distinct owners. Edges and
    // GlobalStr's host-provided character buffer remain borrowed.
    for (DExpression *expression : AllExprs) delete expression;
    for (GlobalVal *variable : AllExternalVars) delete variable;
    AllExprs.clear();
    AllExternalVars.clear();
}''', "graph reset releases its owned expressions and values")
    text = replace_once(text, '''    if (seTy.isFP())
        ret = AllExternalVars.insert(new GlobalFP(variableName, seTy.dim()));
    else if (seTy.isString())
        ret = AllExternalVars.insert(new GlobalStr(variableName));
    else
        assert(false);

    return ret.first;''', '''    std::unique_ptr<GlobalVal> variable;
    if (seTy.isFP())
        variable.reset(new GlobalFP(variableName, seTy.dim()));
    else if (seTy.isString())
        variable.reset(new GlobalStr(variableName));
    else
        throw std::invalid_argument("Expression graph requires a value type");
    ret = AllExternalVars.insert(variable.get());
    if (ret.second) variable.release();
    return ret.first;''', "graph variable insertion exception ownership")
    text = replace_once(text,
        "    ret = AllExprs.insert(new DExpression(varName, *this, expr, seTy));\n"
        "    return ret.first;",
        "    std::unique_ptr<DExpression> expression(new DExpression(varName, *this, expr, seTy));\n"
        "    try {\n"
        "        ret = AllExprs.insert(expression.get());\n"
        "    } catch (...) {\n"
        "        // Registration may fail after preparation published borrowed\n"
        "        // dependency edges. Their owners are all still live here.\n"
        "        for (DExpression *operand : expression->operandExprs)\n"
        "            operand->val->users.erase(expression.get());\n"
        "        for (GlobalVal *operand : expression->operandVars)\n"
        "            operand->users.erase(expression.get());\n"
        "        throw;\n"
        "    }\n"
        "    if (ret.second) expression.release();\n"
        "    return ret.first;",
        "graph expression insertion exception ownership")
    graph_order = '''std::vector<SeExpr2::DExpression *> evaluationOrder(
    const std::set<SeExpr2::DExpression *> &selected) {
    std::set<SeExpr2::DExpression *> visiting, complete;
    std::vector<SeExpr2::DExpression *> ordered;
    std::function<void(SeExpr2::DExpression *)> visit = [&](SeExpr2::DExpression *expression) {
        if (!selected.count(expression) || complete.count(expression)) return;
        if (!visiting.insert(expression).second)
            throw std::runtime_error("Expression dependency cycle detected");
        for (SeExpr2::DExpression *dependency : expression->operandExprs) visit(dependency);
        visiting.erase(expression);
        complete.insert(expression);
        ordered.push_back(expression);
    };
    for (SeExpr2::DExpression *expression : selected) visit(expression);
    return ordered;
}

'''
    text = replace_once(text,
        "thread_local std::set<SeExpr2::DExpression *> tmpOperandExprs;",
        graph_order + "thread_local std::set<SeExpr2::DExpression *> tmpOperandExprs;",
        "graph dependency ordering")
    old_evaluate = "    for (std::set<DExpression *>::iterator I = ret.begin(), E = ret.end(); I != E; ++I) (*I)->eval();"
    if text.count(old_evaluate) != 2:
        raise RuntimeError("Pinned SeExpr graph setter evaluation no longer matches")
    text = text.replace(old_evaluate,
                        "    for (DExpression *expression : evaluationOrder(ret)) expression->eval();")
    text = replace_once(text, "    return std::make_pair(eh, ret2);",
        "    const std::set<DExpression *> selected(ret2.begin(), ret2.end());\n"
        "    return std::make_pair(eh, evaluationOrder(selected));",
        "graph handle evaluation order")
    graph.write_text(text, encoding="utf-8", newline="\n")

    graph_header = output / "ExprMultiExpr.h"
    text = graph_header.read_text(encoding="utf-8")
    text = replace_once(text,
        "    mutable std::set<DExpression*> operandExprs;",
        "    ~DExpression() override;\n\n"
        "    mutable std::set<DExpression*> operandExprs;",
        "graph expression destructor declaration")
    text = replace_once(text, '''    void reset() {
        resetEval();
        AllExprs.clear();
        AllExternalVars.clear();
    }''', "    void reset();", "graph reset with complete value types")
    graph_header.write_text(text, encoding="utf-8", newline="\n")

    from prepare_seexpr_language import apply_language
    apply_language(output)

    from prepare_seexpr_safety import apply_safety
    apply_safety(output)

    from prepare_seexpr_sdk import apply_sdk
    apply_sdk(output)

    from prepare_seexpr_limits import apply_limits
    apply_limits(output)

    if args.llvm:
        from prepare_seexpr_llvm import prepare_llvm
        prepare_llvm(output)

    raw = output / "parser-input"
    raw.mkdir(exist_ok=True)
    subprocess.run([args.bison, "--name-prefix=SeExpr2",
                    "--defines=" + str(raw / "ExprParser.tab.h"),
                    "--output=" + str(raw / "ExprParser.cpp"),
                    str(source / "ExprParser.y")], check=True)
    subprocess.run([args.flex, "--outfile=" + str(raw / "ExprParserLex.cpp"),
                    str(source / "ExprParserLex.l")], check=True)
    for name in ("ExprParser.cpp", "ExprParser.tab.h", "ExprParserLex.cpp"):
        text = (raw / name).read_text(encoding="utf-8")
        (output / name).write_text(normalize_parser(text), encoding="utf-8", newline="\n")
    (output / "prepared.stamp").write_text("SeExpr core prepared\n", encoding="ascii")


if __name__ == "__main__":
    main()
