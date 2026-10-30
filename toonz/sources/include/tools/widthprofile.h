#pragma once

#ifndef WIDTH_PROFILE_INCLUDED
#define WIDTH_PROFILE_INCLUDED

// Width profiles and the numbers behind the Width mode of the Pump tool.
//
// A width profile describes how the thickness of a vector stroke varies along
// it: a list of (position, ratio) points, with the position being the
// normalized length along the stroke (0 at the start, 1 at the end) and the
// ratio the thickness as a fraction (0 to 1) of the maximum thickness of the
// stroke. Applying a profile gives the stroke the profile shape, keeping its
// maximum thickness.
//
// Everything here is plain arithmetic on vectors of numbers (no strokes, no
// GUI), so that it can be checked by a standalone test. The code that reads
// and writes the control points of a stroke is in widthprofilecommands.cpp
// and pumptool.cpp.
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace WidthProfile {

//-----------------------------------------------------------------------------
// Limits of the thickness (the radius, in stroke units) of a control point
// set by the Width mode and by the profiles.
//-----------------------------------------------------------------------------

//! A stroke is never made thinner than this by the Width tools, so that it
//! cannot vanish (a stroke whose control points all have no thickness is
//! treated as hidden by the renderer).
const double MinThickness = 0.05;
//! The widest radius the Width mode can set.
const double MaxThickness = 250.0;

//-----------------------------------------------------------------------------
// Profile
//-----------------------------------------------------------------------------

struct Point {
  double pos;    //!< Normalized length along the stroke, 0 to 1
  double ratio;  //!< Thickness as a fraction of the stroke maximum, 0 to 1

  Point(double p = 0.0, double r = 1.0) : pos(p), ratio(r) {}
};

struct Profile {
  std::string name;
  std::vector<Point> points;  //!< Sorted by position, see normalize()

  bool empty() const { return points.empty(); }
};

//! Restricts \p v to [lo, hi]; a NaN becomes \p lo.
inline double clampValue(double v, double lo, double hi) {
  if (!(v > lo)) return lo;
  if (v > hi) return hi;
  return v;
}

//! Clamps positions and ratios to [0, 1] and sorts the points by position
//! (points at the same position keep their order).
inline void normalize(Profile &profile) {
  for (Point &p : profile.points) {
    p.pos   = clampValue(p.pos, 0.0, 1.0);
    p.ratio = clampValue(p.ratio, 0.0, 1.0);
  }
  std::stable_sort(
      profile.points.begin(), profile.points.end(),
      [](const Point &a, const Point &b) { return a.pos < b.pos; });
}

//! The thickness ratio of the profile at \p pos, by linear interpolation
//! between the points. Before the first point and after the last one the end
//! ratios are kept. An empty profile is uniform (1), a profile of one point
//! has that ratio everywhere. The result is in [0, 1].
inline double evaluate(const Profile &profile, double pos) {
  const std::vector<Point> &v = profile.points;
  if (v.empty()) return 1.0;
  if (v.size() == 1) return clampValue(v[0].ratio, 0.0, 1.0);

  pos = clampValue(pos, 0.0, 1.0);
  if (pos <= v.front().pos) return clampValue(v.front().ratio, 0.0, 1.0);
  if (pos >= v.back().pos) return clampValue(v.back().ratio, 0.0, 1.0);

  // First point after pos: there is always one, and one before it
  std::vector<Point>::const_iterator it = std::upper_bound(
      v.begin(), v.end(), pos,
      [](double value, const Point &p) { return value < p.pos; });
  if (it == v.begin()) return clampValue(v.front().ratio, 0.0, 1.0);
  if (it == v.end()) return clampValue(v.back().ratio, 0.0, 1.0);

  const Point &a = *(it - 1);
  const Point &b = *it;
  double span    = b.pos - a.pos;
  if (!(span > 0.0)) return clampValue(b.ratio, 0.0, 1.0);
  double t = (pos - a.pos) / span;
  return clampValue(a.ratio + (b.ratio - a.ratio) * t, 0.0, 1.0);
}

//-----------------------------------------------------------------------------
// Built-in profiles
//-----------------------------------------------------------------------------

enum BuiltIn {
  UNIFORM = 0,
  TAPER_START,
  TAPER_END,
  TAPER_BOTH,
  PULSE,
  BUILTIN_COUNT
};

inline const char *builtInName(int kind) {
  switch (kind) {
  case UNIFORM:
    return "Uniform";
  case TAPER_START:
    return "Taper Start";
  case TAPER_END:
    return "Taper End";
  case TAPER_BOTH:
    return "Taper Both Ends";
  case PULSE:
    return "Pulse";
  }
  return "";
}

namespace detail {

const double Pi = 3.14159265358979323846;

//! A taper: the thickness grows from nothing to the full thickness over the
//! first \p taperLength of the stroke (and/or shrinks over the last), with a
//! rounded start so the tip is not a straight wedge.
inline Profile taper(bool atStart, bool atEnd) {
  const double taperLength = 0.35;
  const int steps          = 8;

  Profile p;
  if (atStart) {
    for (int i = 0; i <= steps; ++i) {
      double t = double(i) / steps;
      p.points.push_back(Point(t * taperLength, std::sin(0.5 * Pi * t)));
    }
  } else
    p.points.push_back(Point(0.0, 1.0));

  if (atEnd) {
    for (int i = steps; i >= 0; --i) {
      double t = double(i) / steps;
      p.points.push_back(Point(1.0 - t * taperLength, std::sin(0.5 * Pi * t)));
    }
  } else
    p.points.push_back(Point(1.0, 1.0));
  return p;
}

}  // namespace detail

//! One of the built-in profiles (see BuiltIn). An unknown kind is Uniform.
inline Profile builtIn(int kind) {
  Profile p;
  switch (kind) {
  case TAPER_START:
    p = detail::taper(true, false);
    break;
  case TAPER_END:
    p = detail::taper(false, true);
    break;
  case TAPER_BOTH:
    p = detail::taper(true, true);
    break;
  case PULSE: {
    // Thin at both ends, full in the middle
    const int steps = 8;
    for (int i = 0; i <= steps; ++i) {
      double t = double(i) / steps;
      p.points.push_back(Point(t, 0.25 + 0.75 * std::sin(detail::Pi * t)));
    }
    break;
  }
  case UNIFORM:
  default:
    p.points.push_back(Point(0.0, 1.0));
    p.points.push_back(Point(1.0, 1.0));
    kind = UNIFORM;
    break;
  }
  p.name = builtInName(kind);
  return p;
}

//-----------------------------------------------------------------------------
// Building a profile from a stroke
//-----------------------------------------------------------------------------

//! Removes the points that the straight line between their kept neighbours
//! reproduces within \p tolerance (in ratio units). The first and last points
//! are kept.
inline Profile simplify(const Profile &in, double tolerance) {
  const std::vector<Point> &v = in.points;
  if (v.size() <= 2) return in;

  std::vector<char> keep(v.size(), 0);
  keep.front() = keep.back() = 1;

  std::vector<std::pair<size_t, size_t>> pending;
  pending.push_back(std::pair<size_t, size_t>(0, v.size() - 1));
  while (!pending.empty()) {
    size_t a = pending.back().first;
    size_t b = pending.back().second;
    pending.pop_back();
    if (b <= a + 1) continue;

    double span     = v[b].pos - v[a].pos;
    double worst    = -1.0;
    size_t worstIdx = a;
    for (size_t i = a + 1; i < b; ++i) {
      double t        = span > 0.0 ? (v[i].pos - v[a].pos) / span : 0.0;
      double expected = v[a].ratio + (v[b].ratio - v[a].ratio) * t;
      double dev      = std::fabs(v[i].ratio - expected);
      if (dev > worst) {
        worst    = dev;
        worstIdx = i;
      }
    }
    if (worst > tolerance) {
      keep[worstIdx] = 1;
      pending.push_back(std::pair<size_t, size_t>(a, worstIdx));
      pending.push_back(std::pair<size_t, size_t>(worstIdx, b));
    }
  }

  Profile out;
  out.name = in.name;
  for (size_t i = 0; i < v.size(); ++i)
    if (keep[i]) out.points.push_back(v[i]);
  return out;
}

//! Positions in [0, 1] of samples whose cumulative lengths are \p lengths
//! (non-decreasing, as returned by a stroke for its control points). The
//! positions never decrease. When all the lengths are equal (no length at all)
//! every position is 0.
inline std::vector<double> normalizedPositions(
    const std::vector<double> &lengths) {
  std::vector<double> pos(lengths.size(), 0.0);
  if (lengths.empty()) return pos;

  double total = lengths.back() - lengths.front();
  if (!(total > 0.0)) return pos;

  double last = 0.0;
  for (size_t i = 0; i < lengths.size(); ++i) {
    double p = clampValue((lengths[i] - lengths.front()) / total, 0.0, 1.0);
    last     = std::max(last, p);
    pos[i]   = last;
  }
  return pos;
}

//! Builds a profile from thickness samples taken along a stroke: \p lengths
//! are the cumulative lengths of the samples and \p thickness the thickness at
//! each. The ratios are relative to the largest sample, the points are then
//! simplified within \p tolerance.
//! A stroke with no thickness gives an empty profile; samples with no length
//! (a dot) give a profile of one point.
inline Profile fromSamples(const std::vector<double> &lengths,
                           const std::vector<double> &thickness,
                           double tolerance = 0.01) {
  Profile profile;
  size_t n = std::min(lengths.size(), thickness.size());
  if (n == 0) return profile;

  double maxThick = 0.0;
  for (size_t i = 0; i < n; ++i)
    if (thickness[i] > maxThick) maxThick = thickness[i];
  if (!(maxThick > 0.0)) return profile;

  std::vector<double> lens(lengths.begin(), lengths.begin() + n);
  std::vector<double> pos = normalizedPositions(lens);
  double total            = lens.back() - lens.front();

  if (n == 1 || !(total > 0.0)) {
    profile.points.push_back(
        Point(0.0, clampValue(thickness[0] / maxThick, 0.0, 1.0)));
    return profile;
  }

  for (size_t i = 0; i < n; ++i)
    profile.points.push_back(
        Point(pos[i], clampValue(thickness[i] / maxThick, 0.0, 1.0)));
  return simplify(profile, tolerance);
}

//-----------------------------------------------------------------------------
// Applying a profile to a stroke
//-----------------------------------------------------------------------------

//! The thickness to give to control points at the cumulative lengths
//! \p lengths (one entry per control point, on and off the curve alike) of a
//! stroke whose maximum thickness is \p maxThickness: the profile at the
//! normalized length of the point times the maximum thickness, but never less
//! than \p minThickness (so a taper keeps a hairline tip rather than a stroke
//! that vanishes). The result has one value per length.
//! A stroke with no thickness stays without (all zero). A stroke with no
//! length (a dot) is set to the maximum thickness. An empty profile is
//! uniform.
inline std::vector<double> mapToThickness(const Profile &profile,
                                          const std::vector<double> &lengths,
                                          double maxThickness,
                                          double minThickness = MinThickness) {
  std::vector<double> out(lengths.size(), 0.0);
  if (lengths.empty() || !(maxThickness > 0.0)) return out;

  double floorThick = clampValue(minThickness, 0.0, maxThickness);

  double total = lengths.back() - lengths.front();
  if (!(total > 0.0)) {
    std::fill(out.begin(), out.end(), maxThickness);
    return out;
  }

  std::vector<double> pos = normalizedPositions(lengths);
  for (size_t i = 0; i < out.size(); ++i) {
    double t = maxThickness * evaluate(profile, pos[i]);
    out[i]   = clampValue(std::max(t, floorThick), 0.0, maxThickness);
  }
  return out;
}

//-----------------------------------------------------------------------------
// Width mode: the thickness at the dragged point and its fade to the sides
//-----------------------------------------------------------------------------

//! Blend a profile with the original pressure widths. A zero strength is an
//! exact identity; Multiply retains the original pressure variation.
inline std::vector<double> applyBlended(const Profile &profile,
    const std::vector<double> &lengths, const std::vector<double> &original,
    double strength, bool multiply) {
  if (original.size() != lengths.size() || original.empty()) return original;
  strength = clampValue(strength, 0.0, 1.0);
  if (strength == 0.0) return original;
  const double maximum = *std::max_element(original.begin(), original.end());
  auto target = mapToThickness(profile, lengths, maximum);
  const auto positions = normalizedPositions(lengths);
  for (size_t i = 0; i < target.size(); ++i) {
    if (multiply) target[i] = original[i] * evaluate(profile, positions[i]);
    target[i] = original[i] + strength * (target[i] - original[i]);
  }
  return target;
}

//! The thickness set at the dragged point: the distance between the pointer
//! and the center of the stroke at that point, kept within the limits.
inline double thicknessFromDistance(double distance,
                                    double minThickness = MinThickness,
                                    double maxThickness = MaxThickness) {
  return clampValue(distance, minThickness, maxThickness);
}

//! The share (1 at the dragged point, 0 at \p radius and beyond, smooth in
//! between) of the thickness change that a point at \p distance (along the
//! stroke) from the dragged point gets.
inline double falloffWeight(double distance, double radius) {
  double d = std::fabs(distance);
  if (!(radius > 0.0)) return d > 0.0 ? 0.0 : 1.0;
  if (d >= radius) return 0.0;
  return 0.5 * (1.0 + std::cos(detail::Pi * d / radius));
}

//! The falloff weights of the control points whose cumulative lengths are
//! \p lengths, for a change centered at the length \p center. For a closed
//! stroke (\p closed, whose total length is \p totalLength) distances are
//! measured the short way round, so both sides of the seam agree.
inline std::vector<double> falloffWeights(const std::vector<double> &lengths,
                                          double center, double radius,
                                          bool closed        = false,
                                          double totalLength = 0.0) {
  std::vector<double> out(lengths.size(), 0.0);
  for (size_t i = 0; i < lengths.size(); ++i) {
    double d = std::fabs(lengths[i] - center);
    if (closed && totalLength > 0.0 && d > 0.5 * totalLength)
      d = std::max(0.0, totalLength - d);
    out[i] = falloffWeight(d, radius);
  }
  return out;
}

//! The new thickness of a control point of thickness \p base for a change of
//! \p delta at the dragged point, scaled by the \p weight of the point. It
//! stays within the limits, except that a point already outside them (for
//! instance a zero thickness tip) is not pushed further out.
inline double blendThickness(double base, double delta, double weight,
                             double minThickness = MinThickness,
                             double maxThickness = MaxThickness) {
  double lower = std::min(base, minThickness);
  double upper = std::max(base, maxThickness);
  return clampValue(base + delta * weight, lower, upper);
}

//-----------------------------------------------------------------------------
// Saving profiles in a text file: one profile per line,
//   name TAB position:ratio,position:ratio,...
//-----------------------------------------------------------------------------

//! One profile as a line of text (without the newline). Characters that would
//! break the line format are replaced by spaces in the name.
inline std::string toText(const Profile &profile) {
  std::ostringstream os;
  os.imbue(std::locale::classic());
  os << std::setprecision(5);

  std::string name = profile.name;
  for (char &c : name)
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  os << name << '\t';
  for (size_t i = 0; i < profile.points.size(); ++i) {
    if (i) os << ',';
    os << profile.points[i].pos << ':' << profile.points[i].ratio;
  }
  return os.str();
}

//! Reads a line written by toText(). Returns false, leaving \p out empty, if
//! the line has no name or no valid point. Positions and ratios are clamped
//! and the points sorted.
inline bool fromText(const std::string &line, Profile &out) {
  out = Profile();

  size_t tab = line.find('\t');
  if (tab == std::string::npos || tab == 0) return false;
  std::string name = line.substr(0, tab);
  while (!name.empty() && (name.back() == ' ' || name.back() == '\r'))
    name.pop_back();
  if (name.empty()) return false;

  Profile profile;
  profile.name = name;

  std::string rest = line.substr(tab + 1);
  while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n'))
    rest.pop_back();

  size_t start = 0;
  while (start <= rest.size()) {
    size_t end = rest.find(',', start);
    if (end == std::string::npos) end = rest.size();
    std::string item = rest.substr(start, end - start);
    start            = end + 1;
    if (item.empty()) continue;

    size_t colon = item.find(':');
    if (colon == std::string::npos) return false;

    std::istringstream a(item.substr(0, colon)), b(item.substr(colon + 1));
    a.imbue(std::locale::classic());
    b.imbue(std::locale::classic());
    double pos = 0.0, ratio = 0.0;
    if (!(a >> pos) || !(b >> ratio)) return false;
    if (!std::isfinite(pos) || !std::isfinite(ratio)) return false;
    a >> std::ws;
    b >> std::ws;
    if (!a.eof() || !b.eof()) return false;
    profile.points.push_back(Point(pos, ratio));
  }
  if (profile.points.empty()) return false;

  normalize(profile);
  out = profile;
  return true;
}

//! All the profiles of a text (see toText()). Blank lines, lines starting
//! with '#' and lines that cannot be read are skipped.
inline std::vector<Profile> parseProfiles(const std::string &text) {
  std::vector<Profile> profiles;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    start            = end + 1;
    if (line.empty() || line[0] == '#') continue;
    Profile p;
    if (fromText(line, p)) profiles.push_back(p);
  }
  return profiles;
}

//! The text for a list of profiles, one line each.
inline std::string formatProfiles(const std::vector<Profile> &profiles) {
  std::string text;
  for (const Profile &p : profiles) text += toText(p) + "\n";
  return text;
}

//! Adds \p profile to \p profiles, replacing the one with the same name.
inline void addOrReplace(std::vector<Profile> &profiles,
                         const Profile &profile) {
  for (Profile &p : profiles)
    if (p.name == profile.name) {
      p = profile;
      return;
    }
  profiles.push_back(profile);
}

}  // namespace WidthProfile

#endif  // WIDTH_PROFILE_INCLUDED
