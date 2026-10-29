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

// Light Wrap: the background, blurred, bleeds onto the edges of the
// foreground, so a character takes the light of the scene it is placed in.
// The math is in celcomposite.h.
using CelPixels::bounded;
using CelPixels::discrete;
using CelPixels::megabytes;
using CelPixels::renderScale;

class LightWrapFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(LightWrapFx)
  TRasterFxPort m_source, m_background;
  TDoubleParamP m_width, m_softness, m_intensity, m_blend, m_output;

  CelComposite::WrapSettings settings(double frame,
                                      const TRenderSettings &render) const {
    const double scale = renderScale(render);
    CelComposite::WrapSettings s;
    s.width     = bounded(m_width->getValue(frame), 0, 1000) * scale;
    s.softness  = bounded(m_softness->getValue(frame), 0, 1000) * scale;
    s.intensity = bounded(m_intensity->getValue(frame), 0, 4);
    s.blend     = discrete(m_blend->getValue(frame), 1) == 1
                      ? CelComposite::WrapBlend::Add
                      : CelComposite::WrapBlend::Screen;
    return s;
  }
  bool overBackground(double frame) const {
    return discrete(m_output->getValue(frame), 1) == 0;
  }

public:
  LightWrapFx()
      : m_width(8.0)
      , m_softness(4.0)
      , m_intensity(0.6)
      , m_blend(0.0)
      , m_output(0.0) {
    addInputPort("Source", m_source);
    addInputPort("Background", m_background);
    m_width->setValueRange(0, 1000);
    m_width->setUISliderRange(0, 50);
    m_softness->setValueRange(0, 1000);
    m_softness->setUISliderRange(0, 50);
    m_intensity->setValueRange(0, 4);
    m_intensity->setUISliderRange(0, 2);
    m_blend->setValueRange(0, 1);
    m_blend->setUIChoices({"Screen", "Add"});
    m_output->setValueRange(0, 1);
    m_output->setUIChoices({"Over Background", "Foreground Only"});
    bindParam(this, "width", m_width);
    bindParam(this, "softness", m_softness);
    bindParam(this, "intensity", m_intensity);
    bindParam(this, "blend", m_blend);
    bindParam(this, "output", m_output);
    enableComputeInFloat(true);
  }
  bool canHandle(const TRenderSettings &info, double) override {
    return CelPixels::isotropic(info.m_affine);
  }
  bool doGetBBox(double frame, TRectD &box,
                 const TRenderSettings &settings) override {
    box = TRectD();
    TRectD source, background;
    if (m_source.isConnected()) m_source->doGetBBox(frame, source, settings);
    if (overBackground(frame) && m_background.isConnected())
      m_background->doGetBBox(frame, background, settings);
    box = source + background;
    return !box.isEmpty();
  }
  void transform(double frame, int, const TRectD &output,
                 const TRenderSettings &settings, TRectD &input,
                 TRenderSettings &inputSettings) override {
    inputSettings = settings;
    input =
        output.enlarge(CelComposite::wrapHalo(this->settings(frame, settings)));
  }
  void doDryCompute(TRectD &rect, double frame,
                    const TRenderSettings &settings) override {
    const int halo = CelComposite::wrapHalo(this->settings(frame, settings));
    TRectD input   = rect.enlarge(halo);
    if (m_source.isConnected()) m_source->dryCompute(input, frame, settings);
    input = rect.enlarge(halo);
    if (m_background.isConnected())
      m_background->dryCompute(input, frame, settings);
  }
  int getMemoryRequirement(const TRectD &rect, double frame,
                           const TRenderSettings &settings) override {
    // Two sources, the blurred light, its pass, a mask, a field and the result
    return megabytes(rect,
                     CelComposite::wrapHalo(this->settings(frame, settings)),
                     5 * 32 + 16);
  }
  std::string getAlias(double frame,
                       const TRenderSettings &settings) const override {
    std::ostringstream alias;
    alias.imbue(std::locale::classic());
    const CelComposite::WrapSettings s = this->settings(frame, settings);
    alias << TRasterFx::getAlias(frame, settings) << "[lightWrap:1:"
          << std::setprecision(std::numeric_limits<double>::max_digits10)
          << s.width << ',' << s.softness << ',' << s.intensity << ','
          << int(s.blend) << ',' << overBackground(frame) << ']';
    return alias.str();
  }
  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &settings) override {
    tile.getRaster()->clear();
    if (settings.m_isCanceled && *settings.m_isCanceled) return;
    const CelComposite::WrapSettings s = this->settings(frame, settings);
    const int halo                     = CelComposite::wrapHalo(s);
    TTile source, background;
    source.setRaster(CelPixels::padded(tile.getRaster(), halo));
    source.m_pos = tile.m_pos - TPointD(halo, halo);
    background.setRaster(CelPixels::padded(tile.getRaster(), halo));
    background.m_pos = source.m_pos;
    if (m_source.isConnected()) m_source->compute(source, frame, settings);
    if (m_background.isConnected())
      m_background->compute(background, frame, settings);
    if (settings.m_isCanceled && *settings.m_isCanceled) return;
    using CelComposite::Color;
    const int w = source.getRaster()->getLx(), h = source.getRaster()->getLy();
    const std::vector<Color> fg = CelPixels::read<Color>(source.getRaster());
    const std::vector<Color> bg =
        CelPixels::read<Color>(background.getRaster());
    std::vector<Color> out;
    if (!CelComposite::lightWrap(fg, bg, w, h, s, out, settings.m_isCanceled))
      return;
    if (overBackground(frame))
      for (std::size_t i = 0; i < out.size(); ++i) {
        const double keep = 1.0 - out[i].a;
        out[i] = Color{out[i].r + bg[i].r * keep, out[i].g + bg[i].g * keep,
                       out[i].b + bg[i].b * keep, out[i].a + bg[i].a * keep};
      }
    CelPixels::write(out, w, halo, halo, tile.getRaster());
  }
};
FX_PLUGIN_IDENTIFIER(LightWrapFx, "lightWrapFx")
