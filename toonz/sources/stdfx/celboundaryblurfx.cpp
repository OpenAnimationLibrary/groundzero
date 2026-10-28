#include "stdfx.h"
#include "tfxparam.h"
#include "celblur.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace {
double blurValue(double value, double low, double high) {
  return std::isfinite(value) ? std::max(low, std::min(high, value)) : low;
}

template <class Pixel> double blurChannelScale() {
  return Pixel::maxChannelValue;
}
template <> double blurChannelScale<TPixelF>() { return 1; }

template <class Pixel>
bool blurRaster(const TRasterPT<Pixel> &input, const TRasterPT<Pixel> &output,
                int margin, const CelBlur::Settings &settings,
                const int *canceled) {
  const int width = input->getLx(), height = input->getLy();
  std::vector<CelBlur::Pixel> pixels(CelBlur::checkedArea(width, height));
  const double scale = blurChannelScale<Pixel>();
  input->lock();
  for (int y = 0; y < height; ++y) {
    if (canceled && *canceled) { input->unlock(); return false; }
    const Pixel *row = input->pixels(y);
    for (int x = 0; x < width; ++x) {
      if (row[x].m <= 0 || !std::isfinite(double(row[x].m)) ||
          !std::isfinite(double(row[x].r)) || !std::isfinite(double(row[x].g)) ||
          !std::isfinite(double(row[x].b))) continue;
      pixels[std::size_t(y) * width + x] =
          {{row[x].r / scale, row[x].g / scale, row[x].b / scale,
            row[x].m / scale}};
    }
  }
  input->unlock();
  if (!CelBlur::filter(pixels, width, height, settings, canceled)) return false;
  output->lock();
  for (int y = 0; y < output->getLy(); ++y) {
    if (canceled && *canceled) { output->unlock(); return false; }
    Pixel *row = output->pixels(y);
    for (int x = 0; x < output->getLx(); ++x) {
      const auto &p = pixels[std::size_t(y + margin) * width + x + margin];
      auto channel = [=](double value) {
        return scale == 1 ? value : std::floor(blurValue(value, 0, 1) * scale + 0.5);
      };
      row[x] = Pixel(channel(p[0]), channel(p[1]), channel(p[2]), channel(p[3]));
    }
  }
  output->unlock();
  return true;
}
}  // namespace

class CelBoundaryBlurFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(CelBoundaryBlurFx)
  TRasterFxPort m_source;
  TDoubleParamP m_amount, m_smoothness, m_repeats, m_bias;

  CelBlur::Settings parameters(double frame, const TRenderSettings &info) const {
    CelBlur::Settings result;
    result.radius = blurValue(m_amount->getValue(frame), 1, 100) *
                    std::sqrt(std::abs(info.m_affine.det()));
    result.blend = blurValue(m_smoothness->getValue(frame), 1, 100) / 100;
    result.repeats = int(blurValue(m_repeats->getValue(frame), 0, 10) + 0.5);
    result.vertical = blurValue(m_bias->getValue(frame), 0, 1) < 0.5;
    return result;
  }

public:
  CelBoundaryBlurFx()
      : m_amount(10), m_smoothness(50), m_repeats(1), m_bias(0) {
    addInputPort("Source", m_source);
    bindParam(this, "amount", m_amount);
    bindParam(this, "smoothness", m_smoothness);
    bindParam(this, "repeats", m_repeats);
    bindParam(this, "bias", m_bias);
    m_amount->setValueRange(1, 100);
    m_smoothness->setValueRange(1, 100);
    m_repeats->setValueRange(0, 10);
    m_bias->setValueRange(0, 1);
    m_bias->setUIChoices({"Vertical", "Horizontal"});
    enableComputeInFloat(true);
  }

  bool requiresFullPrecisionSerialization() const override { return true; }
  bool canHandle(const TRenderSettings &info, double) override {
    // The host applies rotation/shear after filtering in the cel's own axes.
    return info.m_affine.a12 == 0 && info.m_affine.a21 == 0 &&
           info.m_affine.a11 == info.m_affine.a22 && info.m_affine.a11 > 0;
  }
  bool toBeComputedInLinearColorSpace(bool, bool) const override { return false; }
  bool doGetBBox(double frame, TRectD &box, const TRenderSettings &info) override {
    if (!m_source.isConnected()) { box = TRectD(); return false; }
    return m_source->doGetBBox(frame, box, info);
  }
  void transform(double frame, int, const TRectD &out,
                 const TRenderSettings &info, TRectD &in,
                 TRenderSettings &inputInfo) override {
    inputInfo = info;
    in = out.enlarge(CelBlur::support(parameters(frame, info)));
  }
  void doDryCompute(TRectD &rect, double frame,
                    const TRenderSettings &info) override {
    if (!m_source.isConnected() || (info.m_isCanceled && *info.m_isCanceled)) return;
    TRectD request = rect.enlarge(CelBlur::support(parameters(frame, info)));
    m_source->dryCompute(request, frame, info);
  }
  int getMemoryRequirement(const TRectD &rect, double frame,
                           const TRenderSettings &info) override {
    return memorySize(rect.enlarge(CelBlur::support(parameters(frame, info))),
                      160 * 8);
  }
  std::string getAlias(double frame, const TRenderSettings &info) const override {
    const auto values = parameters(frame, info);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << TRasterFx::getAlias(frame, info) << "[celBoundary1:"
        << std::setprecision(std::numeric_limits<double>::max_digits10)
        << values.radius << ',' << values.blend << ',' << values.repeats
        << ',' << values.vertical << ']';
    return out.str();
  }
  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &info) override {
    tile.getRaster()->clear();
    if (!m_source.isConnected() || (info.m_isCanceled && *info.m_isCanceled)) return;
    const auto values = parameters(frame, info);
    const int margin = CelBlur::support(values);
    if (!values.repeats) {
      m_source->compute(tile, frame, info);
      if (info.m_isCanceled && *info.m_isCanceled) tile.getRaster()->clear();
      return;
    }
    const int width = tile.getRaster()->getLx(), height = tile.getRaster()->getLy();
    if (width > std::numeric_limits<int>::max() - 2 * margin ||
        height > std::numeric_limits<int>::max() - 2 * margin)
      throw TException("Cel Blur: requested image is too large");
    CelBlur::checkedArea(width + 2 * margin, height + 2 * margin);
    TTile input;
    m_source->allocateAndCompute(input, tile.m_pos - TPointD(margin, margin),
        TDimension(width + 2 * margin, height + 2 * margin), tile.getRaster(), frame, info);
    bool complete = false;
    if (TRaster32P output = tile.getRaster())
      complete = blurRaster<TPixel32>(input.getRaster(), output, margin, values, info.m_isCanceled);
    else if (TRaster64P output = tile.getRaster())
      complete = blurRaster<TPixel64>(input.getRaster(), output, margin, values, info.m_isCanceled);
    else if (TRasterFP output = tile.getRaster())
      complete = blurRaster<TPixelF>(input.getRaster(), output, margin, values, info.m_isCanceled);
    else throw TException("Cel Blur: unsupported raster type");
    if (!complete) tile.getRaster()->clear();
  }
};

FX_PLUGIN_IDENTIFIER(CelBoundaryBlurFx, "celBoundaryBlurFx")
