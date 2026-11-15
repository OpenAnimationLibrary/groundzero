#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tpixelutils.h"
#include "watercolour.h"
#include "celfxsourcegrid.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <vector>

// Watercolour: flat-colour art becomes a painted wash. Edges wander, colour
// bleeds past them, pigment gathers at the edges and in a grain, and the
// result can sit on grained paper. The noise holds for a number of frames.
// The math is in watercolour.h.
namespace {
double bounded(double value, double minimum, double maximum) {
  return std::isfinite(value) ? std::max(minimum, std::min(maximum, value))
                              : minimum;
}
int discrete(double value, int minimum, int maximum) {
  return int(std::floor(bounded(value, minimum, maximum) + 0.5));
}
TDimension paddedSize(int width, int height, int halo, int pixelBytes) {
  const int64_t w = int64_t(width) + 2LL * halo;
  const int64_t h = int64_t(height) + 2LL * halo;
  if (w <= 0 || h <= 0 || w > std::numeric_limits<int>::max() ||
      h > std::numeric_limits<int>::max() || pixelBytes <= 0 ||
      w * h > std::numeric_limits<int>::max() / pixelBytes)
    throw TException("Watercolour: requested raster is too large");
  return TDimension(int(w), int(h));
}
class RasterLock {
  TRasterP m_raster;

public:
  explicit RasterLock(const TRasterP &raster) : m_raster(raster) {
    m_raster->lock();
  }
  ~RasterLock() { m_raster->unlock(); }
};

template <class Pixel>
void paint(const TRasterPT<Pixel> &source, const TRasterPT<Pixel> &output,
           int halo, TPointD origin, const Watercolour::Settings &settings,
           double frame, const int *canceled) {
  const int w = source->getLx(), h = source->getLy();
  std::vector<Watercolour::Color> image(CelDistance::checkedArea(w, h));
  {
    RasterLock lock(source);
    for (int y = 0; y < h; ++y) {
      const Pixel *row = source->pixels(y);
      for (int x = 0; x < w; ++x) {
        const TPixelD p          = PixelConverter<TPixelD>::from(row[x]);
        image[size_t(y) * w + x] = {p.r, p.g, p.b, p.m};
      }
    }
  }
  std::vector<Watercolour::Color> result;
  if (!Watercolour::render(image, w, h, origin.x, origin.y, settings, frame,
                           result, canceled)) {
    output->clear();
    return;
  }
  RasterLock lock(output);
  for (int y = 0; y < output->getLy(); ++y) {
    Pixel *row = output->pixels(y);
    for (int x = 0; x < output->getLx(); ++x) {
      const Watercolour::Color &c = result[size_t(y + halo) * w + x + halo];
      row[x] = PixelConverter<Pixel>::from(TPixelD(c.r, c.g, c.b, c.a));
    }
  }
}
}  // namespace

class WatercolourFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(WatercolourFx)
public:
  // Per padded pixel: the source tile (up to 16 bytes), the source, the wash,
  // a blur pass and the result (32 bytes each), the edge amounts and the
  // distance field (8 each), its nearest seeds (4) and the seeds (1): 165
  static constexpr double bytesPerPixel = 192;

private:
  TRasterFxPort m_source;
  TDoubleParamP m_wobble, m_wobbleSize, m_bleed, m_edgeDarkening, m_edgeWidth,
      m_edgeTolerance, m_granulation, m_granuleSize, m_paperGrain, m_paperSize,
      m_hold, m_seed;
  TBoolParamP m_paper, m_whiteToTransparent;
  TPixelParamP m_paperColor;

  // Distances are in the level's own pixels
  double pixelScale(double frame, const TRenderSettings &settings) const {
    const TAffine drawing = settings.m_affine * CelFxSourceGrid::nativeToWorld(
                                                    m_source.getFx(), frame);
    const double scale = std::sqrt(std::abs(drawing.det()));
    return std::isfinite(scale) && std::abs(scale - 1) >= 1e-10 && scale > 0
               ? scale
               : 1.0;
  }
  // With everything off the source passes through
  bool isStill(double frame) const {
    return bounded(m_wobble->getValue(frame), 0, 1000) <= 0 &&
           bounded(m_bleed->getValue(frame), 0, 1000) < 1 &&
           bounded(m_edgeDarkening->getValue(frame), 0, 1) <= 0 &&
           bounded(m_granulation->getValue(frame), 0, 1) <= 0 &&
           bounded(m_paperGrain->getValue(frame), 0, 1) <= 0 &&
           !m_paper->getValue() && !m_whiteToTransparent->getValue();
  }

public:
  Watercolour::Settings settings(double frame,
                                 const TRenderSettings &render) const {
    const double scale = pixelScale(frame, render);
    Watercolour::Settings s;
    s.wobble = bounded(m_wobble->getValue(frame) * scale, 0,
                       Watercolour::maxWobble);
    s.wobbleSize = bounded(m_wobbleSize->getValue(frame) * scale, 1, 100);
    s.bleed =
        bounded(m_bleed->getValue(frame) * scale, 0, Watercolour::maxBleed);
    s.edgeDarkening = bounded(m_edgeDarkening->getValue(frame), 0, 1);
    s.edgeWidth     = bounded(m_edgeWidth->getValue(frame) * scale, 0,
                              Watercolour::maxEdgeWidth);
    s.edgeTolerance = bounded(m_edgeTolerance->getValue(frame), 0, 1);
    s.granulation   = bounded(m_granulation->getValue(frame), 0, 1);
    s.granuleSize   = bounded(m_granuleSize->getValue(frame) * scale, 1, 100);
    s.whiteToTransparent = m_whiteToTransparent->getValue();
    s.paper         = m_paper->getValue();
    const TPixelD paper = m_paperColor->getValueD(frame);
    s.paperColor        = Watercolour::Rgb{paper.r, paper.g, paper.b};
    s.paperGrain        = bounded(m_paperGrain->getValue(frame), 0, 1);
    s.paperSize  = bounded(m_paperSize->getValue(frame) * scale, 1, 100);
    s.hold       = discrete(m_hold->getValue(frame), 1, 1000);
    s.seed       = std::uint32_t(discrete(m_seed->getValue(frame), 0, 100000));
    return s;
  }

  WatercolourFx()
      : m_wobble(3.0)
      , m_wobbleSize(30.0)
      , m_bleed(4.0)
      , m_edgeDarkening(0.35)
      , m_edgeWidth(6.0)
      , m_edgeTolerance(0.08)
      , m_granulation(0.25)
      , m_granuleSize(12.0)
      , m_paperGrain(0.15)
      , m_paperSize(2.0)
      , m_hold(2.0)
      , m_seed(1.0)
      , m_paper(true)
      , m_whiteToTransparent(false)
      , m_paperColor(TPixel32(250, 245, 235)) {
    addInputPort("Source", m_source);
    m_wobble->setValueRange(0, Watercolour::maxWobble);
    m_wobble->setUISliderRange(0, 20);
    m_wobbleSize->setValueRange(1, 100);
    m_bleed->setValueRange(0, Watercolour::maxBleed);
    m_bleed->setUISliderRange(0, 20);
    m_edgeDarkening->setValueRange(0, 1);
    m_edgeWidth->setValueRange(0, Watercolour::maxEdgeWidth);
    m_edgeWidth->setUISliderRange(0, 20);
    m_edgeTolerance->setValueRange(0, 1);
    m_granulation->setValueRange(0, 1);
    m_granuleSize->setValueRange(1, 100);
    m_paperGrain->setValueRange(0, 1);
    m_paperSize->setValueRange(1, 100);
    m_paperColor->enableMatte(false);  // the paper is opaque
    m_paperSize->setUISliderRange(1, 20);
    m_hold->setValueRange(1, 1000);
    m_hold->setUISliderRange(1, 12);
    m_seed->setValueRange(0, 100000);
    m_seed->setUISliderRange(0, 1000);
    bindParam(this, "wobble", m_wobble);
    bindParam(this, "wobbleSize", m_wobbleSize);
    bindParam(this, "bleed", m_bleed);
    bindParam(this, "edgeDarkening", m_edgeDarkening);
    bindParam(this, "edgeWidth", m_edgeWidth);
    bindParam(this, "edgeTolerance", m_edgeTolerance);
    bindParam(this, "granulation", m_granulation);
    bindParam(this, "granuleSize", m_granuleSize);
    bindParam(this, "whiteToTransparent", m_whiteToTransparent);
    bindParam(this, "paper", m_paper);
    bindParam(this, "paperColor", m_paperColor);
    bindParam(this, "paperGrain", m_paperGrain);
    bindParam(this, "paperSize", m_paperSize);
    bindParam(this, "hold", m_hold);
    bindParam(this, "seed", m_seed);
    enableComputeInFloat(true);
  }
  bool requiresFullPrecisionSerialization() const override { return true; }
  bool usesCustomRenderGrid() const override { return true; }
  bool canHandle(const TRenderSettings &, double frame) override {
    return isStill(frame);
  }
  TAffine handledAffine(const TRenderSettings &settings,
                        double frame) override {
    return isStill(frame)
               ? settings.m_affine
               : CelFxSourceGrid::renderAffine(m_source.getFx(), frame);
  }
  bool toBeComputedInLinearColorSpace(bool, bool) const override {
    return false;
  }
  bool doGetBBox(double frame, TRectD &box,
                 const TRenderSettings &settings) override {
    box = TRectD();
    if (!m_source.isConnected()) return false;
    // Paper lies under the drawing and its bleed, not the whole frame: the
    // wash is computed on the drawing's own grid, which can hold many more
    // pixels than the camera for a scaled-down drawing
    if (!m_source->doGetBBox(frame, box, settings)) return false;
    if (!box.isEmpty() && box != TConsts::infiniteRectD && !isStill(frame)) {
      const TAffine placement =
          CelFxSourceGrid::nativeToWorld(m_source.getFx(), frame);
      const double reach = bounded(m_wobble->getValue(frame), 0, 1000) +
                           bounded(m_bleed->getValue(frame), 0, 1000) + 2.0;
      const double dx = reach * std::hypot(placement.a11, placement.a12);
      const double dy = reach * std::hypot(placement.a21, placement.a22);
      box = TRectD(box.x0 - dx, box.y0 - dy, box.x1 + dx, box.y1 + dy);
    }
    return !box.isEmpty();
  }
  void transform(double frame, int, const TRectD &output,
                 const TRenderSettings &settings, TRectD &input,
                 TRenderSettings &inputSettings) override {
    inputSettings = settings;
    input         = isStill(frame)
                        ? output
                        : output.enlarge(
                              Watercolour::halo(this->settings(frame, settings)));
  }
  void doDryCompute(TRectD &rect, double frame,
                    const TRenderSettings &settings) override {
    if (!m_source.isConnected()) return;
    TRectD input =
        isStill(frame)
            ? rect
            : rect.enlarge(Watercolour::halo(this->settings(frame, settings)));
    m_source->dryCompute(input, frame, settings);
  }
  int getMemoryRequirement(const TRectD &rect, double frame,
                           const TRenderSettings &settings) override {
    if (isStill(frame)) return 0;
    const double h = Watercolour::halo(this->settings(frame, settings));
    const double bytes =
        (rect.getLx() + 2 * h) * (rect.getLy() + 2 * h) * bytesPerPixel;
    const double megabytes = std::ceil(bytes / (1024.0 * 1024.0));
    return int(std::min(double(std::numeric_limits<int>::max()),
                        std::max(0.0, megabytes)));
  }
  std::string getAlias(double frame,
                       const TRenderSettings &settings) const override {
    std::ostringstream alias;
    alias.imbue(std::locale::classic());
    const Watercolour::Settings s = this->settings(frame, settings);
    // The noise plane, not the frame: the frames of a hold share a cache
    // entry, and each new hold gets its own
    alias << TRasterFx::getAlias(frame, settings) << "[watercolour:1:"
          << std::setprecision(std::numeric_limits<double>::max_digits10)
          << s.wobble << ',' << s.wobbleSize << ',' << s.bleed << ','
          << s.edgeDarkening << ',' << s.edgeWidth << ',' << s.edgeTolerance
          << ',' << s.granulation << ',' << s.granuleSize << ',' << s.whiteToTransparent << ',' << s.paper
          << ',' << s.paperGrain << ',' << s.paperSize << ',' << s.seed << ','
          << Watercolour::plane(s, frame);
    const TAffine grid = CelFxSourceGrid::renderAffine(m_source.getFx(), frame);
    for (double value :
         {grid.a11, grid.a12, grid.a13, grid.a21, grid.a22, grid.a23})
      alias << ',' << value;
    alias << ']';
    return alias.str();
  }
  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &settings) override {
    tile.getRaster()->clear();
    if (!m_source.isConnected() ||
        (settings.m_isCanceled && *settings.m_isCanceled))
      return;
    if (isStill(frame)) {
      m_source->compute(tile, frame, settings);
      if (settings.m_isCanceled && *settings.m_isCanceled)
        tile.getRaster()->clear();
      return;
    }
    const Watercolour::Settings s = this->settings(frame, settings);
    const int h                   = Watercolour::halo(s);
    const TDimension size =
        paddedSize(tile.getRaster()->getLx(), tile.getRaster()->getLy(), h,
                   tile.getRaster()->getPixelSize());
    TTile input;
    input.setRaster(tile.getRaster()->create(size.lx, size.ly));
    input.m_pos = tile.m_pos - TPointD(h, h);
    input.getRaster()->clear();
    m_source->compute(input, frame, settings);
    if (settings.m_isCanceled && *settings.m_isCanceled) return;
    if (TRaster32P output = tile.getRaster())
      paint<TPixel32>(input.getRaster(), output, h, input.m_pos, s, frame,
                      settings.m_isCanceled);
    else if (TRaster64P output = tile.getRaster())
      paint<TPixel64>(input.getRaster(), output, h, input.m_pos, s, frame,
                      settings.m_isCanceled);
    else if (TRasterFP output = tile.getRaster())
      paint<TPixelF>(input.getRaster(), output, h, input.m_pos, s, frame,
                     settings.m_isCanceled);
    else
      throw TException("Watercolour: unsupported pixel type");
  }
};
FX_PLUGIN_IDENTIFIER(WatercolourFx, "watercolourFx")
