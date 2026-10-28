#include "stdfx.h"
#include "celsmoothing.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tpixelutils.h"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace CelSmootherLocal {
inline void checkInputSize(int width, int height, int pixelSize) {
  const std::size_t count = CelSmoothing::pixelCount(width, height);
  // The host allocator forms width * height * pixelSize in signed int.
  // Check that product before entering the allocator or constructing buffers.
  if (pixelSize <= 0 || count >
      std::size_t(std::numeric_limits<int>::max()) / std::size_t(pixelSize))
    throw TException("Cel Smoother: source exceeds the raster allocation limit");
}
inline int smoothRange(double value) {
  return std::isfinite(value)
             ? int(std::floor(std::max(0.0, std::min(255.0, value)) + .5))
             : 6;
}
inline bool bounded(const TPixelD &c) {
  return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b) &&
         c.r >= 0 && c.g >= 0 && c.b >= 0 && c.r <= 1 && c.g <= 1 && c.b <= 1;
}
inline bool keyMatches(const TPixel32 &p, const TPixelD &c) {
  if (!bounded(c)) return false;
  TPixel32 key = toPixel32(TPixelD(c.r, c.g, c.b, 1));
  key.m = p.m;
  key = premultiply(key);
  return p.r == key.r && p.g == key.g && p.b == key.b;
}
inline bool keyMatches(const TPixel64 &p, const TPixelD &c) {
  if (!bounded(c)) return false;
  TPixel64 key = toPixel64(TPixelD(c.r, c.g, c.b, 1));
  key.m = p.m;
  key = premultiply(key);
  return p.r == key.r && p.g == key.g && p.b == key.b;
}
inline bool keyMatches(const TPixelF &p, const TPixelD &c) {
  const float tolerance = .0001f * p.m;
  return std::abs(p.r - float(c.r) * p.m) <= tolerance &&
         std::abs(p.g - float(c.g) * p.m) <= tolerance &&
         std::abs(p.b - float(c.b) * p.m) <= tolerance;
}
template <class Pixel> double maximum() { return Pixel::maxChannelValue; }
template <> inline double maximum<TPixelF>() { return 1; }
template <class Pixel> Pixel encode(const CelSmoothing::Pixel &p) {
  const double max = maximum<Pixel>();
  const auto channel = [max](double v) {
    return int(std::floor(std::max(0.0, std::min(1.0, v)) * max + .5));
  };
  return Pixel(channel(p.r), channel(p.g), channel(p.b), channel(p.a));
}
template <> inline TPixelF encode<TPixelF>(const CelSmoothing::Pixel &p) {
  return TPixelF(float(p.r), float(p.g), float(p.b), float(p.a));
}

struct RasterLock {
  TRasterP raster;
  explicit RasterLock(const TRasterP &r) : raster(r) { raster->lock(); }
  ~RasterLock() { raster->unlock(); }
};

template <class Pixel>
void render(const TRasterPT<Pixel> &input, const TRasterPT<Pixel> &output,
            int offsetX, int offsetY, int range, bool useKey,
            const TPixelD &key, const int *canceled) {
  const int width = input->getLx(), height = input->getLy();
  std::vector<CelSmoothing::Pixel> source(CelSmoothing::pixelCount(width, height));
  const double max = maximum<Pixel>();
  {
    RasterLock lock(input);
    for (int y = 0; y < height; ++y) {
      if (canceled && *canceled) return;
      const Pixel *row = input->pixels(y);
      for (int x = 0; x < width; ++x) {
        const Pixel &p = row[x];
        if (!(p.m > 0) || !std::isfinite(double(p.m)) ||
            !std::isfinite(double(p.r)) || !std::isfinite(double(p.g)) ||
            !std::isfinite(double(p.b)) || (useKey && keyMatches(p, key)))
          continue;
        source[std::size_t(y) * width + x] =
            {p.r / max, p.g / max, p.b / max, p.m / max};
      }
    }
  }
  std::vector<CelSmoothing::Pixel> smoothed;
  if (!CelSmoothing::smooth(width, height, source, smoothed, range, 100, canceled))
    return;
  {
    RasterLock lock(output);
    for (int y = 0; y < output->getLy(); ++y) {
      if (canceled && *canceled) break;
      const long long sy = static_cast<long long>(y) + offsetY;
      if (sy < 0 || sy >= height) continue;
      Pixel *row = output->pixels(y);
      for (int x = 0; x < output->getLx(); ++x) {
        const long long sx = static_cast<long long>(x) + offsetX;
        if (sx >= 0 && sx < width)
          row[x] = encode<Pixel>(smoothed[std::size_t(sy) * width + sx]);
      }
    }
  }
  if (canceled && *canceled) output->clear();
}
}  // namespace CelSmootherLocal

class CelSmootherFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(CelSmootherFx)
  TRasterFxPort m_source;
  TDoubleParamP m_useColorKey;
  TPixelParamP m_colorKey;
  TDoubleParamP m_smoothRange;

  TRectD domain(double frame, const TRenderSettings &settings) const {
    if (!m_source.isConnected()) return TRectD();
    TRenderSettings fullSettings(settings);
    fullSettings.m_getFullSizeBBox = true;
    TRectD box;
    static_cast<TRasterFx *>(m_source.getFx())->getBBox(frame, box, fullSettings);
    if (box == TConsts::infiniteRectD) box = settings.m_cameraBox;
    if (box.isEmpty()) return TRectD();
    if (!std::isfinite(box.x0) || !std::isfinite(box.y0) ||
        !std::isfinite(box.x1) || !std::isfinite(box.y1))
      throw TException("Cel Smoother: a finite source or camera is required");
    box = TRectD(std::floor(box.x0), std::floor(box.y0),
                 std::ceil(box.x1), std::ceil(box.y1));
    const double limit = std::numeric_limits<int>::max() / 16;
    if (box.getLx() > limit || box.getLy() > limit ||
        std::abs(box.x0) > limit || std::abs(box.y0) > limit ||
        box.getLx() * box.getLy() >
            double(std::numeric_limits<std::size_t>::max() /
                   (2 * sizeof(CelSmoothing::Pixel) + 16)))
      throw TException("Cel Smoother: source dimensions exceed storage limits");
    return box;
  }

public:
  CelSmootherFx()
      : m_useColorKey(0.0), m_colorKey(TPixel32::White), m_smoothRange(6.0) {
    addInputPort("Source", m_source);
    bindParam(this, "useColorKey", m_useColorKey);
    bindParam(this, "colorKey", m_colorKey);
    bindParam(this, "smoothRange", m_smoothRange);
    m_useColorKey->setValueRange(0, 1);
    m_useColorKey->setUIChoices({"Off", "On"}, true);
    m_colorKey->enableMatte(false);
    m_smoothRange->setValueRange(0, 255);
    enableComputeInFloat(true);
  }
  bool canHandle(const TRenderSettings &, double) override { return false; }
  TAffine handledAffine(const TRenderSettings &, double) override {
    return TAffine();
  }
  bool requiresFullPrecisionSerialization() const override { return true; }
  bool toBeComputedInLinearColorSpace(bool, bool) const override { return false; }
  bool doGetBBox(double frame, TRectD &box,
                 const TRenderSettings &settings) override {
    if (!m_source.isConnected()) { box = TRectD(); return false; }
    const bool present = m_source->doGetBBox(frame, box, settings);
    // Coverage can cross a source alpha bound into the adjacent canvas pixel.
    // Keep a conservative output bound even when the input reports tight alpha.
    if (present && !box.isEmpty() && box != TConsts::infiniteRectD)
      box = box.enlarge(1);
    return present;
  }
  void transform(double frame, int, const TRectD &,
                 const TRenderSettings &settings, TRectD &input,
                 TRenderSettings &inputSettings) override {
    inputSettings = settings;
    input = domain(frame, settings);
  }
  int getMemoryRequirement(const TRectD &, double frame,
                           const TRenderSettings &settings) override {
    const TRectD box = domain(frame, settings);
    const double bytes = box.getLx() * box.getLy() *
        (2 * sizeof(CelSmoothing::Pixel) + std::max(4, settings.m_bpp / 8));
    return int(std::min(double(std::numeric_limits<int>::max()),
                        std::ceil(std::max(0.0, bytes) / (1024 * 1024))));
  }
  std::string getAlias(double frame,
                       const TRenderSettings &settings) const override {
    std::ostringstream alias;
    alias.imbue(std::locale::classic());
    const TPixelD key = m_colorKey->getValueD(frame);
    const TRectD box = domain(frame, settings);
    alias << TRasterFx::getAlias(frame, settings) << "[celSmoother:1:"
          << std::setprecision(std::numeric_limits<double>::max_digits10)
          << (m_useColorKey->getValue(frame) >= .5) << ';' << key.r << ','
          << key.g << ',' << key.b << ';'
          << CelSmootherLocal::smoothRange(m_smoothRange->getValue(frame))
          << ';' << box.x0 << ',' << box.y0 << ',' << box.x1 << ',' << box.y1;
    return alias.str() + ']';
  }
  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &settings) override {
    tile.getRaster()->clear();
    if (!m_source.isConnected() ||
        (settings.m_isCanceled && *settings.m_isCanceled)) return;
    const TRectD box = domain(frame, settings);
    if (box.isEmpty()) return;
    const double dx = tile.m_pos.x - box.x0, dy = tile.m_pos.y - box.y0;
    const double limit = std::numeric_limits<int>::max() / 2;
    if (!std::isfinite(dx) || !std::isfinite(dy) ||
        std::abs(dx) > limit || std::abs(dy) > limit) return;
    if (dx != std::floor(dx) || dy != std::floor(dy))
      throw TException("Cel Smoother: the input must use the native pixel grid");
    const int range = CelSmootherLocal::smoothRange(m_smoothRange->getValue(frame));
    const bool useKey = m_useColorKey->getValue(frame) >= .5;
    const TPixelD key = m_colorKey->getValueD(frame);
    CelSmootherLocal::checkInputSize(int(box.getLx()), int(box.getLy()),
                                    tile.getRaster()->getPixelSize());
    TTile input;
    TRasterP sourceRaster = tile.getRaster()->create(int(box.getLx()), int(box.getLy()));
    sourceRaster->setLinear(tile.getRaster()->isLinear());
    sourceRaster->clear();
    input.setRaster(sourceRaster);
    input.m_pos = box.getP00();
    m_source->compute(input, frame, settings);
    if (settings.m_isCanceled && *settings.m_isCanceled) return;
    if (TRaster32P output = tile.getRaster())
      CelSmootherLocal::render<TPixel32>(input.getRaster(), output, int(dx), int(dy),
                                        range, useKey, key, settings.m_isCanceled);
    else if (TRaster64P output = tile.getRaster())
      CelSmootherLocal::render<TPixel64>(input.getRaster(), output, int(dx), int(dy),
                                        range, useKey, key, settings.m_isCanceled);
    else if (TRasterFP output = tile.getRaster())
      CelSmootherLocal::render<TPixelF>(input.getRaster(), output, int(dx), int(dy),
                                       range, useKey, key, settings.m_isCanceled);
    else throw TException("Cel Smoother: unsupported pixel type");
  }
};
FX_PLUGIN_IDENTIFIER(CelSmootherFx, "celSmootherFx")
