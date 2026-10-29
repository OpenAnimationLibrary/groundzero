#include "tseexpr.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace TSeExpr;

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected, const char *message) {
  require(std::abs(actual - expected) <= 1e-12, message);
}

Options scalarOptions() {
  Options result;
  result.resultType = Type::Scalar;
  return result;
}

Value run(Program &program) {
  Value result;
  if (!program.evaluate(result)) throw std::runtime_error(program.errorMessage());
  return result;
}

void languageAndTypes() {
  Program scalar;
  require(scalar.compile("sin(PI/2)+pow(2,3)", {}, {}, scalarOptions()),
          "scalar math did not compile");
  near(run(scalar).number[0], 9, "SeExpr trigonometry must retain radians");

  require(scalar.compile("a=2; if ($x>0) { a=5; } else { a=7; } a*$x",
                         {{"x", Value::scalar(3)}}, {}, scalarOptions()),
          "assignments and branches did not compile");
  near(run(scalar).number[0], 15, "branch produced the wrong value");
  require(scalar.setScalar("$x", -2), "scalar update rejected a valid binding");
  near(run(scalar).number[0], -14, "varying variable retained an old result");
  require(scalar.compile("getVar(\"missing\", 17)", {}, {}, scalarOptions()),
          "getVar default did not compile");
  near(run(scalar).number[0], 17, "getVar did not use its default");
  require(scalar.compile("getVar(\"gain\", 17)",
                         {{"gain", Value::scalar(4)}}, {}, scalarOptions()),
          "getVar external binding did not compile");
  near(run(scalar).number[0], 4, "getVar did not resolve its external binding");

  Options four;
  four.resultDimension = 4;
  Program vector;
  require(vector.compile("def FLOAT[4] twice(FLOAT[4] v) {v*2} twice($v)",
                         {{"v", Value::numeric({1, 2, 3, 4})}}, {}, four),
          "four-dimensional local function did not compile");
  require(run(vector).number == std::vector<double>({2, 4, 6, 8}),
          "four-dimensional results were truncated or incorrect");
  require(vector.setVariable("v", Value::numeric({4, 3, 2, 1})),
          "arbitrary-dimensional external update failed");
  require(run(vector).number == std::vector<double>({8, 6, 4, 2}),
          "four-dimensional variable did not update");

  Options strings;
  strings.resultType = Type::String;
  Program text;
  require(text.compile("$label + sprintf(\"%04d\", $index)",
                       {{"label", Value::string("frame-")},
                        {"index", Value::scalar(42)}}, {}, strings),
          "string variables and formatting did not compile");
  const Value first = run(text);
  require(first.text == "frame-0042", "string formatting produced the wrong result");
  require(text.setString("label", "drawing-") && text.setScalar("index", 7),
          "string or numeric binding update failed");
  require(run(text).text == "drawing-0007", "string evaluation reused stale storage");
  require(first.text == "frame-0042", "returned strings did not own their data");

  Program constant;
  require(constant.compile("$gain+1",
                           {{"gain", Value::scalar(4), Lifetime::Constant}}, {},
                           scalarOptions()),
          "constant binding failed");
  near(run(constant).number[0], 5, "constant binding evaluated incorrectly");
  require(!constant.setScalar("gain", 9),
          "constant bindings changed without recompilation");
  near(run(constant).number[0], 5, "rejected constant update changed evaluation");

  require(constant.compile("$gain+1",
                           {{"gain", Value::scalar(4), Lifetime::Uniform}}, {},
                           scalarOptions()),
          "uniform binding failed");
  require(constant.setScalar("gain", 9), "uniform binding could not update");
  near(run(constant).number[0], 10, "uniform binding retained a stale value");
}

void customFunctionsAndBatch() {
  Function color;
  color.name = "sample_rgb";
  color.documentation = "sample_rgb(x,y): fixed test image sampler";
  color.resultType = Type::Vector;
  color.argumentTypes = {Type::Scalar, Type::Scalar};
  color.evaluate = [](const std::vector<Value> &args) {
    return Value::vector(args[0].number[0], args[1].number[0], 0.5);
  };
  Program image;
  require(image.compile("sample_rgb($x,$y)",
                        {{"x", Value::scalar(0.25)},
                         {"y", Value::scalar(0.75)}}, {color}),
          "host sampling callback did not compile");
  require(run(image).number == std::vector<double>({0.25, 0.75, 0.5}),
          "host sampler returned incorrect channels");
  const auto variables = image.usedVariables();
  require(variables == std::vector<std::string>({"x", "y"}),
          "external dependency names were not retained");
  const auto functions = image.usedFunctions();
  require(std::find(functions.begin(), functions.end(), "sample_rgb") != functions.end(),
          "custom function dependency was not retained");

  Function lookup;
  lookup.name = "channel";
  lookup.argumentTypes = {Type::String, Type::Scalar};
  lookup.evaluate = [](const std::vector<Value> &args) {
    if (args[0].text != "gain") throw std::runtime_error("Unknown test channel.");
    return Value::scalar(args[1].number[0] * 2);
  };
  Program animation;
  require(animation.compile("channel(\"gain\", $frame)",
                            {{"frame", Value::scalar(1)}}, {lookup}, scalarOptions()),
          "string channel callback did not compile");
  near(run(animation).number[0], 2, "channel callback returned the wrong value");
  std::vector<Value> output;
  require(animation.evaluateBatch({{{"frame", Value::scalar(2)}},
                                    {{"frame", Value::scalar(5)}},
                                    {{"frame", Value::scalar(8)}}}, output),
          "batch evaluation failed");
  require(output.size() == 3, "batch result length differs from its input");
  near(output[0].number[0], 4, "first batch sample differs");
  near(output[1].number[0], 10, "second batch sample differs");
  near(output[2].number[0], 16, "third batch sample differs");

  require(animation.compile("channel(\"missing\", 1)", {}, {lookup}, scalarOptions()),
          "host callback test expression failed compilation");
  Value result;
  require(!animation.evaluate(result) &&
              animation.errorMessage().find("Unknown test channel") != std::string::npos,
          "callback exception did not become an actionable evaluation failure");

  Function vector4;
  vector4.name = "reverse4";
  vector4.resultType = Type::Vector;
  vector4.resultDimension = 4;
  vector4.argumentTypes = {Type::Vector};
  vector4.argumentDimensions = {4};
  vector4.evaluate = [](const std::vector<Value> &args) {
    auto values = args[0].number;
    std::reverse(values.begin(), values.end());
    return Value::numeric(std::move(values));
  };
  Options four;
  four.resultDimension = 4;
  require(image.compile("reverse4([1,2,3,4])", {}, {vector4}, four),
          "arbitrary-dimensional host function did not compile");
  require(run(image).number == std::vector<double>({4, 3, 2, 1}),
          "arbitrary-dimensional callback was truncated");
}

void errorsAndBudgets() {
  const auto failures = failureCount();
  Program program;
  require(!program.compile("sin(", {}, {}, scalarOptions()) && !program.errors().empty(),
          "invalid syntax was accepted or lacked diagnostics");
  require(program.errors().front().begin >= 0 &&
              program.errors().front().end <= static_cast<int>(program.source().size()),
          "diagnostic offsets escaped the source");
  require(!program.compile("[1,2,3]", {}, {}, scalarOptions()),
          "vector output silently replaced a scalar result");
  require(!program.compile("$x", {{"x", Value::scalar(1)},
                                 {"x", Value::scalar(2)}}, {}, scalarOptions()),
          "duplicate external variables were accepted");
  require(!program.compile("def FLOAT[1000000] f(FLOAT a){a} f(1)", {}, {},
                           scalarOptions()),
          "oversized type dimensions reached interpreter allocation");
  require(!program.compile("printf(\"%f\", 1)", {}, {}, scalarOptions()),
          "rendering context permitted console output");

  Options enabled = scalarOptions();
  enabled.allowConsoleOutput = true;
  require(program.compile("printf(\"SeExpr core validation %f\", 1)", {}, {}, enabled),
          "explicit console-enabled core context lost printf support");
  near(run(program).number[0], 0, "printf result changed from its native semantics");

  require(program.compile("1/$x", {{"x", Value::scalar(0)}}, {}, scalarOptions()),
          "nonfinite result test did not compile");
  Value result;
  require(!program.evaluate(result), "nonfinite render output was accepted");
  require(program.setScalar("x", 2), "finite recovery input was rejected");
  near(run(program).number[0], 0.5, "program did not recover after a numeric error");

  Options limited = scalarOptions();
  limited.maxInstructions = 2000;
  std::string source = "def FLOAT f0(FLOAT a){a+1} ";
  for (int i = 1; i <= 12; ++i) {
    const std::string previous = "f" + std::to_string(i - 1);
    source += "def FLOAT f" + std::to_string(i) + "(FLOAT a){" +
              previous + "(a)+" + previous + "(a)} ";
  }
  source += "$run ? f12($x) : $x";
  require(program.compile(source, {{"run", Value::scalar(1)},
                                   {"x", Value::scalar(3)}}, {}, limited),
          "bounded local function expression did not compile");
  require(!program.evaluate(result) &&
              program.errorMessage().find("budget") != std::string::npos,
          "exponential local function work was not interrupted by its budget");
  require(program.setScalar("run", 0), "budget recovery input was rejected");
  near(run(program).number[0], 3, "interpreter call stack did not recover after budget failure");

  std::atomic<bool> cancel{false};
  Options cancellation = scalarOptions();
  cancellation.cancelled = [&] { return cancel.load(); };
  require(program.compile("$x*2", {{"x", Value::scalar(5)}}, {}, cancellation),
          "cancellable expression did not compile");
  cancel = true;
  require(!program.evaluate(result), "cancelled evaluation continued");
  cancel = false;
  near(run(program).number[0], 10, "evaluation failed after cancellation was cleared");

  Options strings;
  strings.resultType = Type::String;
  strings.maxStringBytes = 32;
  require(!program.compile("$text", {{"text", Value::string(std::string(33, 'x'))}},
                           {}, strings),
          "oversized string variable was accepted");
  require(failureCount() > failures, "native failures were not observable by batch hosts");

  if (!Program::backendAvailable(Backend::LLVM)) {
    Options llvm = scalarOptions();
    llvm.backend = Backend::LLVM;
    require(!program.compile("1", {}, {}, llvm),
            "unavailable LLVM backend silently fell back to another evaluator");
  }
}

void parallelAndCatalog() {
  const auto catalog = Program::builtinFunctions();
  for (const char *name : {"noise", "fbm", "cellnoise", "voronoi", "curve",
                            "sprintf", "printf", "getVar", "sin", "cbrt"})
    require(std::any_of(catalog.begin(), catalog.end(),
                        [&](const FunctionInfo &entry) { return entry.name == name; }),
            "actual core function catalog lost a required builtin");
  const std::string source =
      "def FLOAT shape(FLOAT x){sin(x)+x*x} shape($x)+fbm([$x,2,3])";
  Program reference;
  require(reference.compile(source, {{"x", Value::scalar(0)}}, {}, scalarOptions()),
          "parallel reference expression did not compile");
  std::vector<double> expected;
  for (int i = 0; i < 32; ++i) {
    require(reference.setScalar("x", i / 8.0), "reference update failed");
    expected.push_back(run(reference).number[0]);
  }
  std::vector<std::future<void>> workers;
  for (int worker = 0; worker < 6; ++worker)
    workers.push_back(std::async(std::launch::async, [&] {
      Program owned;
      require(owned.compile(source, {{"x", Value::scalar(0)}}, {}, scalarOptions()),
              "worker-owned expression did not compile");
      for (int repeat = 0; repeat < 4; ++repeat)
        for (int i = 31; i >= 0; --i) {
          require(owned.setScalar("x", i / 8.0), "worker update failed");
          near(run(owned).number[0], expected[static_cast<std::size_t>(i)],
               "parallel or reordered evaluation changed a deterministic result");
        }
    }));
  for (auto &worker : workers) worker.get();
}
}  // namespace

int main() {
  try {
    languageAndTypes();
    customFunctionsAndBatch();
    errorsAndBudgets();
    parallelAndCatalog();
    std::cout << "seexpr-runtime: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "seexpr-runtime: FAIL: " << error.what() << '\n';
    return 1;
  }
}
