#pragma once

#include "trenderer.h"
#include "tthread.h"

// Finite analytical source whose single opaque pixel is transformed by the
// host render affine. This exercises production compute rather than doCompute.
class HostMotionPixel final : public TStandardRasterFx {
  TPointD m_pixel;
  bool m_white;
public:
  HostMotionPixel(TPointD pixel, bool white = true) : m_pixel(pixel), m_white(white) {}
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(-10, -10, 10, 10); return true;
  }
  std::string getAlias(double, const TRenderSettings &) const override {
    return "host-motion-pixel:" + std::to_string(m_pixel.x) + ":" +
           std::to_string(m_pixel.y) + ":" + std::to_string(m_white);
  }
  void compute(TTile &tile, double, const TRenderSettings &info) override {
    TRaster32P raster = tile.getRaster();
    if (!raster) throw TException("Host fixture requires 8-bit pixels");
    const TAffine inverse = info.m_affine.inv();
    raster->lock();
    for (int y = 0; y < raster->getLy(); ++y) {
      auto *row = raster->pixels(y);
      for (int x = 0; x < raster->getLx(); ++x) {
        const TPointD p = inverse * (tile.m_pos + TPointD(x, y));
        const bool hit = std::floor(p.x + 0.5) == m_pixel.x &&
                         std::floor(p.y + 0.5) == m_pixel.y;
        row[x] = hit && m_white ? TPixel32::White : TPixel32::Transparent;
      }
    }
    raster->unlock();
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &info) override {
    compute(tile, frame, info);
  }
};

// A connected upstream node translates the noise image before it reaches the
// blur; the shift is transformed into the current render coordinates.
class HostMotionShift final : public TStandardRasterFx {
  TRasterFxPort m_source;
  TPointD m_shift;
public:
  HostMotionShift(TRasterFx *source, TPointD shift) : m_shift(shift) {
    addInputPort("Source", m_source); m_source.setFx(source);
  }
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double frame, TRectD &box, const TRenderSettings &info) override {
    if (!m_source->doGetBBox(frame, box, info)) return false;
    box = TRectD(box.x0 + m_shift.x, box.y0 + m_shift.y,
                 box.x1 + m_shift.x, box.y1 + m_shift.y); return true;
  }
  std::string getAlias(double frame, const TRenderSettings &info) const override {
    return "host-motion-shift:" + std::to_string(m_shift.x) + ":" +
           std::to_string(m_shift.y) + ":" +
           static_cast<TRasterFx *>(m_source.getFx())->getAlias(frame, info);
  }
  void compute(TTile &tile, double frame, const TRenderSettings &info) override {
    const TPointD shift = info.m_affine * m_shift - info.m_affine * TPointD();
    TTile translated(tile.getRaster(), tile.m_pos - shift);
    m_source->compute(translated, frame, info);
  }
  void doCompute(TTile &tile, double frame, const TRenderSettings &info) override {
    compute(tile, frame, info);
  }
};

class HostMotionContext {
  TRenderer m_renderer;
  unsigned long m_id;
public:
  HostMotionContext() : m_renderer(1), m_id(TRenderer::buildRenderId()) {
    m_renderer.install(m_id); m_renderer.declareRenderStart(m_id); m_renderer.declareFrameStart(0);
  }
  ~HostMotionContext() {
    m_renderer.declareFrameEnd(0); m_renderer.declareRenderEnd(m_id); m_renderer.uninstall();
  }
};
