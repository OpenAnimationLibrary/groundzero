#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "deformerfxcommon.h"

#include "toonz/tdeformer.h"
#include "toonz/tshapeawaredeformer.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

//==============================================================================
//    Shape Aware Deformer Fx
//
// Bends the Source with pins, bones and a cage acting together (see
// TShapeAwareDeformer). Each handle moves the Source around it, and the way
// the handles share the plane follows their layout: the weights are smooth
// bounded biharmonic-style weights computed on a grid over the handles, which
// are never negative, sum to 1, and leave the Source in place away from the
// handles.
//
// There are 4 Pins, 2 Bones and a Cage of 4 corners, each off by default (the
// Pin On, Bone On and Cage On toggles). Every handle has its Rest geometry
// and the geometry it is moved to. A pin moves the Source near it by the
// translation of the pin and, optionally, an Angle and a Scale about its rest
// position. A bone carries the Source near its rest segment as the rest
// segment is carried onto the moved one (with Stretch on, it also scales by
// the ratio of the lengths). The cage corners are the corners of a closed
// polygon whose edges move with the corners. A pin that is not moved still
// holds the Source in place around it, which is how an area is kept still.
//
// Resolution is the number of cells along the longest side of the weight
// grid, and Margin extends the grid beyond the handles (at least a quarter of
// their extent is always added). The weights are computed once for the rest
// geometry and kept while it does not change, so animating the moved handles
// does not recompute them.
//
// With no handle on, or every handle on its rest geometry, the Source passes
// through unchanged. Limits: the warp is made on the rendered Source raster,
// so vector art is resampled rather than deformed as strokes; handles closer
// than a few grid cells compete for its nodes (raise the Resolution); a strong
// motion may fold the Source, as in any linear blend.
//==============================================================================

namespace {

enum { PIN_COUNT = 4, BONE_COUNT = 2, CAGE_COUNT = 4 };

typedef TShapeAwareDeformer::Handle Handle;

// Default positions, so that the gadgets of the handles do not lie on top of
// each other
TPointD defaultPin(int i) {
  const TPointD corners[PIN_COUNT] = {
      TPointD(-100.0, 100.0), TPointD(100.0, 100.0), TPointD(-100.0, -100.0),
      TPointD(100.0, -100.0)};
  return corners[i];
}

TPointD defaultBoneHead(int i) { return TPointD(-120.0 + 120.0 * i, 0.0); }
TPointD defaultBoneTail(int i) { return TPointD(120.0 * i, 0.0); }

TPointD defaultCage(int i) {
  const TPointD corners[CAGE_COUNT] = {
      TPointD(-150.0, -150.0), TPointD(150.0, -150.0), TPointD(150.0, 150.0),
      TPointD(-150.0, 150.0)};
  return corners[i];
}

TPointParamP makePoint(const TPointD &p) {
  TPointParamP param(p);
  param->getX()->setMeasureName("fxLength");
  param->getY()->setMeasureName("fxLength");
  return param;
}

}  // namespace

//==============================================================================

class ShapeAwareDeformerFx final : public TStandardRasterFx,
                                   public TDeformerSource {
  FX_PLUGIN_DECLARATION(ShapeAwareDeformerFx)

  TRasterFxPort m_input;
  TIntParamP m_resolution;
  TDoubleParamP m_margin;
  TBoolParamP m_stretch;

  TBoolParamP m_pinOn[PIN_COUNT];
  TPointParamP m_pin[PIN_COUNT];      // Moved to
  TPointParamP m_pinRest[PIN_COUNT];  // Rest position
  TDoubleParamP m_pinAngle[PIN_COUNT], m_pinScale[PIN_COUNT];

  TBoolParamP m_boneOn[BONE_COUNT];
  TPointParamP m_boneHead[BONE_COUNT], m_boneTail[BONE_COUNT];
  TPointParamP m_boneRestHead[BONE_COUNT], m_boneRestTail[BONE_COUNT];

  TBoolParamP m_cageOn;
  TPointParamP m_cage[CAGE_COUNT];      // Moved to
  TPointParamP m_cageRest[CAGE_COUNT];  // Rest

  // The last weights computed, for the rest geometry in m_cacheKey
  mutable std::mutex m_cacheMutex;
  mutable std::vector<double> m_cacheKey;
  mutable std::shared_ptr<const TShapeAwareDeformer::Weights> m_cache;

public:
  ShapeAwareDeformerFx()
      : m_resolution(48), m_margin(0.0), m_stretch(false), m_cageOn(false) {
    addInputPort("Source", m_input);

    m_resolution->setValueRange(16, 128);
    bindParam(this, "resolution", m_resolution);

    m_margin->setMeasureName("fxLength");
    m_margin->setValueRange(0.0, 1e6);
    bindParam(this, "margin", m_margin);

    bindParam(this, "stretch", m_stretch);

    for (int i = 0; i < PIN_COUNT; ++i) {
      std::string suffix = std::to_string(i + 1);

      m_pinOn[i] = TBoolParamP(false);
      bindParam(this, "pinOn" + suffix, m_pinOn[i]);

      m_pin[i] = makePoint(defaultPin(i));
      bindParam(this, "pin" + suffix, m_pin[i]);

      m_pinRest[i] = makePoint(defaultPin(i));
      bindParam(this, "pinRest" + suffix, m_pinRest[i]);

      m_pinAngle[i] = TDoubleParamP(0.0);
      m_pinAngle[i]->setMeasureName("angle");
      bindParam(this, "pinAngle" + suffix, m_pinAngle[i]);

      m_pinScale[i] = TDoubleParamP(1.0);
      m_pinScale[i]->setValueRange(0.01, 100.0);
      bindParam(this, "pinScale" + suffix, m_pinScale[i]);
    }

    for (int i = 0; i < BONE_COUNT; ++i) {
      std::string suffix = std::to_string(i + 1);

      m_boneOn[i] = TBoolParamP(false);
      bindParam(this, "boneOn" + suffix, m_boneOn[i]);

      m_boneHead[i] = makePoint(defaultBoneHead(i));
      bindParam(this, "boneHead" + suffix, m_boneHead[i]);

      m_boneTail[i] = makePoint(defaultBoneTail(i));
      bindParam(this, "boneTail" + suffix, m_boneTail[i]);

      m_boneRestHead[i] = makePoint(defaultBoneHead(i));
      bindParam(this, "boneRestHead" + suffix, m_boneRestHead[i]);

      m_boneRestTail[i] = makePoint(defaultBoneTail(i));
      bindParam(this, "boneRestTail" + suffix, m_boneRestTail[i]);
    }

    bindParam(this, "cageOn", m_cageOn);
    for (int i = 0; i < CAGE_COUNT; ++i) {
      std::string suffix = std::to_string(i + 1);

      m_cage[i] = makePoint(defaultCage(i));
      bindParam(this, "cage" + suffix, m_cage[i]);

      m_cageRest[i] = makePoint(defaultCage(i));
      bindParam(this, "cageRest" + suffix, m_cageRest[i]);
    }

    enableComputeInFloat(true);
  }

  bool doGetBBox(double frame, TRectD &bBox,
                 const TRenderSettings &info) override;

  void doCompute(TTile &tile, double frame, const TRenderSettings &ri) override;

  void transform(double frame, int port, const TRectD &rectOnOutput,
                 const TRenderSettings &infoOnOutput, TRectD &rectOnInput,
                 TRenderSettings &infoOnInput) override;

  // The deformation is built in the fx's space and placed by the render's
  // affine: let the other parts of the affine be applied on the result.
  bool canHandle(const TRenderSettings &info, double frame) override {
    return false;
  }

  int getMemoryRequirement(const TRectD &rect, double frame,
                           const TRenderSettings &info) override;

  void getParamUIs(TParamUIConcept *&concepts, int &length) override;

private:
  // Returns the handles at frame, in the fx's space, and the rest geometry
  // of the enabled ones in restPoints
  void getHandles(double frame, std::vector<Handle> &handles,
                  std::vector<TPointD> *restPoints = 0) const;

  // Returns the deformer at frame, placed by aff, and the rest geometry in
  // the space that aff maps the fx's space to.
  TShapeAwareDeformer getDeformer(double frame, const TAffine &aff,
                                  std::vector<TPointD> *restPoints = 0) const;

  // Returns whether the Source passes through unchanged at frame: there is no
  // handle, or all of them are on their rest geometry.
  bool isPassThrough(double frame) const;

  // Returns the pixel-aligned rect of the Source needed to render rect,
  // in the space of ri; empty when nothing of the Source shows in rect.
  TRectD getSourceRect(double frame, const TRectD &rect,
                       const TRenderSettings &ri);

public:
  // The deformer of the handles at frame, for Bake Deformers into Vector
  // Level (see TDeformerSource)
  std::unique_ptr<TDeformer> makeDeformer(
      double frame, const TAffine &placement) const override {
    if (isPassThrough(frame)) return nullptr;
    return std::unique_ptr<TDeformer>(
        new TShapeAwareDeformer(getDeformer(frame, placement)));
  }
};

//-------------------------------------------------------------------

void ShapeAwareDeformerFx::getHandles(double frame,
                                      std::vector<Handle> &handles,
                                      std::vector<TPointD> *restPoints) const {
  handles.clear();
  if (restPoints) restPoints->clear();

  // Pins first, then bones and the cage: the first handles keep the nodes of
  // the weight grid that several of them claim
  for (int i = 0; i < PIN_COUNT; ++i) {
    if (!m_pinOn[i]->getValue()) continue;

    TPointD rest = m_pinRest[i]->getValue(frame);
    handles.push_back(Handle::makePin(rest, m_pin[i]->getValue(frame),
                                      m_pinAngle[i]->getValue(frame),
                                      m_pinScale[i]->getValue(frame)));
    if (restPoints) restPoints->push_back(rest);
  }

  const bool stretch = m_stretch->getValue();
  for (int i = 0; i < BONE_COUNT; ++i) {
    if (!m_boneOn[i]->getValue()) continue;

    TPointD restHead = m_boneRestHead[i]->getValue(frame);
    TPointD restTail = m_boneRestTail[i]->getValue(frame);
    handles.push_back(
        Handle::makeBone(restHead, restTail, m_boneHead[i]->getValue(frame),
                         m_boneTail[i]->getValue(frame), stretch));
    if (restPoints) {
      restPoints->push_back(restHead);
      restPoints->push_back(restTail);
    }
  }

  if (m_cageOn->getValue())
    for (int i = 0; i < CAGE_COUNT; ++i) {
      TPointD rest = m_cageRest[i]->getValue(frame);
      handles.push_back(
          Handle::makeCageVertex(rest, m_cage[i]->getValue(frame)));
      if (restPoints) restPoints->push_back(rest);
    }
}

//-------------------------------------------------------------------

TShapeAwareDeformer ShapeAwareDeformerFx::getDeformer(
    double frame, const TAffine &aff, std::vector<TPointD> *restPoints) const {
  std::vector<Handle> handles;
  getHandles(frame, handles, restPoints);

  TShapeAwareDeformer::Settings settings(m_resolution->getValue(),
                                         m_margin->getValue(frame));

  // The weights depend on the rest geometry only: keep them from one call to
  // the next, as the tiles and the frames of an animation share them
  std::shared_ptr<const TShapeAwareDeformer::Weights> weights;
  {
    std::vector<double> key =
        TShapeAwareDeformer::makeWeightsKey(handles, settings);

    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (!m_cache || key != m_cacheKey) {
      m_cache    = TShapeAwareDeformer::computeWeights(handles, settings);
      m_cacheKey = key;
    }
    weights = m_cache;
  }

  if (restPoints) deformerfx::mapPoints(aff, *restPoints);

  // The handles are in the fx's space: the deformer places them in the
  // render's space
  return TShapeAwareDeformer(handles, settings, aff, weights);
}

//-------------------------------------------------------------------

bool ShapeAwareDeformerFx::isPassThrough(double frame) const {
  std::vector<Handle> handles;
  getHandles(frame, handles);

  for (const Handle &h : handles)
    if (!h.isIdentity(1e-6)) return false;
  return true;
}

//-------------------------------------------------------------------

bool ShapeAwareDeformerFx::doGetBBox(double frame, TRectD &bBox,
                                     const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);
  if (isPassThrough(frame)) return ret;

  // An infinite bbox stays infinite
  TShapeAwareDeformer deformer(getDeformer(frame, TAffine()));
  bBox = deformer.deformedBounds(bBox, frame);
  return ret;
}

//-------------------------------------------------------------------

TRectD ShapeAwareDeformerFx::getSourceRect(double frame, const TRectD &rect,
                                           const TRenderSettings &ri) {
  if (rect.isEmpty()) return TRectD();
  if (isPassThrough(frame)) return rect;

  std::vector<TPointD> rest;
  TShapeAwareDeformer deformer(getDeformer(frame, ri.m_affine, &rest));

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  return deformerfx::getSourceRect(deformer, rest, frame, rect, inBBox);
}

//-------------------------------------------------------------------

void ShapeAwareDeformerFx::transform(double frame, int port,
                                     const TRectD &rectOnOutput,
                                     const TRenderSettings &infoOnOutput,
                                     TRectD &rectOnInput,
                                     TRenderSettings &infoOnInput) {
  infoOnInput = infoOnOutput;
  rectOnInput = getSourceRect(frame, rectOnOutput, infoOnOutput);
}

//-------------------------------------------------------------------

int ShapeAwareDeformerFx::getMemoryRequirement(const TRectD &rect, double frame,
                                               const TRenderSettings &info) {
  if (!m_input.isConnected()) return 0;

  return TRasterFx::memorySize(getSourceRect(frame, rect, info), info.m_bpp);
}

//-------------------------------------------------------------------

void ShapeAwareDeformerFx::doCompute(TTile &tile, double frame,
                                     const TRenderSettings &ri) {
  if (!m_input.isConnected()) return;

  if (isPassThrough(frame)) {
    m_input->compute(tile, frame, ri);
    return;
  }

  TRasterP tileRas(tile.getRaster());
  TRectD tileRect(tile.m_pos, TDimensionD(tileRas->getLx(), tileRas->getLy()));

  TRectD inRect = getSourceRect(frame, tileRect, ri);

  TShapeAwareDeformer deformer(getDeformer(frame, ri.m_affine));
  deformerfx::warpSourceTile(m_input, deformer, inRect, tile, frame, ri);
}

//-------------------------------------------------------------------

void ShapeAwareDeformerFx::getParamUIs(TParamUIConcept *&concepts,
                                       int &length) {
  concepts = new TParamUIConcept[length = 3 * PIN_COUNT + 4 * BONE_COUNT +
                                          3 * CAGE_COUNT];
  int n    = 0;

  // For each kind of handle the rest gadgets come first: the viewer picks the
  // gadget drawn last, so the moved handles are dragged where they lie on the
  // rest ones.
  for (int i = 0; i < PIN_COUNT; ++i) {
    std::string suffix = std::to_string(i + 1);

    TParamUIConcept &rest = concepts[n++];
    rest.m_type           = TParamUIConcept::POINT;
    rest.m_label          = "Rest Pin " + suffix;
    rest.m_params.push_back(m_pinRest[i]);

    TParamUIConcept &offset = concepts[n++];
    offset.m_type           = TParamUIConcept::VECTOR;
    offset.m_params.push_back(m_pinRest[i]);
    offset.m_params.push_back(m_pin[i]);

    TParamUIConcept &pin = concepts[n++];
    pin.m_type           = TParamUIConcept::POINT;
    pin.m_label          = "Pin " + suffix;
    pin.m_params.push_back(m_pin[i]);
  }

  for (int i = 0; i < BONE_COUNT; ++i) {
    std::string suffix = std::to_string(i + 1);

    TParamUIConcept &restHead = concepts[n++];
    restHead.m_type           = TParamUIConcept::POINT;
    restHead.m_label          = "Rest Bone Head " + suffix;
    restHead.m_params.push_back(m_boneRestHead[i]);

    TParamUIConcept &restTail = concepts[n++];
    restTail.m_type           = TParamUIConcept::POINT;
    restTail.m_label          = "Rest Bone Tail " + suffix;
    restTail.m_params.push_back(m_boneRestTail[i]);

    TParamUIConcept &head = concepts[n++];
    head.m_type           = TParamUIConcept::POINT;
    head.m_label          = "Bone Head " + suffix;
    head.m_params.push_back(m_boneHead[i]);

    TParamUIConcept &tail = concepts[n++];
    tail.m_type           = TParamUIConcept::POINT;
    tail.m_label          = "Bone Tail " + suffix;
    tail.m_params.push_back(m_boneTail[i]);
  }

  for (int i = 0; i < CAGE_COUNT; ++i) {
    std::string suffix = std::to_string(i + 1);

    TParamUIConcept &rest = concepts[n++];
    rest.m_type           = TParamUIConcept::POINT;
    rest.m_label          = "Rest Cage " + suffix;
    rest.m_params.push_back(m_cageRest[i]);

    TParamUIConcept &offset = concepts[n++];
    offset.m_type           = TParamUIConcept::VECTOR;
    offset.m_params.push_back(m_cageRest[i]);
    offset.m_params.push_back(m_cage[i]);

    TParamUIConcept &cage = concepts[n++];
    cage.m_type           = TParamUIConcept::POINT;
    cage.m_label          = "Cage " + suffix;
    cage.m_params.push_back(m_cage[i]);
  }
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(ShapeAwareDeformerFx, "shapeAwareDeformerFx");
