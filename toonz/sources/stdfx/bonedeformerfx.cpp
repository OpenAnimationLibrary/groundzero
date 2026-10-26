#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "deformerfxcommon.h"
#include "deformerparams.h"

#include "toonz/tdeformer.h"

#include <memory>
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
// (the class supports trees of any size, for later use by game export). The
// parameters and the deformer are made by deformerfx::BoneChainParams, which
// a Bone layer of the Deformer Stack Fx shares.
//==============================================================================

namespace {

enum { BONE_COUNT = 4, JOINT_COUNT = BONE_COUNT + 1 };

}  // namespace

//==============================================================================

class BoneDeformerFx final : public TStandardRasterFx, public TDeformerSource {
  FX_PLUGIN_DECLARATION(BoneDeformerFx)

  TRasterFxPort m_input;
  deformerfx::BoneChainParams m_chain;

public:
  BoneDeformerFx() {
    addInputPort("Source", m_input);

    // By default, the joints are along a horizontal line from -200 to 200,
    // on the rest ones
    m_chain.bind(this, "", JOINT_COUNT, 200.0);

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
  // The deformer of the bones at frame, for Bake Deformers into Vector Level
  // (see TDeformerSource)
  std::unique_ptr<TDeformer> makeDeformer(
      double frame, const TAffine &placement) const override {
    if (isPassThrough(frame)) return nullptr;
    return std::unique_ptr<TDeformer>(
        new TBoneDeformer(getDeformer(frame, placement)));
  }
};

//-------------------------------------------------------------------

bool BoneDeformerFx::doGetBBox(double frame, TRectD &bBox,
                               const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);

  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_chain.buildDeformer(frame, TAffine(), rest);
  if (!deformer) return ret;

  // An infinite bbox stays infinite
  bBox = deformer->deformedBounds(bBox, frame);
  return ret;
}

//-------------------------------------------------------------------

TRectD BoneDeformerFx::getSourceRect(double frame, const TRectD &rect,
                                     const TRenderSettings &ri) {
  if (rect.isEmpty()) return TRectD();

  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_chain.buildDeformer(frame, ri.m_affine, rest);
  if (!deformer) return rect;

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  return deformerfx::getSourceRect(*deformer, rest, frame, rect, inBBox);
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

  // The deformer is built in the render's space, which differs from the fx's
  // by a similarity (see canHandle()).
  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_chain.buildDeformer(frame, ri.m_affine, rest);
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

void BoneDeformerFx::getParamUIs(TParamUIConcept *&concepts, int &length) {
  concepts = new TParamUIConcept[m_chain.conceptCount()];
  length   = m_chain.getParamUIs(concepts, "");
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(BoneDeformerFx, "boneDeformerFx");
