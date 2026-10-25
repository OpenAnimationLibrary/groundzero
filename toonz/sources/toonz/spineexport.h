#pragma once

#ifndef SPINEEXPORT_H
#define SPINEEXPORT_H

#include <cmath>

//=============================================================================
// SpineExport
//-----------------------------------------------------------------------------
// Pure math helpers of the "Export to Spine JSON" command (spineexport.cpp),
// kept free of OpenToonz and Qt dependencies so that they can be checked
// standalone (see tests/spineexport_test.cpp).
//-----------------------------------------------------------------------------

namespace SpineExport {

//! Local transform of a Spine bone. Angles are in degrees, counterclockwise;
//! shearX is always 0.
struct BoneTransform {
  double m_x = 0.0, m_y = 0.0;
  double m_rotation = 0.0;
  double m_scaleX = 1.0, m_scaleY = 1.0;
  double m_shearY = 0.0;
};

//! Returns the angle equivalent to \e angle in (-180, 180].
inline double normalizeDegrees(double angle) {
  angle = std::fmod(angle, 360.0);
  if (angle <= -180.0)
    angle += 360.0;
  else if (angle > 180.0)
    angle -= 360.0;
  return angle;
}

//! Returns the angle equivalent to \e angle nearest to \e reference.
inline double unwrapDegrees(double angle, double reference) {
  return reference + normalizeDegrees(angle - reference);
}

/*!
Decomposes the affine transform
  | a11 a12 a13 |
  | a21 a22 a23 |
into a Spine bone transform, as Spine runtimes compose them:
  a11 = cos(rotation) * scaleX    a12 = cos(rotation + 90 + shearY) * scaleY
  a21 = sin(rotation) * scaleX    a22 = sin(rotation + 90 + shearY) * scaleY
  a13 = x                         a23 = y
Every transform has two such decompositions, the second one negating both
scales and turning by 180 degrees. The one with the rotation nearest to the
\e previous one is returned, and its angles are unwrapped against it, so that
keys baked frame by frame interpolate without spins. Without \e previous (the
first frame), positive scales are preferred unless the transform flips, in
which case the rotation nearest to 0 is preferred.
*/
inline BoneTransform decomposeAffine(double a11, double a12, double a13,
                                     double a21, double a22, double a23,
                                     const BoneTransform *previous = nullptr) {
  const double toDegrees = 180.0 / 3.14159265358979323846;
  const double eps       = 1e-12;

  BoneTransform t;
  t.m_x = a13, t.m_y = a23;

  double scaleX = std::hypot(a11, a21), scaleY = std::hypot(a12, a22);
  double det = a11 * a22 - a12 * a21;

  double rotation = 0.0;
  if (scaleX > eps)
    rotation = std::atan2(a21, a11) * toDegrees;
  else if (scaleY > eps)
    rotation = std::atan2(a22, a12) * toDegrees - 90.0;

  if (det < 0.0) scaleY = -scaleY;

  double shearY = 0.0;
  if (std::abs(scaleY) > eps)
    shearY = normalizeDegrees(
        std::atan2(a22 / scaleY, a12 / scaleY) * toDegrees - rotation - 90.0);

  // Choose between (scaleX, scaleY, rotation) and its 180 degrees twin
  double reference = previous ? previous->m_rotation : 0.0;
  bool useTwin     = (previous || det < 0.0) &&
                 std::abs(normalizeDegrees(rotation + 180.0 - reference)) <
                     std::abs(normalizeDegrees(rotation - reference));
  if (useTwin) {
    scaleX = -scaleX, scaleY = -scaleY;
    rotation += 180.0;
  }

  t.m_scaleX = scaleX, t.m_scaleY = scaleY;
  if (previous) {
    t.m_rotation = unwrapDegrees(rotation, previous->m_rotation);
    t.m_shearY   = unwrapDegrees(shearY, previous->m_shearY);
  } else {
    t.m_rotation = normalizeDegrees(rotation);
    t.m_shearY   = shearY;
  }
  return t;
}

}  // namespace SpineExport

#endif  // SPINEEXPORT_H
