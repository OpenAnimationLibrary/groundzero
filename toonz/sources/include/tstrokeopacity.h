#pragma once
#include "tgeometry.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

// Piecewise linear opacity envelope in normalized arc length. Empty is the
// identity envelope. Independent of control-point sampling and stroke style.
namespace StrokeOpacity {
using Profile = std::vector<TPointD>;
constexpr size_t MaxKnots = 65536;
inline bool valid(const Profile &profile) {
  if (profile.empty()) return true;
  if (profile.size() < 2 || profile.size() > MaxKnots ||
      profile.front().x != 0. || profile.back().x != 1.) return false;
  double previous = -1.;
  bool duplicate = false;
  for (const auto &knot : profile) {
    if (!std::isfinite(knot.x) || !std::isfinite(knot.y) ||
        knot.x < previous || (duplicate && knot.x == previous) || knot.x < 0. || knot.x > 1. ||
        knot.y < 0. || knot.y > 1.) return false;
    duplicate = knot.x == previous;
    previous = knot.x;
  }
  return true;
}
inline double evaluate(const Profile &profile, double position) {
  if (profile.empty()) return 1.;
  // Endpoint caps use the outermost knot, so reversing a stroke exchanges
  // its two cap opacities exactly even when an endpoint contains a step.
  if (position <= 0.) return profile.front().y;
  if (position >= 1.) return profile.back().y;
  auto next = std::upper_bound(profile.begin(), profile.end(), position,
      [](double x, const TPointD &point) { return x < point.x; });
  if (next == profile.begin()) return next->y;
  if (next == profile.end()) return profile.back().y;
  const auto &a = *(next - 1), &b = *next;
  const double t = (position - a.x) / (b.x - a.x);
  return a.y * (1. - t) + b.y * t;
}
// At a duplicated position, the first value is the left-hand limit and the
// second is the right-hand value. This preserves joins with different opacity
// exactly, rather than hiding a discontinuity in a precision-dependent epsilon.
inline double evaluateLeft(const Profile &profile, double position) {
  auto at = std::lower_bound(profile.begin(), profile.end(), position,
      [](const TPointD &point, double x) { return point.x < x; });
  return at != profile.end() && at->x == position ? at->y : evaluate(profile, position);
}
inline double evaluateRight(const Profile &profile, double position) {
  if (position == 0. && !profile.empty()) {
    auto next = std::upper_bound(profile.begin(), profile.end(), position,
        [](double x, const TPointD &point) { return x < point.x; });
    return (next - 1)->y;
  }
  return evaluate(profile, position);
}
inline Profile slice(const Profile &profile, double first, double last) {
  if (profile.empty()) return {};
  if (last <= first) return {{0., evaluate(profile, first)}, {1., evaluate(profile, first)}};
  Profile result;
  if (first == 0.) {
    for (const auto &knot : profile)
      if (knot.x == 0.) result.push_back(knot);
      else break;
  } else result.emplace_back(0., evaluate(profile, first));
  for (const auto &knot : profile)
    if (knot.x > first && knot.x < last)
      result.emplace_back((knot.x - first) / (last - first), knot.y);
  if (last == 1.) {
    for (const auto &knot : profile)
      if (knot.x == 1.) result.push_back(knot);
  } else result.emplace_back(1., evaluateLeft(profile, last));
  return result;
}
inline Profile reversed(const Profile &profile) {
  Profile result;
  for (auto i = profile.rbegin(); i != profile.rend(); ++i) result.emplace_back(1. - i->x, i->y);
  return result;
}
// Concatenate actual geometric spans, including an optional connecting segment.
// Each profile keeps its own scalar opacity; the returned envelope has scalar 1.
// Build this before changing a live drawing, since the native knot limit can be
// exceeded even when every input envelope is valid individually.
struct Span {
  Profile profile;
  double length;
  double opacity;
};
inline Profile concatenate(const std::vector<Span> &spans) {
  double total = 0.;
  for (const auto &span : spans) {
    if (!valid(span.profile) || !std::isfinite(span.length) || span.length < 0. ||
        !std::isfinite(span.opacity) || span.opacity < 0. || span.opacity > 1.)
      throw std::invalid_argument("Invalid stroke opacity span");
    total += span.length;
  }
  if (!std::isfinite(total)) throw std::invalid_argument("Invalid stroke length");
  if (spans.empty()) return {};
  if (total == 0.) {
    const double opacity = evaluate(spans.front().profile, 0.) * spans.front().opacity;
    return {{0., opacity}, {1., opacity}};
  }
  Profile result;
  auto append = [&](double x, double y) {
    if (!result.empty() && result.back().x == x) {
      if (result.back().y == y) return;
      // Several touching spans still have only a left and a right limit.
      if (result.size() > 1 && result[result.size() - 2].x == x) {
        result.back().y = y;
        return;
      }
    }
    if (result.size() == MaxKnots)
      throw std::length_error("The joined opacity envelope exceeds the supported knot limit");
    result.emplace_back(x, y);
  };
  double offset = 0.;
  for (const auto &span : spans) {
    if (span.length == 0.) continue;
    const double start = offset / total;
    offset += span.length;
    const double end = offset / total;
    if (span.profile.empty()) {
      append(start, span.opacity);
      append(end, span.opacity);
    } else {
      for (const auto &knot : span.profile)
        append(knot.x == 0. ? start : knot.x == 1. ? end :
               start + knot.x * (end - start), knot.y * span.opacity);
    }
  }
  if (!valid(result)) throw std::length_error("The joined opacity envelope cannot be represented");
  return result;
}
inline Profile blend(const Profile &first, const Profile &last, double t) {
  if (first.empty() && last.empty()) return {};
  std::vector<double> positions{0., 1.};
  for (const auto &p : first) positions.push_back(p.x);
  for (const auto &p : last) positions.push_back(p.x);
  std::sort(positions.begin(), positions.end());
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
  Profile result;
  for (double x : positions) {
    const double left = evaluateLeft(first, x) * (1. - t) + evaluateLeft(last, x) * t;
    const double right = evaluateRight(first, x) * (1. - t) + evaluateRight(last, x) * t;
    result.emplace_back(x, left);
    if (right != left) result.emplace_back(x, right);
  }
  return result;
}
} // namespace StrokeOpacity
