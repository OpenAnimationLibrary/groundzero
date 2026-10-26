#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "deformerfxcommon.h"

#include "toonz/tdeformer.h"

#include <memory>
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
// rather than deformed as strokes. To stack it with other deformers, see the
// Deformer Stack Fx, which shares its envelope parameters and render code
// (deformerfxcommon.h).
//==============================================================================

class EnvelopeDeformerFx final : public TStandardRasterFx,
                                 public TDeformerSource {
  FX_PLUGIN_DECLARATION(EnvelopeDeformerFx)

  TRasterFxPort m_input;
  deformerfx::EnvelopeParams m_envelope;

public:
  EnvelopeDeformerFx() {
    addInputPort("Source", m_input);

    // By default, the envelope is a square around the origin, with a point on
    // each corner and on the middle of each side.
    m_envelope.bind(this, "", 200.0);

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
  // Returns the pixel-aligned rect of the Source needed to render rect,
  // in the space of ri; empty when nothing of the Source shows in rect.
  TRectD getSourceRect(double frame, const TRectD &rect,
                       const TRenderSettings &ri);

public:
  // The deformer of the envelope at frame, for Bake Deformers into Vector
  // Level (see TDeformerSource)
  std::unique_ptr<TDeformer> makeDeformer(
      double frame, const TAffine &placement) const override {
    std::vector<TPointD> rest, deformed;
    m_envelope.getEnvelopes(frame, rest, deformed);
    if (deformerfx::isPassThrough(rest, deformed)) return nullptr;

    deformerfx::mapPoints(placement, rest);
    deformerfx::mapPoints(placement, deformed);
    return std::unique_ptr<TDeformer>(new TEnvelopeDeformer(rest, deformed));
  }
};

//-------------------------------------------------------------------

bool EnvelopeDeformerFx::doGetBBox(double frame, TRectD &bBox,
                                   const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);

  std::vector<TPointD> rest, deformed;
  m_envelope.getEnvelopes(frame, rest, deformed);
  if (deformerfx::isPassThrough(rest, deformed)) return ret;

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
  m_envelope.getEnvelopes(frame, rest, deformed);
  if (deformerfx::isPassThrough(rest, deformed)) return rect;

  deformerfx::mapPoints(ri.m_affine, rest);
  deformerfx::mapPoints(ri.m_affine, deformed);
  TEnvelopeDeformer deformer(rest, deformed);

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  return deformerfx::getSourceRect(deformer, rest, frame, rect, inBBox);
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
  m_envelope.getEnvelopes(frame, rest, deformed);
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

void EnvelopeDeformerFx::getParamUIs(TParamUIConcept *&concepts, int &length) {
  concepts = new TParamUIConcept[length = 3 * deformerfx::POINT_COUNT];
  m_envelope.getParamUIs(concepts, "");
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(EnvelopeDeformerFx, "envelopeDeformerFx");
