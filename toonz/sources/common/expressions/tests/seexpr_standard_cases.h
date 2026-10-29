#pragma once

#include "tseexpr.h"

#include <SeExpr2/ExprFunc.h>
#include <SeExpr2/Expression.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace TSeExprStandardQualification {

enum class Signal { Failure };
inline thread_local int failureMode = 0;

inline void checkFailure() {
  if (failureMode == 1) throw std::runtime_error("fixture SDK standard callback failure");
  if (failureMode == 2) throw Signal::Failure;
}

inline double scalar0() { checkFailure(); return 7; }
inline double scalar1(double a) { checkFailure(); return a + 1; }
inline double scalar2(double a, double b) { checkFailure(); return a + 10 * b; }
inline double scalar3(double a, double b, double c) {
  checkFailure(); return a + 10 * b + 100 * c;
}
inline double scalar4(double a, double b, double c, double d) {
  checkFailure(); return a + 10 * b + 100 * c + 1000 * d;
}
inline double scalar5(double a, double b, double c, double d, double e) {
  checkFailure(); return a + 10 * b + 100 * c + 1000 * d + 10000 * e;
}
inline double scalar6(double a, double b, double c, double d, double e, double f) {
  checkFailure(); return a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * f;
}
inline double scalarN(int count, double *arguments) {
  checkFailure();
  double result = 0;
  for (int index = 0; index < count; ++index) result = result * 10 + arguments[index];
  return result;
}

inline double encode(const SeExpr2::Vec3d &value) {
  return value[0] + 10 * value[1] + 100 * value[2];
}
inline double vectorScalar1(const SeExpr2::Vec3d &a) { checkFailure(); return encode(a); }
inline double vectorScalar2(const SeExpr2::Vec3d &a, const SeExpr2::Vec3d &b) {
  checkFailure(); return encode(a) + 1000 * encode(b);
}
inline double vectorScalarN(int count, const SeExpr2::Vec3d *arguments) {
  checkFailure();
  double result = 0;
  for (int index = 0; index < count; ++index) result += (index + 1) * encode(arguments[index]);
  return result;
}
inline SeExpr2::Vec3d vector1(const SeExpr2::Vec3d &a) {
  checkFailure(); return SeExpr2::Vec3d(a[0] + 10, a[1] + 20, a[2] + 30);
}
inline SeExpr2::Vec3d vector2(const SeExpr2::Vec3d &a, const SeExpr2::Vec3d &b) {
  checkFailure(); return SeExpr2::Vec3d(a[0] + 2 * b[0], a[1] + 2 * b[1], a[2] + 2 * b[2]);
}
inline SeExpr2::Vec3d vectorN(int count, const SeExpr2::Vec3d *arguments) {
  checkFailure();
  SeExpr2::Vec3d result(0.0);
  for (int index = 0; index < count; ++index)
    for (int component = 0; component < 3; ++component)
      result[component] += (index + 1) * arguments[index][component];
  return result;
}

struct Case {
  const char *source;
  SeExpr2::ExprFunc function;
  TSeExpr::Value expected;
};

class Expression final : public SeExpr2::Expression {
  class Variable final : public SeExpr2::ExprVarRef {
    std::vector<double> m_value;

  public:
    explicit Variable(std::vector<double> value)
        : SeExpr2::ExprVarRef(SeExpr2::ExprType().FP(static_cast<int>(value.size())).Varying())
        , m_value(std::move(value)) {}
    void eval(double *output) override { std::copy(m_value.begin(), m_value.end(), output); }
    void eval(const char **) override { throw std::runtime_error("unexpected string variable callback"); }
  };

  mutable Variable m_scalar{{2.0}}, m_vector{{1.0, 2.0, 3.0}}, m_four{{1.0, 2.0, 3.0, 4.0}};
  mutable SeExpr2::ExprFunc m_function;
  const TSeExpr::Type m_type;
  const std::size_t m_dimension;

public:
  Expression(const Case &fixture, EvaluationStrategy strategy)
      : SeExpr2::Expression(fixture.source,
                            fixture.expected.type == TSeExpr::Type::String
                                ? SeExpr2::ExprType().String().Varying()
                                : SeExpr2::ExprType().FP(static_cast<int>(fixture.expected.number.size())).Varying(),
                            strategy)
      , m_function(fixture.function)
      , m_type(fixture.expected.type)
      , m_dimension(fixture.expected.number.size()) {}
  SeExpr2::ExprVarRef *resolveVar(const std::string &name) const override {
    if (name == "x") return &m_scalar;
    if (name == "v") return &m_vector;
    if (name == "four") return &m_four;
    return nullptr;
  }
  SeExpr2::ExprFunc *resolveFunc(const std::string &name) const override {
    return name == "standard" || name == "printf" ? &m_function : nullptr;
  }
  TSeExpr::Value evaluate() {
    if (m_type == TSeExpr::Type::String) return TSeExpr::Value::string(evalStr());
    const double *data = evalFP();
    return TSeExpr::Value::numeric(std::vector<double>(data, data + m_dimension));
  }
};

inline void run(bool includeLLVM = true) {
  using SeExpr2::ExprFunc;
  using TSeExpr::Value;
  const std::vector<Case> cases{
      {"standard()", ExprFunc(&scalar0), Value::scalar(7)},
      {"standard($x)", ExprFunc(&scalar1), Value::scalar(3)},
      {"standard($x,3)", ExprFunc(&scalar2), Value::scalar(32)},
      {"standard($x,3,4)", ExprFunc(&scalar3), Value::scalar(432)},
      {"standard($x,3,4,5)", ExprFunc(&scalar4), Value::scalar(5432)},
      {"standard($x,3,4,5,6)", ExprFunc(&scalar5), Value::scalar(65432)},
      {"standard($x,3,4,5,6,7)", ExprFunc(&scalar6), Value::scalar(765432)},
      {"standard($x,3,4,5)", ExprFunc(&scalarN, 0, -1), Value::scalar(2345)},
      {"standard()", ExprFunc(&scalarN, 0, -1), Value::scalar(0)},
      {"standard($v)", ExprFunc(&vectorScalar1), Value::scalar(321)},
      {"standard($v,2)", ExprFunc(&vectorScalar2), Value::scalar(222321)},
      {"standard($v,2,[4,5,6])", ExprFunc(&vectorScalarN, 0, -1), Value::scalar(2727)},
      {"standard($v)", ExprFunc(&vector1), Value::vector(11, 22, 33)},
      {"standard($v,[4,5,6])", ExprFunc(&vector2), Value::vector(9, 12, 15)},
      {"standard($v,2,[4,5,6])", ExprFunc(&vectorN, 0, -1), Value::vector(17, 21, 25)},
      {"standard($v)", ExprFunc(&vectorN), Value::vector(1, 2, 3)},
      {"standard()", ExprFunc(&vectorN, 0, -1), Value::vector(0, 0, 0)},
      {"standard($v)", ExprFunc(&vector1), Value::scalar(11)},
      {"standard($v,[4,5,6])", ExprFunc(&vector2), Value::scalar(9)},
      {"standard($v,2,[4,5,6])", ExprFunc(&vectorN, 0, -1), Value::scalar(17)},
      {"standard($four)", ExprFunc(&scalar1), Value::numeric({2, 3, 4, 5})},
      {"standard($v)", ExprFunc(&scalar1), Value::scalar(2)},
      {"standard($v,4)", ExprFunc(&scalarN, 0, -1), Value::vector(14, 24, 34)},
      {"standard(standard($x))", ExprFunc(&scalar1), Value::scalar(4)},
      {"standard(standard($v))", ExprFunc(&vector1), Value::vector(21, 42, 63)},
      {"sprintf(\"[%f]\",standard($x))", ExprFunc(&scalar1), Value::string("[3.000000]")},
      {"extern FLOAT standard(FLOAT) standard($x)", ExprFunc(&scalar1), Value::scalar(3)},
      {"printf($x)", ExprFunc(&scalar1), Value::scalar(3)}};
  std::vector<SeExpr2::Expression::EvaluationStrategy> strategies{SeExpr2::Expression::UseInterpreter};
  if (includeLLVM) strategies.push_back(SeExpr2::Expression::UseLLVM);
  for (const auto &fixture : cases) {
    for (const auto strategy : strategies) {
      failureMode = 0;
      Expression expression(fixture, strategy);
      if (!expression.isValid())
        throw std::runtime_error("Typed SDK standard fixture did not compile: " +
                                 std::string(fixture.source) + ": " + expression.parseError());
      if (std::string(fixture.source).find('$') != std::string::npos &&
          !expression.returnType().isLifetimeVarying())
        throw std::runtime_error("A standard callback lost an input's varying lifetime: " +
                                 std::string(fixture.source));
      const auto expect = [&]() {
        const Value result = expression.evaluate();
        if (result.type != fixture.expected.type || result.number != fixture.expected.number ||
            result.text != fixture.expected.text)
          throw std::runtime_error("Typed SDK standard callback returned an incorrect result: " +
                                   std::string(fixture.source));
      };
      expect();
      for (const int mode : {1, 2}) {
        failureMode = mode;
        for (int attempt = 0; attempt < 2; ++attempt) {
          bool reported = false;
          try {
            expression.evaluate();
          } catch (const std::runtime_error &error) {
            if (mode != 1 || std::string(error.what()).find("fixture SDK standard callback failure") == std::string::npos)
              throw;
            reported = true;
          } catch (const Signal signal) {
            if (mode != 2 || signal != Signal::Failure) throw;
            reported = true;
          }
          if (!reported)
            throw std::runtime_error("Typed SDK standard callback exception was swallowed: " +
                                     std::string(fixture.source));
        }
      }
      failureMode = 0;
      expect();
    }
  }
  for (const auto &function : {ExprFunc(&scalar2), ExprFunc(&vectorScalar2)}) {
    const Case invalid{
        "def FLOAT UNIFORM invalid(FLOAT VARYING a){standard(a,1)} invalid($x)",
        function, Value::scalar(0)};
    for (const auto strategy : strategies) {
      failureMode = 0;
      Expression expression(invalid, strategy);
      if (expression.isValid())
        throw std::runtime_error("A trailing constant hid a varying standard callback input");
    }
  }
}

}  // namespace TSeExprStandardQualification
