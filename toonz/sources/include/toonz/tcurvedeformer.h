#pragma once

#ifndef TCURVEDEFORMER_H
#define TCURVEDEFORMER_H

#include "toonz/tdeformer.h"

#include <vector>

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//================================================================================================
//
// TBezierChain
//
//================================================================================================

//! The TBezierChain is an open chain of cubic Bezier segments through a list
//! of anchors, each with an in and an out tangent handle, parametrized by arc
//! length.
/*!
  The segment between the anchors i and i + 1 has the control points
  pos(i), out(i), in(i + 1) and pos(i + 1): the in handle of the first anchor
  and the out handle of the last one are not used. Handles are points (not
  offsets) in the same space as the anchors.

  The curve is parametrized by u, the fraction of its length from the first
  anchor: u = 0 is the first anchor and u = 1 the last one. Outside [0, 1] the
  curve goes on as the straight tangent lines of its ends, at the same speed,
  so that every point of the plane has a projection on it. The length is
  measured on a polyline sampling of the segments, and the same sampling is
  used both ways, so getPoint() and project() are exact inverses of each
  other.
*/
class DVAPI TBezierChain {
public:
  struct Anchor {
    TPointD m_pos, m_in, m_out;

    Anchor() {}
    Anchor(const TPointD &pos, const TPointD &in, const TPointD &out)
        : m_pos(pos), m_in(in), m_out(out) {}
  };

  //! The coordinates of a point of the plane with respect to the curve.
  struct Projection {
    double m_u;  //!< Fraction of the length of the nearest point of the curve
    double m_d;  //!< Signed distance along the normal (the tangent rotated
                 //!< 90 degrees counterclockwise): positive on its left
    double m_distance;  //!< Distance from the curve (see distanceOf())
  };

private:
  std::vector<Anchor> m_anchors;
  std::vector<TPointD> m_samples;  //!< Curve points at the uniform parameters
  std::vector<double> m_lengths;   //!< Length of the curve up to each sample
  double m_length;

public:
  explicit TBezierChain(const std::vector<Anchor> &anchors);

  //! Returns whether the curve is defined: at least 2 anchors, and a length
  //! that is not almost zero.
  bool isValid() const;

  int getAnchorCount() const { return (int)m_anchors.size(); }
  const Anchor &getAnchor(int i) const { return m_anchors[i]; }
  double getLength() const { return m_length; }

  //! Returns the point of the curve at the fraction u of its length.
  TPointD getPoint(double u) const;

  //! Returns the unit tangent at the fraction u of the length. Where the
  //! curve has a zero speed, the direction of its acceleration or of the
  //! chord of the segment is used.
  TPointD getTangent(double u) const;

  //! Returns the unit normal at u: the tangent rotated 90 degrees
  //! counterclockwise.
  TPointD getNormal(double u) const { return rotate90(getTangent(u)); }

  //! Finds the nearest point of the curve to p (it is sampled to find the
  //! basin of the nearest point, then refined with Newton steps) and gives
  //! the coordinates (u, d) with p = getPoint(u) + d * getNormal(u). Returns
  //! false if the curve is not valid or p is not finite.
  /*!
    Beyond the ends the nearest point is the end of the curve, and u is
    outside [0, 1]. Where the curve bends more tightly than the distance of
    p, the nearest point is not unique and may jump from one branch to
    another.
  */
  bool project(const TPointD &p, Projection &result) const;

  //! Returns the distance from the curve of the point of coordinates (u, d):
  //! |d| along the curve, and the distance from the end past it.
  double distanceOf(double u, double d) const;

  //! Returns the box of the control points that shape the curve (it bounds
  //! the curve).
  TRectD getControlBounds() const;
};

//================================================================================================
//
// TCurveDeformer
//
//================================================================================================

//! The TCurveDeformer bends the plane along a Bezier chain: the points near a
//! rest curve follow it onto a deformed curve.
/*!
  Both curves are TBezierChains through the same number of anchors. A source
  point is expressed by its arc length fraction u on the rest curve and its
  signed distance d along the curve's normal, and is mapped to
  deformed(u) + d * scale * normalDeformed(u), where scale is the optional
  width scale. Because the two curves are parametrized by the fraction of
  their lengths, the art stretches along the curve when the deformed one is
  longer or shorter. undeform() projects onto the deformed curve in the same
  way, so the two mappings are inverse by construction.

  The deformation fades with the distance from the rest curve: it applies in
  full within width of the curve, falls smoothly (smoothstep) to nothing at
  falloff, and the plane farther away does not move. A point between width and
  falloff moves by a fraction of its full displacement, which makes undeform()
  iterative there (as in TDeformerStack); where the weight is 1, it is direct.
  The map stays one to one as long as the displacement of the curve is small
  with respect to the band between width and falloff; otherwise the plane folds.

  Limits: where the curve bends more tightly than the distance of a point
  (its radius of curvature is smaller than |d|), the projection of the point is
  not unique and the mapping flips there. Keep the width within the smallest
  radius of curvature of both curves.

  The TCurveDeformer is a TDeformer, so it can be a layer of a TDeformerStack
  as it is (with its own falloff, a uniform weight is usually what the layer
  needs). It holds no animated data: it is built from the control points
  already evaluated at a frame, which the frame arguments ignore.
*/
class DVAPI TCurveDeformer final : public TDeformer {
  TBezierChain m_rest, m_deformed;
  double m_width, m_falloff;
  double m_widthScale;
  double m_scale;     //!< Size of the curves, for the tolerances
  TRectD m_restBox;   //!< Where the deformation applies, in rest pose
  TRectD m_reachBox;  //!< Box of the points moved, or moved to
  TRectD m_deformedBox;

public:
  //! Builds the deformer from the anchors of the rest and of the deformed
  //! curve, and the distances from the rest curve of full influence (width)
  //! and of no influence (falloff). Negative values count as 0, and a
  //! falloff smaller than the width as equal to it, which makes the
  //! influence drop sharply. The widthScale multiplies the distances from the
  //! curve in the deformed pose (1 keeps them).
  TCurveDeformer(const std::vector<TBezierChain::Anchor> &rest,
                 const std::vector<TBezierChain::Anchor> &deformed,
                 double width, double falloff, double widthScale = 1.0);

  //! Returns whether the deformation is defined: both curves are valid and
  //! have the same number of anchors.
  bool isValid() const;

  //! Returns whether no control point of the deformed curve is moved farther
  //! than tolerance from the rest curve's, and the width scale is 1.
  bool isIdentity(double tolerance) const;

  //! Returns the weight in [0, 1] of the deformation at the passed distance
  //! from the rest curve.
  double influence(double distance) const;

  TPointD deform(const TPointD &p, double frame) const override;
  bool undeform(const TPointD &p, double frame, TPointD &result) const override;

  TRectD deformedBounds(const TRectD &sourceRect, double frame) const override;
  TRectD sourceBounds(const TRectD &deformedRect, double frame) const override;
};

#endif  // TCURVEDEFORMER_H
