#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "deformerfxcommon.h"

#include "toonz/tdeformer.h"
#include "toonz/tbonedeformer.h"

#include <string>
#include <vector>

//==============================================================================
//    Bone Deformer Fx
//
// Bends the Source with a chain of 4 bones (see TBoneDeformer) that needs no
// mesh. The chain goes through 5 joints: the bone i runs from joint i to joint
// i + 1. The Rest joints (rest1 to rest5) are the chain in rest pose, by
// default along a horizontal line from -200 to 200; the Joints (point1 to
// point5) are where they are moved to, by default on the rest ones, and the
// Source then passes through unchanged.
//
// Each bone moves the part of the plane near its rest segment rigidly, as the
// rest bone is carried onto the deformed one (its rest head to the deformed
// head, and its direction turned to the deformed direction). The Inner Radius
// is the distance from the rest bone within which the bone has its full
// influence, and the Outer Radius the one where it falls (smoothly) to 0; the
// neighbouring bones share the plane around a joint by their weights. A point
// out of the Outer Radius of all the bones does not move.
//
// The bones are rigid: a joint moved so that a bone changes length does not
// stretch the art, and the bone's tail then misses the joint. A strong bend
// collapses the art around the joint, as linear blend skinning does; widen the
// radii for a softer bend.
//
// Limits: the warp is made on the rendered Source raster, so vector art is
// resampled rather than deformed as strokes; the chain is fixed at 4 bones
// (the class supports trees of any size, for later use by the Deformer Stack
// and game export).
//==============================================================================

namespace {

enum { BONE_COUNT = 4, JOINT_COUNT = BONE_COUNT + 1 };

}  // namespace

//==============================================================================

class BoneDeformerFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(BoneDeformerFx)

  TRasterFxPort m_input;
  TPointParamP m_point[JOINT_COUNT];  // The joints, deformed
  TPointParamP m_rest[JOINT_COUNT];   // The joints in rest pose
  TDoubleParamP m_inner, m_outer;

public:
  BoneDeformerFx() {
    addInputPort("Source", m_input);

    for (int i = 0; i < JOINT_COUNT; ++i) {
      TPointD joint(-200.0 + 100.0 * i, 0.0);

      m_point[i] = TPointParamP(joint);
      m_point[i]->getX()->setMeasureName("fxLength");
      m_point[i]->getY()->setMeasureName("fxLength");
      bindParam(this, "point" + std::to_string(i + 1), m_point[i]);
    }

    for (int i = 0; i < JOINT_COUNT; ++i) {
      TPointD joint(-200.0 + 100.0 * i, 0.0);

      m_rest[i] = TPointParamP(joint);
      m_rest[i]->getX()->setMeasureName("fxLength");
      m_rest[i]->getY()->setMeasureName("fxLength");
      bindParam(this, "rest" + std::to_string(i + 1), m_rest[i]);
    }

    m_inner = TDoubleParamP(30.0);
    m_inner->setMeasureName("fxLength");
    m_inner->setValueRange(0.0, 1e6);
    bindParam(this, "inner", m_inner);

    m_outer = TDoubleParamP(90.0);
    m_outer->setMeasureName("fxLength");
    m_outer->setValueRange(0.0, 1e6);
    bindParam(this, "outer", m_outer);

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
  // Returns the deformer at frame, in the space that aff maps the fx's space
  // to, and the rest joints in that space.
  TBoneDeformer getDeformer(double frame, const TAffine &aff,
                            std::vector<TPointD> *restJoints = 0) const;

  // Returns whether the Source passes through unchanged at frame: the joints
  // are on the rest ones.
  bool isPassThrough(double frame) const;

  // Returns the pixel-aligned rect of the Source needed to render rect,
  // in the space of ri; empty when nothing of the Source shows in rect.
  TRectD getSourceRect(double frame, const TRectD &rect,
                       const TRenderSettings &ri);
};

//-------------------------------------------------------------------

TBoneDeformer BoneDeformerFx::getDeformer(
    double frame, const TAffine &aff, std::vector<TPointD> *restJoints) const {
  std::vector<TPointD> rest(JOINT_COUNT), joints(JOINT_COUNT);
  for (int i = 0; i < JOINT_COUNT; ++i) {
    rest[i]   = m_rest[i]->getValue(frame);
    joints[i] = m_point[i]->getValue(frame);
  }

  // The bones are in the fx's space: the deformer maps them to the render's
  // space, which differs from it by a similarity (see canHandle()).
  TBoneDeformer deformer(
      TBoneDeformer::makeChain(rest, joints, m_inner->getValue(frame),
                               m_outer->getValue(frame)),
      aff);

  if (restJoints) {
    deformerfx::mapPoints(aff, rest);
    *restJoints = rest;
  }
  return deformer;
}

//-------------------------------------------------------------------

bool BoneDeformerFx::isPassThrough(double frame) const {
  TBoneDeformer deformer(getDeformer(frame, TAffine()));
  return !deformer.isValid() || deformer.isIdentity(1e-6);
}

//-------------------------------------------------------------------

bool BoneDeformerFx::doGetBBox(double frame, TRectD &bBox,
                               const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);
  if (isPassThrough(frame)) return ret;

  // An infinite bbox stays infinite
  TBoneDeformer deformer(getDeformer(frame, TAffine()));
  bBox = deformer.deformedBounds(bBox, frame);
  return ret;
}

//-------------------------------------------------------------------

TRectD BoneDeformerFx::getSourceRect(double frame, const TRectD &rect,
                                     const TRenderSettings &ri) {
  if (rect.isEmpty()) return TRectD();
  if (isPassThrough(frame)) return rect;

  std::vector<TPointD> rest;
  TBoneDeformer deformer(getDeformer(frame, ri.m_affine, &rest));

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  return deformerfx::getSourceRect(deformer, rest, frame, rect, inBBox);
}

//-------------------------------------------------------------------

void BoneDeformerFx::transform(double frame, int port,
                               const TRectD &rectOnOutput,
                               const TRenderSettings &infoOnOutput,
                               TRectD &rectOnInput,
                               TRenderSettings &infoOnInput) {
  infoOnInput = infoOnOutput;
  rectOnInput = getSourceRect(frame, rectOnOutput, infoOnOutput);
}

//-------------------------------------------------------------------

int BoneDeformerFx::getMemoryRequirement(const TRectD &rect, double frame,
                                         const TRenderSettings &info) {
  if (!m_input.isConnected()) return 0;

  return TRasterFx::memorySize(getSourceRect(frame, rect, info), info.m_bpp);
}

//-------------------------------------------------------------------

void BoneDeformerFx::doCompute(TTile &tile, double frame,
                               const TRenderSettings &ri) {
  if (!m_input.isConnected()) return;

  if (isPassThrough(frame)) {
    m_input->compute(tile, frame, ri);
    return;
  }

  TRasterP tileRas(tile.getRaster());
  TRectD tileRect(tile.m_pos, TDimensionD(tileRas->getLx(), tileRas->getLy()));

  TRectD inRect = getSourceRect(frame, tileRect, ri);

  TBoneDeformer deformer(getDeformer(frame, ri.m_affine));
  deformerfx::warpSourceTile(m_input, deformer, inRect, tile, frame, ri);
}

//-------------------------------------------------------------------

void BoneDeformerFx::getParamUIs(TParamUIConcept *&concepts, int &length) {
  concepts = new TParamUIConcept[length = 3 * JOINT_COUNT];

  // The rest joints come first: the viewer picks the gadget drawn last, so the
  // deformed joints are dragged where they lie on the rest ones.
  for (int i = 0; i < JOINT_COUNT; ++i) {
    std::string suffix = std::to_string(i + 1);

    TParamUIConcept &rest = concepts[i];
    rest.m_type           = TParamUIConcept::POINT;
    rest.m_label          = "Rest Joint " + suffix;
    rest.m_params.push_back(m_rest[i]);

    TParamUIConcept &offset = concepts[JOINT_COUNT + i];
    offset.m_type           = TParamUIConcept::VECTOR;
    offset.m_params.push_back(m_rest[i]);
    offset.m_params.push_back(m_point[i]);

    TParamUIConcept &joint = concepts[2 * JOINT_COUNT + i];
    joint.m_type           = TParamUIConcept::POINT;
    joint.m_label          = "Joint " + suffix;
    joint.m_params.push_back(m_point[i]);
  }
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(BoneDeformerFx, "boneDeformerFx");
