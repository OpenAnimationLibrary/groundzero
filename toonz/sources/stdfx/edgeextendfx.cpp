#include "stdfx.h"
#include "tfxparam.h"
#include "celcomposite.h"
#include "celpixels.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <vector>

// Edge Extend: the colour of a drawing's solid interior pushed out over its
// antialiased edge and beyond, so keys and blurs do not pick up the edge's
// mix with the old background. The math is in celcomposite.h.
using CelPixels::bounded;
using CelPixels::discrete;
using CelPixels::megabytes;
using CelPixels::renderScale;

class EdgeExtendFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(EdgeExtendFx)
  TRasterFxPort m_source;
  TDoubleParamP m_radius, m_solid, m_extendAlpha;

  CelComposite::ExtendSettings settings(double frame,
                                        const TRenderSettings &render) const {
    CelComposite::ExtendSettings s;
    s.radius =
        bounded(m_radius->getValue(frame), 0, 1000) * renderScale(render);
    s.solid       = bounded(m_solid->getValue(frame), 0.01, 1);
    s.extendAlpha = discrete(m_extendAlpha->getValue(frame), 1) == 1;
    return s;
  }

public:
  EdgeExtendFx() : m_radius(4.0), m_solid(0.99), m_extendAlpha(0.0) {
    addInputPort("Source", m_source);
    m_radius->setValueRange(0, 1000);
    m_radius->setUISliderRange(0, 50);
    m_solid->setValueRange(0.01, 1);
    m_extendAlpha->setValueRange(0, 1);
    m_extendAlpha->setUIChoices({"Off", "On"}, true);
    bindParam(this, "radius", m_radius);
    bindParam(this, "solid", m_solid);
    bindParam(this, "extendAlpha", m_extendAlpha);
    enableComputeInFloat(true);
  }
  bool canHandle(const TRenderSettings &info, double) override {
    return CelPixels::isotropic(info.m_affine);
  }
  bool doGetBBox(double frame, TRectD &box,
                 const TRenderSettings &settings) override {
    box = TRectD();
    if (!m_source.isConnected() || !m_source->doGetBBox(frame, box, settings))
      return false;
    if (discrete(m_extendAlpha->getValue(frame), 1) == 1 &&
        box != TConsts::infiniteRectD)
      box = box.enlarge(
          CelComposite::extendHalo(this->settings(frame, settings)));
    return !box.isEmpty();
  }
  void transform(double frame, int, const TRectD &output,
                 const TRenderSettings &settings, TRectD &input,
                 TRenderSettings &inputSettings) override {
    inputSettings = settings;
    input         = output.enlarge(
        CelComposite::extendHalo(this->settings(frame, settings)));
  }
  void doDryCompute(TRectD &rect, double frame,
                    const TRenderSettings &settings) override {
    if (!m_source.isConnected()) return;
    TRectD input =
        rect.enlarge(CelComposite::extendHalo(this->settings(frame, settings)));
    m_source->dryCompute(input, frame, settings);
  }
  int getMemoryRequirement(const TRectD &rect, double frame,
                           const TRenderSettings &settings) override {
    return megabytes(
        rect, CelComposite::extendHalo(this->settings(frame, settings)), 96);
  }
  std::string getAlias(double frame,
                       const TRenderSettings &settings) const override {
    std::ostringstream alias;
    alias.imbue(std::locale::classic());
    const CelComposite::ExtendSettings s = this->settings(frame, settings);
    alias << TRasterFx::getAlias(frame, settings) << "[edgeExtend:1:"
          << std::setprecision(std::numeric_limits<double>::max_digits10)
          << s.radius << ',' << s.solid << ',' << s.extendAlpha << ']';
    return alias.str();
  }
  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &settings) override {
    tile.getRaster()->clear();
    if (!m_source.isConnected() ||
        (settings.m_isCanceled && *settings.m_isCanceled))
      return;
    const CelComposite::ExtendSettings s = this->settings(frame, settings);
    const int halo                       = CelComposite::extendHalo(s);
    TTile input;
    input.setRaster(CelPixels::padded(tile.getRaster(), halo));
    input.m_pos = tile.m_pos - TPointD(halo, halo);
    m_source->compute(input, frame, settings);
    if (settings.m_isCanceled && *settings.m_isCanceled) return;
    using CelComposite::Color;
    const int w = input.getRaster()->getLx(), h = input.getRaster()->getLy();
    std::vector<Color> out;
    if (!CelComposite::edgeExtend(CelPixels::read<Color>(input.getRaster()), w,
                                  h, s, out, settings.m_isCanceled))
      return;
    CelPixels::write(out, w, halo, halo, tile.getRaster());
  }
};
FX_PLUGIN_IDENTIFIER(EdgeExtendFx, "edgeExtendFx")
