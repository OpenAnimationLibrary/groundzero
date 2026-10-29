#include <SeExpr2/ExprFunc.h>
#include <SeExpr2/ExprMultiExpr.h>
#include <SeExpr2/Expression.h>
#include <SeExpr2/VarBlock.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

void near(double actual, double expected, const char *message) {
  require(std::isfinite(actual) && std::abs(actual - expected) < 1e-12, message);
}

void registryParity() {
  // Reviewed public function names in the pinned upstream registry. Keeping
  // this corpus independent of the runtime help prevents platform guards from
  // quietly shrinking the feature set while the editor still appears usable.
  const char *expected[] = {
      "abs", "acos", "asin", "atan", "atan2", "ceil", "cos", "cosh",
      "exp", "floor", "fmod", "log", "log10", "pow", "sin", "sinh",
      "sqrt", "tan", "tanh", "cbrt", "asinh", "acosh", "atanh", "trunc",
      "deg", "rad", "cosd", "sind", "tand", "acosd", "asind", "atand",
      "atan2d", "clamp", "round", "max", "min", "invert", "compress",
      "expand", "fit", "gamma", "bias", "contrast", "boxstep", "linearstep",
      "smoothstep", "gaussstep", "remap", "mix", "hsi", "midhsi",
      "hsltorgb", "rgbtohsl", "saturate", "hash", "noise", "snoise",
      "vnoise", "cnoise", "snoise4", "vnoise4", "cnoise4", "turbulence",
      "vturbulence", "cturbulence", "fbm", "vfbm", "cfbm", "cellnoise",
      "ccellnoise", "pnoise", "fbm4", "vfbm4", "cfbm4", "dist", "length",
      "hypot", "dot", "norm", "cross", "angle", "ortho", "rotate", "up",
      "cycle", "pick", "choose", "wchoose", "swatch", "spline", "voronoi",
      "cvoronoi", "pvoronoi", "curve", "ccurve", "getVar", "printf", "sprintf"};
  SeExpr2::ExprFunc::init();
  std::vector<std::string> names;
  SeExpr2::ExprFunc::getFunctionNames(names);
  for (const char *name : expected) {
    if (std::find(names.begin(), names.end(), name) == names.end())
      throw std::runtime_error(std::string("Missing SDK builtin: ") + name);
    require(!SeExpr2::ExprFunc::getDocString(name).empty(),
            "A registered SDK builtin lost its documentation");
  }
}

void blockEvaluation() {
  using namespace SeExpr2;
  VarBlockCreator creator;
  const int input = creator.registerVariable("input", ExprType().FP(4).Varying());
  const int output = creator.registerVariable("output", ExprType().FP(4).Varying());
  auto block = creator.create();
  std::vector<double> samples{0, 1, 2, 3, 4, 5, 6, 7, -1, -2, -3, -4, 8, 9, 10, 11};
  std::vector<double> values(samples.size(), -999);
  block.Pointer(input) = samples.data();
  block.Pointer(output) = values.data();
  Expression expression(Expression::UseInterpreter);
  expression.setExpr("$input * 2 + 1");
  expression.setVarBlockCreator(&creator);
  expression.setDesiredReturnType(ExprType().FP(4).Varying());
  require(expression.isValid(), "Four-component SDK block expression did not compile");
  expression.evalMultiple(&block, output, 1, 3);
  for (std::size_t i = 0; i < samples.size(); ++i)
    near(values[i], i >= 4 && i < 12 ? samples[i] * 2 + 1 : -999,
         "SDK batch range or vector stride is incorrect");
  block.indirectIndex = 2;
  const double *single = expression.evalFP(&block);
  for (int component = 0; component < 4; ++component)
    near(single[component], samples[8 + component] * 2 + 1,
         "SDK indexed evaluation differs from block evaluation");
}

void expressionGraphIsolation() {
  using namespace SeExpr2;
  // Consecutive graph lifetimes must not inherit old compilation dependencies.
  // This also exercises both constant and loop-variable invalidation paths.
  for (int iteration = 0; iteration < 4; ++iteration) {
    Expressions graph;
    const auto input = graph.addExternalVariable("input", ExprType().FP(1).Varying());
    const auto first = graph.addExpression("first", ExprType().FP(1).Varying(), "$input + 1");
    require((*first)->operandExprs.empty(), "A new SDK graph inherited old expression pointers");
    require((*first)->operandVars.size() == 1, "A new SDK graph inherited old variable pointers");
    const auto second = graph.addExpression("second", ExprType().FP(1).Varying(), "$first * 2");
    require(graph.isValid(), "SDK expression graph did not compile");
    require((*second)->operandExprs.size() == 1, "SDK graph dependencies are incorrect");
    graph.setVariable(input, 2.0);
    const auto loop = graph.getLoopVarSetHandle(input);
    const auto handle = graph.getExprEvalHandle(second);
    graph.setLoopVariable(loop, 5.0);
    const auto &values = graph.evalFP(handle);
    require(values.size() == 1, "SDK graph result dimension changed");
    near(values[0], 12, "SDK graph did not update dependent results");
  }
}
}  // namespace

int main() {
  try {
    SeExpr2::Expression::defaultEvaluationStrategy = SeExpr2::Expression::UseInterpreter;
    registryParity();
    blockEvaluation();
    expressionGraphIsolation();
    std::cout << "SeExpr SDK parity: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "SeExpr SDK parity: FAIL: " << error.what() << '\n';
    return 1;
  }
}
