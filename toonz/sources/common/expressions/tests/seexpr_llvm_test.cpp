#include "tseexpr.h"
#include "seexpr_equality_cases.h"
#include "seexpr_standard_cases.h"

#include <SeExpr2/ExprFunc.h>
#include <SeExpr2/Expression.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

void equal(const TSeExpr::Value &first, const TSeExpr::Value &second,
           const std::string &source) {
  require(first.type == second.type, "backend output types differ: " + source);
  if (first.type == TSeExpr::Type::String) {
    require(first.text == second.text, "backend strings differ: " + source);
    return;
  }
  require(first.number.size() == second.number.size(),
          "backend vector sizes differ: " + source);
  for (std::size_t index = 0; index < first.number.size(); ++index) {
    const double magnitude = std::max(1.0, std::abs(first.number[index]));
    require(std::abs(first.number[index] - second.number[index]) <= 1e-10 * magnitude,
            "backend numbers differ: " + source);
  }
}

void parity(const std::string &source, TSeExpr::Type type,
            const std::vector<TSeExpr::Variable> &variables = {},
            const std::vector<TSeExpr::Function> &functions = {}, int dimension = 3) {
  TSeExpr::Program interpreted, compiled;
  TSeExpr::Options options;
  options.resultType = type;
  options.resultDimension = dimension;
  if (!interpreted.compile(source, variables, functions, options))
    throw std::runtime_error("interpreter compile failed: " + interpreted.errorMessage() +
                             "\nFixture: " + source);
  options.backend = TSeExpr::Backend::LLVM;
  if (!compiled.compile(source, variables, functions, options))
    throw std::runtime_error("LLVM compile failed: " + compiled.errorMessage() +
                             "\nFixture: " + source);
  for (int repeat = 0; repeat < 3; ++repeat) {
    TSeExpr::Value first, second;
    require(interpreted.evaluate(first), interpreted.errorMessage());
    require(compiled.evaluate(second), compiled.errorMessage());
    equal(first, second, source);
  }
}

struct StringCase {
  const char *source;
  TSeExpr::Value expected;
};

void stringCallbackErrors() {
  using namespace TSeExpr;
  bool fail = false;
  Function function;
  function.name = "stringFault";
  function.resultType = Type::String;
  function.argumentTypes = {Type::String};
  function.evaluate = [&](const std::vector<Value> &arguments) {
    if (fail) throw std::runtime_error("fixture string callback failure");
    return arguments.at(0);
  };
  const StringCase cases[] = {
      {"stringFault($s)+\" suffix\"", Value::string("sample suffix")},
      {"\"prefix \"+stringFault($s)", Value::string("prefix sample")},
      {"stringFault($s)==\"sample\"", Value::scalar(1)},
      {"stringFault($s)!=\"other\"", Value::scalar(1)},
      {"sprintf(\"[%s]\",stringFault($s))", Value::string("[sample]")},
      {"def STRING wrap(STRING a){stringFault(a)+\"!\"} wrap($s)==\"sample!\"",
       Value::scalar(1)}};
  for (const auto &fixture : cases) {
    Options options;
    options.backend = Backend::LLVM;
    options.resultType = fixture.expected.type;
    Program program;
    require(program.compile(fixture.source, {{"s", Value::string("sample")}},
                            {function}, options),
            "String callback fixture did not compile: " + program.errorMessage());
    Value result;
    fail = false;
    require(program.evaluate(result), program.errorMessage());
    equal(result, fixture.expected, fixture.source);
    fail = true;
    for (int attempt = 0; attempt < 2; ++attempt)
      require(!program.evaluate(result) &&
                  program.errorMessage().find("fixture string callback failure") != std::string::npos,
              "String callback failure was lost inside a generated consumer: " +
                  std::string(fixture.source));
    fail = false;
    require(program.evaluate(result), "String callback did not recover: " + program.errorMessage());
    equal(result, fixture.expected, fixture.source);
  }

  // Every instruction limit through this expression must either report its
  // budget error or return the correct result. This covers an early callback
  // return after its argument evaluation without coupling to node counts.
  bool sawEvaluationBudget = false;
  bool sawSuccess = false;
  for (std::size_t limit = 1; limit <= 32; ++limit) {
    Options options;
    options.backend = Backend::LLVM;
    options.resultType = Type::Scalar;
    options.maxInstructions = limit;
    Program program;
    if (!program.compile("stringFault($s)==\"sample\"",
                         {{"s", Value::string("sample")}}, {function}, options)) {
      require(program.errorMessage().find("budget") != std::string::npos,
              "Budgeted string callback compilation reported an unrelated failure");
      continue;
    }
    Value result;
    if (program.evaluate(result)) {
      equal(result, Value::scalar(1), "budgeted string callback comparison");
      sawSuccess = true;
    } else {
      require(program.errorMessage().find("budget") != std::string::npos,
              "Budgeted string callback reported an unrelated failure");
      sawEvaluationBudget = true;
    }
  }
  require(sawEvaluationBudget && sawSuccess,
          "String callback budget sweep did not cover failed and successful JIT evaluation");
}

struct SDKFailures {
  bool variable = false;
  bool setup = false;
  bool callback = false;
};

class SDKStringExpression final : public SeExpr2::Expression {
  class Variable final : public SeExpr2::ExprVarRef {
    SDKFailures &m_failures;

  public:
    explicit Variable(SDKFailures &failures)
        : SeExpr2::ExprVarRef(SeExpr2::ExprType().String().Varying()), m_failures(failures) {}
    void eval(double *) override { throw std::runtime_error("unexpected numeric variable callback"); }
    void eval(const char **result) override {
      if (m_failures.variable) {
        *result = nullptr;
        throw std::runtime_error("fixture SDK string variable failure");
      }
      *result = "sample";
    }
  };

  class Function final : public SeExpr2::ExprFuncSimple {
    SDKFailures &m_failures;

  public:
    explicit Function(SDKFailures &failures) : ExprFuncSimple(true), m_failures(failures) {}
    SeExpr2::ExprType prep(SeExpr2::ExprFuncNode *node, bool,
                          SeExpr2::ExprVarEnvBuilder &environment) const override {
      return node->checkArg(0, SeExpr2::ExprType().String(), environment)
                 ? SeExpr2::ExprType().String().Varying()
                 : SeExpr2::ExprType().Error();
    }
    SeExpr2::ExprFuncNode::Data *evalConstant(const SeExpr2::ExprFuncNode *,
                                            ArgHandle arguments) const override {
      if (arguments.data)
        throw std::runtime_error("fixture SDK setup received an invalid initial data pointer");
      if (m_failures.setup) throw std::runtime_error("fixture SDK string setup failure");
      return new SeExpr2::ExprFuncNode::Data(true);
    }
    void eval(ArgHandle arguments) override {
      if (m_failures.callback) {
        arguments.outStr = nullptr;
        throw std::runtime_error("fixture SDK string callback failure");
      }
      arguments.outStr = arguments.inStr(0);
    }
  };

  mutable Variable m_variable;
  Function m_simple;
  mutable SeExpr2::ExprFunc m_function;
  const bool m_stringResult;

public:
  SDKStringExpression(const StringCase &fixture, SDKFailures &failures)
      : Expression(fixture.source,
                   fixture.expected.type == TSeExpr::Type::String
                       ? SeExpr2::ExprType().String().Varying()
                       : SeExpr2::ExprType().FP(1).Varying(),
                   Expression::UseLLVM)
      , m_variable(failures)
      , m_simple(failures)
      , m_function(m_simple, 1, 1)
      , m_stringResult(fixture.expected.type == TSeExpr::Type::String) {}
  SeExpr2::ExprVarRef *resolveVar(const std::string &name) const override {
    return name == "s" ? &m_variable : nullptr;
  }
  SeExpr2::ExprFunc *resolveFunc(const std::string &name) const override {
    return name == "sdkFault" ? &m_function : nullptr;
  }
  TSeExpr::Value evaluate() {
    return m_stringResult ? TSeExpr::Value::string(evalStr())
                          : TSeExpr::Value::scalar(evalFP()[0]);
  }
};

void sdkStringCallbackErrors() {
  using namespace TSeExpr;
  const StringCase cases[] = {
      {"sdkFault($s)+\" suffix\"", Value::string("sample suffix")},
      {"sdkFault($s)==\"sample\"", Value::scalar(1)},
      {"sdkFault($s)!=\"other\"", Value::scalar(1)},
      {"sprintf(\"[%s]\",sdkFault($s))", Value::string("[sample]")}};
  for (const auto &fixture : cases) {
    for (int failure = 0; failure < 3; ++failure) {
      SDKFailures failures;
      SDKStringExpression expression(fixture, failures);
      require(expression.isValid(), "SDK string fixture did not compile: " + expression.parseError());
      failures.variable = failure == 0;
      failures.setup = failure == 1;
      failures.callback = failure == 2;
      for (int attempt = 0; attempt < 2; ++attempt) {
        bool reported = false;
        try {
          expression.evaluate();
        } catch (const std::runtime_error &error) {
          const std::string expected = failure == 0 ? "fixture SDK string variable failure" :
                                       failure == 1 ? "fixture SDK string setup failure" :
                                                      "fixture SDK string callback failure";
          require(std::string(error.what()).find(expected) != std::string::npos,
                  "SDK string evaluation replaced the original callback failure");
          reported = true;
        }
        require(reported, "SDK swallowed a string exception before the generated consumer");
      }
      failures = {};
      equal(expression.evaluate(), fixture.expected, fixture.source);
    }
  }
}

void concatenationRecovery() {
  using namespace TSeExpr;
  const StringCase cases[] = {
      {"($s+$s)+\"!\"", Value::string("abab!")},
      {"($s+$s)==\"abab\"", Value::scalar(1)},
      {"sprintf(\"[%s]\",$s+$s)", Value::string("[abab]")}};
  for (const auto &fixture : cases) {
    Options options;
    options.backend = Backend::LLVM;
    options.resultType = fixture.expected.type;
    options.maxStringBytes = 8;
    Program program;
    require(program.compile(fixture.source, {{"s", Value::string("ab")}}, {}, options),
            program.errorMessage());
    Value result;
    require(program.evaluate(result), program.errorMessage());
    equal(result, fixture.expected, fixture.source);
    require(program.setString("s", "abcde"), "Concatenation fixture variable update failed");
    require(!program.evaluate(result) &&
                program.errorMessage().find("string") != std::string::npos,
            "A generated consumer hid the concatenation size failure");
    require(program.setString("s", "ab"), "Concatenation recovery variable update failed");
    require(program.evaluate(result), program.errorMessage());
    equal(result, fixture.expected, fixture.source);
  }
}

void budgetAndErrors() {
  using namespace TSeExpr;
  Options options;
  options.backend = Backend::LLVM;
  options.resultType = Type::Scalar;
  options.maxInstructions = 128;
  Program bounded;
  std::string repeated = "sin($x)";
  for (int index = 1; index < 32; ++index) repeated += "+sin($x)";
  require(bounded.compile(repeated, {{"x", Value::scalar(1)}}, {}, options),
          bounded.errorMessage());
  Value result;
  require(!bounded.evaluate(result) && bounded.errorMessage().find("budget") != std::string::npos,
          "JIT execution did not enforce its evaluation budget");

  Program localBudget;
  if (!localBudget.compile("def FLOAT inner(FLOAT a){a+a+a+a} "
                           "def FLOAT outer(FLOAT a){inner(a)+inner(a)+inner(a)+inner(a)} "
                           "outer($x)+outer($x)+outer($x)+outer($x)",
                           {{"x", Value::scalar(2)}}, {}, options))
    throw std::runtime_error("JIT local budget fixture failed compilation: " +
                             localBudget.errorMessage());
  require(!localBudget.evaluate(result) &&
              localBudget.errorMessage().find("budget") != std::string::npos,
          "JIT local procedure did not count against the evaluation budget");

  bool cancelled = false;
  options.maxInstructions = 10000;
  options.cancelled = [&] { return cancelled; };
  Program cancel;
  require(cancel.compile("sin($x)+1", {{"x", Value::scalar(0.4)}}, {}, options),
          cancel.errorMessage());
  cancelled = true;
  require(!cancel.evaluate(result), "JIT ignored cancellation");
  cancelled = false;
  require(cancel.evaluate(result), "cancelled JIT did not recover on a fresh evaluation");

  Function fault;
  fault.name = "fault";
  fault.resultType = Type::Scalar;
  fault.argumentTypes = {Type::Scalar};
  fault.evaluate = [](const std::vector<Value> &) -> Value {
    throw std::runtime_error("fixture callback failure");
  };
  Program callback;
  require(callback.compile("fault($x)", {{"x", Value::scalar(2)}}, {fault}, options),
          callback.errorMessage());
  require(!callback.evaluate(result) && callback.errorMessage().find("fixture callback failure") != std::string::npos,
          "callback exception did not return safely through JIT frames");

  options.resultType = Type::String;
  options.maxStringBytes = 4;
  Program strings;
  require(strings.compile("$s+$s", {{"s", Value::string("abc")}}, {}, options),
          strings.errorMessage());
  require(!strings.evaluate(result) && strings.errorMessage().find("string") != std::string::npos,
          "JIT concatenation exceeded the configured string bound");
}
}  // namespace

int main() {
  try {
    using namespace TSeExpr;
    require(Program::backendAvailable(Backend::LLVM), "LLVM qualification was built without the backend");
    TSeExprQualification::equality(Backend::LLVM);
    parity("sin($x)*2 + pow($x,3) - 4", Type::Scalar, {{"x", Value::scalar(0.4)}});
    parity("$x>0 ? $x*2 : -$x", Type::Scalar, {{"x", Value::scalar(0.4)}});
    parity("$a=2; if($x>0){$a=$x+3;}else{$a=8;} $a", Type::Scalar,
           {{"x", Value::scalar(0.4)}});
    parity("def FLOAT testfun(FLOAT a, FLOAT b) {a+b} testfun($x,4)",
           Type::Scalar, {{"x", Value::scalar(0.4)}});
    parity("def FLOAT combine(FLOAT a,FLOAT b){a*10+b} "
           "combine(combine(1,2),combine(3,4))", Type::Scalar);
    parity("def FLOAT first(FLOAT a){a+2} "
           "def second(FLOAT a){b=first(a); if(a>0){b=b*3;}else{b=4;} b} "
           "second($x)+first($x)", Type::Scalar, {{"x", Value::scalar(2)}});
    parity("def FLOAT captured(FLOAT a){a+$x} captured(1)",
           Type::Scalar, {{"x", Value::scalar(2)}});
    parity("def FLOAT[4] twice(FLOAT[4] a){a*2} twice($v)", Type::Vector,
           {{"v", Value::numeric({1.0, 2.0, 3.0, 4.0})}}, {}, 4);
    parity("def FLOAT[4] splat(FLOAT a){a} splat($x)", Type::Vector,
           {{"x", Value::scalar(2)}}, {}, 4);
    parity("def STRING mark(STRING a){a+\"!\"} mark($s)+mark(\"longer\")",
           Type::String, {{"s", Value::string("first")}});
    parity("def STRING join(STRING a,STRING b){a+b} "
           "join(\"a\",join(\"b\",\"c\"))", Type::String);
    parity("[$x,2,3]*[4,5,6]", Type::Vector, {{"x", Value::scalar(0.4)}});
    parity("$v+[1,2,3,4]", Type::Vector,
           {{"v", Value::numeric({1.0, 2.0, 3.0, 4.0})}}, {}, 4);
    parity("noise([$x,2,3])+fbm([$x,2,3])", Type::Scalar,
           {{"x", Value::scalar(0.4)}});
    parity("$s+\" native\"", Type::String, {{"s", Value::string("SeExpr")}});
    parity("$s==\"SeExpr\"", Type::Scalar, {{"s", Value::string("SeExpr")}});
    parity("$s!=\"different\"", Type::Scalar, {{"s", Value::string("SeExpr")}});
    parity("$s==\"different\"", Type::Scalar, {{"s", Value::string("SeExpr")}});
    parity("sprintf(\"%s:%04d\",$s,7)", Type::String, {{"s", Value::string("SeExpr")}});
    Function offset;
    offset.name = "offset";
    offset.resultType = Type::Vector;
    offset.argumentTypes = {Type::Vector, Type::Scalar};
    offset.evaluate = [](const std::vector<Value> &arguments) {
      return Value::vector(arguments[0].number[0] + arguments[1].number[0],
                           arguments[0].number[1], arguments[0].number[2]);
    };
    parity("offset([$x,2,3],4)", Type::Vector,
           {{"x", Value::scalar(0.4)}}, {offset});
    parity("extern FLOAT[3] offset(FLOAT[3],FLOAT) offset([$x,2,3],4)",
           Type::Vector, {{"x", Value::scalar(0.4)}}, {offset});
    budgetAndErrors();
    stringCallbackErrors();
    sdkStringCallbackErrors();
    concatenationRecovery();
    TSeExprStandardQualification::run();
    std::cout << "SeExpr LLVM parity: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "SeExpr LLVM parity: FAIL: " << error.what() << '\n';
    return 1;
  }
}
