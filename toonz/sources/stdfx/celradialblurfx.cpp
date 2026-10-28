#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tparamuiconcept.h"
#include "tpixelutils.h"
#include "celmotion.h"
#include "celnoise.h"
#include "celdistance.h"
#include "celradialgeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <type_traits>
#include <vector>

namespace {
double limited(double value, double minimum, double maximum, double fallback = 0.0) {
  return std::isfinite(value) ? std::max(minimum, std::min(maximum, value)) : fallback;
}
int mode(double value, int maximum) {
  return int(std::floor(limited(value, 0.0, double(maximum)) + 0.5));
}
bool isCanceled(const int *cancel) { return cancel && *cancel; }
}

class CelRadialBlurFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(CelRadialBlurFx)
  TRasterFxPort m_source, m_noise;
  TPointParamP m_center;
  TDoubleParamP m_blurType, m_outerStrength, m_outerOffsetMode, m_outerOffset, m_outerFade;
  TDoubleParamP m_innerStrength, m_innerOffsetMode, m_innerOffset, m_innerFade, m_repeatBorder;
  TDoubleParamP m_ellipseRatio, m_ellipseAngle, m_quality, m_brightness, m_sizeVariation;
  TDoubleParamP m_noiseVariation, m_noiseType, m_seed, m_noiseOffset, m_thickness;

public:
  CelRadialBlurFx()
      : m_center(TPointD(0, 0)), m_blurType(0.0), m_outerStrength(0.0), m_outerOffsetMode(0.0),
        m_outerOffset(0.0), m_outerFade(0.0), m_innerStrength(0.0), m_innerOffsetMode(0.0),
        m_innerOffset(0.0), m_innerFade(0.0), m_repeatBorder(1.0), m_ellipseRatio(1.0),
        m_ellipseAngle(0.0), m_quality(5.0), m_brightness(1.0), m_sizeVariation(0.0),
        m_noiseVariation(0.0), m_noiseType(0.0), m_seed(1.0), m_noiseOffset(0.0), m_thickness(10.0) {
    addInputPort("Source", m_source);
    addInputPort("Noise", m_noise);
    m_center->getX()->setMeasureName("fxLength");
    m_center->getY()->setMeasureName("fxLength");
    auto choice = [](const TDoubleParamP &param, const std::vector<std::string> &options, bool checkbox = false) {
      param->setValueRange(0.0, double(options.size() - 1));
      param->setUIChoices(options, checkbox);
    };
    choice(m_blurType, {"Zoom", "Rotation"});
    choice(m_outerOffsetMode, {"Add", "Max", "Override"});
    choice(m_innerOffsetMode, {"Add", "Max", "Override"});
    choice(m_repeatBorder, {"Off", "On"}, true);
    choice(m_noiseType, {"Smooth", "Block", "Layer"});
    m_outerStrength->setValueRange(0, 2000); m_innerStrength->setValueRange(0, 2000);
    m_outerOffset->setValueRange(0, 500); m_innerOffset->setValueRange(0, 500);
    m_outerFade->setValueRange(0, 100); m_innerFade->setValueRange(0, 100);
    m_ellipseRatio->setValueRange(0, 5);
    m_quality->setValueRange(1, 50); m_brightness->setValueRange(0, 10);
    m_sizeVariation->setValueRange(0, 100); m_noiseVariation->setValueRange(0, 100);
    m_seed->setValueRange(1, 1000); m_thickness->setValueRange(1, 100);
    bindParam(this, "center", m_center); bindParam(this, "blurType", m_blurType);
    bindParam(this, "outerStrength", m_outerStrength); bindParam(this, "outerOffsetMode", m_outerOffsetMode);
    bindParam(this, "outerOffset", m_outerOffset); bindParam(this, "outerFade", m_outerFade);
    bindParam(this, "innerStrength", m_innerStrength); bindParam(this, "innerOffsetMode", m_innerOffsetMode);
    bindParam(this, "innerOffset", m_innerOffset); bindParam(this, "innerFade", m_innerFade);
    bindParam(this, "repeatBorder", m_repeatBorder); bindParam(this, "ellipseRatio", m_ellipseRatio);
    bindParam(this, "ellipseAngle", m_ellipseAngle); bindParam(this, "quality", m_quality);
    bindParam(this, "brightness", m_brightness); bindParam(this, "sizeVariation", m_sizeVariation);
    bindParam(this, "noiseVariation", m_noiseVariation); bindParam(this, "noiseType", m_noiseType);
    bindParam(this, "seed", m_seed); bindParam(this, "noiseOffset", m_noiseOffset);
    bindParam(this, "thickness", m_thickness);
    enableComputeInFloat(true);
  }
  bool requiresFullPrecisionSerialization() const override { return true; }
  bool canHandle(const TRenderSettings &, double) override { return false; }
  TAffine handledAffine(const TRenderSettings &, double) override { return TAffine(); }
  bool toBeComputedInLinearColorSpace(bool, bool) const override { return false; }
  bool sourceDomain(double frame, const TRenderSettings &settings, TRectD &domain) const;
  double support(double frame) const;
  bool doGetBBox(double frame, TRectD &box, const TRenderSettings &settings) override;
  void transform(double frame, int port, const TRectD &output, const TRenderSettings &settings,
                 TRectD &input, TRenderSettings &inputSettings) override;
  int getMemoryRequirement(const TRectD &rect, double frame, const TRenderSettings &settings) override;
  std::string getAlias(double frame, const TRenderSettings &settings) const override;
  void getParamUIs(TParamUIConcept *&concepts, int &length) override;
  void doCompute(TTile &tile, double frame, const TRenderSettings &settings) override;
};

namespace {
double sideLength(double strength, double offset, int offsetMode) {
  if (offsetMode == 1) return std::max(strength, offset);
  if (offsetMode == 2) return offset;
  return strength + offset;
}
bool finiteRect(const TRectD &r) {
  return std::isfinite(r.x0) && std::isfinite(r.y0) &&
         std::isfinite(r.x1) && std::isfinite(r.y1) && !r.isEmpty() &&
         r != TConsts::infiniteRectD;
}
}

bool CelRadialBlurFx::sourceDomain(double frame, const TRenderSettings &settings,
                                    TRectD &domain) const {
  if (!m_source.isConnected()) return false;
  TRenderSettings full(settings);
  full.m_getFullSizeBBox = true;
  if (!m_source->getBBox(frame, domain, full) || domain.isEmpty()) return false;
  // Infinite generators use the finite scene canvas, never the output tile.
  if (!finiteRect(domain)) domain = settings.m_cameraBox;
  if (!finiteRect(domain))
    throw TException("Cel Radial Blur requires a finite source or camera domain");
  domain = TRectD(std::floor(domain.x0), std::floor(domain.y0),
                  std::ceil(domain.x1), std::ceil(domain.y1));
  if (domain.isEmpty()) return false;
  if (domain.getLx() > std::numeric_limits<int>::max() ||
      domain.getLy() > std::numeric_limits<int>::max())
    throw TException("Cel Radial Blur: source domain exceeds raster limits");
  const double area = domain.getLx() * domain.getLy();
  if (!std::isfinite(area) || area > double(std::numeric_limits<size_t>::max() / 96))
    throw TException("Cel Radial Blur: source domain exceeds memory limits");
  return true;
}

double CelRadialBlurFx::support(double frame) const {
  const double outer = sideLength(limited(m_outerStrength->getValue(frame), 0, 2000),
      limited(m_outerOffset->getValue(frame), 0, 500), mode(m_outerOffsetMode->getValue(frame), 2));
  const double inner = sideLength(limited(m_innerStrength->getValue(frame), 0, 2000),
      limited(m_innerOffset->getValue(frame), 0, 500), mode(m_innerOffsetMode->getValue(frame), 2));
  return std::ceil(std::max(inner, outer)) + 2.0;
}

bool CelRadialBlurFx::doGetBBox(double frame, TRectD &box, const TRenderSettings &settings) {
  if (!m_source.isConnected()) { box = TRectD(); return false; }
  if (!m_source->doGetBBox(frame, box, settings)) return false;
  if (box != TConsts::infiniteRectD) box = box.enlarge(support(frame));
  return true;
}

void CelRadialBlurFx::transform(double frame, int port, const TRectD &,
                                const TRenderSettings &settings, TRectD &input,
                                TRenderSettings &inputSettings) {
  inputSettings = settings;
  input = TRectD();
  if ((port == 0 && m_source.isConnected()) ||
      (port == 1 && m_noise.isConnected() && mode(m_noiseType->getValue(frame), 2) == 2))
    sourceDomain(frame, settings, input);
}

int CelRadialBlurFx::getMemoryRequirement(const TRectD &, double frame,
                                          const TRenderSettings &settings) {
  TRectD domain;
  if (!sourceDomain(frame, settings, domain)) return 0;
  // Two source rasters, double-precision images, labels and distance work.
  const double megabytes = std::ceil(domain.getLx() * domain.getLy() * 128.0 / (1024.0 * 1024.0));
  return megabytes >= double(std::numeric_limits<int>::max()) ?
      std::numeric_limits<int>::max() : int(megabytes);
}

std::string CelRadialBlurFx::getAlias(double frame, const TRenderSettings &settings) const {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << TRasterFx::getAlias(frame, settings) << "[celRadial:" <<
      std::setprecision(std::numeric_limits<double>::max_digits10);
  const TPointD center = m_center->getValue(frame);
  out << center.x << ',' << center.y;
  for (const TDoubleParamP &param : {m_blurType, m_outerStrength, m_outerOffsetMode, m_outerOffset,
       m_outerFade, m_innerStrength, m_innerOffsetMode, m_innerOffset, m_innerFade, m_repeatBorder,
       m_ellipseRatio, m_ellipseAngle, m_quality, m_brightness, m_sizeVariation, m_noiseVariation,
       m_noiseType, m_seed, m_noiseOffset, m_thickness}) out << ';' << param->getValue(frame);
  // Domain selection for infinite generators also depends on the scene canvas.
  out << ';' << settings.m_cameraBox.x0 << ',' << settings.m_cameraBox.y0 << ','
      << settings.m_cameraBox.x1 << ',' << settings.m_cameraBox.y1;
  return out.str() + ']';
}

void CelRadialBlurFx::getParamUIs(TParamUIConcept *&concepts, int &length) {
  concepts = new TParamUIConcept[length = 2];
  concepts[0].m_type = TParamUIConcept::POINT;
  concepts[0].m_label = "Center";
  concepts[0].m_params.push_back(m_center);
  concepts[1].m_type = TParamUIConcept::ELLIPSE;
  concepts[1].m_label = "Outer Strength";
  concepts[1].m_params.push_back(m_outerStrength);
  concepts[1].m_params.push_back(m_center);
  concepts[1].m_params.push_back(m_ellipseRatio);
  concepts[1].m_params.push_back(m_ellipseAngle);
}

namespace {
struct RasterLock {
  TRasterP raster;
  explicit RasterLock(const TRasterP &r) : raster(r) { raster->lock(); }
  ~RasterLock() { raster->unlock(); }
};

template <class Pixel>
bool readImage(const TRasterPT<Pixel> &raster, const TRectD &domain,
               CelMotion::Image &image, const int *cancel) {
  image.width = raster->getLx(); image.height = raster->getLy();
  image.originX = domain.x0; image.originY = domain.y0;
  const size_t area = CelMotion::checkedArea(image.width, image.height);
  image.pixels.resize(area);
  RasterLock lock(raster);
  for (int y = 0; y < image.height; ++y) {
    if (isCanceled(cancel)) return false;
    const Pixel *row = raster->pixels(y);
    for (int x = 0; x < image.width; ++x) {
      const TPixelD p = toPixelD(row[x]);
      auto &target = image.pixels[size_t(y) * image.width + x];
      if (std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b) &&
          std::isfinite(p.m) && p.m > 0)
        target = {{p.r, p.g, p.b, p.m}};
      else target = {{0, 0, 0, 0}};
    }
  }
  return true;
}

void allocateClearedTile(TTile &tile, const TPointD &origin, const TDimension &size,
                         const TRasterP &prototype) {
  TRasterP raster;
  const TRaster32P depth8(prototype);
  const TRaster64P depth16(prototype);
  const TRasterFP depthFloat(prototype);
  if (depth8) raster = TRaster32P(size);
  else if (depth16) raster = TRaster64P(size);
  else if (depthFloat) raster = TRasterFP(size);
  else throw TException("Cel Radial Blur: unsupported raster type");
  raster->setLinear(prototype->isLinear());
  raster->clear();
  tile.setRaster(raster);
  tile.m_pos = origin;
}

bool readAnyImage(const TRasterP &raster, const TRectD &domain,
                  CelMotion::Image &image, const int *cancel) {
  if (TRaster32P value = raster) return readImage<TPixel32>(value, domain, image, cancel);
  if (TRaster64P value = raster) return readImage<TPixel64>(value, domain, image, cancel);
  if (TRasterFP value = raster) return readImage<TPixelF>(value, domain, image, cancel);
  throw TException("Cel Radial Blur: unsupported input pixel type");
}

struct RadialSettings {
  CelRadial::Point center;
  bool rotation = false, repeat = false;
  double outer = 0, inner = 0, outerOffset = 0, innerOffset = 0;
  int outerMode = 0, innerMode = 0;
  double outerFade = 0, innerFade = 0, ratio = 1, angle = 0;
  int quality = 5;
  double gain = 1, sizeVariation = 0, noiseVariation = 0;
  CelNoise::Settings noise;
};

// Identify the dominant contributing connected component for a bilinear sample.
size_t dominantPixel(const CelMotion::Image &image, const CelMotion::Shapes &shapes,
                     double x, double y) {
  x -= image.originX; y -= image.originY;
  if (!std::isfinite(x) || !std::isfinite(y) || x < -1 || y < -1 ||
      x >= image.width || y >= image.height) return size_t(-1);
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  const double tx = x - ix, ty = y - iy;
  double best = 0; size_t result = size_t(-1);
  for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
    const int sx = ix + dx, sy = iy + dy;
    if (sx < 0 || sy < 0 || sx >= image.width || sy >= image.height) continue;
    const size_t index = size_t(sy) * image.width + sx;
    if (!shapes.labels[index]) continue;
    const double weight = image.pixels[index][3] * (dx ? tx : 1 - tx) * (dy ? ty : 1 - ty);
    if (weight > best) { best = weight; result = index; }
  }
  return result;
}

template <class Pixel>
bool renderRadial(const CelMotion::Image &source, const CelMotion::Image *noiseImage,
                  const CelMotion::Shapes &shapes, const CelDistance::Field &edges,
                  const RadialSettings &settings, const TRasterPT<Pixel> &output,
                  const TPointD &origin, const int *cancel) {
  RasterLock lock(output);
  const double maximumSupport = std::max(
      sideLength(settings.outer, settings.outerOffset, settings.outerMode),
      sideLength(settings.inner, settings.innerOffset, settings.innerMode)) + 2.0;
  for (int y = 0; y < output->getLy(); ++y) {
    if (isCanceled(cancel)) return false;
    Pixel *row = output->pixels(y);
    for (int x = 0; x < output->getLx(); ++x) {
      const double gx = origin.x + x, gy = origin.y + y;
      if (gx < source.originX - maximumSupport || gy < source.originY - maximumSupport ||
          gx >= source.originX + source.width + maximumSupport ||
          gy >= source.originY + source.height + maximumSupport) continue;
      CelMotion::Pixel sum{};
      double totalWeight = 0;
      auto add = [&](const CelMotion::Pixel &pixel, double weight) {
        if (pixel[3] <= 0 || weight <= 0) return;
        for (int c = 0; c < 4; ++c) sum[c] += pixel[c] * weight;
        totalWeight += weight;
      };
      add(CelMotion::sample(source, gx, gy), 1.0);
      const double luma = noiseImage ? CelMotion::luminance(*noiseImage, gx, gy) : 0;
      const double noise = CelNoise::sample(settings.noise, gx, gy, luma);
      const double noiseFactor = 1.0 - settings.noiseVariation + settings.noiseVariation * noise;
      for (int side = 0; side < 2; ++side) {
        const bool outer = side == 0;
        const double strength = outer ? settings.outer : settings.inner;
        const double offset = outer ? settings.outerOffset : settings.innerOffset;
        const int offsetMode = outer ? settings.outerMode : settings.innerMode;
        const double fade = outer ? settings.outerFade : settings.innerFade;
        const double maximum = sideLength(strength, offset, offsetMode);
        if (!(maximum > 0)) continue;
        for (int i = 1; i <= settings.quality; ++i) {
          if ((i & 15) == 0 && isCanceled(cancel)) return false;
          const double fraction = double(i) / settings.quality;
          const double distance = fraction * maximum;
          const CelRadial::Point sample = CelRadial::trajectory(
              {gx, gy}, settings.center, settings.ratio, settings.angle,
              distance, settings.rotation, outer);
          double sx = sample.x, sy = sample.y;
          const double minX = source.originX, minY = source.originY;
          const double maxX = minX + source.width - 1, maxY = minY + source.height - 1;
          if (sx < minX || sy < minY || sx > maxX || sy > maxY) {
            if (settings.repeat && fade > 0) { sx = std::max(minX, std::min(maxX, sx)); sy = std::max(minY, std::min(maxY, sy)); }
            else { totalWeight += fade / settings.quality; continue; }
          }
          const size_t index = dominantPixel(source, shapes, sx, sy);
          if (index == size_t(-1)) continue;
          const auto label = shapes.labels[index];
          const auto &component = shapes.groups[label - 1];
          const double sizeFactor = CelMotion::sizeFactor(component, shapes, settings.sizeVariation);
          const double allowed = sideLength(strength * sizeFactor * noiseFactor,
                                            offset * sizeFactor * noiseFactor, offsetMode);
          if (distance > allowed) continue;
          const double fadeRange = std::max(1.0, std::sqrt(double(component.count)) *
                                                   (0.05 + 0.45 * settings.sizeVariation));
          const double edge = std::isfinite(edges.distance[index])
                                  ? std::max(0.0, std::min(1.0, (edges.distance[index] - 0.5) / fadeRange))
                                  : 1.0;
          const double weight = (1.0 - fade * (1.0 - edge) * (1.0 - fraction)) / settings.quality;
          add(CelMotion::sample(source, sx, sy), weight);
        }
      }
      if (totalWeight <= 0) { row[x] = Pixel::Transparent; continue; }
      const double inverse = 1.0 / totalWeight;
      TPixelD result(sum[0] * inverse * settings.gain,
                     sum[1] * inverse * settings.gain,
                     sum[2] * inverse * settings.gain, sum[3] * inverse);
      if constexpr (std::is_same<Pixel, TPixelF>::value) {
        const double maxFloat = std::numeric_limits<float>::max();
        auto finiteFloat = [maxFloat](double value) {
          return std::max(-maxFloat, std::min(maxFloat, value));
        };
        result.r = finiteFloat(result.r); result.g = finiteFloat(result.g);
        result.b = finiteFloat(result.b); result.m = finiteFloat(result.m);
      }
      row[x] = PixelConverter<Pixel>::from(result);
    }
  }
  return !isCanceled(cancel);
}
}

void CelRadialBlurFx::doCompute(TTile &tile, double frame,
                                const TRenderSettings &settings) {
  tile.getRaster()->clear();
  if (!m_source.isConnected() || isCanceled(settings.m_isCanceled)) return;
  TRectD domain;
  if (!sourceDomain(frame, settings, domain)) return;
  const TDimension size{int(domain.getLx()), int(domain.getLy())};
  CelMotion::checkedArea(size.lx, size.ly);
  // The host allocator computes width * height * pixel size as signed int.
  const int bytesPerPixel = TRasterFP(tile.getRaster()) ? 16 :
                            TRaster64P(tile.getRaster()) ? 8 : 4;
  if (!CelRadial::fitsRasterAllocation(size.lx, size.ly, bytesPerPixel))
    throw TException("Cel Radial Blur: source raster exceeds allocator limits");
  TTile sourceTile;
  allocateClearedTile(sourceTile, domain.getP00(), size, tile.getRaster());
  m_source->compute(sourceTile, frame, settings);
  if (isCanceled(settings.m_isCanceled)) return;
  CelMotion::Image source;
  if (!readAnyImage(sourceTile.getRaster(), domain, source, settings.m_isCanceled)) return;
  RadialSettings params;
  const TPointD center = m_center->getValue(frame);
  params.center = {center.x, center.y};
  params.rotation = mode(m_blurType->getValue(frame), 1) == 1;
  params.outer = limited(m_outerStrength->getValue(frame), 0, 2000);
  params.inner = limited(m_innerStrength->getValue(frame), 0, 2000);
  params.outerOffset = limited(m_outerOffset->getValue(frame), 0, 500);
  params.innerOffset = limited(m_innerOffset->getValue(frame), 0, 500);
  params.outerMode = mode(m_outerOffsetMode->getValue(frame), 2);
  params.innerMode = mode(m_innerOffsetMode->getValue(frame), 2);
  params.outerFade = limited(m_outerFade->getValue(frame), 0, 100) / 100.0;
  params.innerFade = limited(m_innerFade->getValue(frame), 0, 100) / 100.0;
  params.repeat = mode(m_repeatBorder->getValue(frame), 1) != 0;
  params.ratio = limited(m_ellipseRatio->getValue(frame), 0, 5, 1);
  const double rawAngle = m_ellipseAngle->getValue(frame);
  params.angle = std::isfinite(rawAngle) ? std::remainder(rawAngle, 360.0) : 0.0;
  params.quality = std::max(1, mode(m_quality->getValue(frame), 50));
  params.gain = limited(m_brightness->getValue(frame), 0, 10, 1);
  params.sizeVariation = limited(m_sizeVariation->getValue(frame), 0, 100) / 100.0;
  params.noiseVariation = limited(m_noiseVariation->getValue(frame), 0, 100) / 100.0;
  params.noise.type = static_cast<CelNoise::Type>(mode(m_noiseType->getValue(frame), 2));
  params.noise.seed = std::uint32_t(std::floor(limited(m_seed->getValue(frame), 1, 1000, 1) + 0.5));
  const double rawNoiseOffset = m_noiseOffset->getValue(frame);
  params.noise.offset = std::isfinite(rawNoiseOffset) ? rawNoiseOffset : 0.0;
  params.noise.thickness = limited(m_thickness->getValue(frame), 1, 100, 10);
  CelMotion::Image noise;
  const CelMotion::Image *noisePtr = nullptr;
  if (params.noise.type == CelNoise::Layer && m_noise.isConnected()) {
    TTile noiseTile;
    allocateClearedTile(noiseTile, domain.getP00(), size, tile.getRaster());
    m_noise->compute(noiseTile, frame, settings);
    if (!readAnyImage(noiseTile.getRaster(), domain, noise, settings.m_isCanceled)) return;
    noisePtr = &noise;
  }
  CelMotion::Shapes shapes;
  if (!CelMotion::analyze(source, shapes, settings.m_isCanceled)) return;
  std::vector<std::uint8_t> background(shapes.labels.size());
  for (size_t i = 0; i < background.size(); ++i) background[i] = shapes.labels[i] == 0;
  CelDistance::Field edges;
  if (!CelDistance::transform(background, source.width, source.height,
                              CelDistance::Metric::Euclidean, edges,
                              settings.m_isCanceled, !params.repeat)) return;
  bool completed = false;
  if (TRaster32P out = tile.getRaster())
    completed = renderRadial<TPixel32>(source, noisePtr, shapes, edges, params, out, tile.m_pos, settings.m_isCanceled);
  else if (TRaster64P out = tile.getRaster())
    completed = renderRadial<TPixel64>(source, noisePtr, shapes, edges, params, out, tile.m_pos, settings.m_isCanceled);
  else if (TRasterFP out = tile.getRaster())
    completed = renderRadial<TPixelF>(source, noisePtr, shapes, edges, params, out, tile.m_pos, settings.m_isCanceled);
  else throw TException("Cel Radial Blur: unsupported output pixel type");
  if (!completed) tile.getRaster()->clear();
}

FX_PLUGIN_IDENTIFIER(CelRadialBlurFx, "celRadialBlurFx")
