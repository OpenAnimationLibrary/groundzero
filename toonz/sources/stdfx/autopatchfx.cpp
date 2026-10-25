#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "ttzpimagefx.h"
#include "trop.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

//==============================================================================
//    Auto-Patch Fx
//
// Hides the seam at the joint of two cut-out pieces, similar to Toon Boom
// Harmony's Auto-Patch node. Top is the piece drawn in front (e.g. the
// forearm), Bottom the piece behind it (e.g. the upper arm). The plain
// composite shows Top's outline where it crosses Bottom's paint; Auto-Patch
// paints over that part of the line, so the two pieces read as one shape,
// while the outer silhouette - where Top's line lies over nothing or over
// Bottom's own line - is kept:
//
//          Top over Bottom                     Auto-Patch
//   +-----------+--------------+      +--------------------------+
//   |  Bottom   (     Top      |      |  Bottom          Top     |
//   +-----------+--------------+      +--------------------------+
//
//   Bottom's line under Top is hidden by Top's paint in both cases; '(' is
//   Top's line over Bottom's paint, the seam that Auto-Patch removes.
//
// The inputs are rendered several times, with palette filter render data
// (as in the Palette Filter fx) asking the level columns for their paint or
// their lines only:
//
//   Base   = Top over Bottom                        (the plain composite)
//   Patch  = Top paint over Bottom paint            (no lines at all)
//   r      = alpha of Bottom paint, where Top lines have any coverage,
//            times the alpha of the Mask when it is connected,
//            or else times the Joint Region weight (see below)
//   result = Base + r * (Patch - Base)
//
// Under Top's line Top has no paint, so the seam takes Bottom's paint; along
// the inner edge of the line it takes Top's paint. Where Bottom has a line,
// its paint (and r) drops to zero and the crossing lines stay.
//
// All of Top's lines over Bottom's paint are treated as joint lines,
// including detail lines drawn inside Top: connect a Mask around the joint
// to limit the patch to it.
//
// Joint Region: without building a mask column, set Joint Radius above 0 and
// place the Joint Point (it has a viewer gadget) on the joint. The patch is
// then limited to a disc around that point, an automatic circular mask:
//
//   w(d), d = distance from the Joint Point
//
//   1 +--------+
//     |        :..
//     |        :  ..          w = 1 for d <= Radius
//     |        :    ..        w = smoothstep down to 0, over Softness
//   0 +--------+------+--> d  w = 0 for d >= Radius + Softness
//          Radius   Radius
//                  + Softness
//
// The weight multiplies r just like a connected Mask would; a connected Mask
// takes precedence and the Joint Region is then ignored. Joint Radius 0, the
// default, switches the region off, so old scenes render as before.
//
// Enable Patch (on by default) off outputs the plain Top over Bottom. This
// is the way to see the pieces unpatched while keeping both of them: the
// render graph's own handling of a disabled fx (the eye / Enable toggle)
// only passes one input port through (getPreferredInputPort, here Top) and
// offers no hook for compositing two ports, so it would drop Bottom.
//
// The paint / lines split only applies to Toonz Vector and Toonz Raster
// levels. Other kinds of levels ignore the filters and render whole, so for
// them Patch equals Base and the fx falls back to plain Top over Bottom.
//==============================================================================

namespace {

// Adds palette filter render data keeping only the paint (paintOnly) or only
// the lines of the Toonz levels upstream.
TRenderSettings filteredSettings(const TRenderSettings &info, bool paintOnly) {
  TRenderSettings ri(info);

  // No listed style: nothing more than "all lines" / "all areas" is deleted
  PaletteFilterFxRenderData *data = new PaletteFilterFxRenderData;
  data->m_keep                    = false;
  data->m_type =
      paintOnly ? eApplyToPaintsDeletingAllInks : eApplyToInksDeletingAllPaints;

  ri.m_data.push_back(TRasterFxRenderDataP(data));
  ri.m_userCachable = false;
  return ri;
}

//------------------------------------------------------------------------------

// The automatic circular mask around the joint point, in the tile's pixels
struct JointRegion {
  bool m_active;
  TPointD m_center;  // relative to the tile's lower left corner
  double m_radius, m_softness;
  JointRegion() : m_active(false), m_radius(0.0), m_softness(0.0) {}

  // 1 up to the radius, then smoothly down to 0 over the softness
  double weight(double x, double y) const {
    double dx = x + 0.5 - m_center.x, dy = y + 0.5 - m_center.y;
    double d2 = dx * dx + dy * dy;
    if (d2 <= m_radius * m_radius) return 1.0;
    if (m_softness <= 0.0) return 0.0;
    double t = (sqrt(d2) - m_radius) / m_softness;
    if (t >= 1.0) return 0.0;
    return 1.0 - t * t * (3.0 - 2.0 * t);
  }
};

//------------------------------------------------------------------------------

template <class PIXEL>
void applyPatch(const TRasterPT<PIXEL> &ras, const TRasterPT<PIXEL> &patch,
                const TRasterPT<PIXEL> &bottomPaint,
                const TRasterPT<PIXEL> &topLines, const TRasterPT<PIXEL> &mask,
                const JointRegion &region) {
  typedef typename PIXEL::Channel Channel;
  const double maxValue = PIXEL::maxChannelValue;
  // Rounding for integer channels only
  const double rounding = std::is_integral<Channel>::value ? 0.5 : 0.0;

  ras->lock();
  patch->lock();
  bottomPaint->lock();
  topLines->lock();
  if (mask) mask->lock();
  for (int y = 0; y < ras->getLy(); ++y) {
    PIXEL *pix            = ras->pixels(y);
    const PIXEL *patchPix = patch->pixels(y);
    const PIXEL *paintPix = bottomPaint->pixels(y);
    const PIXEL *linePix  = topLines->pixels(y);
    const PIXEL *maskPix  = mask ? mask->pixels(y) : 0;
    for (int x = 0; x < ras->getLx();
         ++x, ++pix, ++patchPix, ++paintPix, ++linePix) {
      double r = 0.0;
      if (linePix->m > 0) {
        r = std::min(std::max(paintPix->m / maxValue, 0.0), 1.0);
        if (maskPix)
          r *= std::min(std::max(maskPix->m / maxValue, 0.0), 1.0);
        else if (region.m_active && r > 0.0)
          r *= region.weight(x, y);
      }
      if (maskPix) ++maskPix;
      if (r <= 0.0) continue;

      // Premultiplied channels: a straight interpolation
      pix->r = (Channel)(pix->r + r * (patchPix->r - pix->r) + rounding);
      pix->g = (Channel)(pix->g + r * (patchPix->g - pix->g) + rounding);
      pix->b = (Channel)(pix->b + r * (patchPix->b - pix->b) + rounding);
      pix->m = (Channel)(pix->m + r * (patchPix->m - pix->m) + rounding);
    }
  }
  if (mask) mask->unlock();
  topLines->unlock();
  bottomPaint->unlock();
  patch->unlock();
  ras->unlock();
}

}  // namespace

//------------------------------------------------------------------------------

class AutoPatchFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(AutoPatchFx)

  TRasterFxPort m_top, m_bottom, m_mask;
  TBoolParamP m_enablePatch;
  TPointParamP m_jointPoint;
  TDoubleParamP m_jointRadius, m_softness;

public:
  AutoPatchFx()
      : m_enablePatch(true)
      , m_jointPoint(TPointD(0.0, 0.0))
      , m_jointRadius(0.0)
      , m_softness(10.0) {
    m_jointPoint->getX()->setMeasureName("fxLength");
    m_jointPoint->getY()->setMeasureName("fxLength");
    m_jointRadius->setMeasureName("fxLength");
    m_softness->setMeasureName("fxLength");
    m_jointRadius->setValueRange(0, (std::numeric_limits<double>::max)());
    m_softness->setValueRange(0, (std::numeric_limits<double>::max)());
    bindParam(this, "enablePatch", m_enablePatch);
    bindParam(this, "jointPoint", m_jointPoint);
    bindParam(this, "jointRadius", m_jointRadius);
    bindParam(this, "softness", m_softness);
    addInputPort("Top", m_top);
    addInputPort("Bottom", m_bottom);
    addInputPort("Mask", m_mask);
  }

  bool doGetBBox(double frame, TRectD &bBox,
                 const TRenderSettings &info) override {
    bBox     = TRectD();
    bool ret = false;
    TRectD inBBox;
    if (m_top.isConnected() && m_top->doGetBBox(frame, inBBox, info)) {
      bBox += inBBox;
      ret = true;
    }
    if (m_bottom.isConnected() && m_bottom->doGetBBox(frame, inBBox, info)) {
      bBox += inBBox;
      ret = true;
    }
    return ret;
  }

  bool canHandle(const TRenderSettings &info, double frame) override {
    return true;
  }

  // The ports are rendered with palette filters
  bool allowUserCacheOnPort(int port) override { return false; }

  void getParamUIs(TParamUIConcept *&concepts, int &length) override {
    concepts = new TParamUIConcept[length = 2];

    concepts[0].m_type  = TParamUIConcept::POINT;
    concepts[0].m_label = "Joint Point";
    concepts[0].m_params.push_back(m_jointPoint);

    concepts[1].m_type  = TParamUIConcept::RADIUS;
    concepts[1].m_label = "Joint Radius";
    concepts[1].m_params.push_back(m_jointRadius);
    concepts[1].m_params.push_back(m_jointPoint);
  }

  void doDryCompute(TRectD &rect, double frame,
                    const TRenderSettings &info) override;
  void doCompute(TTile &tile, double frame, const TRenderSettings &ri) override;
};

//------------------------------------------------------------------------------

void AutoPatchFx::doDryCompute(TRectD &rect, double frame,
                               const TRenderSettings &info) {
  if (m_bottom.isConnected()) m_bottom->dryCompute(rect, frame, info);
  if (m_top.isConnected()) m_top->dryCompute(rect, frame, info);
  if (!m_top.isConnected() || !m_bottom.isConnected()) return;
  if (!m_enablePatch->getValue()) return;

  TRenderSettings paintInfo(filteredSettings(info, true));
  m_bottom->dryCompute(rect, frame, paintInfo);
  m_top->dryCompute(rect, frame, paintInfo);
  m_top->dryCompute(rect, frame, filteredSettings(info, false));
  if (m_mask.isConnected()) m_mask->dryCompute(rect, frame, info);
}

//------------------------------------------------------------------------------

void AutoPatchFx::doCompute(TTile &tile, double frame,
                            const TRenderSettings &ri) {
  if (!m_bottom.isConnected()) {
    if (m_top.isConnected()) m_top->compute(tile, frame, ri);
    return;
  }

  m_bottom->compute(tile, frame, ri);
  if (!m_top.isConnected()) return;

  // The plain composite
  const TRasterP ras    = tile.getRaster();
  const TDimension size = ras->getSize();
  const TPointD &pos    = tile.m_pos;

  TTile topTile;
  m_top->allocateAndCompute(topTile, pos, size, ras, frame, ri);
  TRop::over(ras, topTile.getRaster());

  // Patch off: the plain composite
  if (!m_enablePatch->getValue()) return;

  // Top's lines decide where to patch
  TTile topLinesTile;
  m_top->allocateAndCompute(topLinesTile, pos, size, ras, frame,
                            filteredSettings(ri, false));

  // The patch: the paint of both pieces, Top in front
  TRenderSettings paintRi(filteredSettings(ri, true));
  TTile bottomPaintTile, topPaintTile;
  m_bottom->allocateAndCompute(bottomPaintTile, pos, size, ras, frame, paintRi);
  m_top->allocateAndCompute(topPaintTile, pos, size, ras, frame, paintRi);

  const TRasterP bottomPaint = bottomPaintTile.getRaster();
  const TRasterP topLines    = topLinesTile.getRaster();
  TRasterP patch             = bottomPaint->clone();
  TRop::over(patch, topPaintTile.getRaster());

  TTile maskTile;
  TRasterP mask;
  JointRegion region;
  if (m_mask.isConnected()) {
    m_mask->allocateAndCompute(maskTile, pos, size, ras, frame, ri);
    mask = maskTile.getRaster();
  } else {
    // Without a Mask, an optional disc around the joint point
    double scale  = sqrt(fabs(ri.m_affine.det()));
    double radius = m_jointRadius->getValue(frame) * scale;
    if (radius > 0.0) {
      region.m_active   = true;
      region.m_center   = ri.m_affine * m_jointPoint->getValue(frame) - pos;
      region.m_radius   = radius;
      region.m_softness = m_softness->getValue(frame) * scale;
    }
  }

  if (TRaster32P ras32 = ras)
    applyPatch<TPixel32>(ras32, patch, bottomPaint, topLines, mask, region);
  else if (TRaster64P ras64 = ras)
    applyPatch<TPixel64>(ras64, patch, bottomPaint, topLines, mask, region);
  else if (TRasterFP rasF = ras)
    applyPatch<TPixelF>(rasF, patch, bottomPaint, topLines, mask, region);
  else
    throw TException("Auto-Patch: unsupported pixel type");
}

//------------------------------------------------------------------------------

FX_PLUGIN_IDENTIFIER(AutoPatchFx, "autoPatchFx")
