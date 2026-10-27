#pragma once

#ifndef TSHAPEAWAREDEFORMER_H
#define TSHAPEAWAREDEFORMER_H

#include "toonz/tdeformer.h"

#include <cmath>
#include <memory>
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
// TShapeAwareDeformer
//
//================================================================================================

//! The TShapeAwareDeformer deforms the plane with pins, bones and the vertices
//! of a cage acting together, each influencing the plane around it through
//! smooth weights that follow the layout of all the handles.
/*!
  Each handle has a transform (a TAffine) that takes its rest geometry to its
  deformed pose, and a weight field over the plane: the deformation is the
  linear blend deform(p) = sum_i w_i(p) T_i p, where one more implicit handle,
  the anchor, keeps the plane where it is (T = identity).

  The weights are an approximation of the bounded biharmonic weights (Jacobson,
  Baran, Popovic, Sorkine 2011). On a regular grid over the rest geometry of
  the handles, enlarged by a margin, the weight of every handle is the
  biharmonic function (the minimizer of the sum of the squared discrete
  Laplacian, whose smoothness has no cusp at the handles) that is 1 on the
  handle and 0 on the other handles, and 0 on the border of the grid, where the
  anchor has the weight 1. Pins constrain the grid nodes around them, bones the
  nodes along their segment, and the cage the nodes along its edges, with the
  weights of the two vertices of an edge interpolated along it, so the cage
  edge moves with its vertices. The solution is computed by conjugate
  gradients on a pyramid of grids, from coarse to fine, each level started from
  the previous one (the coarsest from an inverse squared distance field to the
  handles). The weights of all the handles (the anchor's is what is left of 1)
  are then clamped to [0, 1] and normalised, so they are never negative, never
  above 1 and sum to 1: this is the bound of the method, imposed by clamping
  after the solve instead of as a constraint of it. Between the nodes of the
  finest grid the weights are interpolated bilinearly, and outside the grid the
  anchor has the weight 1, so the plane there does not move and the
  deformation is continuous everywhere.

  The weights depend on the rest geometry only, not on the deformed pose, so
  an animation computes them once: computeWeights() makes them, and a deformer
  can be built on a shared set of weights.

  Handles of the three types:
  - a pin moves the nearby plane by a translation, and optionally a rotation
    and a uniform scale about its rest position;
  - a bone carries the plane near its rest segment as the rest segment is
    carried onto the deformed one (rotation about the head, plus an optional
    stretch by the ratio of the lengths);
  - the cage vertices, in list order, are the corners of a closed polygon
    (with 3 or more of them; fewer count as pins) that moves with the
    translation of each corner.

  Limits: two handles closer than a few cells of the grid compete for its
  nodes, and the first one in the list (pins before bones before the cage)
  keeps them, so the other one has a weight a little below 1 at its position.
  Like any linear blend, the deformation may fold the plane where the handles
  move strongly against each other, and then undeform() returns the best
  estimate found.

  The handles may be placed in the plane by an affine map, so that a deformer
  can be built in a render's space (see TDeformer) from handles defined in an
  fx's space: the deformation is exactly placement * D * placement^-1.

  A TShapeAwareDeformer is a plain TDeformer, which can be a layer of a
  TDeformerStack.
*/
class DVAPI TShapeAwareDeformer final : public TDeformer {
public:
  struct Handle {
    enum Type { Pin, Bone, CageVertex };

    Type m_type;
    TPointD m_restHead;   //!< Rest position of a pin or cage vertex; rest head
                          //!< of a bone
    TPointD m_restTail;   //!< Rest tail of a bone; unused otherwise
    TAffine m_transform;  //!< Takes the rest geometry to the deformed one

    Handle() : m_type(Pin) {}

    //! A pin at the rest position, moved to pos, rotated about it by
    //! angleDegrees and scaled by scale.
    static Handle makePin(const TPointD &rest, const TPointD &pos,
                          double angleDegrees = 0.0, double scale = 1.0) {
      Handle h;
      h.m_type     = Pin;
      h.m_restHead = h.m_restTail = rest;

      const double a = angleDegrees * 3.14159265358979323846 / 180.0;
      const double c = scale * std::cos(a), s = scale * std::sin(a);
      h.m_transform = TAffine(c, -s, pos.x - (c * rest.x - s * rest.y), s, c,
                              pos.y - (s * rest.x + c * rest.y));
      return h;
    }

    //! A bone from restHead to restTail, carried onto head to tail. The bone
    //! rotates the plane about its head and moves it, and scales it by the
    //! ratio of the lengths when stretch is true.
    static Handle makeBone(const TPointD &restHead, const TPointD &restTail,
                           const TPointD &head, const TPointD &tail,
                           bool stretch = false) {
      Handle h;
      h.m_type     = Bone;
      h.m_restHead = restHead;
      h.m_restTail = restTail;

      TPointD r(restTail - restHead), d(tail - head);
      double nr = norm(r), nd = norm(d);
      double c = 1.0, s = 0.0, k = 1.0;
      if (nr > 1e-9 && nd > 1e-9) {
        c = (r * d) / (nr * nd);
        s = cross(r, d) / (nr * nd);
        if (stretch) k = nd / nr;
      }
      c *= k, s *= k;
      h.m_transform = TAffine(c, -s, head.x - (c * restHead.x - s * restHead.y),
                              s, c, head.y - (s * restHead.x + c * restHead.y));
      return h;
    }

    //! A cage vertex at the rest position, moved to pos.
    static Handle makeCageVertex(const TPointD &rest, const TPointD &pos) {
      Handle h;
      h.m_type     = CageVertex;
      h.m_restHead = h.m_restTail = rest;
      h.m_transform =
          TAffine(1.0, 0.0, pos.x - rest.x, 0.0, 1.0, pos.y - rest.y);
      return h;
    }

    //! Returns whether the transform leaves the plane in place, with the
    //! translation within tolerance.
    bool isIdentity(double tolerance) const {
      const TAffine &a = m_transform;
      return std::fabs(a.a11 - 1.0) < 1e-9 && std::fabs(a.a22 - 1.0) < 1e-9 &&
             std::fabs(a.a12) < 1e-9 && std::fabs(a.a21) < 1e-9 &&
             std::fabs(a.a13) <= tolerance && std::fabs(a.a23) <= tolerance;
    }
  };

  struct Settings {
    int m_resolution;  //!< Cells along the longest side of the finest grid,
                       //!< 16 to 128
    double m_margin;   //!< Extension of the grid beyond the handles, in the
                       //!< plane's units; at least a quarter of their extent
    Settings() : m_resolution(48), m_margin(0.0) {}
    Settings(int resolution, double margin)
        : m_resolution(resolution), m_margin(margin) {}
  };

  //! The weight field: a grid of the weights of the handles, made by
  //! computeWeights() and shared by the deformers built on it.
  struct Weights;

  enum { MAX_HANDLES = 64 };

private:
  std::vector<Handle> m_handles;
  std::vector<TAffine> m_inverses;  // Inverse transform of each handle
  std::shared_ptr<const Weights> m_weights;
  TAffine m_placement, m_placementInv;
  double m_scale;                     // Size of the grid, for the tolerances
  TRectD m_domain, m_deformedDomain;  // In the plane (placement applied)
  bool m_valid;

  TPointD evaluate(const TPointD &q, double *jacobian) const;

public:
  //! Builds the weights of the handles, which depend on their rest geometry
  //! only. Returns null if there are no handles, more than MAX_HANDLES, or
  //! some value is not finite.
  static std::shared_ptr<const Weights> computeWeights(
      const std::vector<Handle> &handles, const Settings &settings);

  //! Returns numbers that are equal for two sets of handles and settings if
  //! and only if computeWeights() gives the same weights for them, to cache the
  //! weights.
  static std::vector<double> makeWeightsKey(const std::vector<Handle> &handles,
                                            const Settings &settings);

  //! Builds the deformer from the handles, in the space of the handles;
  //! placement, the identity by default, is the affine map from this space to
  //! the plane of the deformer. If weights is null they are computed, else
  //! they must be those of computeWeights() for the rest geometry of the
  //! handles.
  TShapeAwareDeformer(const std::vector<Handle> &handles,
                      const Settings &settings = Settings(),
                      const TAffine &placement = TAffine(),
                      const std::shared_ptr<const Weights> &weights = nullptr);

  //! Returns whether the deformation is defined: there is at least one handle
  //! and not too many, all the values are finite and the placement is
  //! invertible. A deformer that is not valid does not move any point.
  bool isValid() const { return m_valid; }

  int getHandleCount() const { return (int)m_handles.size(); }

  //! Returns the weights of the deformer, to cache.
  const std::shared_ptr<const Weights> &getWeights() const { return m_weights; }

  //! Returns the region of the plane (placement applied) where the weights are
  //! computed; the plane does not move outside it.
  const TRectD &getDomain() const { return m_domain; }

  //! Returns whether the deformer is not valid or every handle leaves the
  //! plane in place, up to tolerance.
  bool isIdentity(double tolerance) const;

  //! Fills weights with the weight of each handle at p, as used by deform(),
  //! and returns the weight of the anchor (staying in place). They are in
  //! [0, 1] and sum to 1.
  double weightsAt(const TPointD &p, std::vector<double> &weights) const;

  TPointD deform(const TPointD &p, double frame) const override;

  //! Always succeeds, even where the handles fold the plane; see the class
  //! description.
  bool undeform(const TPointD &p, double frame, TPointD &result) const override;

  TRectD deformedBounds(const TRectD &sourceRect, double frame) const override;
  TRectD sourceBounds(const TRectD &deformedRect, double frame) const override;
};

#endif  // TSHAPEAWAREDEFORMER_H
