#pragma once

#ifndef GRADIENT_HANDLES_INCLUDED
#define GRADIENT_HANDLES_INCLUDED

// Pure geometry behind the Gradient Transform tool: where the on-screen
// handles of a Linear or Radial Gradient region style sit, and which style
// parameters a dragged handle produces.
//
// The conversions repeat exactly what the style drawing code does
// (colorfx/regionstyles.cpp, TLinGradFillStyle::getRects and
// TRadGradFillStyle::drawRegion), so a handle always sits on the thing it
// controls. Both styles draw relative to the bounding box of the region they
// fill:
//   centre   = box centre + (X% * lx / 2, Y% * ly / 2)     (percent of half the
//   box)
// Linear gradient (parameters Angle, X, Y, Size):
//   the colours blend along a line through the centre at Angle degrees
//   (counterclockwise), from Size units before the centre (point colour) to
//   Size units after it (main colour). Size is the "Smoothness" parameter.
// Radial gradient (parameters X, Y, Radius, Smooth):
//   outer radius = max(lx, ly) / 2 * Radius%     (main colour from here out)
//   inner radius = outer radius * (100 - Smooth)%  (point colour inside)
//
// No GUI, no application objects: only tgeometry.h and <cmath>.

#include "tgeometry.h"

#include <algorithm>
#include <cmath>

namespace GradientHandles {

// Tag ids of the two styles (TColorStyle::getTagId()). They are part of the
// palette file format, so they never change.
const int LinearGradientTagId = 1138;  // TLinGradFillStyle
const int RadialGradientTagId = 1139;  // TRadGradFillStyle

enum Kind { NoGradient, Linear, Radial };

inline Kind kindFromTagId(int tagId) {
  if (tagId == LinearGradientTagId) return Linear;
  if (tagId == RadialGradientTagId) return Radial;
  return NoGradient;
}

// Allowed values, as reported by the styles' getParamRange().
const double PercentMin      = -100.0;  // X and Y position of both styles
const double PercentMax      = 100.0;
const double LinearAngleMin  = -180.0;
const double LinearAngleMax  = 180.0;
const double LinearSizeMin   = 1.0;
const double LinearSizeMax   = 500.0;
const double RadialRadiusMin = 0.01;
const double RadialRadiusMax = 100.0;
const double RadialSmoothMin = 0.01;
const double RadialSmoothMax = 100.0;

// Style parameter indices (getParamValue / setParamValue).
enum LinearParamIndex {
  LinearAngleIndex = 0,
  LinearXIndex     = 1,
  LinearYIndex     = 2,
  LinearSizeIndex  = 3
};
enum RadialParamIndex {
  RadialXIndex      = 0,
  RadialYIndex      = 1,
  RadialRadiusIndex = 2,
  RadialSmoothIndex = 3
};

const double Pi = 3.14159265358979323846;

//=============================================================================
// Small helpers
//=============================================================================

inline double clampValue(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

inline double boxWidth(const TRectD &box) {
  return std::max(0.0, box.x1 - box.x0);
}
inline double boxHeight(const TRectD &box) {
  return std::max(0.0, box.y1 - box.y0);
}

// Brings an angle in degrees into [-180, 180] (350 becomes -10).
inline double normalizeAngle(double degrees) {
  if (!std::isfinite(degrees)) return 0.0;
  degrees = std::fmod(degrees, 360.0);
  if (degrees > 180.0)
    degrees -= 360.0;
  else if (degrees < -180.0)
    degrees += 360.0;
  return degrees;
}

// Rounds an angle to a multiple of step degrees (no change if step <= 0).
inline double snapAngle(double degrees, double step) {
  if (!(step > 0.0)) return degrees;
  return normalizeAngle(std::floor(degrees / step + 0.5) * step);
}

// Direction of (dx, dy) in degrees, or fallback if the vector is (almost) null.
inline double angleOfVector(double dx, double dy, double fallback) {
  if (std::hypot(dx, dy) <= 1e-9) return fallback;
  return normalizeAngle(std::atan2(dy, dx) * 180.0 / Pi);
}

// Where the style puts the centre of its gradient.
inline TPointD gradientCenter(const TRectD &box, double xPercent,
                              double yPercent) {
  return TPointD(
      0.5 * (box.x0 + box.x1) + xPercent * 0.01 * boxWidth(box) * 0.5,
      0.5 * (box.y0 + box.y1) + yPercent * 0.01 * boxHeight(box) * 0.5);
}

// Percent (-100..100) that puts the centre coordinate on `coord`. On a flat
// axis (no extent) the percent has no effect and `current` is kept.
inline double percentFromCoordinate(double coord, double lo, double hi,
                                    double current) {
  double extent = hi - lo;
  if (!(extent > 1e-9)) return current;
  return clampValue((coord - 0.5 * (lo + hi)) / (extent * 0.005), PercentMin,
                    PercentMax);
}

//=============================================================================
// Linear gradient
//=============================================================================

struct LinearParams {
  double angle;  // degrees, counterclockwise, -180..180
  double x, y;   // gradient centre, percent of half the box, -100..100
  double size;   // half length of the colour blend, drawing units, 1..500

  LinearParams(double a = 0.0, double px = 0.0, double py = 0.0,
               double s = 100.0)
      : angle(a), x(px), y(py), size(s) {}
};

struct LinearHandles {
  TPointD center;
  TPointD head;  // end of the main colour (centre + size along the angle)
  TPointD tail;  // end of the point colour (centre - size along the angle)
};

inline LinearParams linearParamsFromArray(const double *v) {
  return LinearParams(v[LinearAngleIndex], v[LinearXIndex], v[LinearYIndex],
                      v[LinearSizeIndex]);
}
inline void linearParamsToArray(const LinearParams &p, double *v) {
  v[LinearAngleIndex] = p.angle;
  v[LinearXIndex]     = p.x;
  v[LinearYIndex]     = p.y;
  v[LinearSizeIndex]  = p.size;
}

inline LinearHandles linearParamsToHandles(const TRectD &box,
                                           const LinearParams &p) {
  LinearHandles h;
  h.center  = gradientCenter(box, p.x, p.y);
  double a  = p.angle * Pi / 180.0;
  double dx = std::cos(a) * p.size;
  double dy = std::sin(a) * p.size;
  h.head    = TPointD(h.center.x + dx, h.center.y + dy);
  h.tail    = TPointD(h.center.x - dx, h.center.y - dy);
  return h;
}

// The centre handle was dragged to `center`: only X and Y change.
inline LinearParams linearMoveCenter(const TRectD &box, const LinearParams &p,
                                     const TPointD &center) {
  LinearParams r = p;
  r.x            = percentFromCoordinate(center.x, box.x0, box.x1, p.x);
  r.y            = percentFromCoordinate(center.y, box.y0, box.y1, p.y);
  return r;
}

enum LinearEnd { LinearHead, LinearTail };

// An end handle was dragged to `target`: the line turns to point at it
// (Angle) and its length follows (Size). A handle on the centre keeps the
// angle. snapStep > 0 rounds the angle to a multiple of snapStep degrees.
inline LinearParams linearDragEnd(const TRectD &box, const LinearParams &p,
                                  LinearEnd end, const TPointD &target,
                                  double snapStep = 0.0) {
  LinearParams r = p;
  TPointD c      = gradientCenter(box, p.x, p.y);
  double dx      = target.x - c.x;
  double dy      = target.y - c.y;
  if (end == LinearTail) {
    dx = -dx;
    dy = -dy;
  }
  double angle = angleOfVector(dx, dy, normalizeAngle(p.angle));
  if (snapStep > 0.0) angle = snapAngle(angle, snapStep);
  r.angle = angle;
  r.size  = clampValue(std::hypot(dx, dy), LinearSizeMin, LinearSizeMax);
  return r;
}

//=============================================================================
// Radial gradient
//=============================================================================

struct RadialParams {
  double x, y;    // gradient centre, percent of half the box, -100..100
  double radius;  // outer radius, percent of half the longer box side
  double smooth;  // percent of the outer radius that is blended, 0.01..100

  RadialParams(double px = 0.0, double py = 0.0, double r = 50.0,
               double s = 50.0)
      : x(px), y(py), radius(r), smooth(s) {}
};

struct RadialHandles {
  TPointD center;
  TPointD outer;  // on the circle where the main colour is reached
  TPointD inner;  // on the circle where the blend starts (point colour inside)
  double outerRadius;
  double innerRadius;
};

inline RadialParams radialParamsFromArray(const double *v) {
  return RadialParams(v[RadialXIndex], v[RadialYIndex], v[RadialRadiusIndex],
                      v[RadialSmoothIndex]);
}
inline void radialParamsToArray(const RadialParams &p, double *v) {
  v[RadialXIndex]      = p.x;
  v[RadialYIndex]      = p.y;
  v[RadialRadiusIndex] = p.radius;
  v[RadialSmoothIndex] = p.smooth;
}

// Longer side of the box: the unit of the radius.
inline double radialReferenceLength(const TRectD &box) {
  return std::max(boxWidth(box), boxHeight(box));
}

inline double radialOuterRadius(const TRectD &box, const RadialParams &p) {
  return 0.5 * radialReferenceLength(box) * p.radius * 0.01;
}

inline double radialInnerRadius(const TRectD &box, const RadialParams &p) {
  return radialOuterRadius(box, p) * (100.0 - p.smooth) * 0.01;
}

// handleAngle (degrees) is where the outer handle sits on its circle; the
// inner handle sits on the opposite side so the two never overlap. The angle is
// only a display choice, the style has no such parameter.
inline RadialHandles radialParamsToHandles(const TRectD &box,
                                           const RadialParams &p,
                                           double handleAngle = 0.0) {
  RadialHandles h;
  h.center      = gradientCenter(box, p.x, p.y);
  h.outerRadius = radialOuterRadius(box, p);
  h.innerRadius = radialInnerRadius(box, p);
  double a      = handleAngle * Pi / 180.0;
  double ux     = std::cos(a);
  double uy     = std::sin(a);
  h.outer =
      TPointD(h.center.x + ux * h.outerRadius, h.center.y + uy * h.outerRadius);
  h.inner =
      TPointD(h.center.x - ux * h.innerRadius, h.center.y - uy * h.innerRadius);
  return h;
}

inline RadialParams radialMoveCenter(const TRectD &box, const RadialParams &p,
                                     const TPointD &center) {
  RadialParams r = p;
  r.x            = percentFromCoordinate(center.x, box.x0, box.x1, p.x);
  r.y            = percentFromCoordinate(center.y, box.y0, box.y1, p.y);
  return r;
}

// The outer handle was dragged to `target`: Radius follows its distance from
// the centre. The inner circle keeps its proportion (Smooth is unchanged).
inline RadialParams radialDragOuter(const TRectD &box, const RadialParams &p,
                                    const TPointD &target) {
  RadialParams r = p;
  double unit    = radialReferenceLength(box) * 0.005;  // one Radius percent
  if (!(unit > 1e-12)) return r;
  TPointD c = gradientCenter(box, p.x, p.y);
  double d  = std::hypot(target.x - c.x, target.y - c.y);
  r.radius  = clampValue(d / unit, RadialRadiusMin, RadialRadiusMax);
  return r;
}

// The inner handle was dragged to `target`: Smooth follows, so that the
// handle sits on the circle where the blend starts.
inline RadialParams radialDragInner(const TRectD &box, const RadialParams &p,
                                    const TPointD &target) {
  RadialParams r = p;
  double outer   = radialOuterRadius(box, p);
  if (!(outer > 1e-12)) return r;
  TPointD c = gradientCenter(box, p.x, p.y);
  double d  = std::hypot(target.x - c.x, target.y - c.y);
  r.smooth =
      clampValue(100.0 * (1.0 - d / outer), RadialSmoothMin, RadialSmoothMax);
  return r;
}

}  // namespace GradientHandles

#endif  // GRADIENT_HANDLES_INCLUDED
