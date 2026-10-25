#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "deformerfxcommon.h"

#include "toonz/tcurvedeformer.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
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
// more tightly than the Width the art folds over itself. To stack it with
// other deformers, see TCurveDeformer, which is a TDeformer for a later layer
// of the Deformer Stack Fx.
//==============================================================================

namespace {

enum { ANCHOR_COUNT = 4 };

// Half the length of the default curve: a straight line along the x axis
const double c_defaultHalfLength = 200.0;

//------------------------------------------------------------------------------

TPointParamP makePointParam(const TPointD &p) {
  TPointParamP param(p);
  param->getX()->setMeasureName("fxLength");
  param->getY()->setMeasureName("fxLength");
  return param;
}

//------------------------------------------------------------------------------

// The animatable control points of a curve: the position and the two tangent
// handles of each anchor. The handles are positions, not offsets.
class CurveParams {
public:
  TPointParamP m_pos[ANCHOR_COUNT];
  TPointParamP m_in[ANCHOR_COUNT];
  TPointParamP m_out[ANCHOR_COUNT];

  //! Creates the parameters and binds them to fx as posName, inName and
  //! outName followed by 1 to 4. By default, the curve is a straight line
  //! along the x axis, from -halfLength to halfLength, with the anchors
  //! evenly spaced and the handles a third of the way to the neighbours.
  void bind(TFx *fx, const std::string &posName, const std::string &inName,
            const std::string &outName, double halfLength) {
    const double step   = 2.0 * halfLength / (ANCHOR_COUNT - 1);
    const double handle = step / 3.0;

    for (int i = 0; i < ANCHOR_COUNT; ++i) {
      std::string suffix = std::to_string(i + 1);
      double x           = -halfLength + step * i;

      m_pos[i] = makePointParam(TPointD(x, 0.0));
      m_in[i]  = makePointParam(TPointD(x - handle, 0.0));
      m_out[i] = makePointParam(TPointD(x + handle, 0.0));

      bindParam(fx, posName + suffix, m_pos[i]);
      bindParam(fx, inName + suffix, m_in[i]);
      bindParam(fx, outName + suffix, m_out[i]);
    }
  }

  //! Returns the anchors at frame, in the fx's space.
  void getAnchors(double frame,
                  std::vector<TBezierChain::Anchor> &anchors) const {
    anchors.resize(ANCHOR_COUNT);
    for (int i = 0; i < ANCHOR_COUNT; ++i)
      anchors[i] = TBezierChain::Anchor(m_pos[i]->getValue(frame),
                                        m_in[i]->getValue(frame),
                                        m_out[i]->getValue(frame));
  }

  //! Fills concepts with the gadgets of the curve, whose labels start with
  //! labelPrefix, and returns how many. The vectors from the anchors to their
  //! handles come first, so that the points are drawn (and picked) over them.
  /*!
    The handles that do not shape the curve have no gadget. No more than
    maxConceptCount() concepts are written.
  */
  int getParamUIs(TParamUIConcept *concepts,
                  const std::string &labelPrefix) const {
    int count = 0;

    for (int i = 0; i < ANCHOR_COUNT; ++i) {
      if (i > 0) {
        TParamUIConcept &c = concepts[count++];
        c.m_type           = TParamUIConcept::VECTOR;
        c.m_params.push_back(m_pos[i]);
        c.m_params.push_back(m_in[i]);
      }
      if (i < ANCHOR_COUNT - 1) {
        TParamUIConcept &c = concepts[count++];
        c.m_type           = TParamUIConcept::VECTOR;
        c.m_params.push_back(m_pos[i]);
        c.m_params.push_back(m_out[i]);
      }
    }

    for (int i = 0; i < ANCHOR_COUNT; ++i) {
      std::string suffix = std::to_string(i + 1);

      if (i > 0) {
        TParamUIConcept &c = concepts[count++];
        c.m_type           = TParamUIConcept::POINT;
        c.m_label          = labelPrefix + "In " + suffix;
        c.m_params.push_back(m_in[i]);
      }
      if (i < ANCHOR_COUNT - 1) {
        TParamUIConcept &c = concepts[count++];
        c.m_type           = TParamUIConcept::POINT;
        c.m_label          = labelPrefix + "Out " + suffix;
        c.m_params.push_back(m_out[i]);
      }

      TParamUIConcept &c = concepts[count++];
      c.m_type           = TParamUIConcept::POINT;
      c.m_label          = labelPrefix + "Point " + suffix;
      c.m_params.push_back(m_pos[i]);
    }
    return count;
  }

  static int maxConceptCount() { return 5 * ANCHOR_COUNT - 4; }
};

//------------------------------------------------------------------------------

void mapAnchors(const TAffine &aff,
                std::vector<TBezierChain::Anchor> &anchors) {
  for (TBezierChain::Anchor &a : anchors)
    a.m_pos = aff * a.m_pos, a.m_in = aff * a.m_in, a.m_out = aff * a.m_out;
}

}  // namespace

//==============================================================================

class CurveDeformerFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(CurveDeformerFx)

  TRasterFxPort m_input;
  CurveParams m_deformed;  // The curve, deformed
  CurveParams m_rest;      // The curve in rest pose
  TDoubleParamP m_width, m_falloff;

public:
  CurveDeformerFx() : m_width(100.0), m_falloff(300.0) {
    addInputPort("Source", m_input);

    // By default, the curve is a straight line across the origin
    m_deformed.bind(this, "point", "in", "out", c_defaultHalfLength);
    m_rest.bind(this, "restPoint", "restIn", "restOut", c_defaultHalfLength);

    m_width->setMeasureName("fxLength");
    m_width->setValueRange(0.0, 1e6);
    bindParam(this, "width", m_width);

    m_falloff->setMeasureName("fxLength");
    m_falloff->setValueRange(0.0, 1e6);
    bindParam(this, "falloff", m_falloff);

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
  // Builds in deformer the curve deformer at frame, in the space that aff
  // maps the fx's space to, and fills rest with the control points of the
  // rest curve in that space. Returns false when the Source passes through
  // unchanged: the curve is not moved from its rest pose, the rest curve is
  // degenerate, or nothing is within the falloff.
  bool buildDeformer(double frame, const TAffine &aff,
                     std::unique_ptr<TCurveDeformer> &deformer,
                     std::vector<TPointD> &rest) const;

  // Returns the pixel-aligned rect of the Source needed to render rect,
  // in the space of ri; empty when nothing of the Source shows in rect.
  TRectD getSourceRect(double frame, const TRectD &rect,
                       const TRenderSettings &ri);
};

//-------------------------------------------------------------------

bool CurveDeformerFx::buildDeformer(double frame, const TAffine &aff,
                                    std::unique_ptr<TCurveDeformer> &deformer,
                                    std::vector<TPointD> &rest) const {
  std::vector<TBezierChain::Anchor> restAnchors, deformedAnchors;
  m_rest.getAnchors(frame, restAnchors);
  m_deformed.getAnchors(frame, deformedAnchors);

  // The distances follow the scale of the affine, a similarity
  double radiusScale = std::sqrt(std::fabs(aff.det()));
  double width       = m_width->getValue(frame) * radiusScale;
  double falloff     = m_falloff->getValue(frame) * radiusScale;
  if (!(std::max(width, falloff) > 0.0)) return false;

  mapAnchors(aff, restAnchors);
  mapAnchors(aff, deformedAnchors);

  deformer.reset(
      new TCurveDeformer(restAnchors, deformedAnchors, width, falloff));
  if (!deformer->isValid() || deformer->isIdentity(1e-6 * radiusScale))
    return false;

  rest.clear();
  for (const TBezierChain::Anchor &a : restAnchors)
    rest.push_back(a.m_pos), rest.push_back(a.m_in), rest.push_back(a.m_out);
  return true;
}

//-------------------------------------------------------------------

bool CurveDeformerFx::doGetBBox(double frame, TRectD &bBox,
                                const TRenderSettings &info) {
  if (!m_input.isConnected()) {
    bBox = TRectD();
    return false;
  }

  bool ret = m_input->doGetBBox(frame, bBox, info);

  std::unique_ptr<TCurveDeformer> deformer;
  std::vector<TPointD> rest;
  if (!buildDeformer(frame, TAffine(), deformer, rest)) return ret;

  // An infinite bbox stays infinite
  bBox = deformer->deformedBounds(bBox, frame);
  return ret;
}

//-------------------------------------------------------------------

TRectD CurveDeformerFx::getSourceRect(double frame, const TRectD &rect,
                                      const TRenderSettings &ri) {
  if (rect.isEmpty()) return TRectD();

  std::unique_ptr<TCurveDeformer> deformer;
  std::vector<TPointD> rest;
  if (!buildDeformer(frame, ri.m_affine, deformer, rest)) return rect;

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
  std::unique_ptr<TCurveDeformer> deformer;
  std::vector<TPointD> rest;
  if (!buildDeformer(frame, ri.m_affine, deformer, rest)) {
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
  int maxLength = 2 * CurveParams::maxConceptCount();
  concepts      = new TParamUIConcept[maxLength];

  int count = m_rest.getParamUIs(concepts, "Rest ");
  count += m_deformed.getParamUIs(concepts + count, "");
  length = count;
}

//==============================================================================

FX_PLUGIN_IDENTIFIER(CurveDeformerFx, "curveDeformerFx");
