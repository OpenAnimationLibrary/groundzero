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
//    Free-Form Deformer Fx
//
// Bends the Source with a lattice of 4 x 4 control points (see
// TFreeFormDeformer). The lattice is laid out regularly over a rectangle, the
// rest rectangle, given by its Rest Center, Rest Width and Rest Height (by
// default, a square of half side 200 around the origin). The 16 Points are the
// positions the control points are moved to, numbered row by row from the
// bottom-left one: point1 to point4 are the bottom row, point13 to point16 the
// top one. By default they sit on the rest lattice, and the Source passes
// through unchanged.
//
// The warp follows the cubic B-spline of the control points' displacement:
// smooth, and local to the cells around each moved point. Only the four
// corners are reached exactly; a point inside the lattice moves the art near
// it by a fraction of its displacement. The Points are absolute positions:
// moving the rest rectangle without them moves the rest lattice, and so
// deforms the Source.
//
// Beyond the rest rectangle the displacement stays the one of the nearest
// border point, so the rest of the plane translates along with the border and
// the image stays continuous but not smooth there. Place the rest rectangle
// around the art. Folding the lattice over itself makes the warp
// non-invertible there, which gives smeared pixels.
//
// The warp is made on the rendered Source raster, so vector art is resampled
// rather than deformed as strokes. The parameters and the deformer are made by
// deformerfx::FreeFormParams, which a Free-Form layer of the Deformer Stack Fx
// shares.
//==============================================================================

namespace {

// Cells of the lattice along each side
enum { CELLS = 3 };

}  // namespace

//==============================================================================

class FreeFormDeformerFx final : public TStandardRasterFx,
                                 public TDeformerSource {
  FX_PLUGIN_DECLARATION(FreeFormDeformerFx)

  TRasterFxPort m_input;
  deformerfx::FreeFormParams m_lattice;

public:
  FreeFormDeformerFx() {
    addInputPort("Source", m_input);

    // By default, the rest rectangle is a square of side 400 around the
    // origin, and the points are on the rest lattice
    m_lattice.bind(this, "", CELLS, 400.0);

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
  // The deformer of the lattice at frame, for Bake Deformers into Vector
  // Level (see TDeformerSource)
  std::unique_ptr<TDeformer> makeDeformer(
      double frame, const TAffine &placement) const override {
    if (isPassThrough(frame)) return nullptr;
    return std::unique_ptr<TDeformer>(
        new TFreeFormDeformer(getDeformer(frame, placement)));
  }
};

//-------------------------------------------------------------------

bool FreeFormDeformerFx::doGetBBox(double frame, TRectD &bBox,
                                   const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);

  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_lattice.buildDeformer(frame, TAffine(), rest);
  if (!deformer) return ret;

  // An infinite bbox stays infinite
  bBox = deformer->deformedBounds(bBox, frame);
  return ret;
}

//-------------------------------------------------------------------

TRectD FreeFormDeformerFx::getSourceRect(double frame, const TRectD &rect,
                                         const TRenderSettings &ri) {
  if (rect.isEmpty()) return TRectD();

  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_lattice.buildDeformer(frame, ri.m_affine, rest);
  if (!deformer) return rect;

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  return deformerfx::getSourceRect(*deformer, rest, frame, rect, inBBox);
}

//-------------------------------------------------------------------

void FreeFormDeformerFx::transform(double frame, int port,
                                   const TRectD &rectOnOutput,
                                   const TRenderSettings &infoOnOutput,
                                   TRectD &rectOnInput,
                                   TRenderSettings &infoOnInput) {
  infoOnInput = infoOnOutput;
  rectOnInput = getSourceRect(frame, rectOnOutput, infoOnOutput);
}

//-------------------------------------------------------------------

int FreeFormDeformerFx::getMemoryRequirement(const TRectD &rect, double frame,
                                             const TRenderSettings &info) {
  if (!m_input.isConnected()) return 0;

  return TRasterFx::memorySize(getSourceRect(frame, rect, info), info.m_bpp);
}

//-------------------------------------------------------------------

void FreeFormDeformerFx::doCompute(TTile &tile, double frame,
                                   const TRenderSettings &ri) {
  if (!m_input.isConnected()) return;

  // The deformer is built in the render's space, which differs from the fx's
  // by a similarity (see canHandle()).
  std::vector<TPointD> rest;
  std::shared_ptr<const TDeformer> deformer =
      m_lattice.buildDeformer(frame, ri.m_affine, rest);
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

void FreeFormDeformerFx::getParamUIs(TParamUIConcept *&concepts, int &length) {
  concepts = new TParamUIConcept[m_lattice.conceptCount()];
  length   = m_lattice.getParamUIs(concepts, "");
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(FreeFormDeformerFx, "freeFormDeformerFx");
