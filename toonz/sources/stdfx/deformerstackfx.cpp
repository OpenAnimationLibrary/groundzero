#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "deformerfxcommon.h"

#include "toonz/tdeformer.h"
#include "toonz/tdeformerstack.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

//==============================================================================
//    Deformer Stack Fx
//
// Bends the Source with up to 3 envelope deformers (see the Envelope Deformer
// Fx), applied in order - A, then B, then C - each on the result of the
// previous one and blended by its own weight (see TDeformerStack). A layer is
// a closed envelope of 8 Points and their Rest points, whose names start with
// the layer's letter: a_point1, a_rest1, b_point1 and so on.
//
// A layer's Weight is how much of its deformation applies: 0 leaves the Source
// as it is, 1 applies the whole envelope. With Falloff on, the weight is the
// Weight within the Inner Radius of the Falloff Center, and falls smoothly to
// 0 at the Outer Radius, so the layer acts only around that point.
//
// With every layer on its Rest points, the default, or with a weight of 0, the
// Source passes through unchanged. The layers start as nested squares so that
// their gadgets in the viewer are not all on top of each other.
//
// The warp is made on the rendered Source raster, so vector art is resampled
// rather than deformed as strokes. Only envelope layers exist for now: bone,
// curve and free-form layers will come as other TDeformer types.
//==============================================================================

namespace {

enum { LAYER_COUNT = 3 };

// Letters and half side of the default square of each layer
const char *const c_layerNames[LAYER_COUNT]  = {"a", "b", "c"};
const char *const c_layerLabels[LAYER_COUNT] = {"A", "B", "C"};
const double c_layerSizes[LAYER_COUNT]       = {200.0, 160.0, 120.0};

}  // namespace

//==============================================================================

class DeformerStackFx final : public TStandardRasterFx, public TDeformerSource {
  FX_PLUGIN_DECLARATION(DeformerStackFx)

  struct Layer {
    deformerfx::EnvelopeParams m_envelope;
    TDoubleParamP m_weight;
    TBoolParamP m_falloff;
    TPointParamP m_center;
    TDoubleParamP m_inner, m_outer;
  };

  TRasterFxPort m_input;
  Layer m_layers[LAYER_COUNT];

public:
  DeformerStackFx() {
    addInputPort("Source", m_input);

    for (int i = 0; i < LAYER_COUNT; ++i) {
      Layer &layer       = m_layers[i];
      std::string prefix = std::string(c_layerNames[i]) + "_";

      layer.m_envelope.bind(this, prefix, c_layerSizes[i]);

      layer.m_weight = TDoubleParamP(1.0);
      layer.m_weight->setValueRange(0.0, 1.0, 0.01);
      bindParam(this, prefix + "weight", layer.m_weight);

      layer.m_falloff = TBoolParamP(false);
      bindParam(this, prefix + "falloff", layer.m_falloff);

      layer.m_center = TPointParamP(TPointD(0.0, 0.0));
      layer.m_center->getX()->setMeasureName("fxLength");
      layer.m_center->getY()->setMeasureName("fxLength");
      bindParam(this, prefix + "center", layer.m_center);

      layer.m_inner = TDoubleParamP(100.0);
      layer.m_inner->setMeasureName("fxLength");
      layer.m_inner->setValueRange(0.0, 1e6);
      bindParam(this, prefix + "inner", layer.m_inner);

      layer.m_outer = TDoubleParamP(300.0);
      layer.m_outer->setMeasureName("fxLength");
      layer.m_outer->setValueRange(0.0, 1e6);
      bindParam(this, prefix + "outer", layer.m_outer);
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
  // Fills stack with the layers that change the Source at frame, in the space
  // that aff maps the fx's space to, and rest with their rest points in that
  // space. Returns the number of layers: 0 when the Source passes through
  // unchanged.
  int buildStack(double frame, const TAffine &aff, TDeformerStack &stack,
                 std::vector<TPointD> &rest) const;

  // Returns the pixel-aligned rect of the Source needed to render rect,
  // in the space of ri; empty when nothing of the Source shows in rect.
  TRectD getSourceRect(double frame, const TRectD &rect,
                       const TRenderSettings &ri);

public:
  // The deformer of the layers at frame, for Bake Deformers into Vector
  // Level (see TDeformerSource): whatever layers buildStack() makes, which
  // are the envelope layers for now
  std::unique_ptr<TDeformer> makeDeformer(
      double frame, const TAffine &placement) const override {
    std::unique_ptr<TDeformerStack> stack(new TDeformerStack);
    std::vector<TPointD> rest;
    if (!buildStack(frame, placement, *stack, rest)) return nullptr;
    return std::unique_ptr<TDeformer>(stack.release());
  }
};

//-------------------------------------------------------------------

int DeformerStackFx::buildStack(double frame, const TAffine &aff,
                                TDeformerStack &stack,
                                std::vector<TPointD> &rest) const {
  // The falloff radii follow the scale of the affine, a similarity
  double radiusScale = std::sqrt(std::fabs(aff.det()));

  for (int i = 0; i < LAYER_COUNT; ++i) {
    const Layer &layer = m_layers[i];

    double weight = layer.m_weight->getValue(frame);
    if (!(weight > 0.0)) continue;

    std::vector<TPointD> layerRest, layerDeformed;
    layer.m_envelope.getEnvelopes(frame, layerRest, layerDeformed);
    if (deformerfx::isPassThrough(layerRest, layerDeformed)) continue;

    deformerfx::mapPoints(aff, layerRest);
    deformerfx::mapPoints(aff, layerDeformed);

    TDeformerWeightMap weights = TDeformerWeightMap::makeUniform(weight);
    if (layer.m_falloff->getValue())
      weights = TDeformerWeightMap::makeDistance(
          weight, aff * layer.m_center->getValue(frame),
          layer.m_inner->getValue(frame) * radiusScale,
          layer.m_outer->getValue(frame) * radiusScale);

    stack.addLayer(
        std::make_shared<TEnvelopeDeformer>(layerRest, layerDeformed), weights);
    rest.insert(rest.end(), layerRest.begin(), layerRest.end());
  }

  // The tolerances of the inverse follow the size of the envelopes
  if (stack.getLayerCount() > 0)
    stack.setScale(deformerfx::pointsDiagonal(rest));
  return stack.getLayerCount();
}

//-------------------------------------------------------------------

bool DeformerStackFx::doGetBBox(double frame, TRectD &bBox,
                                const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);

  TDeformerStack stack;
  std::vector<TPointD> rest;
  if (!buildStack(frame, TAffine(), stack, rest)) return ret;

  // An infinite bbox stays infinite
  bBox = stack.deformedBounds(bBox, frame);
  return ret;
}

//-------------------------------------------------------------------

TRectD DeformerStackFx::getSourceRect(double frame, const TRectD &rect,
                                      const TRenderSettings &ri) {
  if (rect.isEmpty()) return TRectD();

  TDeformerStack stack;
  std::vector<TPointD> rest;
  if (!buildStack(frame, ri.m_affine, stack, rest)) return rect;

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  return deformerfx::getSourceRect(stack, rest, frame, rect, inBBox);
}

//-------------------------------------------------------------------

void DeformerStackFx::transform(double frame, int port,
                                const TRectD &rectOnOutput,
                                const TRenderSettings &infoOnOutput,
                                TRectD &rectOnInput,
                                TRenderSettings &infoOnInput) {
  infoOnInput = infoOnOutput;
  rectOnInput = getSourceRect(frame, rectOnOutput, infoOnOutput);
}

//-------------------------------------------------------------------

int DeformerStackFx::getMemoryRequirement(const TRectD &rect, double frame,
                                          const TRenderSettings &info) {
  if (!m_input.isConnected()) return 0;

  return TRasterFx::memorySize(getSourceRect(frame, rect, info), info.m_bpp);
}

//-------------------------------------------------------------------

void DeformerStackFx::doCompute(TTile &tile, double frame,
                                const TRenderSettings &ri) {
  if (!m_input.isConnected()) return;

  // The stack is built in the render's space, which differs from the fx's by
  // a similarity (see canHandle()).
  TDeformerStack stack;
  std::vector<TPointD> rest;
  if (!buildStack(frame, ri.m_affine, stack, rest)) {
    m_input->compute(tile, frame, ri);
    return;
  }

  TRasterP tileRas(tile.getRaster());
  TRectD tileRect(tile.m_pos, TDimensionD(tileRas->getLx(), tileRas->getLy()));

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  TRectD inRect =
      deformerfx::getSourceRect(stack, rest, frame, tileRect, inBBox);

  deformerfx::warpSourceTile(m_input, stack, inRect, tile, frame, ri);
}

//-------------------------------------------------------------------

void DeformerStackFx::getParamUIs(TParamUIConcept *&concepts, int &length) {
  const int layerLength = 3 * deformerfx::POINT_COUNT + 3;
  concepts = new TParamUIConcept[length = LAYER_COUNT * layerLength];

  for (int i = 0; i < LAYER_COUNT; ++i) {
    const Layer &layer = m_layers[i];
    std::string label  = std::string(c_layerLabels[i]) + " ";

    TParamUIConcept *first = concepts + i * layerLength;
    layer.m_envelope.getParamUIs(first, label);

    // The center of the falloff and its two radii
    TParamUIConcept *falloff = first + 3 * deformerfx::POINT_COUNT;

    falloff[0].m_type  = TParamUIConcept::POINT;
    falloff[0].m_label = label + "Falloff Center";
    falloff[0].m_params.push_back(layer.m_center);

    falloff[1].m_type  = TParamUIConcept::RADIUS;
    falloff[1].m_label = label + "Inner Radius";
    falloff[1].m_params.push_back(layer.m_inner);
    falloff[1].m_params.push_back(layer.m_center);

    falloff[2].m_type  = TParamUIConcept::RADIUS;
    falloff[2].m_label = label + "Outer Radius";
    falloff[2].m_params.push_back(layer.m_outer);
    falloff[2].m_params.push_back(layer.m_center);
  }
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(DeformerStackFx, "deformerStackFx");
