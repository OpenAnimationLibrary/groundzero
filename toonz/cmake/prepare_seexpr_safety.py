"""Qualify numeric/string format functions without assertions or variadic UB."""
from pathlib import Path


def replace_once(source, before, after, label):
    if source.count(before) != 1:
        raise RuntimeError(f"Pinned SeExpr safety patch no longer matches: {label}")
    return source.replace(before, after, 1)


def apply_safety(output):
    output = Path(output)
    # A string-only operation legitimately has no floating-point registers.
    # Acquiring its unused base pointer must not dereference vector element 0.
    for name, replacements in (
        ("Interpreter.h", (("&d[0]", "d.data()"),
                           ("&s[0]", "s.data()"),
                           ("&opData[0]", "opData.data()"))),
        ("Interpreter.cpp", (("&opData[0]", "opData.data()"),)),
        ("ExprFuncX.cpp", (("&interpreter->opData[0]", "interpreter->opData.data()"),
                           ("&interpreter->d[0]", "interpreter->d.data()"),
                           ("&interpreter->s[0]", "interpreter->s.data()"))),
    ):
        path = output / name
        source = path.read_text(encoding="utf-8")
        for before, after in replacements:
            source = replace_once(source, before, after, "empty-safe interpreter storage")
        if name == "Interpreter.cpp":
            source = replace_once(source,
                "            memcpy(fp, d.data(), d.size() * sizeof(double));",
                "            if (!d.empty()) memcpy(fp, d.data(), d.size() * sizeof(double));",
                "empty string-only VarBlock copy")
            source = replace_once(source, '#include "seexprbudget.h"',
                '#include "seexprbudget.h"\n#include <cstdlib>\n#include <cstring>\n#include <limits>\n#include <new>',
                "owned string allocation dependencies")
            begin = source.index("struct BinaryStringOp {")
            end = source.index("\n};", begin) + len("\n};")
            source = source[:begin] + '''struct BinaryStringOp {
    static int f(int* opData, double*, char** c, std::vector<int>&) {
        char*& out = *reinterpret_cast<char**>(c[opData[0]]);
        const char* first = c[opData[1]];
        const char* second = c[opData[2]];
        const size_t firstSize = first ? std::strlen(first) : 0;
        const size_t secondSize = second ? std::strlen(second) : 0;
        if (firstSize > std::numeric_limits<size_t>::max() - secondSize ||
            firstSize + secondSize == std::numeric_limits<size_t>::max())
            throw std::bad_alloc();
        const size_t size = firstSize + secondSize;
        TSeExprInternal::checkStringBytes(size);
        // Both interpreter and LLVM nodes release this buffer with free().
        // Copy before releasing the previous output: a host binding may
        // legitimately refer to that output until this evaluation finishes.
        char* joined = static_cast<char*>(std::malloc(size + 1));
        if (!joined) throw std::bad_alloc();
        if (firstSize) std::memcpy(joined, first, firstSize);
        if (secondSize) std::memcpy(joined + firstSize, second, secondSize);
        joined[size] = '\\0';
        std::free(out);
        out = joined;
        c[opData[3]] = out;
        return 1;
    }
};''' + source[end:]
        if name == "ExprFuncX.cpp":
            source = replace_once(source,
                "    interpreter->addOp(EvalOp);",
                "    // ArgHandle binds both output references even when one\n"
                "    // bank is unused for this result type. Reserve that slot\n"
                "    // in both banks before constructing either reference.\n"
                "    if (node->type().isFP()) {\n"
                "        while (interpreter->s.size() <= static_cast<size_t>(outoperand))\n"
                "            interpreter->allocPtr();\n"
                "    } else {\n"
                "        while (interpreter->d.size() <= static_cast<size_t>(outoperand))\n"
                "            interpreter->allocFP(1);\n"
                "    }\n\n"
                "    interpreter->addOp(EvalOp);",
                "valid inactive custom-function output references")
        path.write_text(source, encoding="utf-8", newline="\n")

    path = Path(output) / "ExprBuiltins.cpp"
    source = path.read_text(encoding="utf-8")
    source = source.replace("#include <cassert>",
                            "#include <cassert>\n#include <memory>\n#include <stdexcept>\n#include <sstream>\n#include <cstdio>", 1)
    # Internal per-expression allocations are owned by their nodes. Keep the
    # public Data(false) default intact for SDK hosts that supply borrowed data.
    source = replace_once(source,
        "    VoronoiPointData() : jitter(-1) {}",
        "    VoronoiPointData() : ExprFuncNode::Data(true), cell(0.0), jitter(-1) {}",
        "owned initialized Voronoi cache")
    source = replace_once(source,
        "struct CurveData : public ExprFuncNode::Data {\n    Curve<T> curve;",
        "struct CurveData : public ExprFuncNode::Data {\n"
        "    CurveData() : ExprFuncNode::Data(true) {}\n    Curve<T> curve;",
        "owned scalar/color curve cache")
    source = replace_once(source,
        "        Data(func fIn, int dim) : f(fIn), dim(dim) {}",
        "        Data(func fIn, int dim) : ExprFuncNode::Data(true), f(fIn), dim(dim) {}",
        "owned getVar dispatch cache")
    for name, end_marker, value_type in (
        ("CurveFuncX", "} curve;", "double"),
        ("CCurveFuncX", "} ccurve;", "Vec3d"),
    ):
        begin = source.index("class " + name + " : public ExprFuncSimple {")
        end = source.index(end_marker, begin)
        region = source[begin:end]
        region = replace_once(region,
            "        CurveData<" + value_type + ">* data = new CurveData<" + value_type + ">;",
            "        std::unique_ptr<CurveData<" + value_type + ">> data(new CurveData<" + value_type + ">);",
            "exception-safe curve preparation")
        region = replace_once(region, "        return data;", "        return data.release();",
                              "curve preparation ownership transfer")
        source = source[:begin] + region + source[end:]
    begin = source.index("class PrintFuncX : public ExprFuncSimple {")
    end = source.index("} sprintf;", begin) + len("} sprintf;")
    # Keep the pinned source license/header. The replacement validates the
    # concrete numeric/string ABI before passing any argument to snprintf.
    source = source[:begin] + r'''
struct NativeFormatPart {
    std::string literal;
    std::string specifier;
    char conversion = 0;
};

static std::vector<NativeFormatPart> nativeFormatParts(const std::string& format,
                                                      bool console) {
    if (format.size() > 65536) throw std::runtime_error("Format string is too long");
    std::vector<NativeFormatPart> parts;
    std::string literal;
    for (size_t i = 0; i < format.size();) {
        if (format[i] != '%') { literal += format[i++]; continue; }
        const size_t start = i++;
        if (i == format.size()) throw std::runtime_error("Incomplete format specifier");
        if (format[i] == '%') { literal += '%'; ++i; continue; }
        if (!literal.empty()) { parts.push_back({literal, "", 0}); literal.clear(); }
        if (console) {
            if (format[i] != 'v' && format[i] != 'f')
                throw std::runtime_error("printf accepts only %v, %f and %%");
        } else {
            std::string flags;
            while (i < format.size() && std::string("-+ #0").find(format[i]) != std::string::npos)
                flags += format[i++];
            const auto digits = [&]() {
                size_t value = 0;
                while (i < format.size() && format[i] >= '0' && format[i] <= '9') {
                    value = value * 10 + size_t(format[i++] - '0');
                    if (value > 4096) throw std::runtime_error("Format width or precision exceeds 4096");
                }
            };
            digits();
            const bool precision = i < format.size() && format[i] == '.';
            if (precision) { ++i; digits(); }
            if (i == format.size() || std::string("diouxXceEfFgGaAs").find(format[i]) == std::string::npos)
                throw std::runtime_error("Unsupported numeric/string format conversion");
            const char code = format[i];
            const std::string allowed = code == 's' || code == 'c' ? "-" :
                code == 'd' || code == 'i' ? "-+ 0" :
                code == 'u' ? "-0" :
                code == 'o' || code == 'x' || code == 'X' ? "-#0" : "-+ #0";
            for (char flag : flags)
                if (allowed.find(flag) == std::string::npos)
                    throw std::runtime_error("Format flags do not match the argument conversion");
            if (code == 'c' && precision)
                throw std::runtime_error("Character formatting does not accept precision");
        }
        const char conversion = format[i++];
        parts.push_back({"", format.substr(start, i - start), conversion});
    }
    if (!literal.empty()) parts.push_back({literal, "", 0});
    return parts;
}

static bool nativeFormatPrep(ExprFuncNode* node, ExprVarEnvBuilder& environment,
                             bool console) {
    if (node->numChildren() < 1) {
        node->addError("Formatting requires a literal format string"); return false;
    }
    if (!node->checkArg(0, ExprType().String().Constant(), environment)) return false;
    const auto* literal = dynamic_cast<const ExprStrNode*>(node->child(0));
    if (!literal) {
        node->addError("Format must be a string literal"); return false;
    }
    try {
        const auto parts = nativeFormatParts(literal->str(), console);
        int index = 1;
        for (const auto& part : parts) {
            if (!part.conversion) continue;
            if (index >= node->numChildren()) {
                node->addError("Format requires more arguments"); return false;
            }
            const ExprType desired = part.conversion == 's' ? ExprType().String() :
                                     ExprType().FP(part.conversion == 'v' ? 3 : 1);
            if (!node->checkArg(index++, desired, environment)) return false;
        }
        if (index != node->numChildren()) {
            node->addError("Format has unused arguments"); return false;
        }
        return true;
    } catch (const std::exception& error) {
        node->addError(error.what()); return false;
    }
}

class PrintFuncX : public ExprFuncSimple {
    struct Data : ExprFuncNode::Data {
        Data() : ExprFuncNode::Data(true) {}
        std::vector<NativeFormatPart> parts;
    };
public:
    PrintFuncX() : ExprFuncSimple(false) {}
    ExprType prep(ExprFuncNode* node, bool, ExprVarEnvBuilder& environment) const override {
        return nativeFormatPrep(node, environment, true) ?
            ExprType().FP(1).Varying() : ExprType().Error();
    }
    ExprFuncNode::Data* evalConstant(const ExprFuncNode*, ArgHandle args) const override {
        auto data = std::unique_ptr<Data>(new Data);
        data->parts = nativeFormatParts(args.inStr(0), true);
        return data.release();
    }
    void eval(ArgHandle args) override {
        const auto* data = static_cast<const Data*>(args.data);
        int argument = 1;
        std::ostringstream output;
        for (const auto& part : data->parts) {
            if (!part.conversion) output << part.literal;
            else if (part.conversion == 'v') {
                const auto value = args.inFp<3>(argument++);
                output << '[' << value[0] << ',' << value[1] << ',' << value[2] << ']';
            } else output << args.inFp<1>(argument++)[0];
        }
        std::cerr << output.str() << std::endl;
        args.outFp = 0;
    }
} printf;
static const char* printf_docstring =
    "printf(string format,[scalar|vector,...])\nPrint numeric values with %f, %v and escaped %%.";

template<class T>
static std::string nativeFormatFragment(const std::string& specifier, T value) {
    const int count = std::snprintf(nullptr, 0, specifier.c_str(), value);
    if (count < 0 || count > 4096) throw std::runtime_error("Formatted value exceeds 4096 bytes");
    std::vector<char> buffer(size_t(count) + 1);
    const int written = std::snprintf(buffer.data(), buffer.size(), specifier.c_str(), value);
    if (written != count) throw std::runtime_error("Formatted value could not be produced");
    if (std::find(buffer.begin(), buffer.begin() + count, '\0') != buffer.begin() + count)
        throw std::runtime_error("Formatted strings cannot contain embedded null characters");
    return std::string(buffer.data(), size_t(count));
}

class SPrintFuncX : public ExprFuncSimple {
    struct Data : ExprFuncNode::Data {
        Data() : ExprFuncNode::Data(true) {}
        std::vector<NativeFormatPart> parts;
        std::string value;
    };
public:
    SPrintFuncX() : ExprFuncSimple(false) {}
    ExprType prep(ExprFuncNode* node, bool, ExprVarEnvBuilder& environment) const override {
        return nativeFormatPrep(node, environment, false) ?
            ExprType().String().Varying() : ExprType().Error();
    }
    ExprFuncNode::Data* evalConstant(const ExprFuncNode*, ArgHandle args) const override {
        auto data = std::unique_ptr<Data>(new Data);
        data->parts = nativeFormatParts(args.inStr(0), false);
        return data.release();
    }
    void eval(ArgHandle args) override {
        auto* data = static_cast<Data*>(args.data);
        data->value.clear();
        int argument = 1;
        for (const auto& part : data->parts) {
            std::string fragment;
            if (!part.conversion) fragment = part.literal;
            else if (part.conversion == 's')
                fragment = nativeFormatFragment(part.specifier, args.inStr(argument++));
            else {
                const double number = args.inFp<1>(argument++)[0];
                if (std::string("diouxXc").find(part.conversion) != std::string::npos) {
                    if (!std::isfinite(number)) throw std::runtime_error("Integer formatting requires a finite value");
                    if (std::string("ouxX").find(part.conversion) != std::string::npos) {
                        if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<unsigned int>::max())
                            throw std::runtime_error("Unsigned formatting value is out of range");
                        const auto integer = number < 0 ?
                            static_cast<unsigned int>(static_cast<int>(number)) :
                            static_cast<unsigned int>(number);
                        fragment = nativeFormatFragment(part.specifier, integer);
                    } else {
                        if (number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
                            throw std::runtime_error("Integer formatting value is out of range");
                        fragment = nativeFormatFragment(part.specifier, static_cast<int>(number));
                    }
                } else fragment = nativeFormatFragment(part.specifier, number);
            }
            if (data->value.size() + fragment.size() > 1048576)
                throw std::runtime_error("Formatted string exceeds the result limit");
            data->value += fragment;
        }
        args.outStr = const_cast<char*>(data->value.c_str());
    }
} sprintf;
static const char* sprintf_docstring =
    "sprintf(string format,[number|string,...])\nFormat numeric and string values with bounded width/precision; C length modifiers are unsupported.";
''' + source[end:]
    # Replace the old sprintf help too: the registration below must use the
    # documentation of the qualified conversion contract.
    old_doc = '''
static const char* sprintf_docstring =
    "sprintf(string format, [double|string, double|string, ...])\\n"
    "Returns a string formatted from the given values.  See 'man sprintf' for format details.";'''
    if old_doc not in source:
        raise RuntimeError("Pinned formatting documentation patch no longer matches")
    source = source.replace(old_doc, "", 1)
    path.write_text(source, encoding="utf-8", newline="\n")
