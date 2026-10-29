#pragma once

#include "trasterfx.h"

#include <limits>
#include <locale>
#include <sstream>
#include <string>

namespace ParticleSampling {
// FX aliases describe source content; many real level aliases deliberately omit
// incoming render settings. A cached raster also needs its exact sampling grid.
inline std::string cacheKey(const std::string &kind,
                            const std::string &sourceAlias,
                            const TRenderSettings &settings,
                            const TPointD &origin,
                            const TDimensionD &resolution,
                            const TRasterP &rasterTemplate = TRasterP()) {
  std::ostringstream key;
  key.imbue(std::locale::classic());
  key.precision(std::numeric_limits<double>::max_digits10);
  const TAffine &a = settings.m_affine;
  const TRectD &camera = settings.m_cameraBox;
  key << kind << '[' << sourceAlias << "][" << settings.toString() << "]sample("
      << a.a11 << ',' << a.a12 << ',' << a.a13 << ','
      << a.a21 << ',' << a.a22 << ',' << a.a23 << ';'
      << origin.x << ',' << origin.y << ';' << resolution.lx << ',' << resolution.ly << ';'
      << settings.m_gamma << ',' << settings.m_colorSpaceGamma << ';'
      << settings.m_timeStretchFrom << ',' << settings.m_timeStretchTo << ';'
      << settings.m_stereoscopic << ',' << settings.m_stereoscopicShift << ';'
      << camera.x0 << ',' << camera.y0 << ',' << camera.x1 << ',' << camera.y1 << ';'
      << settings.m_applyShrinkToViewer << ',' << settings.m_getFullSizeBBox << ';'
      << settings.m_mark.getPointer() << ';';
  if (rasterTemplate)
    key << rasterTemplate->getPixelSize() << ',' << rasterTemplate->isLinear();
  else
    key << "default";
  key << ')';
  return key.str();
}
}  // namespace ParticleSampling
