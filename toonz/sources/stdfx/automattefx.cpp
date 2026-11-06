#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "automatte.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <vector>

//==============================================================================
//    Auto Matte Fx
//
// A solid matte of a drawing's silhouette, made from its lines: everything the
// lines enclose is filled, so a drawing with lines only, or with gaps in its
// paint, still gives a solid shape. Use it as the Matte of Matte In or Out, of
// Tone / Highlight, or of any FX that limits itself to a matte. The math is
// in automatte.h.
//
// The fill needs the whole Source, so the Source is rendered over its bounds.
// A Source without bounds, or one too large for the working limit, has no
// outline to close: the matte is then its own alpha.
//==============================================================================

namespace {

// Larger Sources fall back to their own alpha
const double c_maxPixels = 64.0 * 1024 * 1024;

template <class PIXEL>
std::vector<float> readAlpha(const TRasterPT<PIXEL> &ras) {
  std::vector<float> alpha(ras->getLx() * ras->getLy());
  ras->lock();
  for (int y = 0; y < ras->getLy(); ++y) {
    const PIXEL *pix = ras->pixels(y);
    for (int x = 0; x < ras->getLx(); ++x)
      alpha[y * ras->getLx() + x] =
          std::min(1.0f, std::max(0.0f, float(pix[x].m) /
                                            float(PIXEL::maxChannelValue)));
  }
  ras->unlock();
  return alpha;
}

std::vector<float> readAlpha(const TRasterP &ras) {
  if (TRaster32P r = ras) return readAlpha<TPixel32>(r);
  if (TRaster64P r = ras) return readAlpha<TPixel64>(r);
  if (TRasterFP r = ras) return readAlpha<TPixelF>(r);
  throw TException("Auto Matte: unsupported pixel type");
}

// Writes color x alpha (premultiplied) into out. The matte's pixel (0, 0) is
// at (dx, dy) in out; out of the matte the result is transparent.
template <class PIXEL>
void writeMatte(const TRasterPT<PIXEL> &out, const std::vector<float> &alpha,
                int w, int h, int dx, int dy, const TPixelD &color) {
  const double maxValue = PIXEL::maxChannelValue;
  const double rounding = std::is_integral<typename PIXEL::Channel>::value ? 0.5 : 0.0;
  out->lock();
  for (int y = 0; y < out->getLy(); ++y) {
    PIXEL *pix = out->pixels(y);
    for (int x = 0; x < out->getLx(); ++x) {
      const int sx = x - dx, sy = y - dy;
      double a = 0.0;
      if (sx >= 0 && sy >= 0 && sx < w && sy < h) a = alpha[sy * w + sx];
      a *= color.m;
      typedef typename PIXEL::Channel Channel;
      pix[x].r = Channel(color.r * a * maxValue + rounding);
      pix[x].g = Channel(color.g * a * maxValue + rounding);
      pix[x].b = Channel(color.b * a * maxValue + rounding);
      pix[x].m = Channel(a * maxValue + rounding);
    }
  }
  out->unlock();
}

void writeMatte(const TRasterP &out, const std::vector<float> &alpha, int w,
                int h, int dx, int dy, const TPixelD &color) {
  if (TRaster32P r = out) writeMatte<TPixel32>(r, alpha, w, h, dx, dy, color);
  else if (TRaster64P r = out) writeMatte<TPixel64>(r, alpha, w, h, dx, dy, color);
  else if (TRasterFP r = out) writeMatte<TPixelF>(r, alpha, w, h, dx, dy, color);
  else throw TException("Auto Matte: unsupported pixel type");
}

}  // namespace

//------------------------------------------------------------------------------

class AutoMatteFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(AutoMatteFx)

  TRasterFxPort m_source;
  TDoubleParamP m_threshold;
  TPixelParamP m_color;

  // The Source's bounds in the render's pixels, when it can be filled whole
  bool fillableBox(double frame, const TRenderSettings &settings,
                   TRectD &box) {
    box = TRectD();
    if (!m_source.isConnected() || !m_source->getBBox(frame, box, settings) ||
        box.isEmpty() || box == TConsts::infiniteRectD)
      return false;
    box = TRectD(std::floor(box.x0), std::floor(box.y0), std::ceil(box.x1),
                 std::ceil(box.y1));
    return box.getLx() * box.getLy() <= c_maxPixels;
  }

public:
  AutoMatteFx() : m_threshold(0.5), m_color(TPixel32::White) {
    addInputPort("Source", m_source);
    m_threshold->setValueRange(0.01, 1);
    bindParam(this, "threshold", m_threshold);
    bindParam(this, "color", m_color);
    enableComputeInFloat(true);
  }

  bool canHandle(const TRenderSettings &, double) override { return true; }

  bool doGetBBox(double frame, TRectD &box,
                 const TRenderSettings &settings) override {
    box = TRectD();
    if (!m_source.isConnected()) return false;
    return m_source->doGetBBox(frame, box, settings);
  }

  void transform(double frame, int, const TRectD &output,
                 const TRenderSettings &settings, TRectD &input,
                 TRenderSettings &inputSettings) override {
    inputSettings = settings;
    TRectD box;
    input = fillableBox(frame, settings, box) ? box : output;
  }

  void doDryCompute(TRectD &rect, double frame,
                    const TRenderSettings &settings) override {
    if (!m_source.isConnected()) return;
    TRectD box;
    TRectD input = fillableBox(frame, settings, box) ? box : rect;
    m_source->dryCompute(input, frame, settings);
  }

  int getMemoryRequirement(const TRectD &rect, double frame,
                           const TRenderSettings &settings) override {
    TRectD box;
    const TRectD input = fillableBox(frame, settings, box) ? box : rect;
    return TRasterFx::memorySize(input, settings.m_bpp) * 2;
  }

  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &settings) override {
    tile.getRaster()->clear();
    if (!m_source.isConnected() ||
        (settings.m_isCanceled && *settings.m_isCanceled))
      return;
    const double threshold =
        std::min(1.0, std::max(0.01, m_threshold->getValue(frame)));
    const TPixelD color = m_color->getValueD(frame);

    TRectD box;
    if (!fillableBox(frame, settings, box)) {
      // No outline to close: the matte is the Source's own alpha
      m_source->compute(tile, frame, settings);
      const TRasterP ras = tile.getRaster();
      writeMatte(ras, readAlpha(ras), ras->getLx(), ras->getLy(), 0, 0, color);
      return;
    }

    TTile input;
    input.setRaster(tile.getRaster()->create(int(box.getLx()), int(box.getLy())));
    input.getRaster()->clear();
    input.m_pos = box.getP00();
    m_source->compute(input, frame, settings);
    if (settings.m_isCanceled && *settings.m_isCanceled) return;

    const int w = input.getRaster()->getLx(), h = input.getRaster()->getLy();
    const std::vector<float> matte = automatte::silhouette(
        readAlpha(input.getRaster()), w, h, threshold, settings.m_isCanceled);
    if (matte.empty()) return;
    // Tile positions are whole pixels in the render's space, as the box is
    const int dx = int(std::lround(box.x0 - tile.m_pos.x));
    const int dy = int(std::lround(box.y0 - tile.m_pos.y));
    writeMatte(tile.getRaster(), matte, w, h, dx, dy, color);
  }
};

FX_PLUGIN_IDENTIFIER(AutoMatteFx, "autoMatteFx")
