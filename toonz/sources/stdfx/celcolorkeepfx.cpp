#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace {
constexpr int colorCapacity = 100;

int activeColorCount(double value) {
  if (!std::isfinite(value)) return 0;
  return int(std::floor(std::max(0.0, std::min(100.0, value)) + 0.5));
}

std::string colorSlotName(int index) {
  std::ostringstream name;
  name.imbue(std::locale::classic());
  name << "color" << std::setw(3) << std::setfill('0') << index + 1;
  return name.str();
}

// Compare in the source's channel precision. Multiplying the selected straight
// RGB by source alpha avoids division/rounding errors on premultiplied cels.
template <class Pixel>
bool matchesColor(const Pixel &pixel, const TPixelD &color) {
  const double alpha = double(pixel.m);
  auto channelMatches = [alpha](double channel, double selected) {
    if (!std::isfinite(selected) || selected < 0.0 || selected > 1.0)
      return false;
    return channel == std::floor(selected * alpha + 0.5);
  };
  return channelMatches(pixel.r, color.r) &&
         channelMatches(pixel.g, color.g) &&
         channelMatches(pixel.b, color.b);
}

template <>
bool matchesColor(const TPixelF &pixel, const TPixelD &color) {
  // Floating RGB remains unclamped, including finite HDR values. The tolerance
  // is fixed in straight RGB, so partial coverage does not broaden the key.
  const float tolerance = 0.0001f * pixel.m;
  return std::fabs(pixel.r - float(color.r) * pixel.m) <= tolerance &&
         std::fabs(pixel.g - float(color.g) * pixel.m) <= tolerance &&
         std::fabs(pixel.b - float(color.b) * pixel.m) <= tolerance;
}

template <class Pixel>
void keepColors(const TRasterPT<Pixel> &raster,
                const std::array<TPixelD, colorCapacity> &colors, int count) {
  raster->lock();
  for (int y = 0; y < raster->getLy(); ++y) {
    Pixel *row = raster->pixels(y);
    for (int x = 0; x < raster->getLx(); ++x) {
      Pixel &pixel = row[x];
      bool keep = false;
      if (pixel.m > 0 && std::isfinite(double(pixel.m))) {
        for (int i = 0; i < count; ++i) {
          if (matchesColor(pixel, colors[i])) {
            keep = true;
            break;
          }
        }
      }
      // Zero RGB as well as alpha: downstream compositors expect premultiplied
      // pixels and would otherwise let rejected RGB leak into the composite.
      if (!keep) pixel = Pixel::Transparent;
    }
  }
  raster->unlock();
}
}  // namespace

class CelColorKeepFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(CelColorKeepFx)

  TRasterFxPort m_source;
  TDoubleParamP m_enabledColors;
  std::array<TPixelParamP, colorCapacity> m_colors;

public:
  CelColorKeepFx() : m_enabledColors(1.0) {
    addInputPort("Source", m_source);
    bindParam(this, "enabledColors", m_enabledColors);
    m_enabledColors->setValueRange(0.0, 100.0);
    for (int i = 0; i < colorCapacity; ++i) {
      m_colors[i] = TPixelParamP(TPixel32::Black);
      m_colors[i]->enableMatte(false);
      bindParam(this, colorSlotName(i), m_colors[i]);
    }
    enableComputeInFloat(true);
  }

  bool canHandle(const TRenderSettings &, double) override { return false; }

  bool toBeComputedInLinearColorSpace(bool, bool) const override {
    return false;
  }

  bool doGetBBox(double frame, TRectD &bbox,
                 const TRenderSettings &settings) override {
    if (!m_source.isConnected()) {
      bbox = TRectD();
      return false;
    }
    return m_source->doGetBBox(frame, bbox, settings);
  }

  std::string getAlias(double frame,
                       const TRenderSettings &settings) const override {
    // The base alias rounds parameter values to three decimals. Append the
    // evaluated RGB at full precision to distinguish fine keys and HDR colors.
    std::ostringstream alias;
    alias.imbue(std::locale::classic());
    alias << TRasterFx::getAlias(frame, settings) << "[celKeep:"
          << std::setprecision(std::numeric_limits<double>::max_digits10);
    const int count = activeColorCount(m_enabledColors->getValue(frame));
    alias << count;
    for (int i = 0; i < count; ++i) {
      const TPixelD color = m_colors[i]->getValueD(frame);
      alias << ';' << color.r << ',' << color.g << ',' << color.b;
    }
    return alias.str() + ']';
  }

  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &settings) override {
    if (!m_source.isConnected()) {
      tile.getRaster()->clear();
      return;
    }
    m_source->compute(tile, frame, settings);
    const int count = activeColorCount(m_enabledColors->getValue(frame));
    if (count == 0) {
      tile.getRaster()->clear();
      return;
    }
    // Per-render immutable values: tiles and frames never share mutable state.
    std::array<TPixelD, colorCapacity> colors;
    for (int i = 0; i < count; ++i) colors[i] = m_colors[i]->getValueD(frame);
    if (TRaster32P raster = tile.getRaster())
      keepColors<TPixel32>(raster, colors, count);
    else if (TRaster64P raster = tile.getRaster())
      keepColors<TPixel64>(raster, colors, count);
    else if (TRasterFP raster = tile.getRaster())
      keepColors<TPixelF>(raster, colors, count);
    else
      throw TException("Cel Color Keep: unsupported pixel type");
  }
};

FX_PLUGIN_IDENTIFIER(CelColorKeepFx, "celColorKeepFx")
