#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "deformerfxcommon.h"

#include "toonz/tdeformer.h"
#include "toonz/tfreeformdeformer.h"

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
// rather than deformed as strokes.
//==============================================================================

namespace {

// Cells of the lattice along each side
enum { CELLS = 3, POINT_COUNT = (CELLS + 1) * (CELLS + 1) };

//! Returns the rest position, in the fx's space, of the control point (i, j) of
//! the lattice over the rect of the passed center and size.
TPointD restPoint(const TPointD &center, double width, double height, int i,
                  int j) {
  return TPointD(center.x - 0.5 * width + width * i / CELLS,
                 center.y - 0.5 * height + height * j / CELLS);
}

}  // namespace

//==============================================================================

class FreeFormDeformerFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(FreeFormDeformerFx)

  TRasterFxPort m_input;
  TPointParamP m_center;  // Rest rectangle
  TDoubleParamP m_width, m_height;
  TPointParamP m_point[POINT_COUNT];  // The lattice, deformed

public:
  FreeFormDeformerFx() {
    addInputPort("Source", m_input);

    m_center = TPointParamP(TPointD(0.0, 0.0));
    m_center->getX()->setMeasureName("fxLength");
    m_center->getY()->setMeasureName("fxLength");
    bindParam(this, "center", m_center);

    m_width = TDoubleParamP(400.0);
    m_width->setMeasureName("fxLength");
    m_width->setValueRange(0.0, 1e6);
    bindParam(this, "width", m_width);

    m_height = TDoubleParamP(400.0);
    m_height->setMeasureName("fxLength");
    m_height->setValueRange(0.0, 1e6);
    bindParam(this, "height", m_height);

    // By default, the points are on the rest lattice
    for (int j = 0; j <= CELLS; ++j)
      for (int i = 0; i <= CELLS; ++i) {
        int n = j * (CELLS + 1) + i;

        m_point[n] = TPointParamP(restPoint(TPointD(), 400.0, 400.0, i, j));
        m_point[n]->getX()->setMeasureName("fxLength");
        m_point[n]->getY()->setMeasureName("fxLength");
        bindParam(this, "point" + std::to_string(n + 1), m_point[n]);
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
  // Returns the deformer at frame, in the space that aff maps the fx's space
  // to, and its rest rectangle's corners in that space.
  TFreeFormDeformer getDeformer(double frame, const TAffine &aff,
                                std::vector<TPointD> *restCorners = 0) const;

  // Returns whether the Source passes through unchanged at frame: the points
  // are on the rest lattice, or the rest rectangle is degenerate.
  bool isPassThrough(double frame) const;

  // Returns the pixel-aligned rect of the Source needed to render rect,
  // in the space of ri; empty when nothing of the Source shows in rect.
  TRectD getSourceRect(double frame, const TRectD &rect,
                       const TRenderSettings &ri);
};

//-------------------------------------------------------------------

TFreeFormDeformer FreeFormDeformerFx::getDeformer(
    double frame, const TAffine &aff, std::vector<TPointD> *restCorners) const {
  TPointD center(m_center->getValue(frame));
  double width = m_width->getValue(frame), height = m_height->getValue(frame);

  std::vector<TPointD> points(POINT_COUNT);
  for (int n = 0; n < POINT_COUNT; ++n) points[n] = m_point[n]->getValue(frame);

  TRectD rect(center.x - 0.5 * width, center.y - 0.5 * height,
              center.x + 0.5 * width, center.y + 0.5 * height);

  // The rest lattice and the points are in the fx's space: the deformer maps
  // them to the render's space, which differs from it by a similarity (see
  // canHandle()).
  TFreeFormDeformer deformer(rect, CELLS, CELLS, points, aff);

  if (restCorners) {
    restCorners->clear();
    restCorners->push_back(aff * TPointD(rect.x0, rect.y0));
    restCorners->push_back(aff * TPointD(rect.x1, rect.y0));
    restCorners->push_back(aff * TPointD(rect.x1, rect.y1));
    restCorners->push_back(aff * TPointD(rect.x0, rect.y1));
  }
  return deformer;
}

//-------------------------------------------------------------------

bool FreeFormDeformerFx::isPassThrough(double frame) const {
  TFreeFormDeformer deformer(getDeformer(frame, TAffine()));
  return !deformer.isValid() || deformer.isIdentity(1e-6);
}

//-------------------------------------------------------------------

bool FreeFormDeformerFx::doGetBBox(double frame, TRectD &bBox,
                                   const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);
  if (isPassThrough(frame)) return ret;

  // An infinite bbox stays infinite
  TFreeFormDeformer deformer(getDeformer(frame, TAffine()));
  bBox = deformer.deformedBounds(bBox, frame);
  return ret;
}

//-------------------------------------------------------------------

TRectD FreeFormDeformerFx::getSourceRect(double frame, const TRectD &rect,
                                         const TRenderSettings &ri) {
  if (rect.isEmpty()) return TRectD();
  if (isPassThrough(frame)) return rect;

  std::vector<TPointD> rest;
  TFreeFormDeformer deformer(getDeformer(frame, ri.m_affine, &rest));

  TRectD inBBox;
  m_input->getBBox(frame, inBBox, ri);
  return deformerfx::getSourceRect(deformer, rest, frame, rect, inBBox);
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

  if (isPassThrough(frame)) {
    m_input->compute(tile, frame, ri);
    return;
  }

  TRasterP tileRas(tile.getRaster());
  TRectD tileRect(tile.m_pos, TDimensionD(tileRas->getLx(), tileRas->getLy()));

  TRectD inRect = getSourceRect(frame, tileRect, ri);

  TFreeFormDeformer deformer(getDeformer(frame, ri.m_affine));
  deformerfx::warpSourceTile(m_input, deformer, inRect, tile, frame, ri);
}

//-------------------------------------------------------------------

void FreeFormDeformerFx::getParamUIs(TParamUIConcept *&concepts, int &length) {
  concepts = new TParamUIConcept[length = 1 + POINT_COUNT];

  // The rest rectangle comes first: the viewer picks the gadget drawn last, so
  // the points are dragged where they lie on its sides.
  concepts[0].m_type  = TParamUIConcept::RECT;
  concepts[0].m_label = "Rest";
  concepts[0].m_params.push_back(m_width);
  concepts[0].m_params.push_back(m_height);
  concepts[0].m_params.push_back(m_center);

  for (int n = 0; n < POINT_COUNT; ++n) {
    TParamUIConcept &point = concepts[1 + n];
    point.m_type           = TParamUIConcept::POINT;
    point.m_label          = "Point " + std::to_string(n + 1);
    point.m_params.push_back(m_point[n]);
  }
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(FreeFormDeformerFx, "freeFormDeformerFx");
