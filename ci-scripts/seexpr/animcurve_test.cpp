#include "tseexpr.h"
#include "tseexpranimcurve.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using TSeExprAnimCurve::Curve;
using TSeExprAnimCurve::Key;
using TSeExprAnimCurve::PreparedCurve;

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, const char *message) {
  require(std::isfinite(actual) &&
              std::abs(actual - expected) <= 1e-10 * std::max(1.0, std::abs(expected)),
          message);
}
Key key(double time, double value, const std::string &mode = "linear") {
  Key result;
  result.time = time; result.value = value;
  result.inTangent = result.outTangent = mode;
  return result;
}
PreparedCurve prepare(const Curve &curve) {
  PreparedCurve prepared;
  std::string error;
  require(prepared.prepare(curve, error), error);
  return prepared;
}
double sample(const PreparedCurve &curve, double time) {
  double value = 0.0;
  std::string error;
  require(curve.evaluate(time, value, error), error);
  return value;
}
void spansAndTangents() {
  Curve curve;
  near(sample(prepare(curve), -3.5), 0, "empty curve did not return zero");
  curve.keys = {key(1.25, 8.5)};
  auto prepared = prepare(curve);
  near(sample(prepared, -100), 8.5, "single key changed before its time");
  near(sample(prepared, 100), 8.5, "single key changed after its time");

  curve.keys = {key(0, 1), key(2, 5)};
  prepared = prepare(curve);
  near(sample(prepared, .5), 2, "linear tangents changed a fractional sample");
  curve.keys[0].value = 999;
  near(sample(prepared, .5), 2, "prepared curve retained mutable source keys");

  for (const std::string mode : {"flat", "auto"}) {
    curve.keys = {key(0, 0, mode), key(2, 2, mode)};
    near(sample(prepare(curve), .5), .3125,
         "flat/endpoint-auto curve is not Hermite smoothstep");
  }
  curve.keys = {key(0, 0, "spline"), key(2, 2, "spline")};
  near(sample(prepare(curve), .5), .5,
       "one-sided spline tangent did not use its neighbor");

  curve.keys = {key(0, 0, "auto"), key(1, 2, "auto"), key(2, 1, "auto")};
  std::string error;
  require(TSeExprAnimCurve::refreshTangents(curve, error), error);
  near(curve.keys[1].inAngle, 0, "automatic tangent did not flatten a turning point");
  curve.keys[1].inTangent = curve.keys[1].outTangent = "spline";
  require(TSeExprAnimCurve::refreshTangents(curve, error), error);
  near(curve.keys[1].inAngle, std::atan(.5) * 180.0 / std::acos(-1.0),
       "spline tangent differs from the historical central difference");

  curve.keys = {key(0, 1, "fixed"), key(2, 5, "fixed")};
  for (auto &item : curve.keys)
    item.inAngle = item.outAngle = std::atan(2.0) * 180.0 / std::acos(-1.0);
  near(sample(prepare(curve), .5), 2, "fixed angles were not interpreted in degrees");

  for (const std::string mode : {"step", "stepnext"}) {
    curve.keys = {key(0, 3, mode), key(2, 7, mode)};
    prepared = prepare(curve);
    near(sample(prepared, 0), 3, "step changed its exact first key");
    near(sample(prepared, .25), mode == "step" ? 3 : 7,
         "step/stepnext interior selected the wrong endpoint");
    near(sample(prepared, 2), 7, "step changed its exact last key");
  }
}

void weightedBezier() {
  Curve curve;
  curve.weighted = true;
  curve.keys = {key(0, 0, "fixed"), key(2, 1, "fixed")};
  curve.keys[0].outWeight = std::hypot(.25, 1.0);
  curve.keys[0].outAngle = std::atan2(1.0, .25) * 180.0 / std::acos(-1.0);
  curve.keys[1].inWeight = std::hypot(.5, .25);
  curve.keys[1].inAngle = std::atan2(.25, .5) * 180.0 / std::acos(-1.0);
  const auto prepared = prepare(curve);
  for (double u : {.125, .375, .625, .875}) {
    const double v = 1.0 - u;
    // Independent Bernstein expression of the known four control points.
    const double x = 3 * v * v * u * .25 + 3 * v * u * u * 1.5 + u * u * u * 2;
    const double y = 3 * v * v * u + 3 * v * u * u * .75 + u * u * u;
    near(sample(prepared, x), y, "weighted curve did not invert the Bezier time coordinate");
  }
  curve.keys = {key(0, 0, "flat"), key(2, 2, "flat")};
  near(sample(prepare(curve), .5), .3125,
       "generated weighted flat tangents changed the unweighted curve");
}

void endpointTangents() {
  for (bool weighted : {false, true}) {
    for (const std::string mode : {"linear", "spline", "auto", "flat"}) {
      Curve curve;
      curve.weighted = weighted;
      curve.preInfinity = curve.postInfinity = "linear";
      curve.keys = {key(0, 1, mode), key(2, 5, mode)};
      const auto prepared = prepare(curve);
      const bool straight = mode == "linear" || mode == "spline";
      near(sample(prepared, -1), straight ? -1 : 1,
           "outward first-key tangent lost its one-sided or flat endpoint rule");
      near(sample(prepared, 3), straight ? 7 : 5,
           "outward last-key tangent lost its one-sided or flat endpoint rule");
    }
    Curve fixed;
    fixed.weighted = weighted;
    fixed.preInfinity = fixed.postInfinity = "linear";
    fixed.keys = {key(0, 1, "fixed"), key(2, 5, "fixed")};
    fixed.keys.front().inAngle = 45;
    fixed.keys.back().outAngle = -45;
    const auto prepared = prepare(fixed);
    near(sample(prepared, -1), 0, "fixed first-key infinity angle was regenerated");
    near(sample(prepared, 3), 4, "fixed last-key infinity angle was regenerated");
  }
}

void infinityModes() {
  Curve curve;
  curve.keys = {key(0, 1), key(2, 5)};
  struct Fixture { const char *mode; double before, after, preBoundary, postBoundary; };
  for (const auto fixture : {
           Fixture{"constant", 1, 5, 1, 5},
           Fixture{"linear", -1, 7, -3, 9},
           Fixture{"cycle", 3, 3, 5, 1},
           Fixture{"cycleRelative", -1, 7, -3, 9},
           Fixture{"oscillate", 3, 3, 5, 1}}) {
    curve.preInfinity = curve.postInfinity = fixture.mode;
    const auto prepared = prepare(curve);
    near(sample(prepared, -1), fixture.before, "negative-time infinity value differs");
    near(sample(prepared, 3), fixture.after, "post infinity value differs");
    near(sample(prepared, -2), fixture.preBoundary, "exact pre cycle boundary differs");
    near(sample(prepared, 4), fixture.postBoundary, "exact post cycle boundary differs");
    near(sample(prepared, 0), 1, "infinity policy changed exact first key");
    near(sample(prepared, 2), 5, "infinity policy changed exact last key");
  }
  curve.preInfinity = "constant";
  curve.postInfinity = "linear";
  const auto independent = prepare(curve);
  near(sample(independent, -.5), 1, "post policy replaced independent pre policy");
  near(sample(independent, 2.25), 5.5, "pre policy replaced independent post policy");

  curve.preInfinity = curve.postInfinity = "oscillate";
  const auto oscillating = prepare(curve);
  near(sample(oscillating, -4), 1, "second pre oscillation boundary differs");
  near(sample(oscillating, 6), 5, "second post oscillation boundary differs");
}

void invalidData() {
  std::string error;
  Curve baseline;
  baseline.keys = {key(0, 1), key(2, 5)};
  for (int fixture = 0; fixture < 7; ++fixture) {
    auto curve = baseline;
    if (fixture == 0) curve.keys[0].inWeight = -1;
    if (fixture == 1) curve.keys[0].value = std::numeric_limits<double>::quiet_NaN();
    if (fixture == 2) curve.keys[1].time = curve.keys[0].time;
    if (fixture == 3) curve.keys[1].time = -1;
    if (fixture == 4) curve.keys[0].inTangent = "unknown";
    if (fixture == 5) curve.preInfinity = "unknown";
    if (fixture == 6) curve.keys.resize(TSeExprAnimCurve::maximumKeys + 1);
    PreparedCurve prepared;
    require(!prepared.prepare(curve, error) && !error.empty(),
            "invalid curve was accepted without diagnostics");
    double result = 12;
    require(!prepared.evaluate(0, result, error), "failed preparation left an evaluable curve");
  }
  auto prepared = prepare(baseline);
  double result = 12;
  require(!prepared.evaluate(std::numeric_limits<double>::infinity(), result, error),
          "infinite time was accepted");
  baseline.postInfinity = "cycleRelative";
  prepared = prepare(baseline);
  require(!prepared.evaluate(std::numeric_limits<double>::max(), result, error),
          "inexact unbounded repetition was accepted");
  Curve maximum;
  for (std::size_t i = 0; i < TSeExprAnimCurve::maximumKeys; ++i)
    maximum.keys.push_back(key(double(i), double(i)));
  near(sample(prepare(maximum), 255.25), 255.25,
       "a valid curve at the documented key bound was rejected or changed");
}

std::string inlineSource(const std::string &time = "$t") {
  return "animCurve(" + time + ",\"constant\",\"cycleRelative\",0,\"native link\","
         "0,1,1,1,0,0,\"linear\",\"linear\",0,"
         "2,5,1,1,0,0,\"linear\",\"linear\",0)";
}

void programIntegration(TSeExpr::Backend backend) {
  using namespace TSeExpr;
  Options options;
  options.resultType = Type::Scalar; options.backend = backend;
  Program program;
  require(program.compile(inlineSource(), {{"t", Value::scalar(.5)}}, {}, options),
          "inline curve did not compile: " + program.errorMessage());
  Value value;
  require(program.evaluate(value), program.errorMessage());
  near(value.number[0], 2, "inline fractional curve value differs");
  require(program.setScalar("t", 3), "inline sample variable update failed");
  require(program.evaluate(value), program.errorMessage());
  near(value.number[0], 7, "inline infinity did not use the prepared curve");
  const auto functions = program.usedFunctions();
  require(std::find(functions.begin(), functions.end(), "animCurve") != functions.end(),
          "inline animation function was omitted from dependencies");
  require(program.compile(inlineSource() + "+" + inlineSource("$t+2"),
                          {{"t", Value::scalar(.5)}}, {}, options),
          "multiple inline curves did not compile: " + program.errorMessage());
  require(program.evaluate(value), program.errorMessage());
  near(value.number[0], 8, "inline callsites did not retain independent snapshots");

  require(program.compile("animCurve($t,\"constant\",\"constant\",0,\"\")",
                          {{"t", Value::scalar(-2.5)}}, {}, options),
          "empty inline curve did not compile: " + program.errorMessage());
  require(program.evaluate(value), program.errorMessage());
  near(value.number[0], 0, "empty inline curve returned a nonzero value");
  require(program.compile("animCurve($t,\"constant\",\"constant\",-2,\"\")",
                          {{"t", Value::scalar(-2.5)}}, {}, options),
          "finite nonzero boolean metadata lost upstream truth conversion");
  require(program.evaluate(value), program.errorMessage());
  near(value.number[0], 0, "nonzero weighted flag changed an empty curve");
  for (const std::string source : {
           "animCurve($t,\"constant\",\"constant\",1e309,\"\")",
           "animCurve($t,\"unknown\",\"constant\",0,\"\")",
           "animCurve($t,\"constant\",\"constant\",0,\"\",1)",
           "animCurve($t,\"constant\",\"constant\",$t,\"\")"})
    require(!program.compile(source, {{"t", Value::scalar(.5)}}, {}, options),
            "invalid inline metadata compiled: " + source);

  require(program.compile(inlineSource(), {{"t", Value::scalar(1)}}, {}, options),
          "inline compile did not recover after invalid metadata");
  require(program.evaluate(value), program.errorMessage());
  near(value.number[0], 3, "recovered inline evaluator returned a stale value");

  require(program.compile(inlineSource("1/($t-2)"),
                          {{"t", Value::scalar(2)}}, {}, options),
          "runtime-error fixture did not compile: " + program.errorMessage());
  require(!program.evaluate(value) && !program.errorMessage().empty(),
          "nonfinite inline query returned a false successful value");
  require(program.setScalar("t", 3) && program.evaluate(value),
          "prepared inline curve did not recover after a bad query: " + program.errorMessage());
  near(value.number[0], 3, "recovered inline query retained a stale result");
}
}  // namespace

int main() {
  try {
    spansAndTangents(); weightedBezier(); endpointTangents(); infinityModes(); invalidData();
    programIntegration(TSeExpr::Backend::Interpreter);
    if (TSeExpr::Program::backendAvailable(TSeExpr::Backend::LLVM))
      programIntegration(TSeExpr::Backend::LLVM);
    const auto builtins = TSeExpr::Program::builtinFunctions();
    require(std::none_of(builtins.begin(), builtins.end(),
                         [](const TSeExpr::FunctionInfo &entry) {
                           return entry.name == "animCurve";
                         }), "native animation extension changed the upstream builtin registry");
    const auto native = TSeExpr::Program::nativeFunctions();
    require(std::any_of(native.begin(), native.end(),
                        [](const TSeExpr::FunctionInfo &entry) {
                          return entry.name == "animCurve";
                        }), "native animation function is missing from authoring help");
    std::cout << "seexpr_animcurve: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "seexpr_animcurve: FAIL: " << error.what() << '\n';
    return 1;
  }
}
