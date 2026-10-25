#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"

#include "toonz/tdeformer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

//==============================================================================
//    Envelope Deformer Fx
//
// Bends the Source with a closed envelope of 8 points, similar to Toon Boom
// Harmony's Envelope deformer. The Rest points place the envelope around the
// art; moving the Points away from them warps the art smoothly, following the
// mean value coordinates of the envelope (see TEnvelopeDeformer). With the
// Points on the Rest points, the default, the Source passes through unchanged.
//
// The warp is made on the rendered Source raster, so vector art is resampled
// rather than deformed as strokes, and the deformer does not stack with other
// deformers in a deformation chain.
//==============================================================================

namespace {

void mapPoints(const TAffine &aff, std::vector<TPointD> &points) {
  for (TPointD &p : points) p = aff * p;
}

}  // namespace

//==============================================================================

class EnvelopeDeformerFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(EnvelopeDeformerFx)

  enum { POINT_COUNT = 8 };

  TRasterFxPort m_input;
  TPointParamP m_point[POINT_COUNT];  // The envelope, deformed
  TPointParamP m_rest[POINT_COUNT];   // The envelope in rest pose

public:
  EnvelopeDeformerFx() {
    addInputPort("Source", m_input);

    // By default, the envelope is a square around the origin, with a point on
    // each corner and on the middle of each side.
    const double s                      = 200.0;
    const TPointD defaults[POINT_COUNT] = {
        TPointD(-s, -s), TPointD(0.0, -s), TPointD(s, -s), TPointD(s, 0.0),
        TPointD(s, s),   TPointD(0.0, s),  TPointD(-s, s), TPointD(-s, 0.0)};

    for (int i = 0; i < POINT_COUNT; ++i) {
      std::string suffix = std::to_string(i + 1);

      m_point[i] = TPointParamP(defaults[i]);
      m_point[i]->getX()->setMeasureName("fxLength");
      m_point[i]->getY()->setMeasureName("fxLength");
      bindParam(this, "point" + suffix, m_point[i]);
    }

    for (int i = 0; i < POINT_COUNT; ++i) {
      std::string suffix = std::to_string(i + 1);

      m_rest[i] = TPointParamP(defaults[i]);
      m_rest[i]->getX()->setMeasureName("fxLength");
      m_rest[i]->getY()->setMeasureName("fxLength");
      bindParam(this, "rest" + suffix, m_rest[i]);
    }

    enableComputeInFloat(true);
  }

  bool doGetBBox(double frame, TRectD &bBox,
                 const TRenderSettings &info) override;

  void doCompute(TTile &tile, double frame, const TRenderSettings &ri) override;

  void transform(double frame, int port, const TRectD &rectOnOutput,
                 const TRenderSettings &infoOnOutput, TRectD &rectOnInput,
                 TRenderSettings &infoOnInput) override;

  // The deformation is invariant under similarities only: let the other
  // parts of the affine be applied on the result.
  bool canHandle(const TRenderSettings &info, double frame) override {
    return false;
  }

  int getMemoryRequirement(const TRectD &rect, double frame,
                           const TRenderSettings &info) override;

  void getParamUIs(TParamUIConcept *&concepts, int &length) override;

private:
  // Returns the envelope at frame in rest pose and deformed, in the fx's
  // space.
  void getEnvelopes(double frame, std::vector<TPointD> &rest,
                    std::vector<TPointD> &deformed) const;

  // Returns whether the Source passes through unchanged: the envelope is not
  // moved from its rest pose, or the rest pose is degenerate.
  static bool isPassThrough(const std::vector<TPointD> &rest,
                            const std::vector<TPointD> &deformed);

  // Returns the pixel-aligned rect of the Source needed to render rect,
  // in the space of ri; empty when nothing of the Source shows in rect.
  TRectD getSourceRect(double frame, const TRectD &rect,
                       const TRenderSettings &ri);
};

//-------------------------------------------------------------------

void EnvelopeDeformerFx::getEnvelopes(double frame, std::vector<TPointD> &rest,
                                      std::vector<TPointD> &deformed) const {
  rest.resize(POINT_COUNT);
  deformed.resize(POINT_COUNT);
  for (int i = 0; i < POINT_COUNT; ++i) {
    rest[i]     = m_rest[i]->getValue(frame);
    deformed[i] = m_point[i]->getValue(frame);
  }
}

//-------------------------------------------------------------------

bool EnvelopeDeformerFx::isPassThrough(const std::vector<TPointD> &rest,
                                       const std::vector<TPointD> &deformed) {
  TEnvelopeDeformer deformer(rest, deformed);
  return !deformer.isValid() || deformer.isIdentity(1e-6);
}

//-------------------------------------------------------------------

bool EnvelopeDeformerFx::doGetBBox(double frame, TRectD &bBox,
                                   const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);

  std::vector<TPointD> rest, deformed;
  getEnvelopes(frame, rest, deformed);
  if (isPassThrough(rest, deformed)) return ret;

  // An infinite bbox stays infinite
  TEnvelopeDeformer deformer(rest, deformed);
  bBox = deformer.deformedBounds(bBox, frame);
  return ret;
}

//-------------------------------------------------------------------

TRectD EnvelopeDeformerFx::getSourceRect(double frame, const TRectD &rect,
                                         const TRenderSettings &ri) {
  if (rect.isEmpty()) return TRectD();

  std::vector<TPointD> rest, deformed;
  getEnvelopes(frame, rest, deformed);
  if (isPassThrough(rest, deformed)) return rect;

  mapPoints(ri.m_affine, rest);
  mapPoints(ri.m_affine, deformed);
  TEnvelopeDeformer deformer(rest, deformed);

  // Enlarged for the neighbours of the bilinear samples
  TRectD srcRect = deformer.sourceBounds(rect, frame);
  if (srcRect.isEmpty()) return TRectD();
  srcRect = srcRect.enlarge(2.0);

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
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

//-------------------------------------------------------------------

void EnvelopeDeformerFx::transform(double frame, int port,
                                   const TRectD &rectOnOutput,
                                   const TRenderSettings &infoOnOutput,
                                   TRectD &rectOnInput,
                                   TRenderSettings &infoOnInput) {
  infoOnInput = infoOnOutput;
  rectOnInput = getSourceRect(frame, rectOnOutput, infoOnOutput);
}

//-------------------------------------------------------------------

int EnvelopeDeformerFx::getMemoryRequirement(const TRectD &rect, double frame,
                                             const TRenderSettings &info) {
  if (!m_input.isConnected()) return 0;

  return TRasterFx::memorySize(getSourceRect(frame, rect, info), info.m_bpp);
}

//-------------------------------------------------------------------

void EnvelopeDeformerFx::doCompute(TTile &tile, double frame,
                                   const TRenderSettings &ri) {
  if (!m_input.isConnected()) return;

  std::vector<TPointD> rest, deformed;
  getEnvelopes(frame, rest, deformed);
  if (isPassThrough(rest, deformed)) {
    m_input->compute(tile, frame, ri);
    return;
  }

  TRasterP tileRas(tile.getRaster());
  TRectD tileRect(tile.m_pos, TDimensionD(tileRas->getLx(), tileRas->getLy()));

  TRectD inRect = getSourceRect(frame, tileRect, ri);
  if (inRect.isEmpty()) {
    tileRas->clear();
    return;
  }

  TTile inTile;
  m_input->allocateAndCompute(
      inTile, inRect.getP00(),
      TDimension(tround(inRect.getLx()), tround(inRect.getLy())), tileRas,
      frame, ri);

  // The deformer is built in the render's space, which differs from the fx's
  // by a similarity (see canHandle()).
  mapPoints(ri.m_affine, rest);
  mapPoints(ri.m_affine, deformed);
  TEnvelopeDeformer deformer(rest, deformed);

  deformer.warpRaster(tileRas, tile.m_pos, inTile.getRaster(), inTile.m_pos,
                      frame);
}

//-------------------------------------------------------------------

void EnvelopeDeformerFx::getParamUIs(TParamUIConcept *&concepts, int &length) {
  concepts = new TParamUIConcept[length = 3 * POINT_COUNT];

  // The rest points come first: the viewer picks the gadget drawn last, so
  // the deformed points are dragged where they lie on the rest ones.
  for (int i = 0; i < POINT_COUNT; ++i) {
    std::string suffix = std::to_string(i + 1);

    TParamUIConcept &rest = concepts[i];
    rest.m_type           = TParamUIConcept::POINT;
    rest.m_label          = "Rest " + suffix;
    rest.m_params.push_back(m_rest[i]);

    TParamUIConcept &offset = concepts[POINT_COUNT + i];
    offset.m_type           = TParamUIConcept::VECTOR;
    offset.m_params.push_back(m_rest[i]);
    offset.m_params.push_back(m_point[i]);

    TParamUIConcept &point = concepts[2 * POINT_COUNT + i];
    point.m_type           = TParamUIConcept::POINT;
    point.m_label          = "Point " + suffix;
    point.m_params.push_back(m_point[i]);
  }
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(EnvelopeDeformerFx, "envelopeDeformerFx");
