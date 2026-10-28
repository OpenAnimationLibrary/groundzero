#include "cadmiumtypes.h"

#include "ttoonzimage.h"
#include "tsimplecolorstyles.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <limits>
#include <new>

namespace Cadmium {
namespace {

class RasterLock {
  TRasterCM32P m_raster;
public:
  explicit RasterLock(const TRasterCM32P &raster) : m_raster(raster) {
    m_raster->lock();
  }
  ~RasterLock() { m_raster->unlock(); }
};

QColor colorOf(const TPixel32 &pixel) {
  return QColor(pixel.r, pixel.g, pixel.b, pixel.m);
}

bool paletteColors(const TPaletteP &palette, QVector<QColor> &colors,
                   QString &error) {
  if (!palette || palette->getStyleCount() < 1) {
    error = QStringLiteral("The drawing has no palette.");
    return false;
  }
  if (palette->isAnimated()) {
    error = QStringLiteral("Cadmium flat painting requires a static palette.");
    return false;
  }
  colors.resize(palette->getStyleCount());
  for (int id = 0; id < colors.size(); ++id) {
    const TColorStyle *style = palette->getStyle(id);
    if (!style) {
      error = QStringLiteral("The drawing palette contains an invalid style.");
      return false;
    }
    colors[id] = colorOf(style->getMainColor());
  }
  return true;
}

bool solidStyle(const TPaletteP &palette, int id) {
  return id >= 0 && id <= TPixelCM32::getMaxPaint() &&
         id < palette->getStyleCount() &&
         dynamic_cast<const TSolidColorStyle *>(palette->getStyle(id));
}

TToonzImageP cloneDrawing(const TToonzImageP &source) {
  TToonzImageP clone = source->clone();
  // The native copy constructor does not preserve this field.
  clone->setOffset(source->getOffset());
  return clone;
}

QByteArray fingerprint(const TToonzImageP &image,
                       const QVector<QColor> &colors) {
  const TRasterCM32P raster = image->getRaster();
  QByteArray metadata;
  QDataStream stream(&metadata, QIODevice::WriteOnly);
  stream.setVersion(QDataStream::Qt_5_0);
  double dpiX, dpiY;
  image->getDpi(dpiX, dpiY);
  const TPoint offset = image->getOffset();
  const TRect box = image->getSavebox();
  stream << qint32(raster->getLx()) << qint32(raster->getLy())
         << dpiX << dpiY << qint32(offset.x) << qint32(offset.y)
         << qint32(box.x0) << qint32(box.y0) << qint32(box.x1)
         << qint32(box.y1) << qint32(image->getSubsampling())
         << qint32(colors.size());
  for (int id = 0; id < colors.size(); ++id) {
    stream << qint32(id) << quint32(colors[id].rgba())
           << qint32(image->getPalette()->getStyle(id)->getTagId());
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(metadata);
  for (int y = 0; y < raster->getLy(); ++y)
    hash.addData(reinterpret_cast<const char *>(raster->pixels(y)),
                 raster->getLx() * int(sizeof(TPixelCM32)));
  return hash.result();
}

}  // namespace

bool matchesToonzRaster(const TImageP &source, const Frame &frame, QString &error) {
  error.clear();
  const TToonzImageP image(source);
  if (frame.kind != Kind::ToonzRaster || frame.fingerprint.isEmpty() ||
      !image || !image->getRaster() || image->getSubsampling() != 1 ||
      image->getRaster()->getLx() != frame.lines.width() ||
      image->getRaster()->getLy() != frame.lines.height()) {
    error = QStringLiteral("The source drawing no longer matches the paint proposal.");
    return false;
  }
  try {
    QVector<QColor> colors;
    if (!paletteColors(TPaletteP(image->getPalette()), colors, error)) return false;
    RasterLock lock(image->getRaster());
    if (fingerprint(image, colors) != frame.fingerprint) {
      error = QStringLiteral("The drawing or palette changed. Generate a new paint proposal.");
      return false;
    }
    return true;
  } catch (const std::bad_alloc &) {
    error = QStringLiteral("There is not enough memory to validate the paint proposal.");
    return false;
  }
}

bool snapshotToonzRaster(const TImageP &source, Frame &frame, QString &error) {
  error.clear();
  const TToonzImageP image(source);
  if (!image || !image->getRaster()) {
    error = QStringLiteral("A Toonz Raster drawing is required.");
    return false;
  }
  if (image->getSubsampling() != 1) {
    error = QStringLiteral("Reload the drawing at full resolution before painting.");
    return false;
  }
  const TRasterCM32P sourceRaster = image->getRaster();
  const int width = sourceRaster->getLx(), height = sourceRaster->getLy();
  const qint64 count = qint64(width) * height;
  if (width < 1 || height < 1 || count > std::numeric_limits<int>::max() ||
      width > std::numeric_limits<int>::max() / int(sizeof(TPixelCM32))) {
    error = QStringLiteral("The drawing dimensions exceed the supported range.");
    return false;
  }
  try {
    Frame result;
    result.kind = Kind::ToonzRaster;
    TToonzImageP copy;
    {
      RasterLock lock(sourceRaster);
      copy = cloneDrawing(image);
    }
    result.original = copy;
    result.palette = TPaletteP(copy->getPalette());
    QVector<QColor> palette;
    if (!paletteColors(result.palette, palette, error)) return false;
    QVector<bool> solid;
    solid.reserve(palette.size());
    for (int id = 0; id < palette.size(); ++id)
      solid.append(solidStyle(result.palette, id));
    const TRasterCM32P raster = copy->getRaster();
    RasterLock lock(raster);
    result.lines = QImage(width, height, QImage::Format_ARGB32);
    if (result.lines.isNull()) {
      error = QStringLiteral("There is not enough memory for the line image.");
      return false;
    }
    result.labels.fill(-1, int(count));
    // Index zero is reserved for the exterior and fully opaque line pixels.
    result.regionStyles.append(0);
    result.regionColors.append(QColor(0, 0, 0, 0));
    auto pixelAt = [&](int index) -> const TPixelCM32 & {
      return raster->pixels(height - 1 - index / width)[index % width];
    };
    for (int y = 0; y < height; ++y) {
      QRgb *line = reinterpret_cast<QRgb *>(result.lines.scanLine(y));
      const TPixelCM32 *pixels = raster->pixels(height - 1 - y);
      for (int x = 0; x < width; ++x) {
        const TPixelCM32 &pixel = pixels[x];
        if (pixel.getInk() >= palette.size() || pixel.getPaint() >= palette.size()) {
          error = QStringLiteral("A drawing pixel references a missing palette style.");
          return false;
        }
        if (!solid[pixel.getInk()] || !solid[pixel.getPaint()]) {
          error = QStringLiteral("Cadmium flat painting requires solid ink and paint styles.");
          return false;
        }
        const QColor ink = palette[pixel.getInk()];
        const int alpha = ((255 - pixel.getTone()) * ink.alpha() + 127) / 255;
        line[x] = qRgba(ink.red(), ink.green(), ink.blue(), alpha);
        if (pixel.getTone() < 128 && ink.alpha() > 0)
          result.labels[y * width + x] = 0;
      }
    }
    QVector<int> component;
    for (int seed = 0; seed < int(count); ++seed) {
      if (result.labels[seed] != -1) continue;
      component.clear();
      component.append(seed);
      result.labels[seed] = -2;
      const int paint = pixelAt(seed).getPaint();
      bool exterior = false;
      for (int head = 0; head < component.size(); ++head) {
        const int index = component[head];
        const int x = index % width, y = index / width;
        exterior = exterior || x == 0 || x == width - 1 || y == 0 || y == height - 1;
        const int adjacent[] = {x > 0 ? index - 1 : -1,
                                x + 1 < width ? index + 1 : -1,
                                y > 0 ? index - width : -1,
                                y + 1 < height ? index + width : -1};
        for (int next : adjacent) {
          if (next >= 0 && result.labels[next] == -1 &&
              pixelAt(next).getPaint() == paint) {
            result.labels[next] = -2;
            component.append(next);
          }
        }
      }
      const int label = exterior && paint == 0 ? 0 : result.regionStyles.size();
      if (label) {
        result.regionStyles.append(paint);
        result.regionColors.append(palette[paint]);
      }
      for (int index : component) result.labels[index] = label;
    }
    // Extend native fill labels into partial-coverage line pixels. Paint below
    // an opaque ink pixel is intentionally excluded; the ink and tone never change.
    component.clear();
    for (int i = 0; i < int(count); ++i)
      if (result.labels[i] > 0) component.append(i);
    for (int head = 0; head < component.size(); ++head) {
      const int index = component[head];
      const int x = index % width, y = index / width;
      const int adjacent[] = {x > 0 ? index - 1 : -1,
                              x + 1 < width ? index + 1 : -1,
                              y > 0 ? index - width : -1,
                              y + 1 < height ? index + width : -1};
      for (int next : adjacent) {
        if (next < 0 || result.labels[next] != 0) continue;
        const TPixelCM32 &pixel = pixelAt(next);
        if (pixel.getTone() > 0 && pixel.getTone() < 128 &&
            pixel.getPaint() == pixelAt(index).getPaint()) {
          result.labels[next] = result.labels[index];
          component.append(next);
        }
      }
    }
    result.fingerprint = fingerprint(copy, palette);
    frame = result;
    return true;
  } catch (const std::bad_alloc &) {
    error = QStringLiteral("There is not enough memory to snapshot the drawing.");
    return false;
  }
}

TImageP applyToonzRaster(const Frame &frame, const QVector<int> &styles,
                        const QVector<QColor> &, bool repaint, QString &error) {
  error.clear();
  const TToonzImageP original(frame.original);
  if (frame.kind != Kind::ToonzRaster || !original || !original->getRaster() ||
      !frame.palette) {
    error = QStringLiteral("The Toonz Raster proposal has no source snapshot.");
    return TImageP();
  }
  const TRasterCM32P source = original->getRaster();
  const int width = source->getLx(), height = source->getLy();
  if (frame.lines.size() != QSize(width, height) ||
      qint64(frame.labels.size()) != qint64(width) * height ||
      styles.size() != frame.regionStyles.size() || styles.isEmpty()) {
    error = QStringLiteral("The paint proposal does not match the drawing regions.");
    return TImageP();
  }
  for (int style : styles) {
    if (style == -100 || style == -1) continue;
    if (!solidStyle(frame.palette, style)) {
      error = QStringLiteral("The paint proposal contains an invalid palette style.");
      return TImageP();
    }
  }
  for (int label : frame.labels) {
    if (label < 0 || label >= styles.size()) {
      error = QStringLiteral("The paint proposal contains an invalid region label.");
      return TImageP();
    }
  }
  try {
    TToonzImageP result = cloneDrawing(original);
    const TRasterCM32P raster = result->getRaster();
    RasterLock lock(raster);
    TRect changed;
    for (int y = 0; y < height; ++y) {
      const int nativeY = height - 1 - y;
      TPixelCM32 *pixels = raster->pixels(nativeY);
      for (int x = 0; x < width; ++x) {
        const int label = frame.labels[y * width + x];
        if (label == 0) continue;
        const int style = styles[label];
        if (style < 0 || (!repaint && pixels[x].getPaint() != 0) ||
            pixels[x].getPaint() == style) continue;
        pixels[x].setPaint(style);
        changed = changed + TRect(x, nativeY, x, nativeY);
      }
    }
    if (!changed.isEmpty()) result->setSavebox(result->getSavebox() + changed);
    return TImageP(result);
  } catch (const std::bad_alloc &) {
    error = QStringLiteral("There is not enough memory to apply the paint proposal.");
    return TImageP();
  }
}

}  // namespace Cadmium
