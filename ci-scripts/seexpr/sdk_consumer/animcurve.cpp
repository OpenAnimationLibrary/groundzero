#include <GroundzeroSeExpr/tseexpranimcurve.h>

#include <cmath>
#include <iostream>

int main() {
  TSeExprAnimCurve::Curve curve;
  curve.preInfinity = "linear";
  curve.postInfinity = "cycleRelative";
  TSeExprAnimCurve::Key first, last;
  first.time = 1; first.value = 2;
  last.time = 5; last.value = 10;
  first.inTangent = first.outTangent = "linear";
  last.inTangent = last.outTangent = "linear";
  curve.keys = {first, last};
  TSeExprAnimCurve::PreparedCurve prepared;
  std::string error;
  if (!prepared.prepare(curve, error)) return 1;
  // Exercise the installed, self-contained header without source-tree includes.
  const double times[] = {-1, 2.5, 8.5};
  const double expected[] = {-2, 5, 17};
  for (int i = 0; i < 3; ++i) {
    double value = 0;
    if (!prepared.evaluate(times[i], value, error) ||
        std::abs(value - expected[i]) > 1e-10) return 1;
  }
  std::cout << "Installed animation-curve SDK: PASS\n";
  return 0;
}
