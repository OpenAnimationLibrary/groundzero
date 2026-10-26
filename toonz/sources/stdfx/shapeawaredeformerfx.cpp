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
// motion may fold the Source, as in any linear blend. The parameters and the
// deformer (with the cache of the weights) are made by
// deformerfx::ShapeAwareParams, which a Shape-Aware layer of the Deformer
// Stack Fx shares.
//==============================================================================

namespace {

enum { PIN_COUNT = 4, BONE_COUNT = 2, CAGE_COUNT = 4 };

}  // namespace

//==============================================================================

class ShapeAwareDeformerFx final : public TStandardRasterFx,
                                   public TDeformerSource {
  FX_PLUGIN_DECLARATION(ShapeAwareDeformerFx)

  TRasterFxPort m_input;
  deformerfx::ShapeAwareParams m_handles;

public:
  ShapeAwareDeformerFx() {
    addInputPort("Source", m_input);

    // The default positions are laid out so that the gadgets of the handles
    // do not lie on top of each other
    m_handles.bind(this, "", PIN_COUNT, BONE_COUNT, CAGE_COUNT, 1.0);

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

bool ShapeAwareDeformerFx::doGetBBox(double frame, TRectD &bBox,
                                     const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);

  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_handles.buildDeformer(frame, TAffine(), rest);
  if (!deformer) return ret;

  // An infinite bbox stays infinite
  bBox = deformer->deformedBounds(bBox, frame);
  return ret;
}

//-------------------------------------------------------------------

TRectD ShapeAwareDeformerFx::getSourceRect(double frame, const TRectD &rect,
                                           const TRenderSettings &ri) {
  if (rect.isEmpty()) return TRectD();

  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_handles.buildDeformer(frame, ri.m_affine, rest);
  if (!deformer) return rect;

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  return deformerfx::getSourceRect(*deformer, rest, frame, rect, inBBox);
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

  // The deformer is placed in the render's space, which differs from the
  // fx's by a similarity (see canHandle()).
  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_handles.buildDeformer(frame, ri.m_affine, rest);
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

void ShapeAwareDeformerFx::getParamUIs(TParamUIConcept *&concepts,
                                       int &length) {
  concepts = new TParamUIConcept[m_handles.conceptCount()];
  length   = m_handles.getParamUIs(concepts, "");
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(ShapeAwareDeformerFx, "shapeAwareDeformerFx");
