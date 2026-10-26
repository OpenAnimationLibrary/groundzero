#pragma once

#ifndef DEFORMERFXCOMMON_H
#define DEFORMERFXCOMMON_H

// Parts shared by the fxs built on the generic deformers (toonz/tdeformer.h):
// Envelope Deformer and Deformer Stack. The parameters of the other deformer
// types, shared with the layers of the Deformer Stack, are in deformerparams.h.

#include "trasterfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "tmathutil.h"

#include "toonz/tdeformer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace deformerfx {

enum { POINT_COUNT = 8 };

//------------------------------------------------------------------------------

inline void mapPoints(const TAffine &aff, std::vector<TPointD> &points) {
  for (TPointD &p : points) p = aff * p;
}

//------------------------------------------------------------------------------

//! Returns the diagonal of the box of the points.
inline double pointsDiagonal(const std::vector<TPointD> &points) {
  if (points.empty()) return 0.0;

  TPointD p0(points[0]), p1(points[0]);
  for (const TPointD &p : points) {
    p0.x = std::min(p0.x, p.x), p0.y = std::min(p0.y, p.y);
    p1.x = std::max(p1.x, p.x), p1.y = std::max(p1.y, p.y);
  }
  return norm(p1 - p0);
}

//------------------------------------------------------------------------------

//! The animatable parameters of an envelope: the 8 points of the deformed
//! envelope and the 8 of the rest one.
class EnvelopeParams {
public:
  TPointParamP m_point[POINT_COUNT];  // The envelope, deformed
  TPointParamP m_rest[POINT_COUNT];   // The envelope in rest pose

  //! Creates the parameters and binds them to fx as prefix + "point1" to
  //! "point8" and prefix + "rest1" to "rest8". By default, the envelope is a
  //! square of half side size around the origin, with a point on each corner
  //! and on the middle of each side.
  void bind(TFx *fx, const std::string &prefix, double size) {
    const double s                      = size;
    const TPointD defaults[POINT_COUNT] = {
        TPointD(-s, -s), TPointD(0.0, -s), TPointD(s, -s), TPointD(s, 0.0),
        TPointD(s, s),   TPointD(0.0, s),  TPointD(-s, s), TPointD(-s, 0.0)};

    for (int i = 0; i < POINT_COUNT; ++i) {
      std::string suffix = std::to_string(i + 1);

      m_point[i] = TPointParamP(defaults[i]);
      m_point[i]->getX()->setMeasureName("fxLength");
      m_point[i]->getY()->setMeasureName("fxLength");
      bindParam(fx, prefix + "point" + suffix, m_point[i]);
    }

    for (int i = 0; i < POINT_COUNT; ++i) {
      std::string suffix = std::to_string(i + 1);

      m_rest[i] = TPointParamP(defaults[i]);
      m_rest[i]->getX()->setMeasureName("fxLength");
      m_rest[i]->getY()->setMeasureName("fxLength");
      bindParam(fx, prefix + "rest" + suffix, m_rest[i]);
    }
  }

  //! Returns the envelope at frame in rest pose and deformed, in the fx's
  //! space.
  void getEnvelopes(double frame, std::vector<TPointD> &rest,
                    std::vector<TPointD> &deformed) const {
    rest.resize(POINT_COUNT);
    deformed.resize(POINT_COUNT);
    for (int i = 0; i < POINT_COUNT; ++i) {
      rest[i]     = m_rest[i]->getValue(frame);
      deformed[i] = m_point[i]->getValue(frame);
    }
  }

  //! Fills the 3 * POINT_COUNT concepts from the passed one with the gadgets
  //! of the envelope, whose labels start with labelPrefix.
  /*!
    The rest points come first: the viewer picks the gadget drawn last, so the
    deformed points are dragged where they lie on the rest ones.
  */
  void getParamUIs(TParamUIConcept *concepts,
                   const std::string &labelPrefix) const {
    for (int i = 0; i < POINT_COUNT; ++i) {
      std::string suffix = std::to_string(i + 1);

      TParamUIConcept &rest = concepts[i];
      rest.m_type           = TParamUIConcept::POINT;
      rest.m_label          = labelPrefix + "Rest " + suffix;
      rest.m_params.push_back(m_rest[i]);

      TParamUIConcept &offset = concepts[POINT_COUNT + i];
      offset.m_type           = TParamUIConcept::VECTOR;
      offset.m_params.push_back(m_rest[i]);
      offset.m_params.push_back(m_point[i]);

      TParamUIConcept &point = concepts[2 * POINT_COUNT + i];
      point.m_type           = TParamUIConcept::POINT;
      point.m_label          = labelPrefix + "Point " + suffix;
      point.m_params.push_back(m_point[i]);
    }
  }
};

//------------------------------------------------------------------------------

//! Returns whether an envelope leaves the Source unchanged: the envelope is
//! not moved from its rest pose, or the rest pose is degenerate.
inline bool isPassThrough(const std::vector<TPointD> &rest,
                          const std::vector<TPointD> &deformed) {
  TEnvelopeDeformer deformer(rest, deformed);
  return !deformer.isValid() || deformer.isIdentity(1e-6);
}

//------------------------------------------------------------------------------

//! Returns the pixel-aligned rect of the Source needed to render rect, given
//! the Source's bbox inBBox, in the space of the deformer; empty when nothing
//! of the Source shows in rect. The rest points are those of the envelopes (in
//! the same space), which bound the area to render of an infinite Source.
inline TRectD getSourceRect(const TDeformer &deformer,
                            const std::vector<TPointD> &rest, double frame,
                            const TRectD &rect, TRectD inBBox) {
  // Enlarged for the neighbours of the bilinear samples
  TRectD srcRect = deformer.sourceBounds(rect, frame);
  if (srcRect.isEmpty()) return TRectD();
  srcRect = srcRect.enlarge(2.0);

  if (inBBox == TConsts::infiniteRectD) {
    // A contracted envelope would ask for a large part of an infinite Source,
    // such as a gradient: bound it around the rect and the rest envelope.
    TRectD area(rect);
    for (const TPointD &p : rest) {
      area.x0 = std::min(area.x0, p.x), area.y0 = std::min(area.y0, p.y);
      area.x1 = std::max(area.x1, p.x), area.y1 = std::max(area.y1, p.y);
    }
    inBBox = area.enlarge(std::max(area.getLx(), area.getLy()));
  }

  srcRect *= inBBox;
  if (srcRect.isEmpty()) return TRectD();

  return TRectD(std::floor(srcRect.x0), std::floor(srcRect.y0),
                std::ceil(srcRect.x1), std::ceil(srcRect.y1));
}

//------------------------------------------------------------------------------

//! Renders the Source over inRect (see getSourceRect()) and warps it with the
//! deformer into the tile, whose raster is cleared if inRect is empty.
inline void warpSourceTile(TRasterFxPort &input, const TDeformer &deformer,
                           const TRectD &inRect, TTile &tile, double frame,
                           const TRenderSettings &ri) {
  TRasterP tileRas(tile.getRaster());
  if (inRect.isEmpty()) {
    tileRas->clear();
    return;
  }

  TTile inTile;
  input->allocateAndCompute(
      inTile, inRect.getP00(),
      TDimension(tround(inRect.getLx()), tround(inRect.getLy())), tileRas,
      frame, ri);

  deformer.warpRaster(tileRas, tile.m_pos, inTile.getRaster(), inTile.m_pos,
                      frame);
}

}  // namespace deformerfx

#endif  // DEFORMERFXCOMMON_H
