#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tpixelutils.h"
#include "dotscreen.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Dot Screen: a lattice of dots. Up to three modifiers each read a control
// image at every dot's centre and change the dot's size, aspect, rotation,
// opacity or colour. With an image on Source and the first modifier on Size
// it is a halftone; without inputs it is a plain pattern. The math is in
// dotscreen.h.
namespace {
double bounded(double value, double minimum, double maximum) {
  return std::isfinite(value) ? std::max(minimum, std::min(maximum, value))
                              : minimum;
}
int discrete(double value, int maximum) {
  return int(std::floor(bounded(value, 0, maximum) + 0.5));
}
class RasterLock {
  TRasterP m_raster;

public:
  explicit RasterLock(const TRasterP &raster) : m_raster(raster) {
    m_raster->lock();
  }
  ~RasterLock() { m_raster->unlock(); }
};

// A control image as premultiplied colours, read at any output position
struct ControlImage {
  int width = 0, height = 0;
  TPointD position;  // of pixel (0, 0), in output coordinates
  std::vector<DotScreen::Rgba> pixels;

  template <class Pixel>
  void read(const TRasterPT<Pixel> &raster, TPointD pos) {
    width    = raster->getLx();
    height   = raster->getLy();
    position = pos;
    pixels.resize(size_t(width) * size_t(height));
    RasterLock lock(raster);
    for (int y = 0; y < height; ++y) {
      const Pixel *row = raster->pixels(y);
      for (int x = 0; x < width; ++x) {
        const TPixelD p = PixelConverter<TPixelD>::from(row[x]);
        pixels[size_t(y) * width + x] = {p.r, p.g, p.b, p.m};
      }
    }
  }
  DotScreen::Rgba at(int x, int y) const {
    return x < 0 || y < 0 || x >= width || y >= height
               ? DotScreen::Rgba()
               : pixels[size_t(y) * width + x];
  }
  // Bilinear, with pixel centres at half integers
  DotScreen::Rgba sample(TPointD output) const {
    if (pixels.empty()) return DotScreen::Rgba();
    const double fx = output.x - position.x - 0.5;
    const double fy = output.y - position.y - 0.5;
    const double x0 = std::floor(fx), y0 = std::floor(fy);
    if (!(x0 > -2 && y0 > -2 && x0 < width + 1 && y0 < height + 1))
      return DotScreen::Rgba();
    const double tx = fx - x0, ty = fy - y0;
    const int ix = int(x0), iy = int(y0);
    const DotScreen::Rgba a = at(ix, iy), b = at(ix + 1, iy),
                          c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
    auto mix = [&](double pa, double pb, double pc, double pd) {
      return (pa * (1 - tx) + pb * tx) * (1 - ty) +
             (pc * (1 - tx) + pd * tx) * ty;
    };
    return {mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g),
            mix(a.b, b.b, c.b, d.b), mix(a.a, b.a, c.a, d.a)};
  }
};

template <class Pixel>
void draw(const TRasterPT<Pixel> &output, TPointD position,
          const TAffine &outputToWorld, double pixel,
          const DotScreen::Grid &grid, const int *canceled) {
  RasterLock lock(output);
  for (int y = 0; y < output->getLy(); ++y) {
    if (canceled && *canceled) return;
    Pixel *row = output->pixels(y);
    for (int x = 0; x < output->getLx(); ++x) {
      const TPointD p =
          outputToWorld * (position + TPointD(x + 0.5, y + 0.5));
      const DotScreen::Rgba c = DotScreen::sample(grid, p.x, p.y, pixel);
      if (c.a > 0)
        row[x] = PixelConverter<Pixel>::from(TPixelD(c.r, c.g, c.b, c.a));
    }
  }
}
}  // namespace

class DotScreenFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(DotScreenFx)
  TRasterFxPort m_control[DotScreen::modifierCount];
  TDoubleParamP m_spacing, m_angle, m_stagger, m_jitter, m_shape, m_size,
      m_aspect, m_rotation, m_opacity, m_softness, m_colorRandom,
      m_sizeRandom, m_rotationRandom, m_opacityRandom, m_hold, m_seed;
  TPixelParamP m_color, m_color2;
  TDoubleParamP m_target[DotScreen::modifierCount],
      m_mode[DotScreen::modifierCount], m_low[DotScreen::modifierCount],
      m_high[DotScreen::modifierCount];
  TBoolParamP m_invert[DotScreen::modifierCount];

  static DotScreen::Rgba colour(const TPixelParamP &param, double frame) {
    const TPixelD c = param->getValueD(frame);
    return DotScreen::Rgba{c.r, c.g, c.b, c.m};
  }
  static double scaleOf(const TAffine &affine) {
    const double scale = std::sqrt(std::abs(affine.det()));
    return std::isfinite(scale) && scale > 0 ? scale : 0.0;
  }
  bool uses(int k, double frame) const {
    return m_control[k].isConnected() &&
           discrete(m_target[k]->getValue(frame), 5) != 0;
  }
  // The margin, in output pixels, that a control image is read beyond a tile
  int halo(const DotScreen::Settings &s, const TRenderSettings &info) const {
    const double pixels = DotScreen::reach(s) * scaleOf(info.m_affine) + 2;
    if (!(pixels < 1 << 20))
      throw TException("Dot Screen: the dots are too large for the raster");
    return int(std::ceil(pixels));
  }

public:
  DotScreen::Settings settings(double frame) const {
    const double degrees = DotScreen::pi / 180.0;
    DotScreen::Settings s;
    s.spacing  = bounded(m_spacing->getValue(frame), 0.5, 100000);
    s.angle    = bounded(m_angle->getValue(frame), -3600, 3600) * degrees;
    s.stagger  = bounded(m_stagger->getValue(frame), 0, 1);
    s.jitter   = bounded(m_jitter->getValue(frame), 0, 1);
    s.shape    = DotScreen::Shape(discrete(m_shape->getValue(frame), 3));
    s.size     = bounded(m_size->getValue(frame), 0, DotScreen::maxSize);
    s.aspect   = bounded(m_aspect->getValue(frame), 0.05, 4);
    s.rotation = bounded(m_rotation->getValue(frame), -3600, 3600) * degrees;
    s.opacity  = bounded(m_opacity->getValue(frame), 0, 1);
    s.softness = bounded(m_softness->getValue(frame), 0, 1000);
    s.colorA   = colour(m_color, frame);
    s.colorB   = colour(m_color2, frame);
    s.colorRandom    = bounded(m_colorRandom->getValue(frame), 0, 1);
    s.sizeRandom     = bounded(m_sizeRandom->getValue(frame), 0, 1);
    s.rotationRandom = bounded(m_rotationRandom->getValue(frame), 0, 1);
    s.opacityRandom  = bounded(m_opacityRandom->getValue(frame), 0, 1);
    s.seed           = DotScreen::frameSeed(
        std::uint32_t(discrete(m_seed->getValue(frame), 100000)), frame,
        discrete(m_hold->getValue(frame), 1000));
    for (int k = 0; k < DotScreen::modifierCount; ++k) {
      DotScreen::Modifier &m = s.modifiers[k];
      m.target = DotScreen::Target(discrete(m_target[k]->getValue(frame), 5));
      m.mode   = DotScreen::Mode(discrete(m_mode[k]->getValue(frame), 2));
      m.low    = bounded(m_low[k]->getValue(frame), -100000, 100000);
      m.high   = bounded(m_high[k]->getValue(frame), -100000, 100000);
      m.invert = m_invert[k]->getValue();
      // Rotation is entered in degrees
      if (m.target == DotScreen::Target::Rotation) {
        m.low *= degrees;
        m.high *= degrees;
      }
    }
    return s;
  }

  DotScreenFx()
      : m_spacing(12.0)
      , m_angle(45.0)
      , m_stagger(0.0)
      , m_jitter(0.0)
      , m_shape(0.0)
      , m_size(0.7)
      , m_aspect(1.0)
      , m_rotation(0.0)
      , m_opacity(1.0)
      , m_softness(0.0)
      , m_colorRandom(0.0)
      , m_sizeRandom(0.0)
      , m_rotationRandom(0.0)
      , m_opacityRandom(0.0)
      , m_hold(0.0)
      , m_seed(1.0)
      , m_color(TPixel32::Black)
      , m_color2(TPixel32::Black) {
    addInputPort("Source", m_control[0]);
    addInputPort("Control2", m_control[1]);
    addInputPort("Control3", m_control[2]);
    m_spacing->setMeasureName("fxLength");
    m_spacing->setValueRange(0.5, 100000);
    m_spacing->setUISliderRange(1, 100);
    m_angle->setMeasureName("angle");
    m_rotation->setMeasureName("angle");
    m_softness->setMeasureName("fxLength");
    m_softness->setValueRange(0, 1000);
    m_softness->setUISliderRange(0, 10);
    m_shape->setValueRange(0, 3);
    m_shape->setUIChoices({"Round", "Square", "Diamond", "Line"});
    m_size->setValueRange(0, DotScreen::maxSize);
    m_size->setUISliderRange(0, 1.5);
    m_aspect->setValueRange(0.05, 4);
    for (TDoubleParamP *share :
         {&m_stagger, &m_jitter, &m_opacity, &m_colorRandom, &m_sizeRandom,
          &m_rotationRandom, &m_opacityRandom})
      (*share)->setValueRange(0, 1);
    m_hold->setValueRange(0, 1000);
    m_hold->setUISliderRange(0, 12);
    m_seed->setValueRange(0, 100000);
    m_seed->setUISliderRange(0, 1000);
    bindParam(this, "spacing", m_spacing);
    bindParam(this, "angle", m_angle);
    bindParam(this, "stagger", m_stagger);
    bindParam(this, "jitter", m_jitter);
    bindParam(this, "shape", m_shape);
    bindParam(this, "size", m_size);
    bindParam(this, "aspect", m_aspect);
    bindParam(this, "rotation", m_rotation);
    bindParam(this, "opacity", m_opacity);
    bindParam(this, "softness", m_softness);
    bindParam(this, "color", m_color);
    bindParam(this, "color2", m_color2);
    bindParam(this, "colorRandom", m_colorRandom);
    bindParam(this, "sizeRandom", m_sizeRandom);
    bindParam(this, "rotationRandom", m_rotationRandom);
    bindParam(this, "opacityRandom", m_opacityRandom);
    bindParam(this, "hold", m_hold);
    bindParam(this, "seed", m_seed);
    for (int k = 0; k < DotScreen::modifierCount; ++k) {
      // The first modifier makes a halftone of whatever is on Source: dark
      // gives a full dot, white gives none
      m_target[k] = TDoubleParamP(k == 0 ? 1.0 : 0.0);
      m_mode[k]   = TDoubleParamP(1.0);
      m_low[k]    = TDoubleParamP(k == 0 ? 1.0 : 0.0);
      m_high[k]   = TDoubleParamP(k == 0 ? 0.0 : 1.0);
      m_invert[k] = TBoolParamP(false);
      m_target[k]->setValueRange(0, 5);
      m_target[k]->setUIChoices(
          {"Off", "Size", "Aspect", "Rotation", "Opacity", "Colour"});
      m_mode[k]->setValueRange(0, 2);
      m_mode[k]->setUIChoices({"Set", "Multiply", "Add"});
      m_low[k]->setValueRange(-100000, 100000);
      m_low[k]->setUISliderRange(0, 2);
      m_high[k]->setValueRange(-100000, 100000);
      m_high[k]->setUISliderRange(0, 2);
      const std::string n = std::to_string(k + 1);
      bindParam(this, "target" + n, m_target[k]);
      bindParam(this, "mode" + n, m_mode[k]);
      bindParam(this, "low" + n, m_low[k]);
      bindParam(this, "high" + n, m_high[k]);
      bindParam(this, "invert" + n, m_invert[k]);
    }
    enableComputeInFloat(true);
  }
  bool requiresFullPrecisionSerialization() const override { return true; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool toBeComputedInLinearColorSpace(bool, bool) const override {
    return false;
  }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TConsts::infiniteRectD;
    return true;
  }
  void transform(double frame, int port, const TRectD &output,
                 const TRenderSettings &info, TRectD &input,
                 TRenderSettings &inputInfo) override {
    inputInfo = info;
    input     = output;
    if (port >= 0 && port < DotScreen::modifierCount && uses(port, frame))
      input = output.enlarge(halo(settings(frame), info));
  }
  void doDryCompute(TRectD &rect, double frame,
                    const TRenderSettings &info) override {
    for (int k = 0; k < DotScreen::modifierCount; ++k)
      if (uses(k, frame)) {
        TRectD input = rect.enlarge(halo(settings(frame), info));
        m_control[k]->dryCompute(input, frame, info);
      }
  }
  int getMemoryRequirement(const TRectD &rect, double frame,
                           const TRenderSettings &info) override {
    const DotScreen::Settings s = settings(frame);
    const double h              = halo(s, info);
    const double scale          = scaleOf(info.m_affine);
    int images                  = 0;
    for (int k = 0; k < DotScreen::modifierCount; ++k)
      if (uses(k, frame)) ++images;
    const double area = (rect.getLx() + 2 * h) * (rect.getLy() + 2 * h);
    // Each control image as doubles and as a tile, and one Dot per cell
    const double cells =
        scale > 0 ? area / std::max(1.0, s.spacing * s.spacing * scale * scale)
                  : 0.0;
    const double megabytes =
        std::ceil((area * images * 48 + cells * 96) / (1024.0 * 1024.0));
    return int(std::min(double(std::numeric_limits<int>::max()),
                        std::max(0.0, megabytes)));
  }
  // The held seed changes the picture without any parameter changing
  std::string getAlias(double frame,
                       const TRenderSettings &info) const override {
    std::ostringstream alias;
    alias.imbue(std::locale::classic());
    alias << TRasterFx::getAlias(frame, info) << "[dotScreen:1:"
          << settings(frame).seed << ']';
    return alias.str();
  }
  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &info) override {
    tile.getRaster()->clear();
    if (info.m_isCanceled && *info.m_isCanceled) return;
    const double scale = scaleOf(info.m_affine);
    if (!(scale > 0)) return;
    const DotScreen::Settings s = settings(frame);
    const int h                 = halo(s, info);
    const int w = tile.getRaster()->getLx(), ht = tile.getRaster()->getLy();

    ControlImage controls[DotScreen::modifierCount];
    for (int k = 0; k < DotScreen::modifierCount; ++k) {
      if (!uses(k, frame)) continue;
      TTile input;
      input.setRaster(tile.getRaster()->create(w + 2 * h, ht + 2 * h));
      input.m_pos = tile.m_pos - TPointD(h, h);
      input.getRaster()->clear();
      m_control[k]->compute(input, frame, info);
      if (info.m_isCanceled && *info.m_isCanceled) return;
      if (TRaster32P raster = input.getRaster())
        controls[k].read<TPixel32>(raster, input.m_pos);
      else if (TRaster64P raster = input.getRaster())
        controls[k].read<TPixel64>(raster, input.m_pos);
      else if (TRasterFP raster = input.getRaster())
        controls[k].read<TPixelF>(raster, input.m_pos);
      else
        throw TException("Dot Screen: unsupported pixel type");
    }

    const TAffine outputToWorld = info.m_affine.inv();
    // The tile's corners in the world: the region the dots must cover
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool first  = true;
    for (double cx : {0.0, double(w)})
      for (double cy : {0.0, double(ht)}) {
        const TPointD p = outputToWorld * (tile.m_pos + TPointD(cx, cy));
        minX = first ? p.x : std::min(minX, p.x);
        maxX = first ? p.x : std::max(maxX, p.x);
        minY = first ? p.y : std::min(minY, p.y);
        maxY = first ? p.y : std::max(maxY, p.y);
        first = false;
      }
    DotScreen::Grid grid;
    try {
      grid = DotScreen::build(s, minX, minY, maxX, maxY,
                              [&](int k, double x, double y) {
                                return controls[k].sample(info.m_affine *
                                                          TPointD(x, y));
                              });
    } catch (const std::exception &error) {
      throw TException(error.what());
    }
    const double pixel = 1.0 / scale;
    if (TRaster32P output = tile.getRaster())
      draw<TPixel32>(output, tile.m_pos, outputToWorld, pixel, grid,
                     info.m_isCanceled);
    else if (TRaster64P output = tile.getRaster())
      draw<TPixel64>(output, tile.m_pos, outputToWorld, pixel, grid,
                     info.m_isCanceled);
    else if (TRasterFP output = tile.getRaster())
      draw<TPixelF>(output, tile.m_pos, outputToWorld, pixel, grid,
                    info.m_isCanceled);
    else
      throw TException("Dot Screen: unsupported pixel type");
    if (info.m_isCanceled && *info.m_isCanceled) tile.getRaster()->clear();
  }
};
FX_PLUGIN_IDENTIFIER(DotScreenFx, "dotScreenFx")
