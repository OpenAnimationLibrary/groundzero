#pragma once

#ifndef MYPAINTTOONZBRUSH_H
#define MYPAINTTOONZBRUSH_H

#include <toonz/mypaint.h>
#include "traster.h"
#include "trastercm.h"
#include "tcurves.h"
#include <QPainter>
#include <QImage>
#include <QString>

#include <memory>
#include <vector>

class RasterController {
public:
  virtual ~RasterController() {}
  virtual bool askRead(const TRect &rect) { return true; }
  virtual bool askWrite(const TRect &rect) { return true; }
};

//=======================================================
//
// MyPaintTip
//
//=======================================================

//! A brush tip image, which the dabs of a brush stamp in place of the round
//! dab (the hardness of the dab is not used then). The tip is a coverage mask
//! (0 is no paint, 255 is full paint) whose longer side spans the diameter of
//! the dab. It keeps a chain of half size copies to be sampled without
//! aliasing when it is drawn small.
class MyPaintTip {
  struct Level {
    int lx, ly;
    std::vector<unsigned char> pixels;
  };
  std::vector<Level> m_levels;

  float texel(const Level &level, int x, int y) const {
    if (x < 0 || y < 0 || x >= level.lx || y >= level.ly) return 0.f;
    return level.pixels[(size_t)y * level.lx + x];
  }

public:
  //! \b coverage holds lx * ly values, top row first.
  MyPaintTip(int lx, int ly, const std::vector<unsigned char> &coverage);

  //! Loads a tip from an image file. An image with transparency gives its
  //! alpha channel as coverage; an opaque one gives the inverse of its
  //! luminance (black is paint, as in the brush tip images of other
  //! programs). Returns 0 if the file cannot be read.
  static std::shared_ptr<const MyPaintTip> load(const QString &path);

  int getLx() const { return m_levels[0].lx; }
  int getLy() const { return m_levels[0].ly; }

  //! Coverage (0 to 1) at the point (x, y) of the full size tip, in pixels
  //! with the origin at the top left corner. \b pixelSize is the size, in
  //! tip pixels, of the area which is drawn on one output pixel: it selects
  //! the size of the copy to sample.
  float sample(float x, float y, float pixelSize) const;
};

//=======================================================
//
// Raster32PMyPaintSurface
//
//=======================================================

class Raster32PMyPaintSurface : public mypaint::Surface {
private:
  class Internal;

  bool drawTipDab(const mypaint::Dab &dab);

  TRaster32P ras;
  RasterController *controller;
  Internal *internal;
  std::shared_ptr<const MyPaintTip> tip;

  inline static void readPixel(const void *pixelPtr, float &colorR,
                               float &colorG, float &colorB, float &colorA) {
    const TPixel32 &pixel = *(const TPixel32 *)pixelPtr;
    colorR                = (float)pixel.r / (float)TPixel32::maxChannelValue;
    colorG                = (float)pixel.g / (float)TPixel32::maxChannelValue;
    colorB                = (float)pixel.b / (float)TPixel32::maxChannelValue;
    colorA                = (float)pixel.m / (float)TPixel32::maxChannelValue;
  }

  inline static void writePixel(void *pixelPtr, float colorR, float colorG,
                                float colorB, float colorA) {
    TPixel32 &pixel = *(TPixel32 *)pixelPtr;
    pixel.r = (TPixel32::Channel)roundf(colorR * TPixel32::maxChannelValue);
    pixel.g = (TPixel32::Channel)roundf(colorG * TPixel32::maxChannelValue);
    pixel.b = (TPixel32::Channel)roundf(colorB * TPixel32::maxChannelValue);
    pixel.m = (TPixel32::Channel)roundf(colorA * TPixel32::maxChannelValue);
  }

  inline static bool askRead(void *surfaceController,
                             const void * /* surfacePointer */, int x0, int y0,
                             int x1, int y1) {
    Raster32PMyPaintSurface &owner =
        *((Raster32PMyPaintSurface *)surfaceController);
    return !owner.controller ||
           owner.controller->askRead(TRect(x0, y0, x1, y1));
  }

  inline static bool askWrite(void *surfaceController,
                              const void * /* surfacePointer */, int x0, int y0,
                              int x1, int y1) {
    Raster32PMyPaintSurface &owner =
        *((Raster32PMyPaintSurface *)surfaceController);
    return !owner.controller ||
           owner.controller->askWrite(TRect(x0, y0, x1, y1));
  }

public:
  explicit Raster32PMyPaintSurface(const TRaster32P &ras);
  explicit Raster32PMyPaintSurface(const TRaster32P &ras,
                                   RasterController &controller);
  ~Raster32PMyPaintSurface();

  bool getColor(float x, float y, float radius, float &colorR, float &colorG,
                float &colorB, float &colorA) override;

  bool drawDab(const mypaint::Dab &dab) override;

  //! Makes the dabs stamp \b tip instead of the round dab. A null tip
  //! restores the round dab.
  void setTip(const std::shared_ptr<const MyPaintTip> &newTip) { tip = newTip; }
  const std::shared_ptr<const MyPaintTip> &getTip() const { return tip; }

  bool getAntialiasing() const;
  void setAntialiasing(bool value);

  RasterController *getController() const { return controller; }
};

//=======================================================
//
// MyPaintToonzBrush
//
//=======================================================

enum class MyPaintToonzEraserMode { Lines, Areas, LinesAndAreas };

class MyPaintToonzBrush {
private:
  struct Params {
    union {
      struct {
        double x, y, pressure, tx, ty, time;
      };
      struct {
        double values[6];
      };
    };

    inline explicit Params(double x = 0.0, double y = 0.0,
                           double pressure = 0.0, double tx = 0.0,
                           double ty = 0.0, double time = 0.0)
        : x(x), y(y), pressure(pressure), tx(tx), ty(ty), time(time) {}

    inline void setMedian(Params &a, Params &b) {
      for (int i = 0; i < (int)sizeof(values) / sizeof(values[0]); ++i)
        values[i] = 0.5 * (a.values[i] + b.values[i]);
    }
  };

  struct Segment {
    Params p1, p2;
  };

  TRaster32P m_ras;
  Raster32PMyPaintSurface m_mypaintSurface;
  mypaint::Brush m_brush;

  bool m_reset;
  Params m_previous, m_current;
  bool m_interpolation;

public:
  MyPaintToonzBrush(const TRaster32P &ras, RasterController &controller,
                    const mypaint::Brush &brush, bool interpolation = false);
  void beginStroke();
  void strokeTo(const TPointD &position, double pressure, const TPointD &tilt,
                double dtime);
  void endStroke();

  // colormapped
  void updateDrawing(
      const TRasterCM32P rasCM, const TRasterCM32P rasBackupCM,
      const TRect &bbox, int styleId, bool lockAlpha, bool eraser = false,
      MyPaintToonzEraserMode eraserMode = MyPaintToonzEraserMode::Lines) const;

  const TRaster32P &getRaster() const { return m_ras; }
  RasterController &getController() {
    return *m_mypaintSurface.getController();
  }
  const mypaint::Brush &getBrush() const { return m_brush; }

  //! See Raster32PMyPaintSurface::setTip().
  void setTip(const std::shared_ptr<const MyPaintTip> &tip) {
    m_mypaintSurface.setTip(tip);
  }
};

#endif  // T_BLUREDBRUSH
