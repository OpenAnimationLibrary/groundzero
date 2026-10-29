#include <SeExpr2/Expression.h>
#include <SeExpr2/ExprFunc.h>

#include <cmath>
#include <iostream>

namespace {
double pixelsToWorld(double pixels) { return pixels / 120.0; }

class HostExpression final : public SeExpr2::Expression {
public:
  HostExpression()
      : SeExpr2::Expression("pixels_to_world(240) + frame", SeExpr2::ExprType().FP(1),
                           SeExpr2::Expression::UseInterpreter)
      , m_function(pixelsToWorld) {}
private:
  struct Frame final : SeExpr2::ExprVarRef {
    Frame() : ExprVarRef(SeExpr2::ExprType().FP(1).Varying()) {}
    void eval(double* output) override { output[0] = 12; }
    void eval(const char**) override {}
  };
  mutable Frame m_frame;
  SeExpr2::ExprFunc m_function;
  SeExpr2::ExprVarRef* resolveVar(const std::string& name) const override {
    return name == "frame" ? &m_frame : nullptr;
  }
  SeExpr2::ExprFunc* resolveFunc(const std::string& name) const override {
    return name == "pixels_to_world" ? const_cast<SeExpr2::ExprFunc*>(&m_function) : nullptr;
  }
};
}

int main() {
  HostExpression expression;
  if (!expression.isValid() || std::abs(expression.evalFP()[0] - 14) > 1e-12)
    return 1;
  std::cout << "SeExpr host variable/function callback: PASS\n";
  return 0;
}
