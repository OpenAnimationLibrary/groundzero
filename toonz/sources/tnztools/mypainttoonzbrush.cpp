
#include <algorithm>

#include "mypainttoonzbrush.h"
#include "tropcm.h"
#include "tpixelutils.h"
#include <toonz/mypainthelpers.hpp>

#include <QColor>

#include <cmath>

namespace {
void putOnRasterCM(const TRasterCM32P &out, const TRaster32P &in, int styleId,
                   bool lockAlpha, bool eraser,
                   MyPaintToonzEraserMode eraserMode) {
  if (!out.getPointer() || !in.getPointer()) return;
  assert(out->getSize() == in->getSize());
  int x, y;
  for (y = 0; y < out->getLy(); y++) {
    for (x = 0; x < out->getLx(); x++) {
#ifdef _DEBUG
      assert(x >= 0 && x < in->getLx());
      assert(y >= 0 && y < in->getLy());
      assert(x >= 0 && x < out->getLx());
      assert(y >= 0 && y < out->getLy());
#endif
      TPixel32 *inPix = &in->pixels(y)[x];
      if (inPix->m == 0) continue;
      TPixelCM32 *outPix = &out->pixels(y)[x];
      if (eraser) {
        const bool eraseLines = eraserMode != MyPaintToonzEraserMode::Areas;
        const bool eraseAreas = eraserMode != MyPaintToonzEraserMode::Lines;

        int ink   = outPix->getInk();
        int paint = eraseAreas ? 0 : outPix->getPaint();
        int tone  = outPix->getTone();
        if (eraseLines)
          tone += ((int)inPix->m * (TPixelCM32::getMaxTone() - tone) + 127) /
                  TPixelCM32::getMaxTone();

        // A fully transparent pixel must not retain a stale ink style.
        if (tone == TPixelCM32::getMaxTone() && paint == 0) ink = 0;
        *outPix = TPixelCM32(ink, paint, tone);
        continue;
      }
      if (lockAlpha && !outPix->isPureInk() && outPix->getPaint() == 0 &&
          outPix->getTone() == 255) {
        *outPix =
            TPixelCM32(outPix->getInk(), outPix->getPaint(), outPix->getTone());
        continue;
      }
      bool sameStyleId = styleId == outPix->getInk();
      // line with lock alpha : use original pixel's tone
      // line with the same style : multiply tones
      // line with different style : pick darker tone
      int tone = lockAlpha     ? outPix->getTone()
                 : sameStyleId ? outPix->getTone() * (255 - inPix->m) / 255
                               : std::min(255 - inPix->m, outPix->getTone());
      int ink  = !sameStyleId && outPix->getTone() < 255 - inPix->m
                     ? outPix->getInk()
                     : styleId;
      *outPix  = TPixelCM32(ink, outPix->getPaint(), tone);
    }
  }
}
}  // namespace

//=======================================================
//
// Raster32PMyPaintSurface::Internal
//
//=======================================================

class Raster32PMyPaintSurface::Internal
    : public mypaint::helpers::SurfaceCustom<readPixel, writePixel, askRead,
                                             askWrite> {
public:
  typedef SurfaceCustom Parent;
  Internal(Raster32PMyPaintSurface &owner)
      : SurfaceCustom(owner.ras->pixels(), owner.ras->getLx(),
                      owner.ras->getLy(), owner.ras->getPixelSize(),
                      owner.ras->getRowSize(), &owner) {}
};

//=======================================================
//
// Raster32PMyPaintSurface
//
//=======================================================

Raster32PMyPaintSurface::Raster32PMyPaintSurface(const TRaster32P &ras)
    : ras(ras), controller(), internal() {
  assert(ras);
  internal = new Internal(*this);
}

Raster32PMyPaintSurface::Raster32PMyPaintSurface(const TRaster32P &ras,
                                                 RasterController &controller)
    : ras(ras), controller(&controller), internal() {
  assert(ras);
  internal = new Internal(*this);
}

Raster32PMyPaintSurface::~Raster32PMyPaintSurface() { delete internal; }

bool Raster32PMyPaintSurface::getColor(float x, float y, float radius,
                                       float &colorR, float &colorG,
                                       float &colorB, float &colorA) {
  return internal->getColor(x, y, radius, colorR, colorG, colorB, colorA);
}

bool Raster32PMyPaintSurface::drawDab(const mypaint::Dab &dab) {
  if (tip) return drawTipDab(dab);
  return internal->drawDab(dab);
}

//-----------------------------------------------------------------------------

// Stamps the tip in place of the round dab. The tip is centered on the dab,
// its longer side spans the diameter (2 * radius), and the aspect ratio and
// the angle of the dab squash and turn it as they do the round dab. The color
// is blended as the round dab does, but for the colorize mode, which is left
// out: normal, eraser and lock alpha. The hardness is not used.
bool Raster32PMyPaintSurface::drawTipDab(const mypaint::Dab &dab) {
  const float minRadius = 0.66f;  // as in mypaint::helpers::SurfaceCustom
  const float minOpaque = 1.f / 256.f;
  const float precision = 1e-4f;

  mypaint::Dab d = dab.getClamped();
  if (d.radius <= precision) return true;
  if (d.radius < minRadius) {
    d.opaque *= d.radius / minRadius;
    d.radius = minRadius;
  }
  if (d.opaque < minOpaque) return false;

  // the corners of the tip are up to sqrt(2) * radius away from the center
  const float reach = d.radius * 1.4143f + 1.f;
  const int x0      = std::max(0, (int)floor(d.x - reach));
  const int x1      = std::min(ras->getLx() - 1, (int)ceil(d.x + reach));
  const int y0      = std::max(0, (int)floor(d.y - reach));
  const int y1      = std::min(ras->getLy() - 1, (int)ceil(d.y + reach));
  if (x0 > x1 || y0 > y1) return false;

  if (controller && (!controller->askRead(TRect(x0, y0, x1, y1)) ||
                     !controller->askWrite(TRect(x0, y0, x1, y1))))
    return false;

  // tip pixels per output pixel, along the tip's x axis
  const float unit =
      0.5f * (float)std::max(tip->getLx(), tip->getLy()) / d.radius;
  const float pixelSize = unit * d.aspectRatio;  // the larger step
  const float angle     = d.angle * (3.14159265f / 180.f);
  const float s         = sinf(angle);
  const float c         = cosf(angle);
  const float centerX   = 0.5f * tip->getLx();
  const float centerY   = 0.5f * tip->getLy();

  const float blendNormal = 1.f - d.lockAlpha;
  const float blendLock   = d.lockAlpha;

  for (int y = y0; y <= y1; ++y) {
    TPixel32 *pixel = ras->pixels(y) + x0;
    for (int x = x0; x <= x1; ++x, ++pixel) {
      float dx  = (float)x + 0.5f - d.x;
      float dy  = (float)y + 0.5f - d.y;
      float ddx = dx * c + dy * s;
      float ddy = (dy * c - dx * s) * d.aspectRatio;

      // the rows of the raster go up, the rows of the tip go down
      float coverage =
          tip->sample(centerX + ddx * unit, centerY - ddy * unit, pixelSize);
      float o = coverage * d.opaque;
      if (o <= precision) continue;

      float destR, destG, destB, destA;
      readPixel(pixel, destR, destG, destB, destA);

      // the raster is premultiplied
      float oa = blendNormal * o;
      float ob = 1.f - oa;
      oa *= d.alphaEraser;
      destR = oa * d.colorR + ob * destR;
      destG = oa * d.colorG + ob * destG;
      destB = oa * d.colorB + ob * destB;
      destA = oa + ob * destA;

      if (blendLock > precision) {
        float la = blendLock * o;
        float lb = 1.f - la;
        la *= destA;
        destR = la * d.colorR + lb * destR;
        destG = la * d.colorG + lb * destG;
        destB = la * d.colorB + lb * destB;
      }

      writePixel(pixel, std::min(std::max(destR, 0.f), 1.f),
                 std::min(std::max(destG, 0.f), 1.f),
                 std::min(std::max(destB, 0.f), 1.f),
                 std::min(std::max(destA, 0.f), 1.f));
    }
  }
  return true;
}

bool Raster32PMyPaintSurface::getAntialiasing() const {
  return internal->antialiasing;
}

void Raster32PMyPaintSurface::setAntialiasing(bool value) {
  internal->antialiasing = value;
}

//=======================================================
//
// MyPaintTip
//
//=======================================================

MyPaintTip::MyPaintTip(int lx, int ly,
                       const std::vector<unsigned char> &coverage) {
  assert(lx > 0 && ly > 0 && coverage.size() == (size_t)lx * ly);
  Level first;
  first.lx     = lx;
  first.ly     = ly;
  first.pixels = coverage;
  m_levels.push_back(first);

  // half size copies, down to one pixel
  while (m_levels.back().lx > 1 || m_levels.back().ly > 1) {
    const Level &src = m_levels.back();
    Level dst;
    dst.lx = (src.lx + 1) / 2;
    dst.ly = (src.ly + 1) / 2;
    dst.pixels.resize((size_t)dst.lx * dst.ly);
    for (int y = 0; y < dst.ly; ++y) {
      int sy0 = 2 * y, sy1 = std::min(2 * y + 1, src.ly - 1);
      for (int x = 0; x < dst.lx; ++x) {
        int sx0 = 2 * x, sx1 = std::min(2 * x + 1, src.lx - 1);
        int sum = src.pixels[(size_t)sy0 * src.lx + sx0] +
                  src.pixels[(size_t)sy0 * src.lx + sx1] +
                  src.pixels[(size_t)sy1 * src.lx + sx0] +
                  src.pixels[(size_t)sy1 * src.lx + sx1];
        dst.pixels[(size_t)y * dst.lx + x] = (unsigned char)((sum + 2) / 4);
      }
    }
    m_levels.push_back(dst);  // invalidates src
  }
}

//-----------------------------------------------------------------------------

std::shared_ptr<const MyPaintTip> MyPaintTip::load(const QString &path) {
  QImage image(path);
  if (image.isNull() || image.width() < 1 || image.height() < 1)
    return std::shared_ptr<const MyPaintTip>();
  image = image.convertToFormat(QImage::Format_ARGB32);

  bool hasAlpha = false;
  for (int y = 0; y < image.height() && !hasAlpha; ++y) {
    const QRgb *line = (const QRgb *)image.constScanLine(y);
    for (int x = 0; x < image.width(); ++x)
      if (qAlpha(line[x]) < 255) {
        hasAlpha = true;
        break;
      }
  }

  std::vector<unsigned char> coverage((size_t)image.width() * image.height());
  for (int y = 0; y < image.height(); ++y) {
    const QRgb *line = (const QRgb *)image.constScanLine(y);
    for (int x = 0; x < image.width(); ++x)
      coverage[(size_t)y * image.width() + x] =
          hasAlpha ? (unsigned char)qAlpha(line[x])
                   : (unsigned char)(255 - qGray(line[x]));
  }
  return std::shared_ptr<const MyPaintTip>(
      new MyPaintTip(image.width(), image.height(), coverage));
}

//-----------------------------------------------------------------------------

float MyPaintTip::sample(float x, float y, float pixelSize) const {
  // pick the copy whose pixels are about the size of the output pixels
  int index = 0;
  while (pixelSize >= 2.f && index + 1 < (int)m_levels.size()) {
    pixelSize *= 0.5f;
    ++index;
  }
  const Level &level = m_levels[index];

  // bilinear, the center of the texel (i, j) is (i + 0.5, j + 0.5)
  float px = x * level.lx / m_levels[0].lx - 0.5f;
  float py = y * level.ly / m_levels[0].ly - 0.5f;
  float fx = floorf(px), fy = floorf(py);
  int ix = (int)fx, iy = (int)fy;
  float wx = px - fx, wy = py - fy;

  float top = texel(level, ix, iy) * (1.f - wx) + texel(level, ix + 1, iy) * wx;
  float bottom =
      texel(level, ix, iy + 1) * (1.f - wx) + texel(level, ix + 1, iy + 1) * wx;
  return (top * (1.f - wy) + bottom * wy) * (1.f / 255.f);
}

//=======================================================
//
// MyPaintToonzBrush
//
//=======================================================

MyPaintToonzBrush::MyPaintToonzBrush(const TRaster32P &ras,
                                     RasterController &controller,
                                     const mypaint::Brush &brush,
                                     bool interpolation)
    : m_ras(ras)
    , m_mypaintSurface(ras, controller)
    , m_brush(brush)
    , m_reset(true)
    , m_interpolation(interpolation) {
  // read brush antialiasing settings
  float aa = this->m_brush.getBaseValue(MYPAINT_BRUSH_SETTING_ANTI_ALIASING);
  m_mypaintSurface.setAntialiasing(aa > 0.5f);

  // reset brush antialiasing to zero to avoid radius and hardness correction
  this->m_brush.setBaseValue(MYPAINT_BRUSH_SETTING_ANTI_ALIASING, 0.f);
  for (int i = 0; i < MYPAINT_BRUSH_INPUTS_COUNT; ++i)
    this->m_brush.setMappingN(MYPAINT_BRUSH_SETTING_ANTI_ALIASING,
                              (MyPaintBrushInput)i, 0);
}

void MyPaintToonzBrush::beginStroke() {
  m_brush.reset();
  m_brush.newStroke();
  m_reset = true;
}

void MyPaintToonzBrush::endStroke() {
  if (!m_reset) {
    if (m_interpolation)
      strokeTo(TPointD(m_current.x, m_current.y), m_current.pressure,
               TPointD(m_current.tx, m_current.ty), 0.f);
    beginStroke();
  }
}

void MyPaintToonzBrush::strokeTo(const TPointD &position, double pressure,
                                 const TPointD &tilt, double dtime) {
  Params next(position.x, position.y, pressure, tilt.x, tilt.y, 0.0);

  if (m_reset) {
    m_current  = next;
    m_previous = m_current;
    m_reset    = false;
    // we need to jump to initial point (heuristic)
    m_brush.setState(MYPAINT_BRUSH_STATE_X, m_current.x);
    m_brush.setState(MYPAINT_BRUSH_STATE_Y, m_current.y);
    m_brush.setState(MYPAINT_BRUSH_STATE_ACTUAL_X, m_current.x);
    m_brush.setState(MYPAINT_BRUSH_STATE_ACTUAL_Y, m_current.y);
    return;
  }

  if (m_interpolation) {
    next.time = m_current.time + dtime;

    // accuracy
    const double threshold    = 1.0;
    const double thresholdSqr = threshold * threshold;
    const int maxLevel        = 16;

    // set initial segment
    Segment stack[maxLevel + 1];
    Params p0;
    Segment *segment    = stack;
    Segment *maxSegment = segment + maxLevel;
    p0.setMedian(m_previous, m_current);
    segment->p1 = m_current;
    segment->p2.setMedian(m_current, next);

    // process
    while (true) {
      double dx = segment->p2.x - p0.x;
      double dy = segment->p2.y - p0.y;
      if (dx * dx + dy * dy > thresholdSqr && segment != maxSegment) {
        Segment *sub = segment + 1;
        sub->p1.setMedian(p0, segment->p1);
        segment->p1.setMedian(segment->p1, segment->p2);
        sub->p2.setMedian(sub->p1, segment->p1);
        segment = sub;
      } else {
        m_brush.strokeTo(m_mypaintSurface, segment->p2.x, segment->p2.y,
                         segment->p2.pressure, segment->p2.tx, segment->p2.ty,
                         segment->p2.time - p0.time);
        if (segment == stack) break;
        p0 = segment->p2;
        --segment;
      }
    }

    // keep parameters for future interpolation
    m_previous = m_current;
    m_current  = next;

    // shift time
    m_previous.time = 0.0;
    m_current.time  = dtime;
  } else {
    m_brush.strokeTo(m_mypaintSurface, position.x, position.y, pressure, tilt.x,
                     tilt.y, dtime);
  }
}

//----------------------------------------------------------------------------------

void MyPaintToonzBrush::updateDrawing(const TRasterCM32P rasCM,
                                      const TRasterCM32P rasBackupCM,
                                      const TRect &bbox, int styleId,
                                      bool lockAlpha, bool eraser,
                                      MyPaintToonzEraserMode eraserMode) const {
  if (!rasCM) return;

  TRect rasRect    = rasCM->getBounds();
  TRect targetRect = bbox * rasRect;
  if (targetRect.isEmpty()) return;

  rasCM->copy(rasBackupCM->extract(targetRect), targetRect.getP00());
  putOnRasterCM(rasCM->extract(targetRect), m_ras->extract(targetRect), styleId,
                lockAlpha, eraser, eraserMode);
}
