#include "stdfx.h"
#include "tfxparam.h"
#include "tnotanimatableparam.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "deformerfxcommon.h"
#include "deformerparams.h"

#include "toonz/tdeformer.h"
#include "toonz/tdeformerstack.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

//==============================================================================
//    Deformer Stack Fx
//
// Bends the Source with up to 3 deformers, applied in order - A, then B, then
// C - each on the result of the previous one and blended by its own weight
// (see TDeformerStack).
//
// Each layer has a Type: Envelope (the default), Curve, Free-Form, Bone or
// Shape-Aware. An Envelope layer is a closed envelope of 8 Points and their
// Rest points, as in the Envelope Deformer Fx, whose names start with the
// layer's letter: a_point1, a_rest1, b_point1 and so on. The other types are
// the deformers of the Curve, Free-Form, Bone and Shape Aware Deformer Fxs,
// built by the same code (deformerparams.h), with fewer points: a curve of 3
// anchors, a lattice of 3 x 3 points, a chain of 2 bones, and 2 pins, 1 bone
// and a cage of 4 corners. Their parameters are named after the layer and the
// type: a_curve_point1, a_freeForm_center, a_bone_rest1, a_shapeAware_pin1
// and so on. A layer uses the parameters of its type only; the others are
// kept for when the type changes.
//
// A layer's Weight is how much of its deformation applies: 0 leaves the Source
// as it is, 1 applies the whole deformer. With Falloff on, the weight is the
// Weight within the Inner Radius of the Falloff Center, and falls smoothly to
// 0 at the Outer Radius, so the layer acts only around that point.
//
// With every layer on its rest pose, the default, or with a weight of 0, the
// Source passes through unchanged. The layers start as nested shapes so that
// their gadgets in the viewer are not all on top of each other. The viewer
// shows the gadgets of each layer's type (and of its falloff), as they are
// when the fx is made current: after changing a Type, select the fx again to
// see the gadgets of the new type.
//
// Compatibility: the parameters of the Envelope layers are unchanged. A
// layer's Type is saved only when it is not Envelope, and the parameters of a
// type only for the layers of that type (see isParamSaved()), so a stack of
// envelopes saves as before. Older builds read a layer of another type as an
// Envelope layer.
//
// The warp is made on the rendered Source raster, so vector art is resampled
// rather than deformed as strokes.
//==============================================================================

namespace {

enum { LAYER_COUNT = 3 };

// Letters and half side of the default square of each layer
const char *const c_layerNames[LAYER_COUNT]  = {"a", "b", "c"};
const char *const c_layerLabels[LAYER_COUNT] = {"A", "B", "C"};
const double c_layerSizes[LAYER_COUNT]       = {200.0, 160.0, 120.0};

// The layer types, the values of the Type parameter
enum LayerType {
  ENVELOPE_LAYER = 0,
  CURVE_LAYER,
  FREEFORM_LAYER,
  BONE_LAYER,
  SHAPEAWARE_LAYER,
  LAYER_TYPE_COUNT
};

// The names that follow the layer's letter in the names of the parameters of
// each type (the Envelope ones have none, as before the types), and the
// names of the types in the fx settings
const char *const c_typeNames[LAYER_TYPE_COUNT]  = {"", "curve", "freeForm",
                                                    "bone", "shapeAware"};
const char *const c_typeLabels[LAYER_TYPE_COUNT] = {
    "Envelope", "Curve", "Free-Form", "Bone", "Shape-Aware"};

// The sizes of the layers of each type
enum {
  CURVE_ANCHORS      = 3,
  FREEFORM_CELLS     = 2,
  BONE_JOINTS        = 3,
  SHAPEAWARE_PINS    = 2,
  SHAPEAWARE_BONES   = 1,
  SHAPEAWARE_CORNERS = 4
};

bool startsWith(const std::string &s, const std::string &prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

//==============================================================================

class DeformerStackFx final : public TStandardRasterFx, public TDeformerSource {
  FX_PLUGIN_DECLARATION(DeformerStackFx)

  struct Layer {
    TIntEnumParamP m_type;
    deformerfx::EnvelopeParams m_envelope;
    deformerfx::CurveDeformerParams m_curve;
    deformerfx::FreeFormParams m_freeForm;
    deformerfx::BoneChainParams m_bone;
    deformerfx::ShapeAwareParams m_shapeAware;
    TDoubleParamP m_weight;
    TBoolParamP m_falloff;
    TPointParamP m_center;
    TDoubleParamP m_inner, m_outer;

    //! Returns the layer's type, a LayerType.
    int getType() const {
      int type = m_type->getValue();
      return (type >= 0 && type < LAYER_TYPE_COUNT) ? type : ENVELOPE_LAYER;
    }

    //! Builds the deformer of the layer's type at frame, in the space that
    //! aff maps the fx's space to, and fills rest with its rest points in
    //! that space. Returns null when the layer leaves the Source unchanged.
    std::shared_ptr<const TDeformer> buildDeformer(
        double frame, const TAffine &aff, std::vector<TPointD> &rest) const {
      switch (getType()) {
      case CURVE_LAYER:
        return m_curve.buildDeformer(frame, aff, rest);
      case FREEFORM_LAYER:
        return m_freeForm.buildDeformer(frame, aff, rest);
      case BONE_LAYER:
        return m_bone.buildDeformer(frame, aff, rest);
      case SHAPEAWARE_LAYER:
        return m_shapeAware.buildDeformer(frame, aff, rest);
      default:
        return deformerfx::buildEnvelopeDeformer(m_envelope, frame, aff, rest);
      }
    }

    //! Returns the number of gadgets of the points of the passed type.
    int getConceptCount(int type) const {
      switch (type) {
      case CURVE_LAYER:
        return m_curve.maxConceptCount();
      case FREEFORM_LAYER:
        return m_freeForm.conceptCount();
      case BONE_LAYER:
        return m_bone.conceptCount();
      case SHAPEAWARE_LAYER:
        return m_shapeAware.conceptCount();
      default:
        return 3 * deformerfx::POINT_COUNT;
      }
    }

    //! Fills concepts with the gadgets of the points of the passed type,
    //! whose labels start with label, and returns how many (at most
    //! getConceptCount()).
    int getParamUIs(int type, TParamUIConcept *concepts,
                    const std::string &label) const {
      switch (type) {
      case CURVE_LAYER:
        return m_curve.getParamUIs(concepts, label);
      case FREEFORM_LAYER:
        return m_freeForm.getParamUIs(concepts, label);
      case BONE_LAYER:
        return m_bone.getParamUIs(concepts, label);
      case SHAPEAWARE_LAYER:
        return m_shapeAware.getParamUIs(concepts, label);
      default:
        m_envelope.getParamUIs(concepts, label);
        return 3 * deformerfx::POINT_COUNT;
      }
    }
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

      // The type, and the parameters of the types other than Envelope
      layer.m_type = TIntEnumParamP(ENVELOPE_LAYER, c_typeLabels[0]);
      for (int t = 1; t < LAYER_TYPE_COUNT; ++t)
        layer.m_type->addItem(t, c_typeLabels[t]);
      bindParam(this, prefix + "type", layer.m_type);

      const double size = c_layerSizes[i];
      layer.m_curve.bind(this, typePrefix(i, CURVE_LAYER), CURVE_ANCHORS, size);
      layer.m_freeForm.bind(this, typePrefix(i, FREEFORM_LAYER), FREEFORM_CELLS,
                            2.0 * size);
      layer.m_bone.bind(this, typePrefix(i, BONE_LAYER), BONE_JOINTS, size);
      layer.m_shapeAware.bind(this, typePrefix(i, SHAPEAWARE_LAYER),
                              SHAPEAWARE_PINS, SHAPEAWARE_BONES,
                              SHAPEAWARE_CORNERS, size / 200.0);
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

  // The layer types other than Envelope leave the saved scene unchanged until
  // they are used: a layer's type is saved when it is not Envelope, and the
  // parameters of a type only for the layers of that type.
  bool isParamSaved(const TParamVar *paramVar) const override;

private:
  // Returns the start of the names of the parameters of the type of layer
  // i: the layer's letter, then the type's name ("a_curve_" and so on).
  static std::string typePrefix(int i, int type) {
    return std::string(c_layerNames[i]) + "_" + c_typeNames[type] + "_";
  }

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
  // Level (see TDeformerSource): whatever layers buildStack() makes, of
  // every layer type
  std::shared_ptr<const TDeformer> makeDeformer(
      double frame, const TAffine &placement) const override {
    std::shared_ptr<TDeformerStack> stack = std::make_shared<TDeformerStack>();
    std::vector<TPointD> rest;
    if (!buildStack(frame, placement, *stack, rest)) return nullptr;
    return stack;
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

    std::vector<TPointD> layerRest;
    std::shared_ptr<const TDeformer> deformer =
        layer.buildDeformer(frame, aff, layerRest);
    if (!deformer) continue;

    TDeformerWeightMap weights = TDeformerWeightMap::makeUniform(weight);
    if (layer.m_falloff->getValue())
      weights = TDeformerWeightMap::makeDistance(
          weight, aff * layer.m_center->getValue(frame),
          layer.m_inner->getValue(frame) * radiusScale,
          layer.m_outer->getValue(frame) * radiusScale);

    stack.addLayer(deformer, weights);
    rest.insert(rest.end(), layerRest.begin(), layerRest.end());
  }

  // The tolerances of the inverse follow the size of the layers' rest points
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
  // The gadgets of the points of each layer's type, then those of its falloff
  int types[LAYER_COUNT];
  int maxLength = 0;
  for (int i = 0; i < LAYER_COUNT; ++i) {
    types[i] = m_layers[i].getType();
    maxLength += m_layers[i].getConceptCount(types[i]) + 3;
  }
  concepts = new TParamUIConcept[maxLength];

  int count = 0;
  for (int i = 0; i < LAYER_COUNT; ++i) {
    const Layer &layer = m_layers[i];
    std::string label  = std::string(c_layerLabels[i]) + " ";

    count += layer.getParamUIs(types[i], concepts + count, label);

    // The center of the falloff and its two radii
    TParamUIConcept *falloff = concepts + count;
    count += 3;

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
  length = count;
}

//-------------------------------------------------------------------

bool DeformerStackFx::isParamSaved(const TParamVar *paramVar) const {
  const std::string name = paramVar->getName();

  for (int i = 0; i < LAYER_COUNT; ++i) {
    if (!startsWith(name, std::string(c_layerNames[i]) + "_")) continue;

    const int type = m_layers[i].getType();
    if (name == std::string(c_layerNames[i]) + "_type")
      return type != ENVELOPE_LAYER;

    for (int t = CURVE_LAYER; t < LAYER_TYPE_COUNT; ++t)
      if (startsWith(name, typePrefix(i, t))) return type == t;

    // A parameter of the Envelope layer, or the weight and the falloff
    return true;
  }
  return true;
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(DeformerStackFx, "deformerStackFx");
