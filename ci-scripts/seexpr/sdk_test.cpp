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

void syntaxQueriesAndRecovery() {
  using namespace SeExpr2;
  Expression expression(Expression::UseInterpreter);
  expression.setDesiredReturnType(ExprType().FP(1).Varying());
  expression.setExpr("$unbound_channel + 1");
  require(expression.syntaxOK(),
          "Syntax inspection incorrectly requires a host variable binding");
  require(!expression.isValid(),
          "An unbound host variable unexpectedly passed semantic validation");
  require(!expression.getErrors().empty(),
          "Semantic failure did not retain an actionable diagnostic");
  require(expression.syntaxOK(),
          "Semantic validation changed the result of syntax-only inspection");

  expression.setExpr("2 + 3");
  require(expression.syntaxOK() && expression.isValid(),
          "SDK expression did not recover after a missing variable");
  near(expression.evalFP()[0], 5,
       "SDK evaluation changed after syntax/semantic inspection");

  // An empty source still caches its failed parse. Replacing it must discard
  // that state; the old nonempty-text guard left subsequent sources unparsed.
  for (int attempt = 0; attempt < 3; ++attempt) {
    expression.setExpr("");
    require(!expression.syntaxOK() && !expression.isValid() &&
                !expression.getErrors().empty(),
            "An empty SDK expression did not report its parse failure");
    expression.setExpr("5*7+2");
    require(expression.syntaxOK() && expression.isValid(),
            "SDK parser did not recover after an empty expression");
    near(expression.evalFP()[0], 37,
         "SDK evaluation reused state from an empty expression");
    expression.setExpr(expression.getExpr());
    require(expression.getExpr() == "5*7+2" && expression.syntaxOK() &&
                expression.isValid(),
            "Aliased SDK source did not survive evaluator reset");
    near(expression.evalFP()[0], 37,
         "Aliased SDK source changed after evaluator reset");
  }

  // A long flat arithmetic chain has shallow punctuation nesting but a deep
  // syntax tree. The raw SDK must reject it before recursive preparation and
  // must also be able to dispose of that tree and parse another expression.
  std::string deep = "1";
  for (int term = 0; term < 1200; ++term) deep += " + 1";
  for (int attempt = 0; attempt < 3; ++attempt) {
    expression.setExpr(deep);
    require(!expression.syntaxOK() && !expression.isValid(),
            "An excessive SDK syntax tree reached semantic evaluation");
    require(expression.parseError().find("depth") != std::string::npos &&
                !expression.getErrors().empty(),
            "An excessive SDK syntax tree has no clear depth diagnostic");
    expression.setExpr("7");
    require(expression.syntaxOK() && expression.isValid(),
            "SDK parser state did not recover after a rejected deep tree");
    near(expression.evalFP()[0], 7,
         "SDK recovery reused stale data after a rejected deep tree");
  }
}

void emptyStandardFunctionState() {
  SeExpr2::ExprFuncStandard empty;
  require(empty.getFuncType() == SeExpr2::ExprFuncStandard::NONE &&
              empty.getFuncPointer() == nullptr,
          "An empty SDK standard function exposed uninitialized state");
  const SeExpr2::ExprFuncStandard copied(empty);
  require(copied.getFuncType() == SeExpr2::ExprFuncStandard::NONE &&
              copied.getFuncPointer() == nullptr,
          "Copying an empty SDK standard function changed its empty state");
  // A custom function also owns an unused default standard-function slot.
  // Builtin registration copies that slot; sanitizer qualification covers
  // both it and the base extension-function state before any preparation.
  const SeExpr2::ExprFunc unset;
  const SeExpr2::ExprFunc copiedUnset(unset);
  require(copiedUnset.minArgs() == 0 && copiedUnset.maxArgs() == 0,
          "Copying an empty SDK function changed its argument contract");
}

struct LifetimeData final : SeExpr2::ExprFuncNode::Data {
  bool &destroyed;
  LifetimeData(bool cleanup, bool &destroyed)
      : SeExpr2::ExprFuncNode::Data(cleanup), destroyed(destroyed) {}
  ~LifetimeData() override { destroyed = true; }
};

class LifetimeFunction final : public SeExpr2::ExprFuncSimple {
  LifetimeData *borrowed;
  bool &destroyed;

public:
  LifetimeFunction(LifetimeData *borrowed, bool &destroyed)
      : SeExpr2::ExprFuncSimple(true), borrowed(borrowed), destroyed(destroyed) {}
  SeExpr2::ExprType prep(SeExpr2::ExprFuncNode *, bool,
                         SeExpr2::ExprVarEnvBuilder &) const override {
    return SeExpr2::ExprType().FP(1).Varying();
  }
  SeExpr2::ExprFuncNode::Data *evalConstant(const SeExpr2::ExprFuncNode *,
                                          ArgHandle) const override {
    return borrowed ? borrowed : new LifetimeData(true, destroyed);
  }
  void eval(ArgHandle args) override { args.outFp = 9; }
};

class LifetimeExpression final : public SeExpr2::Expression {
  mutable SeExpr2::ExprFunc callback;

public:
  explicit LifetimeExpression(LifetimeFunction &function)
      : SeExpr2::Expression("sdk_lifetime()", SeExpr2::ExprType().FP(1).Varying(),
                            SeExpr2::Expression::UseInterpreter),
        callback(function, 0, 0) {}
  SeExpr2::ExprFunc *resolveFunc(const std::string &name) const override {
    return name == "sdk_lifetime" ? &callback : nullptr;
  }
};

void ownedAndBorrowedFunctionData() {
  bool ownedDestroyed = false;
  LifetimeFunction owned(nullptr, ownedDestroyed);
  {
    LifetimeExpression expression(owned);
    require(expression.isValid(), "Owned SDK callback data did not prepare");
    near(expression.evalFP()[0], 9, "Owned SDK callback data changed evaluation");
    require(!ownedDestroyed, "SDK callback data was destroyed before evaluation");
    expression.reset();
    require(ownedDestroyed, "Evaluator reset did not release owned SDK callback data");
  }

  bool borrowedDestroyed = false;
  {
    LifetimeData borrowed(false, borrowedDestroyed);
    LifetimeFunction function(&borrowed, borrowedDestroyed);
    {
      LifetimeExpression expression(function);
      require(expression.isValid(), "Borrowed SDK callback data did not prepare");
      near(expression.evalFP()[0], 9, "Borrowed SDK callback data changed evaluation");
      expression.reset();
      require(!borrowedDestroyed,
              "Evaluator reset deleted data owned by its SDK host");
    }
    require(!borrowedDestroyed,
            "Evaluator destruction deleted borrowed SDK callback data");
  }
  require(borrowedDestroyed, "Borrowed callback data outlived its actual owner");
}

class ThrowingPreparation final : public SeExpr2::ExprFuncSimple {
public:
  ThrowingPreparation() : SeExpr2::ExprFuncSimple(true) {}
  SeExpr2::ExprType prep(SeExpr2::ExprFuncNode *, bool,
                         SeExpr2::ExprVarEnvBuilder &) const override {
    throw std::runtime_error("SDK fixture rejected preparation");
  }
  SeExpr2::ExprFuncNode::Data *evalConstant(const SeExpr2::ExprFuncNode *,
                                          ArgHandle) const override {
    return nullptr;
  }
  void eval(ArgHandle args) override { args.outFp = 0; }
};

void graphResetAndFailureOwnership() {
  using namespace SeExpr2;
  static ThrowingPreparation throwing;
  ExprFunc::define("sdk_lifetime_throw", ExprFunc(throwing, 0, 0));
  Expressions graph;
  for (int iteration = 0; iteration < 8; ++iteration) {
    char caption[] = "cel";
    const auto input = graph.addExternalVariable("input", ExprType().FP(1).Varying());
    const auto text = graph.addExternalVariable("caption", ExprType().String().Varying());
    const auto first = graph.addExpression("first", ExprType().FP(1).Varying(), "$input+1");
    const auto second = graph.addExpression("second", ExprType().FP(1).Varying(),
                                           "$first+$input");
    const auto label = graph.addExpression("label", ExprType().String().Varying(), "$caption");
    require(graph.isValid(), "Reusable SDK expression graph did not compile");
    bool rejected = false;
    try {
      graph.addExpression("rejected", ExprType().FP(1).Varying(),
                          "$first+$input+sdk_lifetime_throw()");
    } catch (const std::runtime_error &) {
      rejected = true;
    }
    require(rejected && graph.AllExprs.size() == 3,
            "Failed SDK graph preparation published an incomplete expression");
    // A failed constructor must remove its borrowed observer registrations.
    // Updating both existing inputs exposes stale registrations under ASan.
    graph.setVariable(input, static_cast<double>(iteration));
    graph.setVariable(text, caption);
    graph.getLoopVarSetHandle(input);
    graph.getLoopVarSetHandle(text);
    const auto &result = graph.evalFP(graph.getExprEvalHandle(second));
    require(result.size() == 1, "Reset graph changed its numeric result shape");
    near(result[0], iteration * 2 + 1, "Reset graph retained a previous context value");
    require(std::string(graph.evalStr(graph.getExprEvalHandle(label))) == "cel",
            "Graph string result did not preserve its borrowed host buffer");
    require((*first)->operandExprs.empty(),
            "A rebuilt graph inherited a destroyed expression dependency");
    graph.reset();
    require(graph.AllExprs.empty() && graph.AllExternalVars.empty(),
            "SDK graph reset retained owned expressions or variables");
    require(std::string(caption) == "cel",
            "SDK graph reset changed a borrowed host string buffer");
    graph.reset();
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
    emptyStandardFunctionState();
    ownedAndBorrowedFunctionData();
    syntaxQueriesAndRecovery();
    blockEvaluation();
    expressionGraphIsolation();
    graphResetAndFailureOwnership();
    std::cout << "SeExpr SDK parity: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "SeExpr SDK parity: FAIL: " << error.what() << '\n';
    return 1;
  }
}
