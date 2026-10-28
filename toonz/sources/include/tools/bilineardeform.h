#pragma once

#ifndef BILINEAR_DEFORM_INCLUDED
#define BILINEAR_DEFORM_INCLUDED

// Pure maths behind the Free Deform of the vector Selection tool. It has no
// dependency on tools, images or panels, so it can be tested on its own.
//
// A point inside the original box [p00, p11] is mapped bilinearly to the
// quadrilateral a, b, c, d where
//   a is the new place of the bottom left corner  (original p00),
//   b is the new place of the bottom right corner (original p10),
//   c is the new place of the top right corner    (original p11),
//   d is the new place of the top left corner     (original p01).
// The local area scale of the map tells how much a small piece of surface
// grows or shrinks at a given original point. A line drawn there is scaled
// by the square root of it, so that its width follows the deformation.

#include "tgeometry.h"
#include <cmath>

namespace BilinearDeform {

class Map {
  TPointD m_p00, m_p11;
  TPointD m_a, m_b, m_c, m_d;
  double m_width, m_height;  // extents of the original box

public:
  Map(const TPointD &originalP00, const TPointD &originalP11, const TPointD &a,
      const TPointD &b, const TPointD &c, const TPointD &d)
      : m_p00(originalP00)
      , m_p11(originalP11)
      , m_a(a)
      , m_b(b)
      , m_c(c)
      , m_d(d)
      , m_width(originalP11.x - originalP00.x)
      , m_height(originalP11.y - originalP00.y) {}

  //! True if the original box has no width or no height. The map is then
  //! not invertible, so no area scale exists.
  bool isDegenerate() const { return m_width == 0 || m_height == 0; }

  //! Position of \b p inside the original box, in [0, 1] x [0, 1] when \b p
  //! is in the box. An axis without extent gives 0.
  TPointD parameters(const TPointD &p) const {
    double s = (m_width == 0) ? 0 : (p.x - m_p00.x) / m_width;
    double t = (m_height == 0) ? 0 : (p.y - m_p00.y) / m_height;
    return TPointD(s, t);
  }

  //! Where the bilinear map sends \b p.
  TPointD map(const TPointD &p) const {
    TPointD st = parameters(p);
    double s = st.x, t = st.y;
    TPointD ad = (1 - t) * m_a + t * m_d;
    TPointD bc = (1 - t) * m_b + t * m_c;
    return (1 - s) * ad + s * bc;
  }

  //! Absolute value of the Jacobian determinant of the map at the original
  //! point \b p, that is how many times a small area around \b p grows. The
  //! map is bilinear, so the derivatives are exact. Returns 1 when the area
  //! is undefined (degenerate original box).
  double areaScale(const TPointD &p) const {
    if (isDegenerate()) return 1.0;
    TPointD st = parameters(p);
    double s = st.x, t = st.y;
    // derivatives of the map with respect to s and t
    TPointD ds  = (1 - t) * (m_b - m_a) + t * (m_c - m_d);
    TPointD dt  = (1 - s) * (m_d - m_a) + s * (m_c - m_b);
    double area = std::fabs(cross(ds, dt)) / std::fabs(m_width * m_height);
    return std::isfinite(area) ? area : 1.0;
  }

  //! Factor to apply to the thickness of a line at the original point \b p:
  //! the square root of the local area scale.
  double thicknessFactor(const TPointD &p) const {
    return std::sqrt(areaScale(p));
  }
};

}  // namespace BilinearDeform

#endif  // BILINEAR_DEFORM_INCLUDED
