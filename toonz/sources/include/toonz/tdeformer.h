#pragma once

#ifndef TDEFORMER_H
#define TDEFORMER_H

#include "tcommon.h"
#include "tgeometry.h"
#include "traster.h"

#include <memory>
#include <vector>

class TVectorImage;

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
// TDeformer
//
//================================================================================================

//! The TDeformer class is the base of the generic 2D deformers: objects that
//! map a point of the undeformed (source) plane to its deformed position, and
//! back.
/*!
  A deformer works in a single plane, so a deformer used in a render must be
  built in the render's space (for instance, with its control points mapped
  through the render affine).

  The raster warp, warpRaster(), and the bounds helpers are implemented once
  here on top of the two mapping functions, so that every deformer (Envelope,
  and in the future Bone, Curve or Free-Form deformers) deforms rasters the
  same way.

  The frame argument is for deformers that hold animated data; deformers built
  from data already evaluated at a frame may ignore it.
*/
class DVAPI TDeformer {
public:
  TDeformer() {}
  virtual ~TDeformer() {}

  //! Returns the deformed position of the source point p.
  virtual TPointD deform(const TPointD &p, double frame) const = 0;

  //! Finds the source point whose deformed position is p. Returns false if
  //! there is none, in which case nothing is drawn at p.
  virtual bool undeform(const TPointD &p, double frame,
                        TPointD &result) const = 0;

  //! Returns a rect bounding the deformed image of the passed source rect.
  //! The default implementation samples deform() on a grid and enlarges the
  //! result by the largest distance between neighbouring samples. An infinite
  //! rect is returned unchanged.
  virtual TRectD deformedBounds(const TRectD &sourceRect, double frame) const;

  //! Returns a rect bounding the source points of the passed deformed rect,
  //! sampling undeform() as deformedBounds() samples deform().
  virtual TRectD sourceBounds(const TRectD &deformedRect, double frame) const;

  //! Fills outRas, whose bottom-left corner lies at outPos, with the
  //! deformed image of inRas, whose bottom-left corner lies at inPos. Each
  //! output pixel takes the bilinear sample of inRas at the undeformed
  //! position of its center, so the warp is done by inverse mapping. Pixels
  //! with no source point, or whose source point is outside inRas, are made
  //! transparent. Supports TRaster32P, TRaster64P and TRasterFP, with inRas
  //! of the same type as outRas.
  void warpRaster(const TRasterP &outRas, const TPointD &outPos,
                  const TRasterP &inRas, const TPointD &inPos,
                  double frame) const;
};

//================================================================================================
//
// TDeformerSource
//
//================================================================================================

//! Implemented by the fxs that deform their Source with a TDeformer (the
//! Envelope, Envelope Plus, Curve, Free-Form, Bone, Shape Aware and Deformer
//! Stack fxs), so that the code that bakes their deformation into the strokes
//! of a vector level can build it without knowing the fxs' classes.
class DVAPI TDeformerSource {
public:
  virtual ~TDeformerSource();

  //! Returns the deformer that the fx applies at frame, built as the fx
  //! builds it for a render whose affine is placement: placement maps the
  //! fx's space (that of the Source column's drawings) to the deformer's
  //! plane, and the identity gives the deformer in the fx's space. Returns
  //! null when the fx leaves its Source unchanged at frame.
  virtual std::shared_ptr<const TDeformer> makeDeformer(
      double frame, const TAffine &placement) const = 0;
};

//================================================================================================
//
// TEnvelopeDeformer
//
//================================================================================================

//! The TEnvelopeDeformer deforms the plane with a closed polygon (the
//! envelope) given in a rest pose and in a deformed pose.
/*!
  The deformation is defined through the mean value coordinates (Floater
  2003; Hormann and Floater 2006 for any polygon) of the deformed envelope:
  a deformed point p is the image of the point having, with respect to the
  rest envelope, the same coordinates that p has with respect to the deformed
  envelope. So undeform() is a direct evaluation, which the raster warp needs
  once per pixel, and deform() is computed by inverting it with a few Newton
  steps.

  The deformation is smooth (except at the envelope's corners, where it is
  only continuous), moves the envelope's edges linearly onto the deformed
  ones, reproduces exactly any similarity or affine change of the envelope,
  and does not depend on the plane's orientation or scale, so a deformer
  built in a render's space gives the same image as one built in the fx's
  space when they differ by a similarity. Points outside the envelope are
  deformed too, by the same coordinates, so art across the envelope stays
  continuous. Folding the deformed envelope over itself makes the mapping
  non-invertible there.
*/
class DVAPI TEnvelopeDeformer final : public TDeformer {
  std::vector<TPointD> m_rest, m_deformed;
  double m_scale;  //!< Size of the envelopes, for the tolerances

public:
  //! Samples per edge used by the Envelope Deformer Plus for smooth edges.
  enum { SPLINE_SAMPLES = 8 };

  //! Builds the deformer from the envelope's vertices in rest pose and in
  //! deformed pose. Both lists must have the same number of points.
  TEnvelopeDeformer(const std::vector<TPointD> &restEnvelope,
                    const std::vector<TPointD> &deformedEnvelope);

  //! Returns whether the deformation is defined: both envelopes have the same
  //! number of points, at least 3, and the rest envelope is not degenerate
  //! (its area is not almost zero).
  bool isValid() const;

  //! Returns whether no vertex is moved farther than tolerance.
  bool isIdentity(double tolerance) const;

  TPointD deform(const TPointD &p, double frame) const override;
  bool undeform(const TPointD &p, double frame, TPointD &result) const override;

  //! Maps p through the mean value coordinates of the from polygon onto the
  //! to polygon, which have the same number of vertices. Returns false where
  //! the coordinates are not defined (the weights sum to almost zero).
  static bool mapThrough(const std::vector<TPointD> &from,
                         const std::vector<TPointD> &to, const TPointD &p,
                         TPointD &result);

  //! Samples the closed Catmull-Rom spline through the anchors (a closed
  //! chain of cubic Bezier edges, whose tangents at an anchor are parallel to
  //! the line joining its neighbours), with samplesPerEdge samples on each
  //! edge, the first one on the edge's start anchor. With one sample per edge
  //! the result is the anchors themselves. Each sample is a combination of
  //! four anchors with weights that sum to one and do not depend on the
  //! anchors' positions, so sampling commutes with any affine map: an
  //! envelope built on the samples of the rest and the deformed anchors still
  //! reproduces affine changes of the anchors exactly.
  static void sampleClosedSpline(const std::vector<TPointD> &anchors,
                                 int samplesPerEdge,
                                 std::vector<TPointD> &samples);
};

//================================================================================================
//
// Vector images
//
//================================================================================================

//! Deforms the strokes of the vector image: the control points of every
//! stroke are mapped through deformer.deform(), keeping their thickness, and
//! the regions of the image are updated (the regions' fill colors follow the
//! strokes that bound them). The image is modified in place.
/*!
  A stroke is a chain of quadratic curves: mapping its control points through
  a smooth deformation approximates the deformed curve, closely enough when
  the deformation varies little along a curve's span.
*/
DVAPI void deformVectorImage(TVectorImage &vi, const TDeformer &deformer,
                             double frame);

#endif  // TDEFORMER_H
