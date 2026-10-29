// Standalone checks of the speed sensitive width maths behind the vector
// Brush's Fluid Width option; no GUI or application initialization.
// g++ -std=c++17 -I../../include fluidwidth_test.cpp -o fluidwidth_test
#include "tools/fluidwidth.h"
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

using namespace FluidWidth;

namespace {

void check(bool condition, const char *what) {
  if (condition) return;
  std::cerr << "Check failed: " << what << '\n';
  std::exit(1);
}

bool closeTo(double a, double b, double tolerance) {
  return std::fabs(a - b) <= tolerance;
}

bool inRange(double f) {
  return std::isfinite(f) && f >= kMinFactor && f <= 1.0;
}

// Deterministic pseudo random numbers in [0, 1).
struct Rng {
  std::uint32_t state;
  explicit Rng(std::uint32_t seed) : state(seed) {}
  double next() {
    state = state * 1664525u + 1013904223u;
    return (state >> 8) / 16777216.0;
  }
};

// count samples, 'dt' seconds apart, 'step' pixels apart.
std::vector<Sample> steady(int count, double dt, double step) {
  std::vector<Sample> samples;
  for (int i = 0; i < count; ++i) samples.push_back(Sample(i * dt, i * step));
  return samples;
}

// 30 slow samples (100 px/s) followed by 30 fast ones (3000 px/s), 10 ms apart.
std::vector<Sample> slowThenFast() {
  std::vector<Sample> samples;
  double length = 0;
  for (int i = 0; i < 60; ++i) {
    if (i > 0) length += i < 30 ? 1.0 : 30.0;
    samples.push_back(Sample(i * 0.01, length));
  }
  return samples;
}

void testStrengthZeroIsIdentity() {
  std::vector<Sample> samples = steady(100, 0.01, 30);
  samples.push_back(Sample(std::numeric_limits<double>::quiet_NaN(), 5));
  samples.push_back(Sample(1e308, std::numeric_limits<double>::infinity()));
  samples.push_back(Sample(-3, -400));
  samples.push_back(Sample(-3, -400));

  const double nan          = std::numeric_limits<double>::quiet_NaN();
  const double strengths[]  = {0.0, -10.0, nan};
  const double smoothings[] = {0.0, 0.25, 1.0};
  for (double strength : strengths)
    for (double smoothing : smoothings) {
      std::vector<double> factors =
          computeFactors(samples, Params(strength, smoothing));
      check(factors.size() == samples.size(), "one factor per sample");
      for (double f : factors) check(f == 1.0, "strength 0 gives exactly 1.0");
    }
  check(factorForSpeed(5000, 0) == 1.0, "factorForSpeed with strength 0");
  check(factorForSpeed(0, 100) == 1.0, "a pen at rest keeps the full width");
  check(computeFactors(std::vector<Sample>(), Params(50)).empty(),
        "no samples, no factors");
}

void testFirstSampleIsFullWidth() {
  for (double strength : {1.0, 50.0, 100.0}) {
    std::vector<double> f =
        computeFactors(steady(5, 0.01, 40), Params(strength));
    check(f[0] == 1.0, "the first sample has no speed yet");
  }
}

void testConstantSpeed() {
  // 500 px/s, no smoothing: constant from the second sample on.
  std::vector<Sample> samples = steady(200, 0.01, 5);
  std::vector<double> f       = computeFactors(samples, Params(60, 0.0));
  const double expected       = factorForSpeed(500, 60);
  check(expected < 1.0 && expected > kMinFactor, "expected factor is a width");
  for (std::size_t i = 1; i < f.size(); ++i)
    check(closeTo(f[i], expected, 1e-9), "constant speed, constant factor");

  // With smoothing the factor eases towards the same value and stays there.
  f = computeFactors(samples, Params(60, kDefaultSmoothing));
  for (std::size_t i = 2; i < f.size(); ++i)
    check(f[i] <= f[i - 1] + 1e-9, "the factor only falls towards the speed");
  check(closeTo(f.back(), expected, 1e-9), "smoothed factor converges");

  // A finer time grid (1 kHz) gives the same speed.
  f = computeFactors(steady(2000, 0.001, 0.5), Params(60, 0.0));
  check(closeTo(f.back(), expected, 1e-6), "same speed at 1 kHz, same factor");
}

void testFastIsThinnerThanSlow() {
  std::vector<double> f = computeFactors(slowThenFast(), Params(100, 0.0));
  const double slow     = f[29];
  const double fast     = f[40];
  check(closeTo(slow, factorForSpeed(100, 100), 1e-9), "slow factor");
  check(closeTo(fast, factorForSpeed(3000, 100), 1e-9), "fast factor");
  check(fast < slow - 0.3, "a fast segment is clearly thinner than a slow one");

  // The curve itself is monotone in speed and in strength.
  double previous = 2.0;
  for (double speed = 0; speed <= 30000; speed += 7.5) {
    double now = factorForSpeed(speed, 80);
    check(now <= previous, "factor never grows with speed");
    previous = now;
  }
  for (double speed : {50.0, 600.0, 3000.0, 20000.0}) {
    double last = 2.0;
    for (double strength = 0; strength <= 100; strength += 10) {
      double now = factorForSpeed(speed, strength);
      check(now <= last, "factor never grows with strength");
      last = now;
    }
  }
}

void testFactorIsClamped() {
  // Full strength at an absurd speed stays above the minimum fraction and
  // reaches about the documented 1 - 0.8.
  std::vector<Sample> absurd;
  absurd.push_back(Sample(0, 0));
  absurd.push_back(Sample(1, 1e300));
  absurd.push_back(Sample(2, 2e300));
  std::vector<double> f = computeFactors(absurd, Params(100, 0.0));
  for (double v : f) check(inRange(v), "absurd speed stays in range");
  check(f[2] > 0.2 && f[2] < 0.26, "fast end of the curve is about 0.2-0.25");
  check(factorForSpeed(1e300, 100) >= kMinFactor, "huge speed");
  check(factorForSpeed(std::numeric_limits<double>::infinity(), 100) >=
            kMinFactor,
        "infinite speed");
  check(factorForSpeed(-50, 100) == 1.0, "negative speed counts as rest");
  check(factorForSpeed(std::numeric_limits<double>::quiet_NaN(), 100) == 1.0,
        "NaN speed counts as rest");

  // Out-of-range parameters are clamped, not rejected.
  std::vector<Sample> samples = slowThenFast();
  check(computeFactors(samples, Params(250, 5)) ==
            computeFactors(samples, Params(100, 1)),
        "strength and smoothing are clamped to their ranges");
  check(computeFactors(samples, Params(100, -1)) ==
            computeFactors(samples, Params(100, 0)),
        "negative smoothing counts as 0");

  // Pseudo random strokes, with repeated, backward and huge steps.
  Rng rng(12345);
  double time = 0, length = 0;
  std::vector<Sample> noisy;
  for (int i = 0; i < 5000; ++i) {
    double r = rng.next();
    if (r < 0.15) {
      // same time stamp as the sample before
    } else if (r < 0.2) {
      time -= rng.next();
    } else {
      time += rng.next() * 0.05;
    }
    if (rng.next() < 0.1)
      length -= 5 * rng.next();
    else
      length += rng.next() * 400;
    noisy.push_back(Sample(time, length));
  }
  for (double strength : {1.0, 25.0, 50.0, 100.0, 250.0})
    for (double smoothing : {0.0, 0.25, 1.0}) {
      std::vector<double> factors =
          computeFactors(noisy, Params(strength, smoothing));
      for (double v : factors)
        check(inRange(v), "random stroke stays in range");
    }
}

void testZeroAndNegativeTimeDelta() {
  Params p(100, kDefaultSmoothing);

  // Nothing but equal time stamps: no speed can be measured, no division.
  std::vector<Sample> same;
  for (int i = 0; i < 4; ++i) same.push_back(Sample(0, i * 10));
  for (double v : computeFactors(same, p))
    check(v == 1.0, "equal time stamps leave the width alone");

  Filter filter;
  filter.push(0, 0, p);
  double measured = filter.push(0.01, 10, p);
  check(measured < 1.0 && inRange(measured), "a speed was measured");
  check(filter.push(0.01, 12, p) == measured,
        "zero time delta leaves the smoothed speed alone");
  check(filter.push(0.005, 13, p) == measured,
        "time going backwards leaves the smoothed speed alone");
  check(filter.push(0.004, 5, p) == measured,
        "length going backwards leaves the smoothed speed alone");

  // The same without smoothing: still finite and in range.
  Params raw(100, 0.0);
  std::vector<Sample> odd;
  odd.push_back(Sample(0, 0));
  odd.push_back(Sample(0, 10));
  odd.push_back(Sample(0, 20));
  odd.push_back(Sample(0.01, 30));
  odd.push_back(Sample(0.01, 30));
  odd.push_back(Sample(0.005, 31));
  odd.push_back(Sample(0.005, 31));
  odd.push_back(Sample(0.02, 40));
  for (double v : computeFactors(odd, raw))
    check(inRange(v), "zero and negative deltas give finite factors");

  // Non-finite samples repeat the previous factor.
  Filter nf;
  nf.push(0, 0, p);
  double before    = nf.push(0.02, 20, p);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  check(nf.push(nan, 25, p) == before, "NaN time repeats the factor");
  check(nf.push(0.03, inf, p) == before, "infinite length repeats the factor");
  check(inRange(nf.push(0.04, 30, p)), "the filter carries on afterwards");
  Filter first;
  check(first.push(nan, nan, p) == 1.0, "a NaN first sample is full width");

  // Huge time and length steps do not overflow into NaN.
  Filter huge;
  huge.push(-1e308, -1e308, p);
  check(inRange(huge.push(1e308, 1e308, p)), "overflowing time delta");
  check(inRange(huge.push(1e308, 1e308, p)), "carries on after overflow");
}

void testSmoothingDelaysAJump() {
  std::vector<Sample> samples = slowThenFast();
  std::vector<double> instant = computeFactors(samples, Params(100, 0.0));
  std::vector<double> smooth  = computeFactors(samples, Params(100, 1.0));
  std::vector<double> usual =
      computeFactors(samples, Params(100, kDefaultSmoothing));

  // Sample 30 is the first fast one.
  check(closeTo(instant[30], factorForSpeed(3000, 100), 1e-9),
        "without smoothing the width follows at once");
  check(smooth[30] < smooth[29], "smoothing still starts to react");
  check(smooth[30] > instant[30] + 0.1, "but a jump is delayed");
  check(usual[30] > instant[30] && usual[30] < smooth[30],
        "less smoothing reacts sooner than more smoothing");
  check(smooth[59] > instant[59], "the delay lasts for a while");
  for (int i = 31; i < 60; ++i)
    check(smooth[i] <= smooth[i - 1] + 1e-12, "and the width falls steadily");

  // Given enough time the smoothed width reaches the fast value.
  std::vector<Sample> longer = slowThenFast();
  for (int i = 60; i < 400; ++i)
    longer.push_back(Sample(i * 0.01, longer.back().length + 30.0));
  std::vector<double> settled = computeFactors(longer, Params(100, 1.0));
  check(closeTo(settled.back(), factorForSpeed(3000, 100), 1e-6),
        "the smoothed width settles at the fast value");

  // Noise on a steady stroke moves the smoothed width less than the raw one.
  Rng rng(777);
  std::vector<Sample> jittery;
  double time = 0, length = 0;
  for (int i = 0; i < 600; ++i) {
    time += 0.008 + 0.004 * rng.next();
    length += 2.0 + 6.0 * rng.next();
    jittery.push_back(Sample(time, length));
  }
  std::vector<double> rawNoise  = computeFactors(jittery, Params(100, 0.0));
  std::vector<double> calmNoise = computeFactors(jittery, Params(100, 1.0));
  double rawSwing = 0, calmSwing = 0;
  for (std::size_t i = 300; i < jittery.size(); ++i) {
    rawSwing += std::fabs(rawNoise[i] - rawNoise[i - 1]);
    calmSwing += std::fabs(calmNoise[i] - calmNoise[i - 1]);
  }
  check(calmSwing < rawSwing * 0.5, "smoothing calms a noisy stroke");
}

void testBurstDoesNotSpike() {
  Params p(100, kDefaultSmoothing);
  Filter filter;
  double steadyFactor = 1.0;
  for (int i = 0; i < 50; ++i) steadyFactor = filter.push(i * 0.01, i * 1.0, p);
  check(steadyFactor < 1.0, "steady stroke has a factor");

  // 20 events with the same time stamp (up to 1 ns apart), 3 px each.
  double time = 49 * 0.01, length = 49.0;
  for (int k = 1; k <= 20; ++k) {
    double f = filter.push(time + k * 1e-9, length + 3.0 * k, p);
    check(closeTo(f, steadyFactor, 1e-4), "a burst of events does not spike");
  }
  // The next ordinary event is back to the pen's real speed.
  double next = filter.push(time + 0.01, length + 60.0 + 1.0, p);
  check(inRange(next), "factor after the burst");
}

void testLongStroke() {
  Params p(70, kDefaultSmoothing);
  Filter filter;
  double f        = 1.0;
  double length   = 5e8;
  const int count = 500000;
  for (int i = 0; i < count; ++i) {
    length += i % 7 == 0 ? 3.0 : 0.5;
    f = filter.push(1e9 + i * 0.001, length, p);
    if (i % 50000 == 0) check(inRange(f), "long stroke stays in range");
  }
  check(inRange(f), "end of a long stroke");
  check(filter.size() == (std::size_t)count, "one record per sample");

  // 100000 samples sharing a single time stamp: nothing to measure, and the
  // lookup of the measuring window must not get slower and slower.
  Filter frozen;
  for (int i = 0; i < 100000; ++i)
    check(frozen.push(5.0, i * 2.0, p) == 1.0, "frozen clock keeps full width");
}

void testTakeBackAndRepeat() {
  Rng rng(4242);
  std::vector<Sample> samples;
  double time = 0, length = 0;
  for (int i = 0; i < 600; ++i) {
    time += 0.002 + 0.02 * rng.next();
    length += 25.0 * rng.next();
    samples.push_back(Sample(time, length));
  }
  Params p(85, 0.4);
  std::vector<double> expected = computeFactors(samples, p);
  check(computeFactors(samples, p) == expected, "same input, same output");

  // Push everything, but now and then push a few preview samples and take them
  // back again, like the tool does when the input pipeline revises its tail.
  Filter filter;
  Rng junk(99);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    double f = filter.push(samples[i].time, samples[i].length, p);
    check(f == expected[i], "result is the same after take backs");
    if (i % 10 == 9) {
      for (int k = 1; k <= 5; ++k)
        filter.push(samples[i].time + 0.001 * k,
                    samples[i].length + 100.0 * junk.next(), p);
      for (int k = 0; k < 5; ++k) filter.pop();
      check(filter.size() == i + 1, "pop takes back exactly what was pushed");
    }
  }

  // Taking back the tail and pushing it again gives the same factors.
  for (int k = 0; k < 30; ++k) filter.pop();
  for (std::size_t i = samples.size() - 30; i < samples.size(); ++i)
    check(filter.push(samples[i].time, samples[i].length, p) == expected[i],
          "re-pushed tail matches");

  Filter blank;
  blank.pop();
  check(blank.empty() && blank.size() == 0 && blank.speed() == 0.0,
        "pop on an empty history is harmless");
}

}  // namespace

int main() {
  testStrengthZeroIsIdentity();
  testFirstSampleIsFullWidth();
  testConstantSpeed();
  testFastIsThinnerThanSlow();
  testFactorIsClamped();
  testZeroAndNegativeTimeDelta();
  testSmoothingDelaysAJump();
  testBurstDoesNotSpike();
  testLongStroke();
  testTakeBackAndRepeat();
  std::cout << "Fluid width checks passed\n";
  return 0;
}
