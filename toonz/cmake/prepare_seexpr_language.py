"""Complete the pinned core's parsed-but-disabled local function language.

The changes operate on private build copies and retain the upstream AST,
type environment, bytecode procedures and LLVM emitter. They do not rewrite
artist source strings or substitute an evaluator for either backend.
"""
from pathlib import Path


def replace_once(text, before, after, label):
    if text.count(before) != 1:
        raise RuntimeError(f"Pinned SeExpr language patch no longer matches: {label}")
    return text.replace(before, after, 1)


def region(text, first, following, replacement, label):
    if text.count(first) != 1 or text.count(following) != 1:
        raise RuntimeError(f"Pinned SeExpr language region no longer matches: {label}")
    begin = text.index(first)
    end = text.index(following, begin)
    return text[:begin] + replacement.rstrip() + "\n\n" + text[end:]


def apply_language(output):
    output = Path(output)
    environment = output / "ExprEnv.h"
    text = environment.read_text(encoding="utf-8")
    text = replace_once(text, "class ExprLocalFunctionNode;",
                        "class ExprLocalFunctionNode;\nclass ExprPrototypeNode;",
                        "prototype environment forward declaration")
    text = replace_once(text, "    FuncDictType _functions;",
                        "    FuncDictType _functions;\n"
                        "    std::map<std::string, ExprPrototypeNode*> _prototypes;",
                        "external prototype environment storage")
    text = replace_once(text,
        "    void addFunction(const std::string& name, ExprLocalFunctionNode* prototype);",
        "    void addFunction(const std::string& name, ExprLocalFunctionNode* prototype);\n"
        "    ExprPrototypeNode* findPrototype(const std::string& name);\n"
        "    void addPrototype(const std::string& name, ExprPrototypeNode* prototype);",
        "external prototype environment interface")
    environment.write_text(text, encoding="utf-8", newline="\n")

    environment = output / "ExprEnv.cpp"
    text = environment.read_text(encoding="utf-8")
    text = replace_once(text,
        "void ExprVarEnv::add(const std::string& name, std::unique_ptr<ExprLocalVar> var) {",
        """ExprPrototypeNode* ExprVarEnv::findPrototype(const std::string& name) {
    const auto found = _prototypes.find(name);
    if (found != _prototypes.end()) return found->second;
    return _parent ? _parent->findPrototype(name) : nullptr;
}

void ExprVarEnv::addPrototype(const std::string& name, ExprPrototypeNode* prototype) {
    if (_parent) _parent->addPrototype(name, prototype);
    else _prototypes[name] = prototype;
}

void ExprVarEnv::add(const std::string& name, std::unique_ptr<ExprLocalVar> var) {""",
        "external prototype environment implementation")
    environment.write_text(text, encoding="utf-8", newline="\n")

    header = output / "ExprNode.h"
    text = header.read_text(encoding="utf-8")
    text = replace_once(text,
        "        : ExprNode(expr, prototype, block) {}",
        "        : ExprNode(expr, prototype, block), _procedurePC(0),\n"
        "          _returnedDataOp(0), _llvmFunction(0) {}",
        "local function initialized backend state")
    text = replace_once(text,
        "    mutable int _procedurePC;\n    mutable int _returnedDataOp;",
        "    mutable int _procedurePC;\n    mutable int _returnedDataOp;\n"
        "    mutable LLVM_VALUE _llvmFunction;\n\n"
        "  public:\n    LLVM_VALUE llvmFunction() const { return _llvmFunction; }",
        "local function emitted identity")
    text = replace_once(text,
        "/// Policy which provides all the AST Types for the parser.",
        """// Results of distinct string-valued calls own separate storage. A later
// invocation of a procedure must not overwrite an earlier call's live value.
struct ExprLocalCallStringData : public ExprFuncNode::Data {
    ExprLocalCallStringData() : ExprFuncNode::Data(true) {}
    std::string text;
};

/// Policy which provides all the AST Types for the parser.""",
        "owned local string call result")
    header.write_text(text, encoding="utf-8", newline="\n")

    node = output / "ExprNode.cpp"
    text = node.read_text(encoding="utf-8")
    text = text.replace('#include "StringUtils.h"',
                        '#include "StringUtils.h"\n#include <set>')
    text = region(text,
        "ExprType ExprPrototypeNode::prep(bool wantScalar, ExprVarEnvBuilder& envBuilder) {",
        "void ExprPrototypeNode::addArgTypes(ExprNode* surrogate) {",
        """ExprType ExprPrototypeNode::prep(bool wantScalar, ExprVarEnvBuilder& envBuilder) {
    bool error = false;
    const bool formal = dynamic_cast<const ExprLocalFunctionNode*>(parent()) != nullptr;
    if (_retTypeSet)
        checkCondition(_retType.isValid() && _retType.isValue() &&
                       (!_retType.isFP() || _retType.dim() <= 16),
                       "Function has an invalid return type", error);
    _argTypes.clear();
    std::set<std::string> names;
    for (int c = 0; c < numChildren(); ++c) {
        auto* variable = dynamic_cast<ExprVarNode*>(child(c));
        if (!checkCondition(variable != nullptr, "Invalid formal argument", error)) continue;
        const ExprType declared = variable->type();
        checkCondition(declared.isValid() && declared.isValue() &&
                       (!declared.isFP() || declared.dim() <= 16),
                       "Function has an invalid argument type", error);
        _argTypes.push_back(declared);
        if (!formal) continue;
        if (!checkCondition(names.insert(variable->name()).second,
                            "Function argument names must be unique", error)) continue;
        envBuilder.current()->add(variable->name(),
                                  std::unique_ptr<ExprLocalVar>(new ExprLocalVar(declared)));
        variable->prep(declared.isFP(1), envBuilder);
    }
    if (!formal) {
        const ExprFunc* function = expr()->resolveFunc(name());
        if (!function) function = ExprFunc::lookup(name());
        if (checkCondition(function != nullptr,
                           "External function " + name() + " has no host definition", error)) {
            const int count = numChildren();
            checkCondition(count >= function->minArgs() &&
                           (function->maxArgs() < 0 || count <= function->maxArgs()),
                           "External function declaration has incompatible arity", error);
        }
        if (!error) envBuilder.current()->addPrototype(name(), this);
    }
    setType(error ? ExprType().Error() : ExprType().None().Constant());
    return _type;
}""", "prototype preparation")
    text = region(text,
        "ExprType ExprLocalFunctionNode::prep(bool wantScalar, ExprVarEnvBuilder& envBuilder) {",
        "ExprType ExprBlockNode::prep(bool wantScalar, ExprVarEnvBuilder& envBuilder) {",
        """ExprType ExprLocalFunctionNode::prep(bool wantScalar, ExprVarEnvBuilder& envBuilder) {
    bool error = false;
    auto* declaration = static_cast<ExprPrototypeNode*>(child(0));
    ExprVarEnv* outer = envBuilder.current();
    if (!checkCondition(outer->findFunction(declaration->name()) == nullptr,
                        "Local function is already defined: " + declaration->name(), error)) {
        setType(ExprType().Error());
        return _type;
    }
    envBuilder.setCurrent(envBuilder.createDescendant(outer));
    struct RestoreScope {
        ExprVarEnvBuilder& builder;
        ExprVarEnv* previous;
        ~RestoreScope() { builder.setCurrent(previous); }
    } restore{envBuilder, outer};
    if (!declaration->prep(false, envBuilder).isValid()) error = true;
    const bool scalar = declaration->isReturnTypeSet() && declaration->returnType().isFP(1);
    const ExprType actual = child(1)->prep(scalar, envBuilder);
    checkCondition(actual.isValid() && actual.isValue(),
                   "Local function body has no valid result", error);
    if (!error && declaration->isReturnTypeSet()) {
        const ExprType expected = declaration->returnType();
        const bool shape = expected.type() == actual.type() &&
                           (expected.dim() == actual.dim() || actual.isFP(1));
        checkCondition(shape && expected.isLifeCompatible(actual),
                       "Local function result " + actual.toString() +
                       " is incompatible with " + expected.toString(), error);
    } else if (!error) declaration->setReturnType(actual);
    // Register only after checking the body: recursion and forward calls are
    // rejected, while earlier definitions remain visible in this scope.
    if (!error) outer->addFunction(declaration->name(), this);
    setType(error ? ExprType().Error() : ExprType().None().Constant());
    return _type;
}

ExprType ExprLocalFunctionNode::prep(ExprFuncNode* callerNode, bool scalarWanted,
                                    ExprVarEnvBuilder& envBuilder) const {
    bool error = false;
    if (!callerNode->checkCondition(callerNode->numChildren() == prototype()->numChildren(),
                                    "Incorrect number of local function arguments", error))
        return ExprType().Error();
    for (int i = 0; i < callerNode->numChildren(); ++i) {
        const ExprType expected = prototype()->argType(i);
        if (!callerNode->checkArg(i, expected, envBuilder)) error = true;
        const ExprType actual = callerNode->child(i)->type();
        callerNode->checkCondition(!expected.isFP(1) || actual.isFP(1),
                                  "A vector argument cannot be narrowed to a scalar", error);
    }
    return error ? ExprType().Error() : prototype()->returnType();
}""", "local definition and call preparation")
    text = replace_once(text,
        "        setTypeWithChildLife(localFunction->prep(this, wantScalar, envBuilder));",
        "        const ExprType result = localFunction->prep(this, wantScalar, envBuilder);\n"
        "        setTypeWithChildLife(result);\n"
        "        _type.setLifetime(_type, result);",
        "local function captured-variable lifetime")
    text = replace_once(text,
        "    return _type;\n}\n\nint ExprFuncNode::buildInterpreter",
        """    if (const auto* declared = envBuilder.current()->findPrototype(_name)) {
        if (checkCondition(numChildren() == declared->numChildren(),
                           "External function arguments differ from its declaration", error)) {
            for (int index = 0; index < numChildren(); ++index) {
                const ExprType actual = child(index)->type();
                const ExprType expected = declared->argType(index);
                checkCondition(actual.type() == expected.type() &&
                               (actual.dim() == expected.dim() || actual.isFP(1)) &&
                               expected.isLifeCompatible(actual),
                               "External function argument differs from its declaration", error);
            }
        }
        const ExprType expected = declared->returnType();
        checkCondition(type().type() == expected.type() && type().dim() == expected.dim() &&
                       expected.isLifeCompatible(type()),
                       "External function result differs from its declaration", error);
        if (error) setType(ExprType().Error());
    }
    return _type;
}

int ExprFuncNode::buildInterpreter""", "external declaration enforcement")
    node.write_text(text, encoding="utf-8", newline="\n")

    functions = output / "ExprFunc.h"
    text = functions.read_text(encoding="utf-8")
    text = replace_once(text,
        """    ExprFunc(ExprFuncStandard::Funcnvv* f)
        : _standardFunc(ExprFuncStandard::FUNC1VV, (void*)f), _func(0), _minargs(1), _maxargs(1) {}""",
        """    ExprFunc(ExprFuncStandard::Funcnvv* f)
        : _standardFunc(ExprFuncStandard::FUNCNVV, (void*)f), _func(0), _minargs(1), _maxargs(1) {}""",
        "single-argument variadic-vector callback keeps its actual ABI")
    functions.write_text(text, encoding="utf-8", newline="\n")

    standard = output / "ExprFuncStandard.cpp"
    text = standard.read_text(encoding="utf-8")
    if text.count("        ExprType retType;") != 2 or text.count("retType.setLifetime(childType);") != 2:
        raise RuntimeError("Pinned SeExpr standard callback lifetime patch no longer matches")
    text = text.replace("        ExprType retType;",
                        "        ExprType retType = ExprType().Constant();")
    text = text.replace("retType.setLifetime(childType);",
                        "retType.setLifetime(retType, childType);")
    standard.write_text(text, encoding="utf-8", newline="\n")

    interpreter = output / "Interpreter.cpp"
    text = interpreter.read_text(encoding="utf-8")
    text = replace_once(text, "#include <algorithm>",
                        "#include <algorithm>\n#include <cstring>",
                        "typed string operation declarations")
    text = region(text,
        "template <char op, int d>\nstruct CompareEqOp {",
        "template <char op>\nstruct CompareEqOp<op, 3> {",
        """template <char op, int d>
struct CompareEqOp {
    static int f(int* opData, double* fp, char**, std::vector<int>&) {
        bool equal = true;
        const double* first = fp + opData[0];
        const double* second = fp + opData[1];
        for (int component = 0; component < d; ++component)
            equal = equal && first[component] == second[component];
        fp[opData[2]] = op == '=' ? equal : !equal;
        return 1;
    }
};""", "vector inequality is the complement of whole-vector equality")
    text = replace_once(text,
        "int ExprLocalFunctionNode::buildInterpreter(Interpreter* interpreter) const {",
        """namespace {
int CopyLocalCallString(int* operands, double*, char** strings, std::vector<int>&) {
    auto* owner = reinterpret_cast<ExprLocalCallStringData*>(strings[operands[1]]);
    const char* value = strings[operands[0]];
    TSeExprInternal::checkStringBytes(value ? std::strlen(value) : 0);
    owner->text.assign(value ? value : "");
    strings[operands[2]] = const_cast<char*>(owner->text.c_str());
    return 1;
}
}

int ExprLocalFunctionNode::buildInterpreter(Interpreter* interpreter) const {""",
        "owned local string result operation")
    text = replace_once(text,
        "    int basePC = interpreter->nextPC();\n    ;\n    interpreter->addOp(ProcedureReturn);",
        """    if (prototype()->returnType().isFP() && child(1)->type().isFP(1) &&
        prototype()->returnType().dim() > 1) {
        const int promoted = interpreter->allocFP(prototype()->returnType().dim());
        interpreter->addOp(getTemplatizedOp<Promote>(prototype()->returnType().dim()));
        interpreter->addOperand(lastOperand);
        interpreter->addOperand(promoted);
        interpreter->endOp();
        lastOperand = promoted;
    }
    int basePC = interpreter->nextPC();
    interpreter->addOp(ProcedureReturn);""", "local return scalar promotion")
    text = region(text,
        "int ExprLocalFunctionNode::buildInterpreterForCall(const ExprFuncNode* callerNode, Interpreter* interpreter) const {",
        "int ExprNode::buildInterpreter(Interpreter* interpreter) const {",
        """int ExprLocalFunctionNode::buildInterpreterForCall(const ExprFuncNode* callerNode,
                                                            Interpreter* interpreter) const {
    // Evaluate all arguments before touching a procedure's fixed activation
    // slots. This is necessary for calls such as f(1, f(2, 3)).
    std::vector<int> arguments;
    for (int index = 0; index < callerNode->numChildren(); ++index)
        arguments.push_back(callerNode->child(index)->buildInterpreter(interpreter));
    for (int index = 0; index < callerNode->numChildren(); ++index) {
        const ExprType actual = callerNode->child(index)->type();
        if (actual.isString()) interpreter->addOp(AssignStrOp::f);
        else if (callerNode->promote(index))
            interpreter->addOp(getTemplatizedOp<Promote>(callerNode->promote(index)));
        else interpreter->addOp(getTemplatizedOp<AssignOp>(actual.dim()));
        interpreter->addOperand(arguments[index]);
        interpreter->addOperand(prototype()->interpreterOps(index));
        interpreter->endOp(false);
    }
    const bool stringResult = callerNode->type().isString();
    const int result = stringResult ? interpreter->allocPtr() :
                                      interpreter->allocFP(callerNode->type().dim());
    const int basePC = interpreter->nextPC();
    interpreter->addOp(ProcedureCall);
    const int returnAddress = interpreter->addOperand(0);
    interpreter->addOperand(_procedurePC - basePC);
    interpreter->endOp(false);
    interpreter->opData[returnAddress] = interpreter->nextPC();
    if (stringResult) {
        auto* data = new ExprLocalCallStringData;
        callerNode->setData(data);
        const int owner = interpreter->allocPtr();
        interpreter->s[owner] = reinterpret_cast<char*>(data);
        interpreter->addOp(CopyLocalCallString);
        interpreter->addOperand(_returnedDataOp);
        interpreter->addOperand(owner);
        interpreter->addOperand(result);
    } else {
        interpreter->addOp(getTemplatizedOp<AssignOp>(callerNode->type().dim()));
        interpreter->addOperand(_returnedDataOp);
        interpreter->addOperand(result);
    }
    interpreter->endOp(false);
    return result;
}""", "typed procedure calls")
    text = region(text,
        "int ExprPrototypeNode::buildInterpreter(Interpreter* interpreter) const {",
        "int ExprCompareEqNode::buildInterpreter(Interpreter* interpreter) const {",
        """int ExprPrototypeNode::buildInterpreter(Interpreter* interpreter) const {
    if (!dynamic_cast<const ExprLocalFunctionNode*>(parent())) return 0;
    _interpreterOps.clear();
    for (int index = 0; index < numChildren(); ++index) {
        const auto* variable = static_cast<const ExprVarNode*>(child(index));
        const int operand = variable->localVar()->buildInterpreter(interpreter);
        _interpreterOps.push_back(operand);
    }
    return 0;
}""", "typed procedure activation slots")
    interpreter.write_text(text, encoding="utf-8", newline="\n")

    codegen = output / "ExprLLVMCodeGeneration.cpp"
    text = codegen.read_text(encoding="utf-8")
    text = replace_once(text, "#include <array>",
                        "#include <array>\n#include <cstring>",
                        "LLVM typed string operation declarations")
    text = replace_once(text,
        "LLVM_VALUE ExprFuncNode::codegen(LLVM_BUILDER Builder) LLVM_BODY {",
        """namespace {
const char* copyLocalStringResult(const char* value, void* storage) noexcept {
    try {
        TSeExprInternal::checkStringBytes(value ? std::strlen(value) : 0);
        auto* owner = static_cast<ExprLocalCallStringData*>(storage);
        owner->text.assign(value ? value : "");
        return owner->text.c_str();
    } catch (...) {
        TSeExprInternal::captureEvaluationFailure(std::current_exception());
        return "";
    }
}
}

LLVM_VALUE ExprFuncNode::codegen(LLVM_BUILDER Builder) LLVM_BODY {""",
        "LLVM owned local string result callback")
    text = replace_once(text,
        "    Function *callee = M->getFunction(calleeName);\n    if (calleeName == \"printf\") {",
        """    Function *callee = M->getFunction(calleeName);
    if (_localFunc) {
        callee = cast<Function>(_localFunc->llvmFunction());
        const auto arguments = promoteArgs(codegenFuncCallArgs(Builder, this),
                                            Builder, callee->getFunctionType());
        LLVM_VALUE result = Builder.CreateCall(callee, arguments);
        if (!type().isString()) return result;
        auto* data = new ExprLocalCallStringData;
        setData(data);
        Type* stringType = Type::getInt8PtrTy(llvmContext);
        FunctionType* signature = FunctionType::get(stringType, {stringType, stringType}, false);
        LLVM_VALUE address = Builder.CreateIntToPtr(
            ConstantInt::get(Type::getInt64Ty(llvmContext),
                             reinterpret_cast<uint64_t>(&copyLocalStringResult)),
            PointerType::getUnqual(signature));
        LLVM_VALUE owner = Builder.CreateIntToPtr(
            ConstantInt::get(Type::getInt64Ty(llvmContext), reinterpret_cast<uint64_t>(data)),
            stringType);
        return Builder.CreateCall(address, {result, owner});
    }
    if (calleeName == "printf") {""", "LLVM local function identity")
    text = replace_once(text,
        """    } else if (callee) {
        std::vector<LLVM_VALUE> args =
            promoteArgs(codegenFuncCallArgs(Builder, this), Builder, callee->getFunctionType());
        return Builder.CreateCall(callee, args);
    }
""", "    }\n", "native callbacks do not alias runtime helper names")
    text = replace_once(text,
        "    Function *F = cast<Function>(child(0)->codegen(Builder));",
        "    Function *F = cast<Function>(child(0)->codegen(Builder));\n"
        "    _llvmFunction = F;",
        "LLVM local declaration identity")
    text = replace_once(text,
        """        AllocaInst *Alloca = createAllocaInst(Builder, AI->getType(), 1, AI->getName());
        Alloca->takeName(&*AI);
        Builder.CreateStore(&*AI, Alloca);""",
        """        const auto* argument = static_cast<const ExprVarNode*>(prototype()->arg(i));
        LLVM_VALUE Alloca = argument->localVar()->codegen(Builder, argument->name(), &*AI);
        Builder.CreateStore(&*AI, Alloca);""", "LLVM bound formal argument values")
    text = replace_once(text,
        "    Builder.CreateRet(result);\n    Builder.restoreIP(oldIP);",
        "    result = promoteToTy(result, F->getReturnType(), Builder);\n"
        "    Builder.CreateRet(result);\n    Builder.restoreIP(oldIP);",
        "LLVM local return promotion")
    text = replace_once(text,
        "LLVM_VALUE ExprPrototypeNode::codegen(LLVM_BUILDER Builder) LLVM_BODY {\n",
        "LLVM_VALUE ExprPrototypeNode::codegen(LLVM_BUILDER Builder) LLVM_BODY {\n"
        "    if (!dynamic_cast<const ExprLocalFunctionNode*>(parent())) return 0;\n",
        "external prototypes bind host callbacks rather than unresolved LLVM symbols")
    codegen.write_text(text, encoding="utf-8", newline="\n")
