#pragma once

#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "trasterimage.h"
#include "tfxutil.h"
#include "trenderer.h"
#include "tthread.h"
#include "tpixelutils.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <sstream>
#include <type_traits>

namespace CelLevelTest {
// Real level/column objects exercise image-cache loading, centered odd canvases,
// precision conversion, and the production level's own affine routing.
class Level {
  ToonzScene m_scene;
  TXshLevelP m_level;
  TXshLevelColumn m_column;
public:
  explicit Level(const TRasterP &image)
      : m_level(new TXshSimpleLevel(L"cel-pixel-fixture")) {
    auto *simple = m_level->getSimpleLevel();
    simple->setType(OVL_XSHLEVEL);
    simple->setScene(&m_scene);
    static unsigned int identity = 0;
    simple->setPath(TFilePath("cel-grid-fixture-" + std::to_string(++identity) + ".png"));
    simple->setFrame(TFrameId(1), TRasterImageP(image));
    if (!m_column.setCell(0, TXshCell(m_level, TFrameId(1))))
      throw std::runtime_error("Cannot create raster level fixture");
  }
  TFxP placed(const TAffine &affine) {
    TFxP source = m_column.getFx();
    TFxP result = TFxUtil::makeAffine(source, affine);
    // The scene builder marks the DPI node, including identity-DPI sources.
    result->setName(L"LevelColumn AffineFx");
    return result;
  }
};

class Context {
  TRenderer m_renderer;
  unsigned long m_id;
public:
  Context() : m_renderer(1), m_id(TRenderer::buildRenderId()) {
    m_renderer.install(m_id);
    m_renderer.declareRenderStart(m_id);
    m_renderer.declareFrameStart(0);
  }
  ~Context() {
    m_renderer.declareFrameEnd(0);
    m_renderer.declareRenderEnd(m_id);
    m_renderer.uninstall();
  }
};

template <class Pixel>
TRasterPT<Pixel> render(TRasterFx &fx, const TAffine &affine, int strips = 1) {
  Context context;
  constexpr int width = 64, height = 48;
  TRasterPT<Pixel> output(width, height); output->clear();
  TRenderSettings settings;
  settings.m_bpp = std::is_same<Pixel, TPixelF>::value ? 128 :
                  std::is_same<Pixel, TPixel64>::value ? 64 : 32;
  settings.m_affine = affine;
  settings.m_cameraBox = TRectD(-32, -24, 32, 24);
  for (int strip = 0; strip < strips; ++strip) {
    const int y0 = height * strip / strips, y1 = height * (strip + 1) / strips;
    TRasterPT<Pixel> raster(width, y1 - y0); raster->clear();
    TTile tile(raster, TPointD(-32, -24 + y0));
    fx.compute(tile, 0, settings);
    output->copy(tile.getRaster(), TPoint(0, y0));
  }
  return output;
}

template <class Pixel>
void compare(const TRasterPT<Pixel> &expected, const TRasterPT<Pixel> &actual,
             const char *message) {
  const double tolerance = std::is_same<Pixel, TPixelF>::value ? 2e-5 :
                           1.0 / Pixel::maxChannelValue;
  bool visible = false;
  for (int y = 0; y < expected->getLy(); ++y)
    for (int x = 0; x < expected->getLx(); ++x) {
      const TPixelD a = toPixelD(expected->pixels(y)[x]);
      const TPixelD b = toPixelD(actual->pixels(y)[x]);
      visible = visible || a.m > 0.1;
      if (std::max({std::abs(a.r-b.r), std::abs(a.g-b.g),
                    std::abs(a.b-b.b), std::abs(a.m-b.m)}) > tolerance) {
        std::ostringstream detail;
        detail << message << " at " << x << ',' << y << ": expected "
               << a.r << ',' << a.g << ',' << a.b << ',' << a.m << "; actual "
               << b.r << ',' << b.g << ',' << b.b << ',' << b.m;
        throw std::runtime_error(detail.str());
      }
    }
  if (!visible) throw std::runtime_error("Level fixture rendered no visible pixels");
}
}  // namespace CelLevelTest
