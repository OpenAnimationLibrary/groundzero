#include "tseexpr.h"

#include "seexprbudget.h"

#include <SeExpr2/ExprFunc.h>
#include <SeExpr2/ExprFuncX.h>
#include <SeExpr2/ExprNode.h>
#include <SeExpr2/Expression.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <utility>

namespace TSeExpr {
namespace {

std::atomic<std::uint64_t> expressionFailures{0};
class FailureRecord {
public:
  bool succeeded = false;
  ~FailureRecord() {
    if (!succeeded) reportFailure();
  }
};

std::string normalizedName(const std::string &name) {
  return !name.empty() && name.front() == '$' ? name.substr(1) : name;
}

bool validType(Type type) {
  return type == Type::Scalar || type == Type::Vector || type == Type::String;
}

bool validLifetime(Lifetime lifetime) {
  return lifetime == Lifetime::Constant || lifetime == Lifetime::Uniform ||
         lifetime == Lifetime::Varying;
}

bool validName(const std::string &name) {
  if (name.empty()) return false;
  const auto first = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  if (!first(static_cast<unsigned char>(name.front()))) return false;
  for (std::size_t i = 1; i < name.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (first(c) || (c >= '0' && c <= '9') || c == '.') continue;
    if (c == ':' && i + 2 < name.size() && name[i + 1] == ':' &&
        first(static_cast<unsigned char>(name[i + 2]))) {
      ++i;
      continue;
    }
    return false;
  }
  return true;
}

int dimension(Type type, int vectorDimension) {
  return type == Type::Vector ? vectorDimension : 1;
}

int dimension(const Value &value) {
  return value.type == Type::Vector ? static_cast<int>(value.number.size()) : 1;
}

SeExpr2::ExprType nativeType(Type type, int vectorDimension,
                            Lifetime lifetime = Lifetime::Varying) {
  SeExpr2::ExprType result;
  if (type == Type::String)
    result.String();
  else
    result.FP(dimension(type, vectorDimension));
  switch (lifetime) {
  case Lifetime::Constant: return result.Constant();
  case Lifetime::Uniform: return result.Uniform();
  case Lifetime::Varying: return result.Varying();
  }
  return result.Varying();
}

void initializeFunctions() {
  static std::once_flag once;
  std::call_once(once, [] { SeExpr2::ExprFunc::init(); });
}

bool validValue(const Value &value, const Options &options) {
  if (!validType(value.type)) return false;
  if (value.type == Type::String)
    return value.text.find('\0') == std::string::npos &&
           value.text.size() <= options.maxStringBytes;
  if (value.number.empty() ||
      value.number.size() > options.maxDimension ||
      value.number.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      (value.type == Type::Scalar && value.number.size() != 1))
    return false;
  return !options.requireFiniteResult || value.isFinite();
}

bool sourceWithinLimits(const std::string &source, const Options &options,
                        Error &error) {
  if (source.empty()) {
    error.message = "Expression source is empty.";
    return false;
  }
  if (source.size() > options.maxSourceBytes ||
      source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    error.message = "Expression source exceeds the configured size limit.";
    return false;
  }
  if (source.find('\0') != std::string::npos) {
    error.message = "Expression source contains an embedded null character.";
    return false;
  }
  std::size_t depth = 0;
  bool comment = false;
  char quote = 0;
  for (std::size_t i = 0; i < source.size(); ++i) {
    const char c = source[i];
    if (comment) {
      if (c == '\n') comment = false;
      continue;
    }
    if (quote) {
      if (c == '\\' && i + 1 < source.size()) {
        ++i;
      } else if (c == quote) {
        quote = 0;
      }
      continue;
    }
    if (c == '#') {
      comment = true;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote = c;
      continue;
    }
    if (c == '(' || c == '[' || c == '{') {
      if (++depth > options.maxNesting) {
        error = {"Expression nesting exceeds the configured limit.",
                 static_cast<int>(i), static_cast<int>(i + 1)};
        return false;
      }
    } else if ((c == ')' || c == ']' || c == '}') && depth) {
      --depth;
    }
    // An upstream FLOAT[N] declaration allocates interpreter registers. Check
    // its numeric dimension before parser conversion/allocation. Do not scan
    // quoted strings or comments as declarations.
    if (source.compare(i, 5, "FLOAT") == 0 &&
        (i == 0 || !(std::isalnum(static_cast<unsigned char>(source[i - 1])) ||
                     source[i - 1] == '_' || source[i - 1] == '$'))) {
      std::size_t next = i + 5;
      while (next < source.size() &&
             std::isspace(static_cast<unsigned char>(source[next])))
        ++next;
      if (next < source.size() && source[next] == '[') {
        ++next;
        while (next < source.size() &&
               std::isspace(static_cast<unsigned char>(source[next])))
          ++next;
        std::size_t end = next;
        while (end < source.size() &&
               (std::isdigit(static_cast<unsigned char>(source[end])) ||
                source[end] == '.' || source[end] == 'e' ||
                source[end] == 'E' || source[end] == '+' || source[end] == '-'))
          ++end;
        if (next != end) {
          try {
            const double count = std::stod(source.substr(next, end - next));
            if (!std::isfinite(count) || count < 1 ||
                count > static_cast<double>(options.maxDimension) ||
                std::floor(count) != count)
              throw std::out_of_range("dimension");
          } catch (...) {
            error = {"Expression vector dimension exceeds the configured limit.",
                     static_cast<int>(next), static_cast<int>(end)};
            return false;
          }
        }
      }
    }
  }
  return true;
}

}  // namespace

Value Value::scalar(double value) {
  Value result;
  result.type = Type::Scalar;
  result.number = {value};
  return result;
}

Value Value::vector(double x, double y, double z) {
  return numeric({x, y, z});
}

Value Value::numeric(std::vector<double> values) {
  Value result;
  result.type = values.size() == 1 ? Type::Scalar : Type::Vector;
  result.number = std::move(values);
  return result;
}

Value Value::string(std::string value) {
  Value result;
  result.type = Type::String;
  result.number.clear();
  result.text = std::move(value);
  return result;
}

bool Value::isFinite() const {
  return type == Type::String ||
         std::all_of(number.begin(), number.end(),
                     [](double value) { return std::isfinite(value); });
}

class Program::Imp {
public:
  class VariableRef final : public SeExpr2::ExprVarRef {
  public:
    Value value;
    Lifetime lifetime;
    explicit VariableRef(const Variable &variable)
        : SeExpr2::ExprVarRef(nativeType(variable.value.type,
                                        dimension(variable.value),
                                        variable.lifetime))
        , value(variable.value)
        , lifetime(variable.lifetime) {}
    void eval(double *result) override {
      std::copy(value.number.begin(), value.number.end(), result);
    }
    void eval(const char **result) override { *result = value.text.c_str(); }
  };

  class Callback final : public SeExpr2::ExprFuncSimple {
    struct Data final : public SeExpr2::ExprFuncNode::Data {
      Data() : SeExpr2::ExprFuncNode::Data(true) {}
      std::vector<Value> arguments;
      std::string resultString;
    };
    Function m_function;
    const Options &m_options;

    Type argumentType(int index) const {
      if (m_function.argumentTypes.empty()) return Type::Scalar;
      return m_function.argumentTypes[std::min<std::size_t>(
          static_cast<std::size_t>(index), m_function.argumentTypes.size() - 1)];
    }
    int argumentDimension(int index) const {
      if (m_function.argumentDimensions.empty()) return 3;
      return m_function.argumentDimensions[std::min<std::size_t>(
          static_cast<std::size_t>(index),
          m_function.argumentDimensions.size() - 1)];
    }

  public:
    SeExpr2::ExprFunc function;
    Callback(Function definition, const Options &options)
        : SeExpr2::ExprFuncSimple(true)
        , m_function(std::move(definition))
        , m_options(options)
        , function(*this,
                   m_function.minArgs < 0
                       ? static_cast<int>(m_function.argumentTypes.size())
                       : m_function.minArgs,
                   m_function.maxArgs < 0
                       ? static_cast<int>(m_function.argumentTypes.size())
                       : m_function.maxArgs) {}

    SeExpr2::ExprType prep(SeExpr2::ExprFuncNode *node, bool,
                          SeExpr2::ExprVarEnvBuilder &env) const override {
      bool valid = true;
      for (int i = 0; i < node->numChildren(); ++i) {
        const auto expected = nativeType(argumentType(i), argumentDimension(i));
        const bool checked = node->checkArg(i, expected, env);
        valid = checked && valid;
        // Upstream value compatibility deliberately permits scalar promotion
        // in both directions. A native scalar callback has one register and
        // must not silently consume the first channel of a vector argument.
        if (checked && expected.isFP(1) && !node->child(i)->type().isFP(1)) {
          node->addError("A scalar function argument cannot consume a vector value.");
          valid = false;
        }
      }
      return valid ? nativeType(m_function.resultType, m_function.resultDimension)
                   : SeExpr2::ExprType().Error();
    }

    SeExpr2::ExprFuncNode::Data *evalConstant(
        const SeExpr2::ExprFuncNode *, ArgHandle args) const override {
      auto *data = new Data;
      data->arguments.reserve(static_cast<std::size_t>(args.nargs()));
      for (int i = 0; i < args.nargs(); ++i) {
        if (argumentType(i) == Type::String)
          data->arguments.push_back(Value::string({}));
        else if (argumentType(i) == Type::Scalar)
          data->arguments.push_back(Value::scalar(0));
        else {
          Value argument = Value::numeric(
              std::vector<double>(static_cast<std::size_t>(argumentDimension(i))));
          // The host signature can deliberately distinguish a one-component
          // vector from a scalar, even though both use upstream FP(1).
          argument.type = Type::Vector;
          data->arguments.push_back(std::move(argument));
        }
      }
      return data;
    }

    void evaluateCallback(ArgHandle args) {
      auto &data = *static_cast<Data *>(args.data);
      for (int i = 0; i < args.nargs(); ++i) {
        Value &value = data.arguments[static_cast<std::size_t>(i)];
        if (value.type == Type::String) {
          const char *text = args.inStr(i);
          value.text = text ? text : "";
        } else {
          const double *input = &args.inFp<1>(i)[0];
          std::copy(input, input + value.number.size(), value.number.begin());
        }
      }
      if (m_options.cancelled && m_options.cancelled())
        throw std::runtime_error("Expression evaluation cancelled.");
      Value result = m_function.evaluate(data.arguments);
      if (result.type != m_function.resultType ||
          (result.type != Type::String &&
           dimension(result) != dimension(m_function.resultType,
                                           m_function.resultDimension)) ||
          !validValue(result, m_options))
        throw std::runtime_error("Custom expression function returned an invalid value.");
      if (result.type == Type::String) {
        data.resultString = std::move(result.text);
        args.outStr = const_cast<char *>(data.resultString.c_str());
      } else {
        std::copy(result.number.begin(), result.number.end(), &args.outFp);
      }
    }

    void eval(ArgHandle args) override {
      if (m_options.backend != Backend::LLVM) {
        evaluateCallback(args);
        return;
      }
      // C++ exception unwinding through generated JIT frames is unsupported.
      // Preserve the exception in the owning evaluation guard, return a safe
      // temporary value, then rethrow at the native evaluate boundary.
      try {
        evaluateCallback(args);
      } catch (...) {
        TSeExprInternal::captureEvaluationFailure(std::current_exception());
        if (m_function.resultType == Type::String)
          args.outStr = const_cast<char *>("");
        else
          std::fill_n(&args.outFp,
                      dimension(m_function.resultType, m_function.resultDimension),
                      0.0);
      }
    }
  };

  class NativeExpression final : public SeExpr2::Expression {
    Imp &m_host;

  public:
    explicit NativeExpression(Imp &host)
        : SeExpr2::Expression(host.source,
                             nativeType(host.options.resultType,
                                        host.options.resultDimension),
                             host.options.backend == Backend::LLVM
                                 ? SeExpr2::Expression::UseLLVM
                                 : SeExpr2::Expression::UseInterpreter)
        , m_host(host) {}

    SeExpr2::ExprVarRef *resolveVar(const std::string &name) const override {
      const auto found = m_host.variables.find(name);
      if (found == m_host.variables.end()) return nullptr;
      m_host.resolvedVariables.insert(name);
      return found->second.get();
    }
    SeExpr2::ExprFunc *resolveFunc(const std::string &name) const override {
      const auto found = m_host.functions.find(name);
      if (found != m_host.functions.end()) {
        m_host.resolvedFunctions.insert(name);
        return &found->second->function;
      }
      const auto *function = SeExpr2::ExprFunc::lookup(name);
      if (function) {
        m_host.resolvedFunctions.insert(name);
        m_host.resolvedBuiltins.insert(name);
      }
      // Upstream's legacy resolveFunc interface is mutable; the compiler only
      // reads this globally owned definition, matching its fallback lookup.
      return const_cast<SeExpr2::ExprFunc *>(function);
    }
  };

  std::string source;
  Options options;
  std::vector<Error> errors;
  std::map<std::string, std::unique_ptr<VariableRef>> variables;
  std::map<std::string, std::unique_ptr<Callback>> functions;
  std::set<std::string> resolvedVariables;
  std::set<std::string> resolvedFunctions;
  std::set<std::string> resolvedBuiltins;
  // Destroy the expression and its per-call data before functions/variables.
  std::unique_ptr<NativeExpression> expression;
  bool valid = false;
  bool compiling = false;
  bool evaluating = false;
  bool rejectedReentry = false;

  bool busy() const { return compiling || evaluating; }

  class CompilationScope {
    Imp &m_host;

  public:
    explicit CompilationScope(Imp &host) : m_host(host) { m_host.compiling = true; }
    ~CompilationScope() { m_host.compiling = false; }
  };

  class EvaluationScope {
    Imp &m_host;

  public:
    explicit EvaluationScope(Imp &host) : m_host(host) { m_host.evaluating = true; }
    ~EvaluationScope() { m_host.evaluating = false; }
  };

  bool rejectReentry(const char *message) {
    rejectedReentry = true;
    return fail(message);
  }

  bool fail(std::string message, int begin = 0, int end = 0) {
    errors.push_back({std::move(message), begin, end});
    return false;
  }
};

Program::Program() : m_imp(new Imp) {}
Program::~Program() = default;
Program::Program(Program &&other) {
  if (other.m_imp && other.m_imp->busy()) {
    other.m_imp->rejectedReentry = true;
    throw std::logic_error("An active expression program cannot be moved.");
  }
  m_imp = std::move(other.m_imp);
}

Program &Program::operator=(Program &&other) {
  if (this == &other) return *this;
  if ((m_imp && m_imp->busy()) || (other.m_imp && other.m_imp->busy())) {
    if (m_imp && m_imp->busy()) m_imp->rejectedReentry = true;
    if (other.m_imp && other.m_imp->busy()) other.m_imp->rejectedReentry = true;
    throw std::logic_error("An active expression program cannot be moved.");
  }
  m_imp = std::move(other.m_imp);
  return *this;
}

std::uint64_t failureCount() {
  return expressionFailures.load(std::memory_order_relaxed);
}

void reportFailure() {
  expressionFailures.fetch_add(1, std::memory_order_relaxed);
}

bool Program::compile(const std::string &sourceInput,
                      const std::vector<Variable> &variables,
                      const std::vector<Function> &functions,
                      const Options &optionsInput) {
  FailureRecord outcome;
  if (m_imp && m_imp->busy())
    return m_imp->rejectReentry("An active expression program cannot be recompiled.");
  // Copy before replacing the old program: sourceInput may alias source().
  auto next = std::make_unique<Imp>();
  next->source = sourceInput;
  next->options = optionsInput;
  m_imp = std::move(next);
  const auto &source = m_imp->source;
  const auto &options = m_imp->options;
  Imp::CompilationScope compilation(*m_imp);
  if (!validType(options.resultType))
    return m_imp->fail("The expression result type is invalid.");
  if (!backendAvailable(options.backend))
    return m_imp->fail("The requested expression backend is unavailable in this build.");
  Error error;
  if (!sourceWithinLimits(source, options, error)) {
    m_imp->errors.push_back(std::move(error));
    return false;
  }
  if (variables.size() > options.maxVariables ||
      functions.size() > options.maxVariables)
    return m_imp->fail("Expression bindings exceed the configured limit.");
  if (options.maxDimension == 0 || options.maxInstructions == 0 ||
      options.maxStringBytes == 0 ||
      options.maxDimension > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      options.maxVariables > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      (options.resultType == Type::Vector &&
       (options.resultDimension < 1 ||
        static_cast<std::size_t>(options.resultDimension) > options.maxDimension)))
    return m_imp->fail("Expression result dimensions or evaluation limits are invalid.");
  if (options.backend == Backend::Interpreter && options.maxDimension > 16)
    return m_imp->fail("The interpreter supports numeric dimensions from 1 through 16.");
  try {
    if (options.cancelled && options.cancelled())
      return m_imp->fail("Expression compilation cancelled.");
    initializeFunctions();
    TSeExprInternal::EvaluationBudgetGuard budget(options.maxInstructions,
                                                 m_imp->options.cancelled,
                                                 options.maxStringBytes,
                                                 options.maxNesting);
    for (const auto &variable : variables) {
      const std::string name = normalizedName(variable.name);
      if (!validName(name) || !validValue(variable.value, options) ||
          !validLifetime(variable.lifetime))
        return m_imp->fail("Expression variable has an invalid name, type or value.");
      if (!m_imp->variables.emplace(name, std::make_unique<Imp::VariableRef>(variable))
               .second)
        return m_imp->fail("Expression variable names must be unique.");
    }
    for (const auto &function : functions) {
      if (function.argumentTypes.size() > options.maxVariables ||
          function.argumentDimensions.size() > options.maxVariables ||
          !validType(function.resultType) || function.minArgs < -1 ||
          function.maxArgs < -1 ||
          !std::all_of(function.argumentTypes.begin(), function.argumentTypes.end(),
                       [](Type type) { return validType(type); }))
        return m_imp->fail("Custom expression function has an invalid type or signature.");
      const int minArgs = function.minArgs < 0
                              ? static_cast<int>(function.argumentTypes.size())
                              : function.minArgs;
      const int maxArgs = function.maxArgs < 0
                              ? static_cast<int>(function.argumentTypes.size())
                              : function.maxArgs;
      if (!validName(function.name) || !function.evaluate || minArgs < 0 ||
          maxArgs < minArgs || maxArgs > static_cast<int>(options.maxVariables) ||
          (function.resultType == Type::Vector &&
           (function.resultDimension < 1 ||
            static_cast<std::size_t>(function.resultDimension) > options.maxDimension)))
        return m_imp->fail("Custom expression function has an invalid signature.");
      for (int count : function.argumentDimensions)
        if (count < 1 || static_cast<std::size_t>(count) > options.maxDimension)
          return m_imp->fail("Custom expression argument dimension is invalid.");
      if (!m_imp->functions
               .emplace(function.name,
                        std::make_unique<Imp::Callback>(function, m_imp->options))
               .second)
        return m_imp->fail("Custom expression function names must be unique.");
    }
    m_imp->expression = std::make_unique<Imp::NativeExpression>(*m_imp);
    if (!m_imp->expression->isValid()) {
      const int length = static_cast<int>(source.size());
      for (const auto &nativeError : m_imp->expression->getErrors()) {
        const int begin = std::max(0, std::min(length, nativeError.startPos));
        const int end = nativeError.endPos >= length
                            ? length
                            : std::max(begin, nativeError.endPos + 1);
        m_imp->errors.push_back({nativeError.error, begin, end});
      }
      if (m_imp->errors.empty())
        m_imp->fail(m_imp->expression->parseError().empty()
                        ? "Expression could not be compiled."
                        : m_imp->expression->parseError());
      return false;
    }
    if (!options.allowConsoleOutput && m_imp->resolvedBuiltins.count("printf"))
      return m_imp->fail("Console output is unavailable in this expression context.");
    if (options.resultType == Type::Scalar &&
        !m_imp->expression->returnType().isFP(1))
      return m_imp->fail("This expression context requires a scalar result.");
    if (options.cancelled && options.cancelled())
      return m_imp->fail("Expression compilation cancelled.");
    TSeExprInternal::rethrowEvaluationFailure();
    if (m_imp->rejectedReentry)
      return m_imp->fail("Expression compilation rejected a reentrant operation.");
    m_imp->valid = true;
    outcome.succeeded = true;
    return true;
  } catch (const std::exception &exception) {
    return m_imp->fail(exception.what());
  } catch (...) {
    if (options.exceptionMessage) {
      try {
        return m_imp->fail(options.exceptionMessage(std::current_exception()));
      } catch (...) {
      }
    }
    return m_imp->fail("Expression compilation failed in a host callback.");
  }
}

bool Program::isValid() const { return m_imp && m_imp->valid; }

bool Program::setVariable(const std::string &name, const Value &value) {
  if (!m_imp) return false;
  if (m_imp->busy())
    return m_imp->rejectReentry("Expression variables cannot change during an active operation.");
  const auto found = m_imp->variables.find(normalizedName(name));
  if (found == m_imp->variables.end())
    return m_imp->fail("Expression variable is not bound.");
  auto &variable = *found->second;
  if (variable.lifetime == Lifetime::Constant)
    return m_imp->fail("Constant expression variables require recompilation to change.");
  if (variable.value.type != value.type ||
      (value.type != Type::String && dimension(variable.value) != dimension(value)) ||
      !validValue(value, m_imp->options))
    return m_imp->fail("Expression variable update changes its type or has an invalid value.");
  variable.value = value;
  return true;
}

bool Program::setScalar(const std::string &name, double value) {
  if (!m_imp) return false;
  if (m_imp->busy())
    return m_imp->rejectReentry("Expression variables cannot change during an active operation.");
  const auto found = m_imp->variables.find(normalizedName(name));
  if (found == m_imp->variables.end())
    return m_imp->fail("Expression variable is not bound.");
  auto &variable = *found->second;
  if (variable.lifetime == Lifetime::Constant)
    return m_imp->fail("Constant expression variables require recompilation to change.");
  if (variable.value.type != Type::Scalar ||
      (m_imp->options.requireFiniteResult && !std::isfinite(value)))
    return m_imp->fail("Expression scalar update has an invalid type or value.");
  variable.value.number[0] = value;
  return true;
}

bool Program::setVector(const std::string &name, double x, double y, double z) {
  if (!m_imp) return false;
  if (m_imp->busy())
    return m_imp->rejectReentry("Expression variables cannot change during an active operation.");
  const auto found = m_imp->variables.find(normalizedName(name));
  if (found == m_imp->variables.end())
    return m_imp->fail("Expression variable is not bound.");
  auto &variable = *found->second;
  if (variable.lifetime == Lifetime::Constant)
    return m_imp->fail("Constant expression variables require recompilation to change.");
  if (variable.value.type != Type::Vector || variable.value.number.size() != 3 ||
      (m_imp->options.requireFiniteResult &&
       (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))))
    return m_imp->fail("Expression vector update has an invalid type or value.");
  variable.value.number[0] = x;
  variable.value.number[1] = y;
  variable.value.number[2] = z;
  return true;
}

bool Program::setString(const std::string &name, const std::string &value) {
  return setVariable(name, Value::string(value));
}

bool Program::evaluate(Value &result) {
  FailureRecord outcome;
  if (m_imp && m_imp->busy())
    return m_imp->rejectReentry("Reentrant evaluation on the same expression program is unsupported.");
  if (!isValid()) return false;
  m_imp->errors.clear();
  m_imp->rejectedReentry = false;
  Imp::EvaluationScope evaluation(*m_imp);
  try {
    if (m_imp->options.cancelled && m_imp->options.cancelled())
      return m_imp->fail("Expression evaluation cancelled.");
    TSeExprInternal::EvaluationBudgetGuard budget(m_imp->options.maxInstructions,
                                                 m_imp->options.cancelled,
                                                 m_imp->options.maxStringBytes,
                                                 m_imp->options.maxNesting);
    if (m_imp->options.resultType == Type::String) {
      const char *text = m_imp->expression->evalStr();
      TSeExprInternal::rethrowEvaluationFailure();
      if (m_imp->rejectedReentry)
        return m_imp->fail("Expression evaluation rejected a reentrant operation.");
      if (!text) return m_imp->fail("Expression returned no string value.");
      if (std::strlen(text) > m_imp->options.maxStringBytes)
        return m_imp->fail("Expression string size limit exceeded.");
      if (m_imp->options.cancelled && m_imp->options.cancelled())
        return m_imp->fail("Expression evaluation cancelled.");
      result.type = Type::String;
      result.text = text;
      result.number.clear();
    } else {
      const double *numbers = m_imp->expression->evalFP();
      TSeExprInternal::rethrowEvaluationFailure();
      if (m_imp->rejectedReentry)
        return m_imp->fail("Expression evaluation rejected a reentrant operation.");
      if (!numbers) return m_imp->fail("Expression returned no numeric value.");
      const int count = dimension(m_imp->options.resultType,
                                  m_imp->options.resultDimension);
      if (m_imp->options.requireFiniteResult &&
          !std::all_of(numbers, numbers + count,
                       [](double value) { return std::isfinite(value); }))
        return m_imp->fail("Expression result is nonfinite.");
      if (m_imp->options.cancelled && m_imp->options.cancelled())
        return m_imp->fail("Expression evaluation cancelled.");
      result.type = m_imp->options.resultType;
      result.number.assign(numbers, numbers + count);
      result.text.clear();
    }
    outcome.succeeded = true;
    return true;
  } catch (const std::exception &exception) {
    return m_imp->fail(exception.what());
  } catch (...) {
    if (m_imp->options.exceptionMessage) {
      try {
        return m_imp->fail(
            m_imp->options.exceptionMessage(std::current_exception()));
      } catch (...) {
      }
    }
    return m_imp->fail("Expression evaluation failed in a host callback.");
  }
}

bool Program::evaluateBatch(const std::vector<std::vector<Variable>> &rows,
                            std::vector<Value> &results) {
  FailureRecord outcome;
  results.clear();
  if (m_imp && m_imp->busy())
    return m_imp->rejectReentry("Reentrant batch evaluation on the same program is unsupported.");
  if (!isValid()) return false;
  for (const auto &row : rows) {
    for (const auto &variable : row)
      if (!setVariable(variable.name, variable.value)) {
        results.clear();
        return false;
      }
    Value value;
    if (!evaluate(value)) {
      results.clear();
      return false;
    }
    results.push_back(std::move(value));
  }
  outcome.succeeded = true;
  return true;
}

const std::string &Program::source() const {
  static const std::string empty;
  return m_imp ? m_imp->source : empty;
}
Type Program::resultType() const {
  return m_imp ? m_imp->options.resultType : Type::Vector;
}
const std::vector<Error> &Program::errors() const {
  static const std::vector<Error> empty;
  return m_imp ? m_imp->errors : empty;
}

std::string Program::errorMessage() const {
  if (!m_imp) return "Expression program has no compiled state.";
  std::string result;
  for (const auto &error : m_imp->errors) {
    if (!result.empty()) result += '\n';
    result += error.message;
  }
  return result;
}

std::vector<std::string> Program::usedVariables() const {
  std::vector<std::string> result;
  if (m_imp && m_imp->expression)
    result.assign(m_imp->resolvedVariables.begin(), m_imp->resolvedVariables.end());
  return result;
}

std::vector<std::string> Program::usedFunctions() const {
  std::vector<std::string> result;
  if (!m_imp || !m_imp->expression) return result;
  for (const auto &name : m_imp->resolvedFunctions)
    if (m_imp->expression->usesFunc(name)) result.push_back(name);
  return result;
}

std::vector<FunctionInfo> Program::builtinFunctions() {
  initializeFunctions();
  std::vector<std::string> names;
  SeExpr2::ExprFunc::getFunctionNames(names);
  std::sort(names.begin(), names.end());
  std::vector<FunctionInfo> result;
  result.reserve(names.size());
  for (const auto &name : names) {
    const auto *function = SeExpr2::ExprFunc::lookup(name);
    if (!function) continue;
    result.push_back({name, SeExpr2::ExprFunc::getDocString(name.c_str()),
                      function->minArgs(), function->maxArgs(),
                      function->funcx()->isThreadSafe()});
  }
  return result;
}

bool Program::backendAvailable(Backend backend) {
  if (backend == Backend::Interpreter) return true;
  if (backend != Backend::LLVM) return false;
#if defined(OT_SEEXPR_HAS_LLVM) && OT_SEEXPR_HAS_LLVM
  return true;
#else
  return false;
#endif
}

std::string Program::engineVersion() {
  return "SeExpr2-8f8c8f2c5e27e96fae70d6b82ac1ff4f4811d6dc";
}

unsigned int Program::functionPolicyVersion() { return 1; }

}  // namespace TSeExpr
