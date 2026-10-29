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

// Matte Choke: the edge of a drawing's alpha moved in (choke) or out
// (spread), with a feather, keeping the colours' hue. The math is in
// celcomposite.h.
using CelPixels::bounded;
using CelPixels::discrete;
using CelPixels::megabytes;
using CelPixels::renderScale;

class MatteChokeFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(MatteChokeFx)
  TRasterFxPort m_source;
  TDoubleParamP m_amount, m_feather;

  CelComposite::ChokeSettings settings(double frame,
                                       const TRenderSettings &render) const {
    const double scale = renderScale(render);
    CelComposite::ChokeSettings s;
    s.amount  = bounded(m_amount->getValue(frame), -1000, 1000) * scale;
    s.feather = bounded(m_feather->getValue(frame), 0, 1000) * scale;
    return s;
  }

public:
  MatteChokeFx() : m_amount(-1.0), m_feather(0.5) {
    addInputPort("Source", m_source);
    m_amount->setValueRange(-1000, 1000);
    m_amount->setUISliderRange(-20, 20);
    m_feather->setValueRange(0, 1000);
    m_feather->setUISliderRange(0, 20);
    bindParam(this, "amount", m_amount);
    bindParam(this, "feather", m_feather);
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
    if (box != TConsts::infiniteRectD)
      box =
          box.enlarge(CelComposite::chokeHalo(this->settings(frame, settings)));
    return !box.isEmpty();
  }
  void transform(double frame, int, const TRectD &output,
                 const TRenderSettings &settings, TRectD &input,
                 TRenderSettings &inputSettings) override {
    inputSettings = settings;
    input         = output.enlarge(
        CelComposite::chokeHalo(this->settings(frame, settings)));
  }
  void doDryCompute(TRectD &rect, double frame,
                    const TRenderSettings &settings) override {
    if (!m_source.isConnected()) return;
    TRectD input =
        rect.enlarge(CelComposite::chokeHalo(this->settings(frame, settings)));
    m_source->dryCompute(input, frame, settings);
  }
  int getMemoryRequirement(const TRectD &rect, double frame,
                           const TRenderSettings &settings) override {
    return megabytes(
        rect, CelComposite::chokeHalo(this->settings(frame, settings)), 128);
  }
  std::string getAlias(double frame,
                       const TRenderSettings &settings) const override {
    std::ostringstream alias;
    alias.imbue(std::locale::classic());
    const CelComposite::ChokeSettings s = this->settings(frame, settings);
    alias << TRasterFx::getAlias(frame, settings) << "[matteChoke:1:"
          << std::setprecision(std::numeric_limits<double>::max_digits10)
          << s.amount << ',' << s.feather << ']';
    return alias.str();
  }
  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &settings) override {
    tile.getRaster()->clear();
    if (!m_source.isConnected() ||
        (settings.m_isCanceled && *settings.m_isCanceled))
      return;
    const CelComposite::ChokeSettings s = this->settings(frame, settings);
    const int halo                      = CelComposite::chokeHalo(s);
    TTile input;
    input.setRaster(CelPixels::padded(tile.getRaster(), halo));
    input.m_pos = tile.m_pos - TPointD(halo, halo);
    m_source->compute(input, frame, settings);
    if (settings.m_isCanceled && *settings.m_isCanceled) return;
    using CelComposite::Color;
    const int w = input.getRaster()->getLx(), h = input.getRaster()->getLy();
    std::vector<Color> out;
    if (!CelComposite::choke(CelPixels::read<Color>(input.getRaster()), w, h, s,
                             out, settings.m_isCanceled))
      return;
    CelPixels::write(out, w, halo, halo, tile.getRaster());
  }
};
FX_PLUGIN_IDENTIFIER(MatteChokeFx, "matteChokeFx")
