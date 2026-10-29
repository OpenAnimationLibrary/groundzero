#include "tseexpr.h"

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
  require(interpreted.compile(source, variables, functions, options),
          "interpreter compile failed: " + interpreted.errorMessage());
  options.backend = TSeExpr::Backend::LLVM;
  require(compiled.compile(source, variables, functions, options),
          "LLVM compile failed: " + compiled.errorMessage());
  for (int repeat = 0; repeat < 3; ++repeat) {
    TSeExpr::Value first, second;
    require(interpreted.evaluate(first), interpreted.errorMessage());
    require(compiled.evaluate(second), compiled.errorMessage());
    equal(first, second, source);
  }
}

void budgetAndErrors() {
  using namespace TSeExpr;
  Options options;
  options.backend = Backend::LLVM;
  options.resultType = Type::Scalar;
  options.maxInstructions = 8;
  Program bounded;
  require(bounded.compile("$x+$x+$x+$x+$x+$x+$x+$x", {{"x", Value::scalar(1)}}, {}, options),
          bounded.errorMessage());
  Value result;
  require(!bounded.evaluate(result) && bounded.errorMessage().find("budget") != std::string::npos,
          "JIT execution did not enforce its evaluation budget");

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
    parity("sin($x)*2 + pow($x,3) - 4", Type::Scalar, {{"x", Value::scalar(0.4)}});
    parity("$x>0 ? $x*2 : -$x", Type::Scalar, {{"x", Value::scalar(0.4)}});
    parity("$a=2; if($x>0){$a=$x+3;}else{$a=8;} $a", Type::Scalar,
           {{"x", Value::scalar(0.4)}});
    parity("def FLOAT testfun(FLOAT a, FLOAT b) {a+b} testfun($x,4)",
           Type::Scalar, {{"x", Value::scalar(0.4)}});
    parity("[$x,2,3]*[4,5,6]", Type::Vector, {{"x", Value::scalar(0.4)}});
    parity("$v+[1,2,3,4]", Type::Vector,
           {{"v", Value::numeric({1.0, 2.0, 3.0, 4.0})}}, {}, 4);
    parity("noise([$x,2,3])+fbm([$x,2,3])", Type::Scalar,
           {{"x", Value::scalar(0.4)}});
    parity("$s+\" native\"", Type::String, {{"s", Value::string("SeExpr")}});
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
    budgetAndErrors();
    std::cout << "SeExpr LLVM parity: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "SeExpr LLVM parity: FAIL: " << error.what() << '\n';
    return 1;
  }
}
