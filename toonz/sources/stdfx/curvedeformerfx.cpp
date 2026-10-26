#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "deformerfxcommon.h"
#include "deformerparams.h"

#include "toonz/tdeformer.h"

#include <memory>
#include <vector>

//==============================================================================
//    Curve Deformer Fx
//
// Bends the Source along a Bezier curve, similar to Toon Boom Harmony's Curve
// deformer. The curve goes through 4 anchors, each with a Point and the two
// tangent handles, In and Out, that shape the curve around it (the In handle
// of the first anchor and the Out handle of the last one are not used). The
// Rest points place the curve along the art; moving the Points, In and Out
// handles away from them bends the art along the new curve (see
// TCurveDeformer): a point of the Source keeps its place along the curve, as
// a fraction of its length, and its distance from it. With the Points on the
// Rest points, the default, the Source passes through unchanged.
//
// The deformation applies in full within Width of the rest curve, falls
// smoothly to nothing at Falloff, and the rest of the Source stays put.
//
// The warp is made on the rendered Source raster, so vector art is resampled
// rather than deformed as strokes. The curve has 4 anchors; where it bends
// more tightly than the Width the art folds over itself. The parameters and
// the deformer are made by deformerfx::CurveDeformerParams, which a Curve
// layer of the Deformer Stack Fx shares.
//==============================================================================

namespace {

enum { ANCHOR_COUNT = 4 };

// Half the length of the default curve: a straight line along the x axis
const double c_defaultHalfLength = 200.0;

}  // namespace

//==============================================================================

class CurveDeformerFx final : public TStandardRasterFx, public TDeformerSource {
  FX_PLUGIN_DECLARATION(CurveDeformerFx)

  TRasterFxPort m_input;
  deformerfx::CurveDeformerParams m_curve;

public:
  CurveDeformerFx() {
    addInputPort("Source", m_input);

    // By default, the curve is a straight line across the origin
    m_curve.bind(this, "", ANCHOR_COUNT, c_defaultHalfLength);

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
  // The deformer of the curve at frame, for Bake Deformers into Vector Level
  // (see TDeformerSource)
  std::shared_ptr<const TDeformer> makeDeformer(
      double frame, const TAffine &placement) const override {
    std::vector<TPointD> rest;
    return m_curve.buildDeformer(frame, placement, rest);
  }
};

//-------------------------------------------------------------------

bool CurveDeformerFx::doGetBBox(double frame, TRectD &bBox,
                                const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);

  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_curve.buildDeformer(frame, TAffine(), rest);
  if (!deformer) return ret;

  // An infinite bbox stays infinite
  bBox = deformer->deformedBounds(bBox, frame);
  return ret;
}

//-------------------------------------------------------------------

TRectD CurveDeformerFx::getSourceRect(double frame, const TRectD &rect,
                                      const TRenderSettings &ri) {
  if (rect.isEmpty()) return TRectD();

  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_curve.buildDeformer(frame, ri.m_affine, rest);
  if (!deformer) return rect;

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  return deformerfx::getSourceRect(*deformer, rest, frame, rect, inBBox);
}

//-------------------------------------------------------------------

void CurveDeformerFx::transform(double frame, int port,
                                const TRectD &rectOnOutput,
                                const TRenderSettings &infoOnOutput,
                                TRectD &rectOnInput,
                                TRenderSettings &infoOnInput) {
  infoOnInput = infoOnOutput;
  rectOnInput = getSourceRect(frame, rectOnOutput, infoOnOutput);
}

//-------------------------------------------------------------------

int CurveDeformerFx::getMemoryRequirement(const TRectD &rect, double frame,
                                          const TRenderSettings &info) {
  if (!m_input.isConnected()) return 0;

  return TRasterFx::memorySize(getSourceRect(frame, rect, info), info.m_bpp);
}

//-------------------------------------------------------------------

void CurveDeformerFx::doCompute(TTile &tile, double frame,
                                const TRenderSettings &ri) {
  if (!m_input.isConnected()) return;

  // The deformer is built in the render's space, which differs from the fx's
  // by a similarity (see canHandle()).
  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_curve.buildDeformer(frame, ri.m_affine, rest);
  if (!deformer) {
    m_input->compute(tile, frame, ri);
    return;
  }

  TRasterP tileRas(tile.getRaster());
  TRectD tileRect(tile.m_pos, TDimensionD(tileRas->getLx(), tileRas->getLy()));

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  TRectD inRect =
      deformerfx::getSourceRect(*deformer, rest, frame, tileRect, inBBox);

  deformerfx::warpSourceTile(m_input, *deformer, inRect, tile, frame, ri);
}

//-------------------------------------------------------------------

void CurveDeformerFx::getParamUIs(TParamUIConcept *&concepts, int &length) {
  // The rest curve comes first: the viewer picks the gadget drawn last, so the
  // deformed points are dragged where they lie on the rest ones.
  concepts = new TParamUIConcept[m_curve.maxConceptCount()];
  length   = m_curve.getParamUIs(concepts, "");
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(CurveDeformerFx, "curveDeformerFx");
