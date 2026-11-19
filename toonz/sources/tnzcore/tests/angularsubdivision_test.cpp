// SPDX-License-Identifier: BSD-3-Clause

// The number of steps used to draw a round cap or join of a stroke. A stroke
// thinner than the pixel size gives a radius for which the step angle used to
// be acos of a value below -1, and the step count was a NaN converted to int.

#include "tellipticbrushP.h"

#include <cfloat>
#include <cmath>
#include <limits>
#include <iostream>
#include <stdexcept>

namespace {

const double kPi = 3.14159265358979323846;

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

int stepsFor(double radius, double angle, double err) {
  int steps = -12345;
  tellipticbrush::buildAngularSubdivision(radius, angle, err, steps);
  return steps;
}

// The formula the function has always used, for radii where it is defined.
int referenceSteps(double radius, double angle, double err) {
  return int(std::ceil(std::fabs(angle) / std::acos(1.0 - err / radius)));
}

void ordinaryRadiiKeepTheirStepCount() {
  require(stepsFor(10.0, kPi, 0.5) == 10, "radius 10, error 0.5, half a turn");
  require(stepsFor(10.0, kPi / 2, 0.5) == 5,
          "radius 10, error 0.5, quarter turn");
  require(stepsFor(100.0, kPi, 1.0) == 23, "radius 100, error 1, half a turn");
  require(stepsFor(1.0, kPi, 0.5) == 3, "radius 1, error 0.5, half a turn");
  require(stepsFor(10.0, 0.0, 0.5) == 0, "no angle needs no step");
  const double radii[]  = {0.3, 0.51, 1.0, 2.5, 10.0, 77.7, 255.0, 4000.0};
  const double errors[] = {0.05, 0.25, 0.5, 1.0, 2.0};
  const double angles[] = {0.0, 0.01, 1.0, kPi / 2, kPi, -kPi, 2 * kPi};
  for (double radius : radii)
    for (double err : errors) {
      if (err / radius > 1.9) continue;
      for (double angle : angles)
        require(
            stepsFor(radius, angle, err) == referenceSteps(radius, angle, err),
            "an ordinary radius gives the step count of the acos formula");
    }
}

void thinStrokesGetAtLeastOneStep() {
  const double err     = 0.5;
  const double radii[] = {0.0, 1e-9, 0.05, 0.2, 0.25 - 1e-9, 0.25};
  for (double radius : radii) {
    require(stepsFor(radius, kPi, err) == 1,
            "a radius up to half the error needs one step");
    require(stepsFor(radius, -kPi / 3, err) == 1,
            "the direction of the angle does not matter");
    require(stepsFor(radius, 0.0, err) == 0,
            "a thin stroke with no angle needs no step");
    require(stepsFor(radius, 2 * kPi, err) == 2,
            "a full turn needs two half-turn steps");
  }
  require(stepsFor(0.0, kPi, 0.0) == 1,
          "a zero radius with a zero error needs one step");
}

void hugeRadiiHaveABoundedStepCount() {
  const int cap        = tellipticbrush::maxAngularSteps;
  const double radii[] = {1e9, 1e12, 1e300, DBL_MAX,
                          std::numeric_limits<double>::infinity()};
  for (double radius : radii) {
    require(stepsFor(radius, kPi, 0.5) == cap,
            "a huge radius is limited to the largest step count");
    require(stepsFor(radius, -2 * kPi, 0.5) == cap,
            "the limit applies to either direction");
    require(stepsFor(radius, 0.0, 0.5) == 0,
            "a huge radius with no angle needs no step");
    require(stepsFor(radius, kPi, 0.0) == cap,
            "a huge radius with a zero error is limited");
  }
  // The largest radius that is not limited keeps the exact count.
  require(stepsFor(1e6, kPi, 0.5) == referenceSteps(1e6, kPi, 0.5),
          "a large radius below the limit keeps the formula's count");
  require(referenceSteps(1e6, kPi, 0.5) < cap,
          "the radius above is below the limit");
  require(stepsFor(std::nan(""), kPi, 0.5) == 1,
          "a radius that is not a number takes half-turn steps");
  require(stepsFor(-5.0, kPi, 0.5) == 0, "a negative radius gives no step");
}

}  // namespace

int main() {
  try {
    ordinaryRadiiKeepTheirStepCount();
    thinStrokesGetAtLeastOneStep();
    hugeRadiiHaveABoundedStepCount();
  } catch (const std::exception& error) {
    std::cerr << "angularsubdivision: FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "angularsubdivision: PASS\n";
  return 0;
}
