"""Qualify numeric/string format functions without assertions or variadic UB."""
from pathlib import Path


def apply_safety(output):
    path = Path(output) / "ExprBuiltins.cpp"
    source = path.read_text(encoding="utf-8")
    source = source.replace("#include <cassert>",
                            "#include <cassert>\n#include <memory>\n#include <stdexcept>\n#include <sstream>\n#include <cstdio>", 1)
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
