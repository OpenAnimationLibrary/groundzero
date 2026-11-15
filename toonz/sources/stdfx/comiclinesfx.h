#pragma once

// The parameters and rendering shared by Motion Lines and Burst Lines. Each
// FX adds its own layout parameters and fills them in through layout(). The
// math is in comiclines.h.

#include "stdfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "tpixelutils.h"
#include "comiclines.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ComicLinesFx {

inline double bounded(double value, double minimum, double maximum) {
  return std::isfinite(value) ? std::max(minimum, std::min(maximum, value))
                              : minimum;
}
inline int discrete(double value, int minimum, int maximum) {
  return int(std::floor(bounded(value, minimum, maximum) + 0.5));
}

struct Defaults {
  double lines, length, lengthRandom, startRandom, thickness, taper, inLength,
      outLength;
};

// The distance an animated speed has moved something by a frame: the sum
// of the speed over the whole frames before it, plus the fraction of the
// current one. A speed without keys gives speed * frame.
inline double travelled(const TDoubleParamP &speed, double frame,
                        double limit) {
  auto at = [&](double f) { return bounded(speed->getValue(f), -limit, limit); };
  if (!std::isfinite(frame)) return 0.0;
  if (!speed->hasKeyframes() || frame <= 0) return at(frame) * frame;
  const int whole = int(std::min(std::floor(frame), 100000.0));
  double sum      = 0;
  for (int f = 0; f < whole; ++f) sum += at(f);
  return sum + (frame - whole) * at(whole);
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
void draw(const TRasterPT<Pixel> &output, TPointD position,
          const TAffine &worldToOutput, const ComicLines::Field &field,
          const int *canceled) {
  const double scale = std::sqrt(std::abs(worldToOutput.det()));
  if (!(scale > 0) || !std::isfinite(scale)) return;
  const TAffine outputToWorld = worldToOutput.inv();
  const double pixel          = 1.0 / scale;
  RasterLock lock(output);
  for (int y = 0; y < output->getLy(); ++y) {
    if (canceled && *canceled) return;
    Pixel *row = output->pixels(y);
    for (int x = 0; x < output->getLx(); ++x) {
      const TPointD p =
          outputToWorld * (position + TPointD(x + 0.5, y + 0.5));
      const ComicLines::Rgba c = ComicLines::sample(field, p.x, p.y, pixel);
      if (c.a > 0)
        row[x] = PixelConverter<Pixel>::from(TPixelD(c.r, c.g, c.b, c.a));
    }
  }
}

class Base : public TStandardZeraryFx {
protected:
  TDoubleParamP m_lines, m_groupSize, m_attraction, m_positionRandom, m_length,
      m_lengthRandom, m_startRandom, m_thickness, m_thicknessRandom, m_taper,
      m_inLength, m_outLength, m_waveAmplitude, m_wavePeriod, m_wavePhase,
      m_wavePhaseRandom, m_colorRandom, m_opacity, m_opacityRandom, m_speed,
      m_hold, m_seed;
  TPixelParamP m_color, m_color2;

  explicit Base(const Defaults &d)
      : m_lines(d.lines)
      , m_groupSize(1.0)
      , m_attraction(0.5)
      , m_positionRandom(1.0)
      , m_length(d.length)
      , m_lengthRandom(d.lengthRandom)
      , m_startRandom(d.startRandom)
      , m_thickness(d.thickness)
      , m_thicknessRandom(0.5)
      , m_taper(d.taper)
      , m_inLength(d.inLength)
      , m_outLength(d.outLength)
      , m_waveAmplitude(0.0)
      , m_wavePeriod(100.0)
      , m_wavePhase(0.0)
      , m_wavePhaseRandom(0.0)
      , m_colorRandom(0.0)
      , m_opacity(1.0)
      , m_opacityRandom(0.0)
      , m_speed(0.0)
      , m_hold(0.0)
      , m_seed(1.0)
      , m_color(TPixel32::Black)
      , m_color2(TPixel32::Black) {
    m_lines->setValueRange(0, ComicLines::maxLines);
    m_lines->setUISliderRange(0, 500);
    m_groupSize->setValueRange(1, 100);
    m_groupSize->setUISliderRange(1, 12);
    for (TDoubleParamP *share :
         {&m_attraction, &m_positionRandom, &m_lengthRandom, &m_startRandom,
          &m_thicknessRandom, &m_taper, &m_inLength, &m_outLength,
          &m_wavePhaseRandom, &m_colorRandom, &m_opacity, &m_opacityRandom})
      (*share)->setValueRange(0, 1);
    for (TDoubleParamP *distance : {&m_length, &m_thickness, &m_waveAmplitude,
                                    &m_wavePeriod, &m_speed})
      (*distance)->setMeasureName("fxLength");
    m_length->setValueRange(0, 100000);
    m_thickness->setValueRange(0, 10000);
    m_waveAmplitude->setValueRange(0, 10000);
    m_wavePeriod->setValueRange(0.1, 100000);
    m_wavePhase->setMeasureName("angle");
    m_speed->setValueRange(-100000, 100000);
    m_hold->setValueRange(0, 1000);
    m_hold->setUISliderRange(0, 12);
    m_seed->setValueRange(0, 100000);
    m_seed->setUISliderRange(0, 1000);
    enableComputeInFloat(true);
  }

  // Bound after the FX's own layout parameters, so those come first
  void bindLineParams() {
    bindParam(this, "lines", m_lines);
    bindParam(this, "groupSize", m_groupSize);
    bindParam(this, "attraction", m_attraction);
    bindParam(this, "positionRandom", m_positionRandom);
    bindParam(this, "length", m_length);
    bindParam(this, "lengthRandom", m_lengthRandom);
    bindParam(this, "startRandom", m_startRandom);
    bindParam(this, "thickness", m_thickness);
    bindParam(this, "thicknessRandom", m_thicknessRandom);
    bindParam(this, "taper", m_taper);
    bindParam(this, "inLength", m_inLength);
    bindParam(this, "outLength", m_outLength);
    bindParam(this, "waveAmplitude", m_waveAmplitude);
    bindParam(this, "wavePeriod", m_wavePeriod);
    bindParam(this, "wavePhase", m_wavePhase);
    bindParam(this, "wavePhaseRandom", m_wavePhaseRandom);
    bindParam(this, "color", m_color);
    bindParam(this, "color2", m_color2);
    bindParam(this, "colorRandom", m_colorRandom);
    bindParam(this, "opacity", m_opacity);
    bindParam(this, "opacityRandom", m_opacityRandom);
    bindParam(this, "speed", m_speed);
    bindParam(this, "hold", m_hold);
    bindParam(this, "seed", m_seed);
  }

  // The FX's own geometry: origin, direction, band or radii
  virtual void layout(ComicLines::Settings &settings, double frame) const = 0;
  virtual const char *aliasName() const = 0;

  static ComicLines::Rgba colour(const TPixelParamP &param, double frame) {
    const TPixelD c = param->getValueD(frame);
    return ComicLines::Rgba{c.r, c.g, c.b, c.m};
  }

public:
  ComicLines::Settings settings(double frame) const {
    ComicLines::Settings s;
    layout(s, frame);
    s.count      = discrete(m_lines->getValue(frame), 0, ComicLines::maxLines);
    s.groupSize  = discrete(m_groupSize->getValue(frame), 1, 100);
    s.attraction = bounded(m_attraction->getValue(frame), 0, 1);
    s.positionRandom  = bounded(m_positionRandom->getValue(frame), 0, 1);
    s.length          = bounded(m_length->getValue(frame), 0, 100000);
    s.lengthRandom    = bounded(m_lengthRandom->getValue(frame), 0, 1);
    s.startRandom     = bounded(m_startRandom->getValue(frame), 0, 1);
    s.thickness       = bounded(m_thickness->getValue(frame), 0, 10000);
    s.thicknessRandom = bounded(m_thicknessRandom->getValue(frame), 0, 1);
    s.taper           = bounded(m_taper->getValue(frame), 0, 1);
    s.inLength        = bounded(m_inLength->getValue(frame), 0, 1);
    s.outLength       = bounded(m_outLength->getValue(frame), 0, 1);
    s.waveAmplitude   = bounded(m_waveAmplitude->getValue(frame), 0, 10000);
    s.wavePeriod      = bounded(m_wavePeriod->getValue(frame), 0.1, 100000);
    s.wavePhase = bounded(m_wavePhase->getValue(frame), -1e6, 1e6) *
                  ComicLines::pi / 180.0;
    s.wavePhaseRandom = bounded(m_wavePhaseRandom->getValue(frame), 0, 1);
    s.colorA          = colour(m_color, frame);
    s.colorB          = colour(m_color2, frame);
    s.colorRandom     = bounded(m_colorRandom->getValue(frame), 0, 1);
    s.opacity         = bounded(m_opacity->getValue(frame), 0, 1);
    s.opacityRandom   = bounded(m_opacityRandom->getValue(frame), 0, 1);
    // Lines move by Speed every frame without any key
    s.travel = travelled(m_speed, frame, 100000);
    s.seed = ComicLines::frameSeed(
        std::uint32_t(discrete(m_seed->getValue(frame), 0, 100000)), frame,
        discrete(m_hold->getValue(frame), 0, 1000));
    return s;
  }

  bool requiresFullPrecisionSerialization() const override { return true; }
  // Rotation and uniform scale only: the antialiasing has one pixel size,
  // so the renderer resamples a squashed or stretched result
  bool canHandle(const TRenderSettings &info, double) override {
    return info.m_affine.isIsotropic(0.001);
  }
  bool toBeComputedInLinearColorSpace(bool, bool) const override {
    return false;
  }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TConsts::infiniteRectD;
    return true;
  }

  // The parameter values alone do not change from frame to frame, but the
  // travel and the held seed do
  std::string getAlias(double frame,
                       const TRenderSettings &info) const override {
    const ComicLines::Settings s = settings(frame);
    std::ostringstream alias;
    alias.imbue(std::locale::classic());
    alias << TRasterFx::getAlias(frame, info) << '[' << aliasName() << ":1:"
          << std::setprecision(std::numeric_limits<double>::max_digits10)
          << s.travel << ',' << s.seed << ']';
    return alias.str();
  }

  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &info) override {
    tile.getRaster()->clear();
    if (info.m_isCanceled && *info.m_isCanceled) return;
    ComicLines::Field field;
    try {
      field = ComicLines::build(settings(frame));
    } catch (const std::invalid_argument &error) {
      throw TException(error.what());
    }
    if (field.lines.empty()) return;
    if (TRaster32P output = tile.getRaster())
      draw<TPixel32>(output, tile.m_pos, info.m_affine, field,
                     info.m_isCanceled);
    else if (TRaster64P output = tile.getRaster())
      draw<TPixel64>(output, tile.m_pos, info.m_affine, field,
                     info.m_isCanceled);
    else if (TRasterFP output = tile.getRaster())
      draw<TPixelF>(output, tile.m_pos, info.m_affine, field,
                    info.m_isCanceled);
    else
      throw TException(std::string(aliasName()) + ": unsupported pixel type");
    if (info.m_isCanceled && *info.m_isCanceled) tile.getRaster()->clear();
  }
};

}  // namespace ComicLinesFx
