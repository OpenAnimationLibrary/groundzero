#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "deformerfxcommon.h"

#include "toonz/tdeformer.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

//==============================================================================
//    Envelope Deformer Plus Fx
//
// An Envelope Deformer (see envelopedeformerfx.cpp) with a variable number of
// points and optional curved edges. The Envelope Deformer Fx is left as it
// was, so that existing scenes render as before.
//
// The envelope has Point Count points, from 3 to 16 (8 by default, laid out
// like those of the Envelope Deformer): only point1 to pointN and rest1 to
// restN are used, and the others are kept for when the count grows. The Rest
// points place the envelope around the art; moving the Points away from them
// warps the art smoothly, following the mean value coordinates of the
// envelope (see TEnvelopeDeformer). With the Points on the Rest points, the
// default, the Source passes through unchanged.
//
// With Smooth Edges the envelope's edges are not straight: the points are the
// anchors of a closed Catmull-Rom spline, a chain of cubic Bezier edges. Both
// the rest and the deformed envelope are sampled (8 samples on each edge) and
// the mean value coordinates are made on the sampled polygons, so the edges
// are mapped onto each other, and an affine change of the points is still
// reproduced exactly.
//
// The warp is made on the rendered Source raster, so vector art is resampled
// rather than deformed as strokes. To deform the strokes of a vector level
// instead, use Level > Adjust > Bake Envelope into Vector Level, which applies
// the warp of the current frame to the control points of the strokes.
//==============================================================================

namespace {

enum { MAX_POINTS = 16, MIN_POINTS = 3, DEFAULT_POINTS = 8 };

//! Returns the default position of the envelope's slot: the first 8 are those
//! of the Envelope Deformer, a square of half side size, with a point on each
//! corner and on the middle of each side; the others are on the circle of
//! radius size, between the angles of the slots of a 16 points circle.
TPointD defaultPoint(int slot, double size) {
  const double s                       = size;
  const TPointD square[DEFAULT_POINTS] = {
      TPointD(-s, -s), TPointD(0.0, -s), TPointD(s, -s), TPointD(s, 0.0),
      TPointD(s, s),   TPointD(0.0, s),  TPointD(-s, s), TPointD(-s, 0.0)};
  if (slot < DEFAULT_POINTS) return square[slot];

  const double pi = 3.14159265358979323846;
  double angle    = 2.0 * pi * (slot + 0.5) / MAX_POINTS;
  return TPointD(size * std::cos(angle), size * std::sin(angle));
}

}  // namespace

//==============================================================================

class EnvelopeDeformerPlusFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(EnvelopeDeformerPlusFx)

  TRasterFxPort m_input;
  TIntParamP m_pointCount;
  TBoolParamP m_smooth;
  TPointParamP m_point[MAX_POINTS];  // The envelope, deformed
  TPointParamP m_rest[MAX_POINTS];   // The envelope in rest pose

public:
  EnvelopeDeformerPlusFx() : m_pointCount(DEFAULT_POINTS), m_smooth(false) {
    addInputPort("Source", m_input);

    m_pointCount->setValueRange(MIN_POINTS, MAX_POINTS);
    bindParam(this, "pointCount", m_pointCount);
    bindParam(this, "smooth", m_smooth);

    for (int i = 0; i < MAX_POINTS; ++i) {
      m_point[i] = TPointParamP(defaultPoint(i, 200.0));
      m_point[i]->getX()->setMeasureName("fxLength");
      m_point[i]->getY()->setMeasureName("fxLength");
      bindParam(this, "point" + std::to_string(i + 1), m_point[i]);
    }

    for (int i = 0; i < MAX_POINTS; ++i) {
      m_rest[i] = TPointParamP(defaultPoint(i, 200.0));
      m_rest[i]->getX()->setMeasureName("fxLength");
      m_rest[i]->getY()->setMeasureName("fxLength");
      bindParam(this, "rest" + std::to_string(i + 1), m_rest[i]);
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
  // Returns the number of points in use.
  int getPointCount() const {
    return std::min<int>(MAX_POINTS,
                         std::max<int>(MIN_POINTS, m_pointCount->getValue()));
  }

  // Returns the envelopes at frame in rest pose and deformed, in the fx's
  // space: the points in use, or the samples of their spline if smooth.
  void getEnvelopes(double frame, std::vector<TPointD> &rest,
                    std::vector<TPointD> &deformed) const;

  // Returns the pixel-aligned rect of the Source needed to render rect,
  // in the space of ri; empty when nothing of the Source shows in rect.
  TRectD getSourceRect(double frame, const TRectD &rect,
                       const TRenderSettings &ri);
};

//-------------------------------------------------------------------

void EnvelopeDeformerPlusFx::getEnvelopes(
    double frame, std::vector<TPointD> &rest,
    std::vector<TPointD> &deformed) const {
  int count = getPointCount();

  std::vector<TPointD> restAnchors(count), deformedAnchors(count);
  for (int i = 0; i < count; ++i) {
    restAnchors[i]     = m_rest[i]->getValue(frame);
    deformedAnchors[i] = m_point[i]->getValue(frame);
  }

  int samples =
      m_smooth->getValue() ? int(TEnvelopeDeformer::SPLINE_SAMPLES) : 1;
  TEnvelopeDeformer::sampleClosedSpline(restAnchors, samples, rest);
  TEnvelopeDeformer::sampleClosedSpline(deformedAnchors, samples, deformed);
}

//-------------------------------------------------------------------

bool EnvelopeDeformerPlusFx::doGetBBox(double frame, TRectD &bBox,
                                       const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);

  std::vector<TPointD> rest, deformed;
  getEnvelopes(frame, rest, deformed);
  if (deformerfx::isPassThrough(rest, deformed)) return ret;

  // An infinite bbox stays infinite
  TEnvelopeDeformer deformer(rest, deformed);
  bBox = deformer.deformedBounds(bBox, frame);
  return ret;
}

//-------------------------------------------------------------------

TRectD EnvelopeDeformerPlusFx::getSourceRect(double frame, const TRectD &rect,
                                             const TRenderSettings &ri) {
  if (rect.isEmpty()) return TRectD();

  std::vector<TPointD> rest, deformed;
  getEnvelopes(frame, rest, deformed);
  if (deformerfx::isPassThrough(rest, deformed)) return rect;

  deformerfx::mapPoints(ri.m_affine, rest);
  deformerfx::mapPoints(ri.m_affine, deformed);
  TEnvelopeDeformer deformer(rest, deformed);

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  return deformerfx::getSourceRect(deformer, rest, frame, rect, inBBox);
}

//-------------------------------------------------------------------

void EnvelopeDeformerPlusFx::transform(double frame, int port,
                                       const TRectD &rectOnOutput,
                                       const TRenderSettings &infoOnOutput,
                                       TRectD &rectOnInput,
                                       TRenderSettings &infoOnInput) {
  infoOnInput = infoOnOutput;
  rectOnInput = getSourceRect(frame, rectOnOutput, infoOnOutput);
}

//-------------------------------------------------------------------

int EnvelopeDeformerPlusFx::getMemoryRequirement(const TRectD &rect,
                                                 double frame,
                                                 const TRenderSettings &info) {
  if (!m_input.isConnected()) return 0;

  return TRasterFx::memorySize(getSourceRect(frame, rect, info), info.m_bpp);
}

//-------------------------------------------------------------------

void EnvelopeDeformerPlusFx::doCompute(TTile &tile, double frame,
                                       const TRenderSettings &ri) {
  if (!m_input.isConnected()) return;

  std::vector<TPointD> rest, deformed;
  getEnvelopes(frame, rest, deformed);
  if (deformerfx::isPassThrough(rest, deformed)) {
    m_input->compute(tile, frame, ri);
    return;
  }

  TRasterP tileRas(tile.getRaster());
  TRectD tileRect(tile.m_pos, TDimensionD(tileRas->getLx(), tileRas->getLy()));

  TRectD inRect = getSourceRect(frame, tileRect, ri);

  // The deformer is built in the render's space, which differs from the fx's
  // by a similarity (see canHandle()).
  deformerfx::mapPoints(ri.m_affine, rest);
  deformerfx::mapPoints(ri.m_affine, deformed);
  TEnvelopeDeformer deformer(rest, deformed);

  deformerfx::warpSourceTile(m_input, deformer, inRect, tile, frame, ri);
}

//-------------------------------------------------------------------

void EnvelopeDeformerPlusFx::getParamUIs(TParamUIConcept *&concepts,
                                         int &length) {
  // The gadgets of the points in use. The rest points come first: the viewer
  // picks the gadget drawn last, so the deformed points are dragged where they
  // lie on the rest ones.
  int count = getPointCount();
  concepts  = new TParamUIConcept[length = 3 * count];

  for (int i = 0; i < count; ++i) {
    std::string suffix = std::to_string(i + 1);

    TParamUIConcept &rest = concepts[i];
    rest.m_type           = TParamUIConcept::POINT;
    rest.m_label          = "Rest " + suffix;
    rest.m_params.push_back(m_rest[i]);

    TParamUIConcept &offset = concepts[count + i];
    offset.m_type           = TParamUIConcept::VECTOR;
    offset.m_params.push_back(m_rest[i]);
    offset.m_params.push_back(m_point[i]);

    TParamUIConcept &point = concepts[2 * count + i];
    point.m_type           = TParamUIConcept::POINT;
    point.m_label          = "Point " + suffix;
    point.m_params.push_back(m_point[i]);
  }
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(EnvelopeDeformerPlusFx, "envelopeDeformerPlusFx");
