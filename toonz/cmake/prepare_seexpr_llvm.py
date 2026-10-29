"""Apply reviewed LLVM 14 compatibility to private pinned source copies.

This helper is called only from the dependency preparation build command. It
does not edit the downloaded archive or any application source file.
"""
from pathlib import Path
import re
import shutil


def replace_once(source, before, after, label):
    if source.count(before) != 1:
        raise RuntimeError(f"Pinned SeExpr LLVM patch no longer matches: {label}")
    return source.replace(before, after, 1)


def prepare_llvm(output):
    output = Path(output)
    shutil.copyfile(Path(__file__).with_name("seexpr_llvm_compat.h"),
                    output / "seexpr_llvm_compat.h")

    header = output / "ExprLLVM.h"
    text = header.read_text(encoding="utf-8")
    text = replace_once(text, "#include <llvm/IR/IRBuilder.h>",
                        '#include "seexpr_llvm_compat.h"', "builder header")
    text = replace_once(text, "typedef llvm::IRBuilder<>& LLVM_BUILDER;",
                        "typedef SeExprIRBuilder& LLVM_BUILDER;", "builder type")
    header.write_text(text, encoding="utf-8", newline="\n")

    all_header = output / "ExprLLVMAll.h"
    text = all_header.read_text(encoding="utf-8")
    text = text.replace("#include <llvm/Analysis/Passes.h>\n", "")
    text = text.replace("#include <llvm/LinkAllPasses.h>\n", "")
    text = text.replace("#include <llvm/Transforms/IPO/PassManagerBuilder.h>",
                        "#include <llvm/Transforms/IPO/PassManagerBuilder.h>\n"
                        "#include <llvm/Transforms/IPO/AlwaysInliner.h>")
    all_header.write_text(text, encoding="utf-8", newline="\n")

    for name in ("ExprLLVMCodeGeneration.cpp", "Evaluator.h"):
        path = output / name
        text = path.read_text(encoding="utf-8")
        text = text.replace("llvm::IRBuilder<>", "SeExprIRBuilder")
        text = text.replace("IRBuilder<>", "SeExprIRBuilder")
        # LLVM 14 uses fixed-vector types and explicit pointee types. Preserve
        # the emitter's fixed dimensions; never use scalable vector semantics.
        text = text.replace("VectorType::get(", "FixedVectorType::get(")
        for expression in ("destTy", "target->getType()", "vecVal->getType()",
                           "arg->getType()", "maxVectorArgType",
                           "newLastVal->getType()"):
            text = text.replace(expression + "->getVectorNumElements()",
                                "seexprLLVMVectorElements(" + expression + ")")
        # LLVM builders are non-copyable. Only this helper took one by value.
        text = text.replace("getFirstElement(LLVM_VALUE V, SeExprIRBuilder Builder)",
                            "getFirstElement(LLVM_VALUE V, SeExprIRBuilder &Builder)")
        path.write_text(text, encoding="utf-8", newline="\n")

    evaluator = output / "Evaluator.h"
    text = evaluator.read_text(encoding="utf-8")
    text = text.replace('#include "VarBlock.h"',
                        '#include "VarBlock.h"\n#include "seexprbudget.h"\n#include <mutex>\n#include <limits>')
    text = replace_once(text, "namespace SeExpr2 {\n#ifdef SEEXPR_ENABLE_LLVM",
        'extern "C" char *GroundzeroSeExprConcat(const char *, const char *, char **) noexcept;\n\n'
        "namespace SeExpr2 {\n#ifdef SEEXPR_ENABLE_LLVM", "string helper declaration")
    text = replace_once(text,
        "            functionPtr(resultData, varBlock ? varBlock->data() : nullptr, varBlock ? varBlock->indirectIndex : 0);",
        "            const std::function<bool()> noCancellation;\n"
        "            TSeExprInternal::EvaluationBudgetGuard callbackBoundary(\n"
        "                std::numeric_limits<std::size_t>::max(), noCancellation,\n"
        "                std::numeric_limits<std::size_t>::max());\n"
        "            functionPtr(resultData, varBlock ? varBlock->data() : nullptr, varBlock ? varBlock->indirectIndex : 0);\n"
        "            TSeExprInternal::rethrowEvaluationFailure();",
        "SDK evaluation exception boundary")
    text = replace_once(text,
        "            functionPtrMultiple(varBlock ? varBlock->data() : nullptr, outputVarBlockOffset, rangeStart, rangeEnd);",
        "            const std::function<bool()> noCancellation;\n"
        "            TSeExprInternal::EvaluationBudgetGuard callbackBoundary(\n"
        "                std::numeric_limits<std::size_t>::max(), noCancellation,\n"
        "                std::numeric_limits<std::size_t>::max());\n"
        "            functionPtrMultiple(varBlock ? varBlock->data() : nullptr, outputVarBlockOffset, rangeStart, rangeEnd);\n"
        "            TSeExprInternal::rethrowEvaluationFailure();",
        "SDK batch evaluation exception boundary")
    text = text.replace("SeExpr2LLVMEvalStrVarRef(SeExpr2::ExprVarRef *seVR, double *result)",
                        "SeExpr2LLVMEvalStrVarRef(SeExpr2::ExprVarRef *seVR, char **result)")
    text = replace_once(text,
        "        InitializeNativeTarget();\n"
        "        InitializeNativeTargetAsmPrinter();\n"
        "        InitializeNativeTargetAsmParser();",
        "        static std::once_flag initialized;\n"
        "        std::call_once(initialized, [] {\n"
        "            InitializeNativeTarget();\n"
        "            InitializeNativeTargetAsmPrinter();\n"
        "            InitializeNativeTargetAsmParser();\n"
        "        });", "native target initialization")
    text = replace_once(text,
        "#if LLVM_VERSION_MAJOR > 4\n"
        "        F->addAttribute(llvm::AttributeList::FunctionIndex, llvm::Attribute::AlwaysInline);\n"
        "#else\n"
        "        F->addAttribute(llvm::AttributeSet::FunctionIndex, llvm::Attribute::AlwaysInline);\n"
        "#endif",
        "        F->addFnAttr(llvm::Attribute::AlwaysInline);",
        "function attribute")
    text = replace_once(text,
        "        // create function and entry BB\n",
        "        // Budget callbacks return normally across JIT frames. A failed\n"
        "        // callback aborts generated execution and the native host then\n"
        "        // reports the saved error outside the generated stack.\n"
        "        Function *budgetStep = Function::Create(\n"
        "            FunctionType::get(Type::getInt1Ty(*_llvmContext), {}, false),\n"
        "            GlobalValue::ExternalLinkage, \"GroundzeroSeExprBudgetStep\", TheModule.get());\n\n"
        "        Function *concat = Function::Create(\n"
        "            FunctionType::get(i8PtrTy, {i8PtrTy, i8PtrTy, i8PtrPtrTy}, false),\n"
        "            GlobalValue::ExternalLinkage, \"GroundzeroSeExprConcat\", TheModule.get());\n\n"
        "        // create function and entry BB\n",
        "budget declaration")
    text = replace_once(text,
        "        altModule->setDataLayout(TheExecutionEngine->getDataLayout());",
        "        if (!TheExecutionEngine) {\n"
        "            parseTree->addError(\"Could not create LLVM execution engine: \" + ErrStr);\n"
        "            return false;\n"
        "        }\n"
        "        altModule->setDataLayout(TheExecutionEngine->getDataLayout());\n"
        "        TheExecutionEngine->addGlobalMapping(budgetStep,\n"
        "            reinterpret_cast<void *>(&TSeExprInternal::instructionStepNoThrow));\n"
        "        TheExecutionEngine->addGlobalMapping(concat, reinterpret_cast<void *>(&GroundzeroSeExprConcat));",
        "engine failure and budget mapping")
    text = replace_once(text, ".setErrorStr(&ErrStr)",
                        ".setErrorStr(&ErrStr)\n                                     .setEngineKind(EngineKind::JIT)",
                        "explicit JIT selection")
    text = replace_once(text,
        "            fprintf(stderr, \"Could not create ExecutionEngine: %s\\n\", ErrStr.c_str());\n"
        "            exit(1);",
        "            parseTree->addError(\"LLVM execution engine became unavailable\");\n"
        "            return false;", "nonfatal engine failure")
    text = text.replace("        fpm->run(*F);", "        fpm->doInitialization();\n        fpm->run(*F);")
    text = text.replace("        fpm->run(*FLOOP);", "        fpm->run(*FLOOP);\n        fpm->doFinalization();")
    evaluator.write_text(text, encoding="utf-8", newline="\n")

    codegen = output / "ExprLLVMCodeGeneration.cpp"
    text = codegen.read_text(encoding="utf-8")
    text = text.replace('#include "StringUtils.h"',
                        '#include "StringUtils.h"\n#include "seexprbudget.h"')
    def instrument(match):
        # Function declarations switch the builder into another function and
        # restore its original insertion point. They are not executed values;
        # instrument their body nodes, not these declaration emitters.
        if match.group(2) in ("ExprPrototypeNode", "ExprLocalFunctionNode"):
            return match.group(1)
        return match.group(1) + "\n    seexprLLVMCheckBudget(Builder);"

    text, count = re.subn(
        r"(LLVM_VALUE (Expr\w+)::codegen\(LLVM_BUILDER Builder\) LLVM_BODY \{)",
        instrument, text)
    if count < 15:
        raise RuntimeError("Pinned SeExpr LLVM node budget instrumentation no longer matches")
    # SeExpr arbitrary numeric vector values use the same alloca path as RGB.
    text = replace_once(text, '            assert(dim == 3 && "future work.");\n',
                        "", "numeric vector dimensions")
    concat_begin = text.index("        // precompute a few things\n")
    concat_end = text.index("        return alloc;", concat_begin) + len("        return alloc;")
    text = text[:concat_begin] + (
        "        LLVMContext &context = Builder.getContext();\n"
        "        Module *module = llvm_getModule(Builder);\n"
        "        PointerType *outType = PointerType::getUnqual(Type::getInt8PtrTy(context));\n"
        "        LLVM_VALUE out = ConstantExpr::getIntToPtr(\n"
        "            ConstantInt::get(Type::getInt64Ty(context), reinterpret_cast<uint64_t>(&_out)), outType);\n"
        "        return Builder.CreateCall(module->getFunction(\"GroundzeroSeExprConcat\"), {op1, op2, out});"
    ) + text[concat_end:]
    text = replace_once(text,
        'extern "C" void SeExpr2LLVMEvalFPVarRef(ExprVarRef *seVR, double *result) { seVR->eval(result); }',
        'extern "C" void SeExpr2LLVMEvalFPVarRef(ExprVarRef *seVR, double *result) {\n'
        '    try { seVR->eval(result); } catch (...) {\n'
        '        TSeExprInternal::captureEvaluationFailure(std::current_exception());\n'
        '        std::fill(result, result + seVR->type().dim(), 0.0);\n'
        '    }\n}', "numeric variable callback boundary")
    text = replace_once(text,
        'extern "C" void SeExpr2LLVMEvalStrVarRef(ExprVarRef *seVR, char **result) { seVR->eval((const char **)result); }',
        'extern "C" void SeExpr2LLVMEvalStrVarRef(ExprVarRef *seVR, char **result) {\n'
        '    try { seVR->eval((const char **)result); } catch (...) {\n'
        '        TSeExprInternal::captureEvaluationFailure(std::current_exception());\n'
        '        *result = nullptr;\n'
        '    }\n}', "string variable callback boundary")
    codegen.write_text(text, encoding="utf-8", newline="\n")

    functions = output / "ExprFuncX.cpp"
    text = functions.read_text(encoding="utf-8")
    text = text.replace('#include "ExprFuncX.h"',
                        '#include "ExprFuncX.h"\n#include "seexprbudget.h"')
    text = replace_once(text,
        "                                   const SeExpr2::ExprFuncNode *node) {\n",
        "                                   const SeExpr2::ExprFuncNode *node) {\n"
        "    if (!TSeExprInternal::instructionStepNoThrow()) return;\n    try {\n",
        "custom function callback boundary begin")
    text = replace_once(text,
        "    funcSimple->eval(handle);\n    // for (int i = 0; i < retSize; ++i) result[i] = fp[1 + i];",
        "    funcSimple->eval(handle);\n"
        "    } catch (...) {\n"
        "        TSeExprInternal::captureEvaluationFailure(std::current_exception());\n"
        "    }\n"
        "    // for (int i = 0; i < retSize; ++i) result[i] = fp[1 + i];",
        "custom function callback boundary end")
    functions.write_text(text, encoding="utf-8", newline="\n")
