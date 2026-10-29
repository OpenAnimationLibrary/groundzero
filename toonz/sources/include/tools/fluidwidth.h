#pragma once

#ifndef FLUIDWIDTH_INCLUDED
#define FLUIDWIDTH_INCLUDED

// Speed sensitive stroke width ("Fluid Width") for the vector Brush tool.
//
// Pure maths with no GUI, Qt or Toonz dependency, so it can be unit tested on
// its own. The tool feeds it the samples of a track (time in seconds and
// travelled length in pixels), one at a time, and gets back a width factor for
// each sample:
//
//   - the factor is 1.0 where the pen moves slowly (or the speed is unknown)
//     and falls towards 1 - 0.8 * strength / 100 where the pen moves fast;
//   - it is always inside [kMinFactor, 1.0], so a stroke can never vanish or
//     become wider than the normal (pressure based) width;
//   - strength 0 gives exactly 1.0 for every sample: the feature is off.
//
// The speed of a sample is the length travelled over a window of at least
// kMinTimeStep seconds ending at that sample (so a burst of mouse events with
// nearly equal time stamps neither divides by zero nor makes a spike), clamped
// to kMaxSpeed, and then smoothed with an exponential filter whose time
// constant follows the "smoothing" parameter, so the width does not jitter
// with noisy input. The filter starts from rest, so the first samples of a
// stroke are thick and the width eases towards the speed of the pen.
//
// The Filter keeps a small record per sample, so a caller that has to take
// samples back (the tool pops preview points) can pop and push again.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace FluidWidth {

// Slider range of the option.
constexpr double kMaxStrength = 100.0;

// Fraction of the width that is removed at full strength when the pen is
// infinitely fast.
constexpr double kMaxReduction = 0.8;

// The factor never goes below this fraction of the normal width.
constexpr double kMinFactor = 0.1;

// Speed (pixels per second) at which half of the maximum reduction applies.
constexpr double kReferenceSpeed = 600.0;

// Speeds above this (pixels per second) are treated as this fast.
constexpr double kMaxSpeed = 10000.0;

// Smallest time window (seconds) a speed is measured over.
constexpr double kMinTimeStep = 0.004;

// Time constant (seconds) of the speed filter at smoothing 1.
constexpr double kMaxTimeConstant = 0.25;

// Smoothing used by the tool: a time constant of about 60 ms.
constexpr double kDefaultSmoothing = 0.25;

struct Params {
  double strength;   //!< 0..100, 0 = off (values outside are clamped)
  double smoothing;  //!< 0..1, 0 = follow the measured speed, 1 = slowest

  explicit Params(double strength = 0.0, double smoothing = kDefaultSmoothing)
      : strength(strength), smoothing(smoothing) {}
};

struct Sample {
  double time;    //!< seconds
  double length;  //!< travelled length in pixels, grows along the stroke

  Sample(double time = 0.0, double length = 0.0) : time(time), length(length) {}
};

//-----------------------------------------------------------------------------

// Clamps to 0..kMaxStrength. NaN counts as 0.
inline double clampStrength(double strength) {
  if (!(strength > 0.0)) return 0.0;
  return strength < kMaxStrength ? strength : kMaxStrength;
}

// Clamps to 0..1. NaN counts as 0.
inline double clampSmoothing(double smoothing) {
  if (!(smoothing > 0.0)) return 0.0;
  return smoothing < 1.0 ? smoothing : 1.0;
}

// The width factor for a (smoothed) speed in pixels per second. Monotone:
// a faster pen never gives a wider stroke. Exactly 1.0 when strength is 0 or
// the speed is 0; always inside [kMinFactor, 1.0], also for negative, NaN or
// infinite speeds.
inline double factorForSpeed(double speed, double strength) {
  const double s = clampStrength(strength) / kMaxStrength;
  if (s <= 0.0) return 1.0;
  const double v = speed > 0.0 ? std::min(speed, kMaxSpeed) : 0.0;
  const double x = v / kReferenceSpeed;
  const double g = x / (1.0 + x);  // 0 at rest, towards 1 when fast
  const double f = 1.0 - kMaxReduction * s * g;
  return std::min(1.0, std::max(kMinFactor, f));
}

//-----------------------------------------------------------------------------

// The history of one stroke: one entry per sample, so the caller can take the
// last samples back (pop) when the input pipeline replaces its preview points,
// and push the new ones; the result is the same as if the stroke had been fed
// in one go.
class Filter {
  struct Entry {
    double time;         // seconds
    double length;       // pixels
    double speed;        // smoothed speed, pixels per second
    std::size_t anchor;  // newest entry that is at least kMinTimeStep older
  };
  std::vector<Entry> m_entries;

public:
  Filter() {}

  std::size_t size() const { return m_entries.size(); }
  bool empty() const { return m_entries.empty(); }
  void clear() { m_entries.clear(); }

  // Smoothed speed after the last sample (pixels per second).
  double speed() const {
    return m_entries.empty() ? 0.0 : m_entries.back().speed;
  }

  // Takes the last sample back. Does nothing on an empty history.
  void pop() {
    if (!m_entries.empty()) m_entries.pop_back();
  }

  // Adds the next sample of the stroke and returns its width factor.
  //
  //  - strength 0 gives exactly 1.0 (the sample is still recorded, so push and
  //    pop stay paired);
  //  - the speed is the length travelled since the newest earlier sample that
  //    is at least kMinTimeStep older, divided by the time between them, so
  //    samples that share (nearly) the same time stamp never divide by zero
  //    and cannot make a spike;
  //  - that speed is clamped to kMaxSpeed and smoothed with an exponential
  //    filter weighted by the time since the previous sample;
  //  - a sample whose time or length is not finite repeats the previous one,
  //    and a time or length that goes backwards restarts the measurement.
  double push(double time, double length, const Params &params) {
    const double strength = clampStrength(params.strength);
    const std::size_t n   = m_entries.size();

    Entry e;
    e.time   = 0.0;
    e.length = 0.0;
    e.speed  = 0.0;
    e.anchor = n;

    if (!std::isfinite(time) || !std::isfinite(length)) {
      if (n > 0) e = m_entries.back();
      m_entries.push_back(e);
      return factorForSpeed(e.speed, strength);
    }

    e.time   = time;
    e.length = length;

    if (n > 0) {
      const Entry &prev = m_entries.back();
      e.speed           = prev.speed;
      const double dt   = time - prev.time;
      if (dt < 0.0 || length < prev.length) {
        // measure again from here, keep the speed
        e.anchor = n;
      } else {
        std::size_t j = prev.anchor;
        while (j + 1 < n && m_entries[j + 1].time <= time - kMinTimeStep) ++j;
        e.anchor = j;

        const double window = time - m_entries[j].time;
        if (window >= kMinTimeStep) {
          const double travelled = length - m_entries[j].length;
          double v               = std::max(travelled, 0.0) / window;
          if (!std::isfinite(v)) v = v > 0.0 ? kMaxSpeed : 0.0;
          v = std::min(v, kMaxSpeed);
          const double tau =
              clampSmoothing(params.smoothing) * kMaxTimeConstant;
          const double alpha = tau > 0.0 ? 1.0 - std::exp(-dt / tau) : 1.0;
          e.speed += alpha * (v - e.speed);
        }
      }
    }

    m_entries.push_back(e);
    return factorForSpeed(e.speed, strength);
  }
};

// All factors of a whole sequence of samples. The same as pushing each sample
// in turn.
inline std::vector<double> computeFactors(const std::vector<Sample> &samples,
                                          const Params &params) {
  std::vector<double> factors;
  factors.reserve(samples.size());
  Filter filter;
  for (std::size_t i = 0; i < samples.size(); ++i)
    factors.push_back(filter.push(samples[i].time, samples[i].length, params));
  return factors;
}

}  // namespace FluidWidth

#endif  // FLUIDWIDTH_INCLUDED
