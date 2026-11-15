#pragma once

// Helpers shared by the Motion Lines and Burst Lines FX tests.

#include "tparamcontainer.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace ComicLinesFxTest {

inline void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
inline bool approx(double a, double b, double tolerance = 1e-4) {
  return std::abs(a - b) <= tolerance;
}
inline TDoubleParamP numeric(TFx &fx, const std::string &name) {
  TDoubleParamP param = TParamP(fx.getParams()->getParam(name));
  require(bool(param), "No numeric parameter " + name);
  return param;
}
inline void set(TFx &fx, const std::string &name, double value) {
  numeric(fx, name)->setDefaultValue(value);
}
inline void setPoint(TFx &fx, const std::string &name, TPointD value) {
  TPointParamP param = TParamP(fx.getParams()->getParam(name));
  require(bool(param), "No point parameter " + name);
  param->setDefaultValue(value);
}

// One line of constant width, nothing random
inline void singleLine(TFx &fx, double length, double thickness) {
  set(fx, "lines", 1);
  for (const char *name : {"positionRandom", "lengthRandom", "startRandom",
                           "thicknessRandom", "inLength", "outLength",
                           "taper"})
    set(fx, name, 0);
  set(fx, "length", length);
  set(fx, "thickness", thickness);
}

template <class Pixel>
TRenderSettings settingsFor() {
  TRenderSettings settings;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value    ? 128
                   : std::is_same<Pixel, TPixel64>::value ? 64
                                                          : 32;
  return settings;
}

template <class Pixel, class Fx>
TRasterPT<Pixel> render(Fx &fx, TPointD position, int width, int height,
                        double frame, const TRenderSettings &settings) {
  TRasterPT<Pixel> raster(width, height);
  TTile tile(raster, position);
  fx.doCompute(tile, frame, settings);
  return raster;
}

template <class Pixel>
double alpha(const TRasterPT<Pixel> &raster, int x, int y) {
  return PixelConverter<TPixelD>::from(raster->pixels(y)[x]).m;
}

template <class Pixel>
void requireSameTile(const TRasterPT<Pixel> &whole,
                     const TRasterPT<Pixel> &part, int left, int bottom) {
  for (int y = 0; y < part->getLy(); ++y)
    for (int x = 0; x < part->getLx(); ++x)
      require(part->pixels(y)[x] == whole->pixels(y + bottom)[x + left],
              "An FX tile differs from the whole frame");
}

}  // namespace ComicLinesFxTest
