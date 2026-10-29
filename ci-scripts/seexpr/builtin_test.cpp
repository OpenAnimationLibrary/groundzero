#include "tseexpr.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace TSeExpr;
constexpr double pi = 3.1415926535897932384626433832795;

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected, double tolerance,
          const std::string &message) {
  require(std::isfinite(actual) && std::isfinite(expected) &&
              std::abs(actual - expected) <=
                  tolerance * std::max(1.0, std::abs(expected)),
          message + ": expected " + std::to_string(expected) +
              ", received " + std::to_string(actual));
}

struct Case {
  std::string name;
  std::string source;
  Type type = Type::Scalar;
  std::vector<double> expected;
  std::string expectedText;
  double lower = 0;
  double upper = 0;
  double tolerance = 1e-10;
  bool bounded = false;
};

Case scalar(const char *name, const char *source, double expected) {
  Case result;
  result.name = name;
  result.source = source;
  result.expected = {expected};
  return result;
}

Case vector(const char *name, const char *source,
            std::vector<double> expected, double tolerance = 1e-10) {
  Case result;
  result.name = name;
  result.source = source;
  result.type = Type::Vector;
  result.expected = std::move(expected);
  result.tolerance = tolerance;
  return result;
}

Case bounded(const char *name, const char *source, Type type,
             double lower, double upper) {
  Case result;
  result.name = name;
  result.source = source;
  result.type = type;
  result.lower = lower;
  result.upper = upper;
  result.bounded = true;
  return result;
}

Case string(const char *name, const char *source, const char *expected) {
  Case result;
  result.name = name;
  result.source = source;
  result.type = Type::String;
  result.expectedText = expected;
  return result;
}

std::vector<Case> cases() {
  // Independent mathematical anchors, documented selection/ramp behavior,
  // integer-lattice Perlin anchors and cell/range invariants. This is an
  // evaluation corpus, not a second implementation of the builtin algorithms.
  return {
      scalar("abs", "abs(-3)", 3),
      scalar("acos", "acos(0)", pi / 2),
      scalar("asin", "asin(1)", pi / 2),
      scalar("atan", "atan(1)", pi / 4),
      scalar("atan2", "atan2(1,0)", pi / 2),
      scalar("ceil", "ceil(1.2)", 2),
      scalar("cos", "cos(PI)", -1),
      scalar("cosh", "cosh(0)", 1),
      scalar("exp", "exp(0)", 1),
      scalar("floor", "floor(-1.2)", -2),
      scalar("fmod", "fmod(7,3)", 1),
      scalar("log", "log(exp(1))", 1),
      scalar("log10", "log10(1000)", 3),
      scalar("pow", "pow(2,3)", 8),
      scalar("sin", "sin(PI/2)", 1),
      scalar("sinh", "sinh(0)", 0),
      scalar("sqrt", "sqrt(9)", 3),
      scalar("tan", "tan(PI/4)", 1),
      scalar("tanh", "tanh(0)", 0),
      scalar("cbrt", "cbrt(-8)", -2),
      scalar("asinh", "asinh(0)", 0),
      scalar("acosh", "acosh(1)", 0),
      scalar("atanh", "atanh(0)", 0),
      scalar("trunc", "trunc(-1.8)", -1),
      scalar("deg", "deg(PI)", 180),
      scalar("rad", "rad(180)", pi),
      scalar("cosd", "cosd(180)", -1),
      scalar("sind", "sind(90)", 1),
      scalar("tand", "tand(45)", 1),
      scalar("acosd", "acosd(0)", 90),
      scalar("asind", "asind(1)", 90),
      scalar("atand", "atand(1)", 45),
      scalar("atan2d", "atan2d(1,0)", 90),
      scalar("clamp", "clamp(-3,0,1)", 0),
      scalar("round", "round(1.75)", 2),
      scalar("max", "max(3,8)", 8),
      scalar("min", "min(3,8)", 3),
      scalar("invert", "invert(0.25)", 0.75),
      scalar("compress", "compress(0.25,2,6)", 3),
      scalar("expand", "expand(3,2,6)", 0.25),
      scalar("fit", "fit(3,2,6,10,18)", 12),
      scalar("gamma", "gamma(0.25,2)", 0.5),
      scalar("bias", "bias(0.25,0.5)", 0.25),
      scalar("contrast", "contrast(0.25,0.5)", 0.25),
      scalar("boxstep", "boxstep(0.5,0.5)", 1),
      scalar("linearstep", "linearstep(0.25,0,1)", 0.25),
      scalar("smoothstep", "smoothstep(0.5,0,1)", 0.5),
      scalar("gaussstep", "gaussstep(0.5,0,1)", 0.25),
      scalar("remap", "remap(0,0,1,2,0)", 1),
      scalar("mix", "mix(2,10,0.25)", 4),
      vector("hsi", "hsi([0.2,0.4,0.6],0,1,1)", {0.2, 0.4, 0.6}),
      vector("midhsi", "midhsi([0.2,0.4,0.6],30,2,4,0.5)", {0.2, 0.4, 0.6}),
      vector("hsltorgb", "hsltorgb([0,1,0.5])", {1, 0, 0}),
      vector("rgbtohsl", "rgbtohsl([1,0,0])", {0, 1, 0.5}),
      vector("saturate", "saturate([1,0,0],0)", {0.2126, 0.2126, 0.2126}),
      bounded("hash", "hash(1,2,3)", Type::Scalar, 0, 1),
      scalar("noise", "noise([0,0,0])", 0.5),
      scalar("snoise", "snoise([0,0,0])", 0),
      vector("vnoise", "vnoise([0,0,0])", {0, 0, 0}),
      vector("cnoise", "cnoise([0,0,0])", {0.5, 0.5, 0.5}),
      scalar("snoise4", "snoise4([0,0,0],0)", 0),
      vector("vnoise4", "vnoise4([0,0,0],0)", {0, 0, 0}),
      vector("cnoise4", "cnoise4([0,0,0],0)", {0.5, 0.5, 0.5}),
      scalar("turbulence", "turbulence([0,0,0],3)", 0.5),
      vector("vturbulence", "vturbulence([0,0,0],3)", {0, 0, 0}),
      vector("cturbulence", "cturbulence([0,0,0],3)", {0.5, 0.5, 0.5}),
      scalar("fbm", "fbm([0,0,0],3)", 0.5),
      vector("vfbm", "vfbm([0,0,0],3)", {0, 0, 0}),
      vector("cfbm", "cfbm([0,0,0],3)", {0.5, 0.5, 0.5}),
      bounded("cellnoise", "cellnoise([0.2,0.3,0.4])", Type::Scalar, 0, 1),
      bounded("ccellnoise", "ccellnoise([0.2,0.3,0.4])", Type::Vector, 0, 1),
      scalar("pnoise", "pnoise([0.25,0.5,0.75],[2,3,4])-pnoise([2.25,3.5,4.75],[2,3,4])", 0),
      scalar("fbm4", "fbm4([0,0,0],0,3)", 0.5),
      vector("vfbm4", "vfbm4([0,0,0],0,3)", {0, 0, 0}),
      vector("cfbm4", "cfbm4([0,0,0],0,3)", {0.5, 0.5, 0.5}),
      scalar("dist", "dist(0,0,0,3,4,0)", 5),
      scalar("length", "length([3,4,0])", 5),
      scalar("hypot", "hypot(3,4)", 5),
      scalar("dot", "dot([1,2,3],[4,5,6])", 32),
      vector("norm", "norm([3,4,0])", {0.6, 0.8, 0}),
      vector("cross", "cross([1,0,0],[0,1,0])", {0, 0, 1}),
      scalar("angle", "angle([1,0,0],[0,1,0])", pi / 2),
      vector("ortho", "ortho([2,0,0],[0,3,0])", {0, 0, 1}),
      vector("rotate", "rotate([1,0,0],[0,0,1],PI/2)", {0, 1, 0}, 1e-6),
      vector("up", "up([1,2,3],[0,1,0])", {1, 2, 3}),
      scalar("cycle", "cycle(-1,2,4)", 4),
      scalar("pick", "pick(17,10,12,0,1,0)", 11),
      scalar("choose", "choose(0.5,10,20,30)", 20),
      scalar("wchoose", "wchoose(0.75,10,1,20,3)", 20),
      vector("swatch", "swatch(0.5,[1,0,0],[0,1,0],[0,0,1])", {0, 1, 0}),
      scalar("spline", "spline(0.5,0,1,2,3)", 1.5),
      bounded("voronoi", "voronoi([0.5,0.5,0.5],2,0.001)", Type::Vector, 0, 0.001),
      bounded("cvoronoi", "cvoronoi([0.5,0.5,0.5],1,0.001)", Type::Vector, 0, 1),
      bounded("pvoronoi", "pvoronoi([0.5,0.5,0.5],0.001)", Type::Vector, 0.4995, 0.5005),
      scalar("curve", "curve(0.25,0,2,1,1,6,1)", 3),
      vector("ccurve", "ccurve(0.25,0,[1,0,0],1,1,[0,0,1],1)", {0.75, 0, 0.25}),
      scalar("getVar", "getVar(\"missing\",17)", 17),
      scalar("printf", "printf(\"builtin %f %v\",2,[1,2,3])", 0),
      string("sprintf", "sprintf(\"n=%04d %.2f %s\",7,1.25,\"ok\")", "n=0007 1.25 ok")};
}

class ConsoleCapture {
  std::ostringstream m_output;
  std::streambuf *m_previous;
public:
  ConsoleCapture() : m_previous(std::cerr.rdbuf(m_output.rdbuf())) {}
  ~ConsoleCapture() { std::cerr.rdbuf(m_previous); }
  std::string value() const { return m_output.str(); }
};

Options options(Type type, Backend backend) {
  Options result;
  result.resultType = type;
  result.backend = backend;
  result.allowConsoleOutput = true;
  return result;
}

Value evaluate(const std::string &source, Type type, Backend backend,
               const std::vector<Variable> &variables = {}) {
  Program program;
  const bool compiled = program.compile(source, variables, {}, options(type, backend));
  require(compiled,
          "Compile failed for " + source + ": " + program.errorMessage());
  Value result;
  const bool evaluated = program.evaluate(result);
  require(evaluated,
          "Evaluation failed for " + source + ": " + program.errorMessage());
  return result;
}

void same(const Value &actual, const Value &expected, double tolerance,
          const std::string &name) {
  require(actual.type == expected.type, "Result type differs for " + name);
  if (actual.type == Type::String) {
    require(actual.text == expected.text, "String result differs for " + name);
    return;
  }
  require(actual.number.size() == expected.number.size(),
          "Result dimension differs for " + name);
  for (std::size_t index = 0; index < actual.number.size(); ++index)
    near(actual.number[index], expected.number[index], tolerance, name);
}

void check(const Case &test, const Value &actual) {
  require(actual.type == test.type, "Wrong result type for " + test.name);
  if (test.type == Type::String) {
    require(actual.text == test.expectedText, "Wrong formatted string for " + test.name);
    return;
  }
  const std::size_t expectedSize = test.type == Type::Scalar ? 1 : 3;
  require(actual.number.size() == expectedSize,
          "Wrong result dimension for " + test.name);
  if (test.bounded) {
    for (double component : actual.number)
      require(std::isfinite(component) && component >= test.lower - test.tolerance &&
                  component <= test.upper + test.tolerance,
              "Result violates the mathematical range for " + test.name);
  } else {
    require(test.expected.size() == actual.number.size(),
            "Builtin case has no independent expected value: " + test.name);
    for (std::size_t component = 0; component < actual.number.size(); ++component)
      near(actual.number[component], test.expected[component], test.tolerance, test.name);
  }
}

void registryCoverage(const std::vector<Case> &tests) {
  std::set<std::string> evaluated;
  for (const auto &test : tests) {
    require(evaluated.insert(test.name).second, "Duplicate builtin case: " + test.name);
    require(test.source.find(test.name + '(') != std::string::npos,
            "Builtin case does not call its named function: " + test.name);
  }
  require(evaluated.size() == 99, "Expected one evaluation case for all 99 builtins");
  const auto functions = Program::builtinFunctions();
  std::set<std::string> registered;
  for (const auto &function : functions) {
    registered.insert(function.name);
    require(!function.documentation.empty(), "Builtin help missing: " + function.name);
  }
  require(registered == evaluated,
          "The builtin registry and actual evaluation corpus differ");
}

void corpus(const std::vector<Case> &tests, Backend backend,
            const std::vector<Value> *reference, std::vector<Value> &results) {
  for (const auto &test : tests) {
    Program program;
    const bool compiled = program.compile(test.source, {}, {}, options(test.type, backend));
    require(compiled,
            "Compile failed for " + test.name + ": " + program.errorMessage());
    Value value;
    for (int iteration = 0; iteration < 3; ++iteration) {
      ConsoleCapture captured;
      const bool evaluated = program.evaluate(value);
      require(evaluated,
              "Evaluation failed for " + test.name + ": " + program.errorMessage());
      check(test, value);
      if (test.name == "printf") {
        require(captured.value().find("builtin 2 [1,2,3]") != std::string::npos &&
                    captured.value().size() <= 256,
                "Console builtin lost its bounded formatted output");
      } else {
        require(captured.value().empty(), "Non-console builtin wrote output: " + test.name);
      }
    }
    if (reference)
      same(value, reference->at(results.size()), std::max(1e-9, test.tolerance),
           "Interpreter/LLVM " + test.name);
    results.push_back(value);
    Program missingArgument;
    const bool acceptedMissing = missingArgument.compile(
        test.name + "()", {}, {}, options(test.type, backend));
    require(!acceptedMissing && !missingArgument.errorMessage().empty(),
            "Missing arguments did not produce a diagnostic for " + test.name);
  }
}

void identitiesAndBoundaries(Backend backend) {
  const std::vector<std::string> identities{
      "noise($p)-(snoise($p)*0.5+0.5)",
      "noise($p)-fbm($p,1)",
      "noise(0,0)-0.5",
      "noise(0,0,0,0)-0.5",
      "turbulence($p,1)-(abs(snoise($p))*0.5+0.5)",
      "length(cnoise($p)-(vnoise($p)*0.5+0.5))",
      "length(cnoise4($p,$t)-(vnoise4($p,$t)*0.5+0.5))",
      "fbm4($p,$t,1)-(snoise4($p,$t)*0.5+0.5)",
      "length(cfbm($p,3)-(vfbm($p,3)*0.5+0.5))",
      "length(cfbm4($p,$t,3)-(vfbm4($p,$t,3)*0.5+0.5))",
      "length(cturbulence($p,3)-(vturbulence($p,3)*0.5+0.5))",
      "pnoise($p,[2,3,4])-pnoise($p+[2,3,4],[2,3,4])",
      "length(hsltorgb(rgbtohsl([0.2,0.7,1.4]))-[0.2,0.7,1.4])",
      "length(rotate($p,[0,0,1],0.7))-length($p)",
      "dot(ortho($p,[0,0,1]),$p)",
      "getVar(\"gain\",17)-4"};
  for (const auto &identity : identities) {
    const auto value = evaluate(identity, Type::Scalar, backend,
        {{"p", Value::vector(0.125, 0.375, 0.625)},
         {"t", Value::scalar(0.25)}, {"gain", Value::scalar(4)}});
    near(value.number[0], 0, 1e-8, "Builtin identity " + identity);
  }

  near(evaluate("linearstep(-1,0,1)", Type::Scalar, backend).number[0], 0, 1e-12,
       "Lower linearstep endpoint");
  near(evaluate("linearstep(2,0,1)", Type::Scalar, backend).number[0], 1, 1e-12,
       "Upper linearstep endpoint");
  near(evaluate("smoothstep(0.25,1,0)", Type::Scalar, backend).number[0], 0.84375, 1e-12,
       "Reversed smoothstep range");
  near(evaluate("round(-1.5)", Type::Scalar, backend).number[0], -2, 1e-12,
       "Negative midpoint rounding");
  same(evaluate("norm([0,0,0])", Type::Vector, backend), Value::vector(0, 0, 0),
       1e-12, "Zero-length vector normalization");
  near(evaluate("angle([0,0,0],[1,0,0])", Type::Scalar, backend).number[0], 0,
       1e-12, "Zero-length vector angle");

  for (const char *name : {"cellnoise", "ccellnoise"}) {
    const Type type = std::string(name) == "cellnoise" ? Type::Scalar : Type::Vector;
    same(evaluate(std::string(name) + "([0.1,1.2,2.3])", type, backend),
         evaluate(std::string(name) + "([0.9,1.8,2.7])", type, backend), 1e-12,
         "Constant integer cell " + std::string(name));
  }

  for (int mode = 0; mode <= 4; ++mode) {
    Program ramp;
    const std::string source = "curve($x,0,0," + std::to_string(mode) +
        ",0.25,0.2," + std::to_string(mode) + ",1,1," + std::to_string(mode) + ')';
    require(ramp.compile(source, {{"x", Value::scalar(0)}}, {},
                         options(Type::Scalar, backend)), ramp.errorMessage());
    for (double x : {0.0, 0.25, 1.0}) {
      require(ramp.setScalar("x", x), ramp.errorMessage());
      Value value;
      require(ramp.evaluate(value), ramp.errorMessage());
      near(value.number[0], x == 0.25 ? 0.2 : x, 1e-10,
           "Curve interpolation did not pass through its control points");
    }
  }

  Program invalidDomain;
  require(invalidDomain.compile("sqrt($x)", {{"x", Value::scalar(-1)}}, {},
                                options(Type::Scalar, backend)),
          invalidDomain.errorMessage());
  Value invalid;
  require(!invalidDomain.evaluate(invalid) && !invalidDomain.errorMessage().empty(),
          "A nonfinite builtin result did not report evaluation failure");
}

std::vector<Value> noiseOrder(const std::vector<int> &order, Backend backend) {
  Program program;
  require(program.compile(
      "[noise($p),cellnoise($p),voronoi($p,2)[0]]",
      {{"p", Value::vector(0, 0, 0)}}, {}, options(Type::Vector, backend)),
      program.errorMessage());
  std::vector<Value> result(order.size());
  for (int index : order) {
    require(program.setVector("p", 0.125 + index * 0.125,
                               0.375 + index * 0.0625, 0.625),
            program.errorMessage());
    require(program.evaluate(result.at(static_cast<std::size_t>(index))),
            program.errorMessage());
  }
  return result;
}

void workerAndFrameOrder(Backend backend) {
  std::vector<int> order(16);
  for (int i = 0; i < 16; ++i) order[i] = i;
  const auto forward = noiseOrder(order, backend);
  std::reverse(order.begin(), order.end());
  const auto reverse = noiseOrder(order, backend);
  std::vector<std::future<std::vector<Value>>> workers;
  for (int i = 0; i < 4; ++i)
    workers.push_back(std::async(std::launch::async, [order, backend] {
      return noiseOrder(order, backend);
    }));
  for (std::size_t i = 0; i < forward.size(); ++i)
    same(reverse[i], forward[i], 1e-12, "Frame-order noise invariance");
  for (auto &worker : workers) {
    const auto result = worker.get();
    for (std::size_t i = 0; i < forward.size(); ++i)
      same(result[i], forward[i], 1e-12, "Worker-owned noise invariance");
  }
}

void rejectedFormat(const std::string &source, Type type, Backend backend,
                    const std::vector<Variable> &variables = {}) {
  Program program;
  ConsoleCapture captured;
  const bool compiled = program.compile(source, variables, {}, options(type, backend));
  require(!compiled && !program.errorMessage().empty(),
          "Malformed format was accepted: " + source);
  require(captured.value().empty(),
          "Invalid formatting produced console output before rejection");
}

void failedFormatEvaluation(const std::string &source, Backend backend) {
  Program program;
  const bool compiled = program.compile(source, {}, {}, options(Type::String, backend));
  require(compiled, "Valid format syntax did not compile: " + source + ": " +
                        program.errorMessage());
  Value output;
  const bool evaluated = program.evaluate(output);
  require(!evaluated && !program.errorMessage().empty(),
          "Unsafe formatting value was accepted: " + source);
}

std::string repeatedStringFormat(int conversions) {
  std::string format = "sprintf(\"";
  for (int i = 0; i < conversions; ++i) format += "%4096s";
  format += '"';
  for (int i = 0; i < conversions; ++i) format += ",$payload";
  return format + ')';
}

void formattingRegressions(Backend backend) {
  // Bad input previously entered assertion paths or reached mismatched C
  // varargs. Validation must return a diagnostic before any formatting runs.
  for (const char *source : {
           "printf(\"%\")",
           "printf(\"%s\",\"text\")",
           "printf(\"%n\",1)",
           "printf(\"%v\")",
           "printf(\"%v\",[1,2,3],4)",
           "printf(\"plain\",1)",
           "printf(\"%f\",\"text\")",
           "printf(\"%v\",\"text\")"})
    rejectedFormat(source, Type::Scalar, backend);

  for (const char *source : {
           "sprintf(\"%\")",
           "sprintf(\"%q\",1)",
           "sprintf(\"%n\",1)",
           "sprintf(\"%ld\",1)",
           "sprintf(\"%lld\",1)",
           "sprintf(\"%hhd\",1)",
           "sprintf(\"%zu\",1)",
           "sprintf(\"%jd\",1)",
           "sprintf(\"%td\",1)",
           "sprintf(\"%Lf\",1)",
           "sprintf(\"%*d\",3,1)",
           "sprintf(\"%.*f\",2,1)",
           "sprintf(\"%1$d\",1)",
           "sprintf(\"%4097d\",1)",
           "sprintf(\"%.4097f\",1)",
           "sprintf(\"%9999999999999999999999999999d\",1)",
           "sprintf(\"%d\")",
           "sprintf(\"plain\",1)",
           "sprintf(\"%s\",1)",
           "sprintf(\"%d\",\"text\")",
           "sprintf(\"%#s\",\"text\")",
           "sprintf(\"%+s\",\"text\")",
           "sprintf(\"%0s\",\"text\")",
           "sprintf(\"%#d\",1)",
           "sprintf(\"%.2c\",65)",
           "sprintf(\"%+u\",1)",
           "sprintf(\"% c\",65)"})
    rejectedFormat(source, Type::String, backend);

  rejectedFormat("printf($fmt,1)", Type::Scalar, backend,
                 {{"fmt", Value::string("%f"), Lifetime::Constant}});
  rejectedFormat("sprintf($fmt,1)", Type::String, backend,
                 {{"fmt", Value::string("%d"), Lifetime::Constant}});

  const struct {
    const char *source;
    const char *expected;
  } valid[] = {
      {"sprintf(\"%s:%d:%s:%04d\",\"%f\",7,\"%s%%\",3)", "%f:7:%s%%:0003"},
      {"sprintf(\"%%:%s:%%:%d\",\"%f\",5)", "%:%f:%:5"},
      {"sprintf(\"%d%d%d\",1,2,3)", "123"},
      {"sprintf(\"%d %i\",-2147483648,2147483647)", "-2147483648 2147483647"},
      {"sprintf(\"%d %i\",1.9,-1.9)", "1 -1"},
      {"sprintf(\"%u %x %X %o\",-1,-1,-1,-1)", "4294967295 ffffffff FFFFFFFF 37777777777"},
      {"sprintf(\"%u\",-1.9)", "4294967295"},
      {"sprintf(\"%u\",4294967295)", "4294967295"},
      {"sprintf(\"%05d\",7)", "00007"},
      {"sprintf(\"%+d|% d|%#x|%#o\",7,7,31,9)", "+7| 7|0x1f|011"},
      {"sprintf(\"%-5s|%5c\",\"ab\",65)", "ab   |    A"},
      {"sprintf(\"%#.0f\",1)", "1."},
      {"sprintf(\"%.0d\",0)", ""},
      {"sprintf(\"%.0s\",\"text\")", ""}};
  for (const auto &test : valid) {
    const auto value = evaluate(test.source, Type::String, backend);
    require(value.text == test.expected,
            std::string("Formatted string differs: ") + test.source);
  }

  for (const char *conversion : {"e", "E", "f", "F", "g", "G", "a", "A"}) {
    const auto value = evaluate(std::string("sprintf(\"%.") +
        (conversion[0] == 'g' || conversion[0] == 'G' ? "4" : "3") +
        conversion + "\",1.25)", Type::String, backend);
    char *end = nullptr;
    const double parsed = std::strtod(value.text.c_str(), &end);
    require(end == value.text.c_str() + value.text.size(),
            std::string("Floating conversion produced invalid numeric text: ") + conversion);
    near(parsed, 1.25, 1e-12,
         std::string("Floating conversion value: ") + conversion);
  }

  {
    Program console;
    const bool compiled = console.compile("printf(\"%%:%f:%v\",1.25,[1,2,3])",
        {}, {}, options(Type::Scalar, backend));
    require(compiled, console.errorMessage());
    ConsoleCapture captured;
    Value output;
    const bool evaluated = console.evaluate(output);
    require(evaluated, console.errorMessage());
    require(captured.value() == "%:1.25:[1,2,3]\n",
            "Console formatting escaped or substituted values incorrectly");
    near(output.number[0], 0, 1e-12, "Console formatting numeric return");
  }

  for (const char *source : {
           "sprintf(\"%d\",2147483648)",
           "sprintf(\"%i\",-2147483649)",
           "sprintf(\"%u\",4294967296)",
           "sprintf(\"%x\",-2147483649)",
           "sprintf(\"%c\",2147483648)",
           "sprintf(\"%c\",0)",
           "sprintf(\"%cX\",256)",
           "sprintf(\"%d\",1/0)",
           "sprintf(\"%u\",0/0)",
           "sprintf(\"%4096.4096f\",1)"})
    failedFormatEvaluation(source, backend);

  const auto widthBoundary = evaluate("sprintf(\"%4096s\",\"z\")",
                                      Type::String, backend);
  require(widthBoundary.text.size() == 4096 && widthBoundary.text.back() == 'z' &&
              widthBoundary.text.find_first_not_of(' ') == 4095,
          "The permitted width boundary was not formatted correctly");

  const std::vector<Variable> payload{{"payload", Value::string(std::string(4096, 'x'))}};
  const auto exactLimit = evaluate(repeatedStringFormat(256), Type::String,
                                   backend, payload);
  require(exactLimit.text.size() == 1048576 &&
              exactLimit.text.find_first_not_of('x') == std::string::npos,
          "The exact aggregate string limit was not preserved");
  Program overLimit;
  bool compiled = overLimit.compile(repeatedStringFormat(257), payload, {},
                                     options(Type::String, backend));
  require(compiled, overLimit.errorMessage());
  Value output;
  const bool evaluated = overLimit.evaluate(output);
  require(!evaluated && !overLimit.errorMessage().empty(),
          "Aggregate formatted output exceeded its limit without failure");

  auto limited = options(Type::String, backend);
  limited.maxStringBytes = 64;
  Program recovery;
  compiled = recovery.compile("sprintf(\"%s\",$payload)",
      {{"payload", Value::string(std::string(65, 'x'))}}, {}, limited);
  require(compiled, recovery.errorMessage());
  const bool oversized = recovery.evaluate(output);
  require(!oversized && !recovery.errorMessage().empty(),
          "A configured string bound was ignored by formatted output");
  require(recovery.setString("payload", "ok"), recovery.errorMessage());
  const bool recovered = recovery.evaluate(output);
  require(recovered && output.type == Type::String && output.text == "ok",
          "A failed format evaluation did not recover after valid input");
}
}  // namespace

int main() {
  try {
#ifdef SEEXPR_FORMATTING_ONLY
    formattingRegressions(Backend::Interpreter);
    if (Program::backendAvailable(Backend::LLVM)) formattingRegressions(Backend::LLVM);
    std::cout << "SeExpr formatting safety: PASS\n";
#else
    const auto tests = cases();
    registryCoverage(tests);
    std::vector<Value> interpreted;
    corpus(tests, Backend::Interpreter, nullptr, interpreted);
    identitiesAndBoundaries(Backend::Interpreter);
    workerAndFrameOrder(Backend::Interpreter);
    if (Program::backendAvailable(Backend::LLVM)) {
      std::vector<Value> compiled;
      corpus(tests, Backend::LLVM, &interpreted, compiled);
      identitiesAndBoundaries(Backend::LLVM);
      workerAndFrameOrder(Backend::LLVM);
    }
    std::cout << "SeExpr builtin evaluation: PASS (99 functions)\n";
#endif
    return 0;
  } catch (const std::exception &error) {
#ifdef SEEXPR_FORMATTING_ONLY
    std::cerr << "SeExpr formatting safety: FAIL: " << error.what() << '\n';
#else
    std::cerr << "SeExpr builtin evaluation: FAIL: " << error.what() << '\n';
#endif
    return 1;
  }
}
