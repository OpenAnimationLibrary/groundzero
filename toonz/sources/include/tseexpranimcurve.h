/*
 * Copyright Disney Enterprises, Inc. All rights reserved.
 *
 * Tangent regeneration follows src/SeExprEditor/CE/CETool.cpp in the
 * historical SeExpr source snapshot 87547a6fcf447d8fe2fab2fb9d0e6e99f34f03b7.
 * Modified for owned native data, bounded evaluation and independent host use.
 * Licensed under the Apache License, Version 2.0, with the following
 * modification: Section 6 Trademarks is deleted and replaced with:
 *
 * 6. Trademarks. This License does not grant permission to use the trade names,
 * trademarks, service marks, or product names of the Licensor and its
 * affiliates, except as required for reproducing the content of the NOTICE
 * file. See the distributed SeExpr license and notices for the complete terms.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Native, owned animation-curve data. This is independent of the external
// animlib binary interface. Angles are degrees and weights are handle lengths.
namespace TSeExprAnimCurve {

constexpr std::size_t maximumKeys = 512;

struct Key {
  double time = 0.0, value = 0.0;
  double inWeight = 1.0, outWeight = 1.0;
  double inAngle = 0.0, outAngle = 0.0;
  std::string inTangent = "auto", outTangent = "auto";
  bool weightsLocked = false;
};

struct Curve {
  std::string preInfinity = "constant", postInfinity = "constant";
  bool weighted = false;
  // A host refresh identifier. It never discovers files or executable code.
  std::string link;
  std::vector<Key> keys;
};

inline bool infinityType(const std::string &name) {
  return name == "constant" || name == "linear" || name == "cycle" ||
         name == "cycleRelative" || name == "oscillate";
}

inline bool tangentType(const std::string &name) {
  return name == "fixed" || name == "auto" || name == "linear" ||
         name == "flat" || name == "step" || name == "stepnext" ||
         name == "spline";
}

inline bool validate(const Curve &curve, std::string &error) {
  error.clear();
  if (!infinityType(curve.preInfinity) || !infinityType(curve.postInfinity)) {
    error = "Unsupported animation-curve infinity mode.";
    return false;
  }
  if (curve.keys.size() > maximumKeys) {
    error = "Animation curve exceeds the key limit.";
    return false;
  }
  if (curve.link.size() > 65536 || curve.link.find('\0') != std::string::npos) {
    error = "Animation-curve refresh link is invalid.";
    return false;
  }
  for (std::size_t i = 0; i < curve.keys.size(); ++i) {
    const auto &key = curve.keys[i];
    if (!std::isfinite(key.time) || !std::isfinite(key.value) ||
        !std::isfinite(key.inWeight) || !std::isfinite(key.outWeight) ||
        !std::isfinite(key.inAngle) || !std::isfinite(key.outAngle) ||
        key.inWeight < 0.0 || key.outWeight < 0.0) {
      error = "Animation keys require finite values, angles and nonnegative weights.";
      return false;
    }
    if (!tangentType(key.inTangent) || !tangentType(key.outTangent)) {
      error = "Unsupported animation-curve tangent mode.";
      return false;
    }
    if (i && (!(curve.keys[i - 1].time < key.time) ||
              !std::isfinite(key.time - curve.keys[i - 1].time))) {
      error = "Animation keys require sorted, distinct times with finite intervals.";
      return false;
    }
  }
  if (curve.keys.size() > 1 &&
      !std::isfinite(curve.keys.back().time - curve.keys.front().time)) {
    error = "Animation-curve time range exceeds the numerical limit.";
    return false;
  }
  return true;
}

namespace Detail {
constexpr double pi = 3.141592653589793238462643383279502884;
inline double radians(double angle) {
  // Reduce before conversion so every finite degree angle remains finite.
  return std::remainder(angle, 360.0) * (pi / 180.0);
}
inline bool finite(double value, double &result, std::string &error) {
  if (!std::isfinite(value)) {
    error = "Animation-curve evaluation exceeded the finite numerical range.";
    return false;
  }
  result = value;
  return true;
}
inline double blend(double first, double second, double t) {
  return (1.0 - t) * first + t * second;
}
inline double bezier(double a, double b, double c, double d, double t) {
  // De Casteljau avoids large polynomial coefficients and cancellation.
  return blend(blend(blend(a, b, t), blend(b, c, t), t),
               blend(blend(b, c, t), blend(c, d, t), t), t);
}
}  // namespace Detail

inline bool refreshTangents(Curve &curve, std::string &error) {
  if (!validate(curve, error)) return false;
  // Apply only after a complete successful regeneration, preserving the draft
  // if an extreme key configuration cannot be represented by finite doubles.
  Curve next = curve;
  for (std::size_t i = 0; i < next.keys.size(); ++i) {
    const auto &key = curve.keys[i];
    const double hPrev = i ? key.time - curve.keys[i - 1].time : 0.0;
    const double hNext = i + 1 < curve.keys.size()
                             ? curve.keys[i + 1].time - key.time : 0.0;
    const double deltaPrev = i ? key.value - curve.keys[i - 1].value : 0.0;
    const double deltaNext = i + 1 < curve.keys.size()
                                 ? curve.keys[i + 1].value - key.value : 0.0;
    double derivative = 0.0;
    if (hPrev || hNext) {
      // Same central/one-sided difference as the historical CE editor.
      derivative = (deltaPrev + deltaNext) / (hPrev + hNext);
      if (!std::isfinite(derivative)) {
        error = "Animation-curve tangent exceeds the finite numerical range.";
        return false;
      }
    }
    double automatic = derivative;
    if (!hPrev || !deltaPrev) automatic = 0.0;
    else {
      const double slope = deltaPrev / hPrev;
      automatic = std::clamp(automatic / slope, 0.0, 3.0) * slope;
    }
    if (!hNext || !deltaNext) automatic = 0.0;
    else {
      const double slope = deltaNext / hNext;
      automatic = std::clamp(automatic / slope, 0.0, 3.0) * slope;
    }
    auto update = [&](bool incoming) {
      auto &out = next.keys[i];
      const auto &mode = incoming ? key.inTangent : key.outTangent;
      if (mode == "fixed") return true;
      double slope = 0.0;
      const double interval = incoming ? hPrev : hNext;
      if (mode == "spline") slope = derivative;
      else if (mode == "auto") slope = automatic;
      else if (mode == "linear") {
        if (interval) slope = (incoming ? deltaPrev : deltaNext) / interval;
        // The outward endpoint tangent defines linear infinity. Reuse its
        // only adjacent segment rather than replacing that slope with zero.
        else if (incoming && hNext) slope = deltaNext / hNext;
        else if (!incoming && hPrev) slope = deltaPrev / hPrev;
      }
      const double angle = std::atan(slope);
      const double weight = curve.weighted && interval
                                ? (interval / 3.0) / std::cos(angle) : 1.0;
      if (!std::isfinite(slope) || !std::isfinite(angle) || !std::isfinite(weight))
        return false;
      (incoming ? out.inAngle : out.outAngle) = angle * (180.0 / Detail::pi);
      (incoming ? out.inWeight : out.outWeight) = weight;
      return true;
    };
    if (!update(true) || !update(false)) {
      error = "Animation-curve tangent exceeds the finite numerical range.";
      return false;
    }
  }
  curve = std::move(next);
  return true;
}

// Prepare once per owned render context. Evaluation neither reparses source
// nor mutates keys and performs at most one binary search and 64 Bézier steps.
class PreparedCurve {
public:
  bool prepare(const Curve &curve, std::string &error) {
    m_valid = false;
    Curve next = curve;
    if (!refreshTangents(next, error)) return false;
    for (const auto &key : next.keys) {
      if (!std::isfinite(std::tan(Detail::radians(key.inAngle))) ||
          !std::isfinite(std::tan(Detail::radians(key.outAngle)))) {
        error = "Animation-curve tangent cannot be represented.";
        return false;
      }
    }
    m_curve = std::move(next);
    m_valid = true;
    return true;
  }

  bool evaluate(double time, double &result, std::string &error) const {
    error.clear();
    if (!m_valid || !std::isfinite(time)) {
      error = "Animation curve is unprepared or its sample time is nonfinite.";
      return false;
    }
    const auto &keys = m_curve.keys;
    if (keys.empty()) { result = 0.0; return true; }
    if (keys.size() == 1) { result = keys.front().value; return true; }
    double offset = 0.0;
    if (time < keys.front().time || time > keys.back().time) {
      const bool before = time < keys.front().time;
      const auto &endpoint = before ? keys.front() : keys.back();
      const auto &mode = before ? m_curve.preInfinity : m_curve.postInfinity;
      if (mode == "constant") { result = endpoint.value; return true; }
      if (mode == "linear") {
        const double slope = std::tan(Detail::radians(
            before ? endpoint.inAngle : endpoint.outAngle));
        return Detail::finite(endpoint.value + (time - endpoint.time) * slope,
                              result, error);
      }
      const double duration = keys.back().time - keys.front().time;
      const double distance = before ? keys.front().time - time
                                     : time - keys.back().time;
      const double ratio = distance / duration;
      if (!std::isfinite(distance) || !std::isfinite(ratio) ||
          ratio >= 4503599627370495.0) {
        error = "Animation-curve repetition exceeds the exact cycle range.";
        return false;
      }
      const double complete = std::floor(ratio), fraction = ratio - complete;
      const double cycles = complete + 1.0;
      const double remainder = fraction * duration;
      if (mode == "oscillate") {
        const bool odd = std::fmod(cycles, 2.0) != 0.0;
        time = before == odd ? keys.front().time + remainder
                             : keys.back().time - remainder;
      } else {
        time = before ? keys.back().time - remainder
                      : keys.front().time + remainder;
        if (mode == "cycleRelative")
          offset = (before ? -cycles : cycles) *
                   (keys.back().value - keys.front().value);
      }
    }
    const auto next = std::lower_bound(keys.begin(), keys.end(), time,
        [](const Key &key, double sample) { return key.time < sample; });
    if (next == keys.end())
      return Detail::finite(keys.back().value + offset, result, error);
    if (next == keys.begin() || next->time == time)
      return Detail::finite(next->value + offset, result, error);
    const auto &first = *(next - 1), &last = *next;
    if (first.outTangent == "step")
      return Detail::finite(first.value + offset, result, error);
    if (first.outTangent == "stepnext")
      return Detail::finite(last.value + offset, result, error);
    const double duration = last.time - first.time;
    const double u = (time - first.time) / duration;
    double value;
    if (!m_curve.weighted) {
      const double out = duration * std::tan(Detail::radians(first.outAngle));
      const double in = duration * std::tan(Detail::radians(last.inAngle));
      // Equivalent cubic Bézier form of Hermite interpolation.
      value = Detail::bezier(first.value, first.value + out / 3.0,
                            last.value - in / 3.0, last.value, u);
    } else {
      const double outAngle = Detail::radians(first.outAngle);
      const double inAngle = Detail::radians(last.inAngle);
      double outX = first.outWeight * std::cos(outAngle);
      double outY = first.outWeight * std::sin(outAngle);
      double inX = last.inWeight * std::cos(inAngle);
      double inY = last.inWeight * std::sin(inAngle);
      // Match native OT's bounded time-handle convention. Stored metadata is
      // retained; only the evaluated span clips handles to its time interval.
      if (outX < 0.0) outX = 0.0;
      if (inX < 0.0) inX = 0.0;
      if (outX > duration) { outY *= duration / outX; outX = duration; }
      if (inX > duration) { inY *= duration / inX; inX = duration; }
      const double x1 = outX / duration, x2 = 1.0 - inX / duration;
      double low = 0.0, high = 1.0;
      for (int iteration = 0; iteration < 64; ++iteration) {
        const double middle = (low + high) * 0.5;
        if (Detail::bezier(0.0, x1, x2, 1.0, middle) < u) low = middle;
        else high = middle;
      }
      value = Detail::bezier(first.value, first.value + outY,
                            last.value - inY, last.value, (low + high) * 0.5);
    }
    return Detail::finite(value + offset, result, error);
  }

private:
  Curve m_curve;
  bool m_valid = false;
};

inline bool evaluate(const Curve &curve, double time, double &result,
                     std::string &error) {
  PreparedCurve prepared;
  return prepared.prepare(curve, error) && prepared.evaluate(time, result, error);
}
}  // namespace TSeExprAnimCurve
