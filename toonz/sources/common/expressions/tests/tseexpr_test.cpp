#include "tseexpr.h"
#include "seexpr_equality_cases.h"
#include "seexpr_standard_cases.h"

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

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

void expectNear(double actual, double expected, const char *message) {
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

void compile(Program &program, const std::string &source,
             const std::vector<Variable> &variables = {},
             const Options &options = scalarOptions(),
             const std::vector<Function> &functions = {}) {
  if (!program.compile(source, variables, functions, options))
    throw std::runtime_error("Compilation failed: " + program.errorMessage() +
                             "\nFixture: " + source);
}

void languageAndTypes() {
  Program scalar;
  require(scalar.compile("sin(PI/2)+pow(2,3)", {}, {}, scalarOptions()),
          "scalar math did not compile");
  expectNear(run(scalar).number[0], 9, "SeExpr trigonometry must retain radians");

  require(scalar.compile("a=2; if ($x>0) { a=5; } else { a=7; } a*$x",
                         {{"x", Value::scalar(3)}}, {}, scalarOptions()),
          "assignments and branches did not compile");
  expectNear(run(scalar).number[0], 15, "branch produced the wrong value");
  require(scalar.setScalar("$x", -2), "scalar update rejected a valid binding");
  expectNear(run(scalar).number[0], -14, "varying variable retained an old result");
  require(scalar.compile("getVar(\"missing\", 17)", {}, {}, scalarOptions()),
          "getVar default did not compile");
  expectNear(run(scalar).number[0], 17, "getVar did not use its default");
  require(scalar.compile("getVar(\"gain\", 17)",
                         {{"gain", Value::scalar(4)}}, {}, scalarOptions()),
          "getVar external binding did not compile");
  expectNear(run(scalar).number[0], 4, "getVar did not resolve its external binding");

  Options four;
  four.resultDimension = 4;
  Program vector;
  compile(vector, "def FLOAT[4] twice(FLOAT[4] v) {v*2} twice($v)",
          {{"v", Value::numeric({1, 2, 3, 4})}}, four);
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
  expectNear(run(constant).number[0], 5, "constant binding evaluated incorrectly");
  require(!constant.setScalar("gain", 9),
          "constant bindings changed without recompilation");
  expectNear(run(constant).number[0], 5, "rejected constant update changed evaluation");

  require(constant.compile("$gain+1",
                           {{"gain", Value::scalar(4), Lifetime::Uniform}}, {},
                           scalarOptions()),
          "uniform binding failed");
  require(constant.setScalar("gain", 9), "uniform binding could not update");
  expectNear(run(constant).number[0], 10, "uniform binding retained a stale value");
}

void localFunctions() {
  Program program;
  compile(program,
      "def FLOAT choose(FLOAT a) {b=a+1; if(a>0){b=b*2;}else{b=3;} b} "
      "choose($x)+choose(-1)", {{"x", Value::scalar(2)}});
  expectNear(run(program).number[0], 9, "local branches did not preserve scope");

  const std::string combine = "def FLOAT combine(FLOAT a,FLOAT b){a*10+b} ";
  compile(program, combine + "combine($x,combine(2,3))", {{"x", Value::scalar(1)}});
  expectNear(run(program).number[0], 33, "nested arguments overwrote an outer argument");
  compile(program, combine + "combine(combine(1,2),combine(3,4))");
  expectNear(run(program).number[0], 154, "sibling nested calls shared result storage");

  compile(program,
      "def FLOAT base(FLOAT a){a+2} def more(FLOAT a){base(a)*3} "
      "more($x)+base($x)", {{"x", Value::scalar(4)}});
  expectNear(run(program).number[0], 24, "earlier local definitions were not callable");

  compile(program, "def FLOAT captured(FLOAT a){a+$x} captured(1)",
          {{"x", Value::scalar(2)}});
  expectNear(run(program).number[0], 3, "captured native variable evaluated incorrectly");
  require(program.setScalar("x", 5), "captured variable update failed");
  expectNear(run(program).number[0], 6, "local function capture was incorrectly constant folded");

  compile(program, "def FLOAT isolated(FLOAT x){x=9; x} isolated($x)+$x",
          {{"x", Value::scalar(2)}});
  expectNear(run(program).number[0], 11, "local assignment changed an external binding");

  Options four;
  four.resultDimension = 4;
  compile(program, "def FLOAT[4] splat(FLOAT a){a} splat($x)",
          {{"x", Value::scalar(2)}}, four);
  require(run(program).number == std::vector<double>({2, 2, 2, 2}),
          "local function scalar return was not promoted");

  Options strings;
  strings.resultType = Type::String;
  compile(program, "def STRING mark(STRING a){a+\"!\"} mark($s)+mark(\"longer\")",
          {{"s", Value::string("first")}}, strings);
  const Value retained = run(program);
  require(retained.text == "first!longer!", "local string calls shared temporary buffers");
  require(program.setString("s", "next"), "local string argument update failed");
  require(run(program).text == "next!longer!" && retained.text == "first!longer!",
          "returned local strings did not retain ownership");
  compile(program, "def STRING join(STRING a,STRING b){a+b} "
                   "join(\"a\",join(\"b\",\"c\"))", {}, strings);
  require(run(program).text == "abc", "nested local string arguments changed values");

  for (const std::string &source : {
      "def FLOAT self(FLOAT a){self(a)} self(1)",
      "def FLOAT first(FLOAT a){later(a)} def FLOAT later(FLOAT a){a} first(1)",
      "def FLOAT f(FLOAT a){a} f()",
      "def FLOAT f(FLOAT a){a} f(1,2)",
      "def FLOAT f(FLOAT a){a} f(\"wrong\")",
      "def FLOAT f(FLOAT a){a} f([1,2,3])",
      "def FLOAT f(FLOAT a){[a,a,a]} f(1)",
      "def FLOAT f(FLOAT a,FLOAT a){a} f(1,2)",
      "extern FLOAT absent(FLOAT) absent(1)"}) {
    require(!program.compile(source, {}, {}, scalarOptions()) && !program.errors().empty(),
            "invalid local declaration was accepted: " + source);
  }

  Function host;
  host.name = "host_gain";
  host.resultType = Type::Scalar;
  host.argumentTypes = {Type::String, Type::Scalar};
  host.evaluate = [](const std::vector<Value> &arguments) {
    if (arguments[0].text != "gain") throw std::runtime_error("unknown fixture binding");
    return Value::scalar(arguments[1].number[0] * 2);
  };
  compile(program, "extern FLOAT host_gain(STRING,FLOAT) host_gain(\"gain\",$x)",
          {{"x", Value::scalar(3)}}, scalarOptions(), {host});
  expectNear(run(program).number[0], 6, "external prototype did not bind its native function");
  require(!program.compile("extern STRING host_gain(STRING,FLOAT) host_gain(\"gain\",1)",
                           {}, {host}, strings),
          "external prototype return mismatch was accepted");
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
  expectNear(run(animation).number[0], 2, "channel callback returned the wrong value");
  std::vector<Value> output;
  require(animation.evaluateBatch({{{"frame", Value::scalar(2)}},
                                    {{"frame", Value::scalar(5)}},
                                    {{"frame", Value::scalar(8)}}}, output),
          "batch evaluation failed");
  require(output.size() == 3, "batch result length differs from its input");
  expectNear(output[0].number[0], 4, "first batch sample differs");
  expectNear(output[1].number[0], 10, "second batch sample differs");
  expectNear(output[2].number[0], 16, "third batch sample differs");

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
  expectNear(run(program).number[0], 0, "printf result changed from its native semantics");

  require(program.compile("1/$x", {{"x", Value::scalar(0)}}, {}, scalarOptions()),
          "nonfinite result test did not compile");
  Value result;
  require(!program.evaluate(result), "nonfinite render output was accepted");
  require(program.setScalar("x", 2), "finite recovery input was rejected");
  expectNear(run(program).number[0], 0.5, "program did not recover after a numeric error");

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
  expectNear(run(program).number[0], 3, "interpreter call stack did not recover after budget failure");

  std::atomic<bool> cancel{false};
  Options cancellation = scalarOptions();
  cancellation.cancelled = [&] { return cancel.load(); };
  require(program.compile("$x*2", {{"x", Value::scalar(5)}}, {}, cancellation),
          "cancellable expression did not compile");
  cancel = true;
  require(!program.evaluate(result), "cancelled evaluation continued");
  cancel = false;
  expectNear(run(program).number[0], 10, "evaluation failed after cancellation was cleared");

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

void nativeBindingContracts() {
  Program program;
  compile(program, "4*5");
  require(program.compile(program.source(), {}, {}, scalarOptions()),
          "recompilation invalidated a source string owned by the program");
  expectNear(run(program).number[0], 20, "source-alias recompilation changed the expression");

  Value invalid = Value::vector(1, 2, 3);
  invalid.type = static_cast<Type>(99);
  require(!program.compile("$value", {{"value", invalid}}, {}, scalarOptions()),
          "an invalid variable type reached interpreter register allocation");
  require(!program.compile("$value", {{"value", Value::scalar(1),
                                        static_cast<Lifetime>(99)}}, {}, scalarOptions()),
          "an invalid variable lifetime was silently reinterpreted");
  Options invalidOptions = scalarOptions();
  invalidOptions.resultType = static_cast<Type>(99);
  require(!program.compile("1", {}, {}, invalidOptions),
          "an invalid result type was silently reinterpreted");
  invalidOptions = scalarOptions();
  invalidOptions.backend = static_cast<Backend>(99);
  require(!program.compile("1", {}, {}, invalidOptions),
          "an invalid backend silently selected a different evaluator");

  Function scalar;
  scalar.name = "scalar_host";
  scalar.argumentTypes = {Type::Scalar};
  scalar.evaluate = [](const std::vector<Value> &arguments) {
    return Value::scalar(arguments[0].number[0]);
  };
  require(!program.compile("scalar_host([1,2,3])", {}, {scalar}, scalarOptions()),
          "a scalar callback silently narrowed a vector argument");
  Function invalidFunction = scalar;
  invalidFunction.argumentTypes = {static_cast<Type>(99)};
  require(!program.compile("scalar_host(1)", {}, {invalidFunction}, scalarOptions()),
          "an invalid callback argument type reached register allocation");
  invalidFunction = scalar;
  invalidFunction.resultType = static_cast<Type>(99);
  require(!program.compile("scalar_host(1)", {}, {invalidFunction}, scalarOptions()),
          "an invalid callback result type reached register allocation");
  invalidFunction = scalar;
  invalidFunction.minArgs = -2;
  require(!program.compile("scalar_host(1)", {}, {invalidFunction}, scalarOptions()),
          "an invalid callback arity sentinel was accepted");

  Function vector;
  vector.name = "vector_host";
  vector.argumentTypes = {Type::Vector};
  vector.argumentDimensions = {1};
  vector.resultType = Type::Vector;
  vector.resultDimension = 1;
  vector.evaluate = [](const std::vector<Value> &arguments) {
    require(arguments[0].type == Type::Vector && arguments[0].number.size() == 1,
            "one-component vector callback arguments lost their declared host type");
    Value result = Value::numeric({arguments[0].number[0] + 1});
    result.type = Type::Vector;
    return result;
  };
  Value one = Value::numeric({2});
  one.type = Type::Vector;
  Options vectorOptions;
  vectorOptions.resultDimension = 1;
  compile(program, "vector_host($value)", {{"value", one}}, vectorOptions, {vector});
  const Value output = run(program);
  require(output.type == Type::Vector && output.number.size() == 1,
          "one-component vector output lost its declared host type");
  expectNear(output.number[0], 3, "one-component vector callback returned the wrong result");

  compile(program, "gain=4;gain", {{"gain", Value::scalar(9)}});
  require(program.usedVariables().empty(),
          "a local variable shadow created a false external dependency");
  compile(program, "def FLOAT noise(FLOAT x){x+2} noise(3)");
  require(program.usedFunctions().empty(),
          "a local function shadow created a false builtin dependency");
  expectNear(run(program).number[0], 5, "a local function shadow used the builtin instead");
  compile(program, "def FLOAT printf(FLOAT x){x+1} printf(2)");
  expectNear(run(program).number[0], 3,
       "render console policy incorrectly blocked a local pure function");

  struct NativeFailure {};
  scalar.evaluate = [](const std::vector<Value> &) -> Value { throw NativeFailure(); };
  Options native = scalarOptions();
  native.exceptionMessage = [](std::exception_ptr failure) -> std::string {
    try {
      std::rethrow_exception(failure);
    } catch (const NativeFailure &) {
      return "Native host callback failed.";
    } catch (...) {
      return "Unexpected native host callback failure.";
    }
  };
  compile(program, "scalar_host(1)", {}, native, {scalar});
  Value result;
  require(!program.evaluate(result) &&
              program.errorMessage() == "Native host callback failed.",
          "native callback exception formatting lost its diagnostic");

  compile(program, "1/$x", {{"x", Value::scalar(2)}});
  std::vector<Value> results;
  require(!program.evaluateBatch({{{"x", Value::scalar(2)}},
                                   {{"x", Value::scalar(0)}}}, results) && results.empty(),
          "a failed batch exposed partial successful-looking results");
  require(program.setScalar("x", 4), "batch failure prevented a valid input update");
  expectNear(run(program).number[0], 0.25, "program did not recover after a failed batch");
}

void evaluationReentrancy() {
  enum class Mode { Idle, Evaluate, Compile, Mutate, MoveConstruct, MoveAssign };
  Mode mode = Mode::Idle;
  Program program;
  Function callback;
  callback.name = "host";
  callback.argumentTypes = {Type::Scalar};
  callback.evaluate = [&](const std::vector<Value> &arguments) {
    switch (mode) {
    case Mode::Idle: break;
    case Mode::Evaluate: {
      Value ignored;
      program.evaluate(ignored);
      break;
    }
    case Mode::Compile:
      program.compile(program.source(), {}, {}, scalarOptions());
      break;
    case Mode::Mutate: program.setScalar("x", 99); break;
    case Mode::MoveConstruct: {
      Program moved(std::move(program));
      break;
    }
    case Mode::MoveAssign: {
      Program replacement;
      program = std::move(replacement);
      break;
    }
    }
    return Value::scalar(arguments[0].number[0]);
  };
  compile(program, "host($x)", {{"x", Value::scalar(7)}}, scalarOptions(), {callback});
  for (Mode attempted : {Mode::Evaluate, Mode::Compile, Mode::Mutate,
                          Mode::MoveConstruct, Mode::MoveAssign}) {
    mode = attempted;
    Value unchanged = Value::scalar(42);
    require(!program.evaluate(unchanged) && !program.errorMessage().empty(),
            "a reentrant callback operation corrupted or replaced the active program");
    expectNear(unchanged.number[0], 42, "a rejected reentrant evaluation exposed a partial result");
    mode = Mode::Idle;
    expectNear(run(program).number[0], 7, "program ownership or variables changed after rejected reentry");
  }

  Program other;
  bool cycle = false;
  Function inner;
  inner.name = "inner";
  inner.evaluate = [&](const std::vector<Value> &) {
    if (cycle) {
      Value ignored;
      program.evaluate(ignored);
    }
    return Value::scalar(11);
  };
  compile(other, "inner()", {}, scalarOptions(), {inner});
  Function outer;
  outer.name = "outer";
  outer.evaluate = [&](const std::vector<Value> &) { return run(other); };
  compile(program, "outer()", {}, scalarOptions(), {outer});
  expectNear(run(program).number[0], 11, "nested evaluations of distinct programs were rejected");
  cycle = true;
  Value ignored;
  require(!program.evaluate(ignored), "an indirect program callback cycle was accepted");
  cycle = false;
  expectNear(run(program).number[0], 11, "program did not recover after an indirect callback cycle");

  Program movable;
  compile(movable, "9");
  Program moved(std::move(movable));
  require(!movable.isValid() && movable.source().empty() && movable.errors().empty(),
          "an idle moved-from program could not be queried safely");
  expectNear(run(moved).number[0], 9, "moving an idle program changed its evaluation");
  compile(movable, "2");
  expectNear(run(movable).number[0], 2, "a moved-from program could not be compiled again");

  bool reenterCompile = true;
  Options cancellation = scalarOptions();
  cancellation.cancelled = [&] {
    if (reenterCompile)
      program.compile("1", {}, {}, scalarOptions());
    return false;
  };
  require(!program.compile("5", {}, {}, cancellation),
          "a cancellation callback replaced its actively compiling program");
  reenterCompile = false;
  compile(program, "5", {}, cancellation);
  expectNear(run(program).number[0], 5, "compilation did not recover after rejected callback reentry");
}

void astDepthLimits() {
  Program program;
  Options limits = scalarOptions();
  limits.maxNesting = 32;
  std::string flat = "1", power = "1";
  for (int index = 0; index < 2048; ++index) {
    flat += "+1";
    power += "^1";
  }
  for (const auto &source : {flat, power, std::string(2048, '-') + "1"}) {
    require(!program.compile(source, {}, {}, limits) &&
                program.errorMessage().find("depth") != std::string::npos,
            "a deep operator tree bypassed the structural compilation limit");
    // Parser rejection must also release a partial deep tree without using
    // the C++ destructor stack for every child.
    require(!program.compile(source + "+", {}, {}, limits),
            "a malformed deep expression was accepted");
    compile(program, "3");
    expectNear(run(program).number[0], 3, "deep-tree rejection corrupted the next compilation");
  }
  Options larger = scalarOptions();
  larger.maxNesting = 256;
  std::string longer = "1";
  for (int index = 0; index < 200; ++index) longer += "+1";
  compile(program, longer, {}, larger);
  expectNear(run(program).number[0], 201, "an explicitly larger valid tree limit was ignored");

  const std::string offsetSource = std::string(40000, ' ') + "unknown_name";
  require(!program.compile(offsetSource, {}, {}, scalarOptions()),
          "an unknown variable after a long prefix was accepted");
  require(std::any_of(program.errors().begin(), program.errors().end(),
                      [](const Error &error) { return error.begin >= 40000; }),
          "diagnostic byte offsets wrapped past the old 16-bit position range");
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
          expectNear(run(owned).number[0], expected[static_cast<std::size_t>(i)],
               "parallel or reordered evaluation changed a deterministic result");
        }
    }));
  for (auto &worker : workers) worker.get();
}
}  // namespace

int main() {
  try {
    languageAndTypes();
    localFunctions();
    TSeExprQualification::equality(Backend::Interpreter);
    TSeExprStandardQualification::run(false);
    customFunctionsAndBatch();
    errorsAndBudgets();
    nativeBindingContracts();
    evaluationReentrancy();
    astDepthLimits();
    parallelAndCatalog();
    std::cout << "seexpr-runtime: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "seexpr-runtime: FAIL: " << error.what() << '\n';
    return 1;
  }
}
