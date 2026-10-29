#include "cadmiumtypes.h"

#include "tofflinegl.h"
#include "tregion.h"
#include "tsimplecolorstyles.h"
#include "tstroke.h"
#include "tvectorimage.h"
#include "tvectorrenderdata.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <exception>

namespace Cadmium {
namespace {
constexpr int canvasSize = 1024;
constexpr int border = 16;
constexpr int maxRegions = 4094;

void appendRegion(TRegion *region, QVector<TRegion *> &regions) {
  regions.append(region);
  for (UINT i = 0; i < region->getSubregionCount(); ++i)
    appendRegion(region->getSubregion(i), regions);
}

QVector<TRegion *> regionsOf(const TVectorImageP &image) {
  QVector<TRegion *> regions;
  for (UINT i = 0; i < image->getRegionCount(); ++i)
    appendRegion(image->getRegion(i), regions);
  return regions;
}

bool solidStyle(const TPaletteP &palette, int style) {
  return palette && style >= 0 && style < palette->getStyleCount() &&
         dynamic_cast<TSolidColorStyle *>(palette->getStyle(style));
}

QByteArray fingerprintOf(const TVectorImageP &image,
                        const QVector<TRegion *> &regions) {
  QByteArray data;
  QDataStream stream(&data, QIODevice::WriteOnly);
  stream.setVersion(QDataStream::Qt_5_9);
  stream << qint32(image->getStrokeCount()) << image->getAutocloseTolerance();
  for (UINT i = 0; i < image->getStrokeCount(); ++i) {
    const TStroke *stroke = image->getStroke(i);
    stream << qint32(stroke->getId()) << qint32(stroke->getStyle())
           << stroke->isSelfLoop()
           << qint32(image->getGroupByStroke(i))
           << qint32(stroke->getControlPointCount());
    for (int j = 0; j < stroke->getControlPointCount(); ++j) {
      const TThickPoint p = stroke->getControlPoint(j);
      stream << p.x << p.y << p.thick;
    }
  }
  stream << qint32(regions.size());
  for (TRegion *region : regions) {
    stream << qint32(region->getStyle()) << qint32(region->getEdgeCount());
    for (UINT i = 0; i < region->getEdgeCount(); ++i) {
      const TEdge *edge = region->getEdge(i);
      stream << qint32(edge->m_type) << qint32(edge->m_index)
             << edge->m_w0 << edge->m_w1;
    }
  }
  const TPalette *palette = image->getPalette();
  stream << qint32(palette->getStyleCount()) << qint32(palette->getFrame());
  for (int i = 0; i < palette->getStyleCount(); ++i) {
    const TColorStyle *style = palette->getStyle(i);
    const TPixel32 color = style->getMainColor();
    stream << qint32(style->getTagId()) << quint32(color.r)
           << quint32(color.g) << quint32(color.b) << quint32(color.m);
  }
  return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QImage imageFromRaster(const TRaster32P &raster) {
  if (!raster) return QImage();
  QImage image(raster->getLx(), raster->getLy(), QImage::Format_RGBA8888);
  if (image.isNull()) return image;
  raster->lock();
  for (int y = 0; y < image.height(); ++y) {
    const TPixel32 *source = raster->pixels(image.height() - 1 - y);
    uchar *destination = image.scanLine(y);
    for (int x = 0; x < image.width(); ++x) {
      const int alpha = source[x].m;
      const auto straight = [alpha](int channel) -> uchar {
        return alpha ? static_cast<uchar>(std::min(255, (channel * 255 + alpha / 2) / alpha)) : 0;
      };
      destination[4 * x] = straight(source[x].r);
      destination[4 * x + 1] = straight(source[x].g);
      destination[4 * x + 2] = straight(source[x].b);
      destination[4 * x + 3] = source[x].m;
    }
  }
  raster->unlock();
  return image;
}

class CurrentContext {
  TOfflineGL &m_gl;
public:
  explicit CurrentContext(TOfflineGL &gl) : m_gl(gl) { m_gl.makeCurrent(); }
  ~CurrentContext() { m_gl.doneCurrent(); }
};
}  // namespace

bool snapshotVector(const TImageP &source, Frame &frame, QString &error) {
  error.clear();
  const TVectorImageP input(source);
  if (!input || !input->getPalette()) {
    error = QObject::tr("Cadmium requires a vector drawing with a palette.");
    return false;
  }
  if (QCoreApplication::instance() &&
      QThread::currentThread() != QCoreApplication::instance()->thread()) {
    error = QObject::tr("Vector analysis must start on the application thread.");
    return false;
  }
  if (input->getPalette()->isAnimated()) {
    error = QObject::tr("Cadmium requires a palette with static flat colors.");
    return false;
  }
  try {
    const TVectorImageP original = input->clone();
    TPaletteP palette = input->getPalette()->clone();
    original->setPalette(palette.getPointer());
    original->findRegions();
    const QVector<TRegion *> regions = regionsOf(original);
    if (regions.isEmpty() || regions.size() > maxRegions) {
      error = QObject::tr("The vector drawing needs between 1 and %1 native fill regions.")
                  .arg(maxRegions);
      return false;
    }
    const TRectD bounds = original->getBBox();
    if (bounds.isEmpty() || !std::isfinite(bounds.x0) ||
        !std::isfinite(bounds.y0) || !std::isfinite(bounds.x1) ||
        !std::isfinite(bounds.y1)) {
      error = QObject::tr("The vector drawing has invalid bounds.");
      return false;
    }
    for (TRegion *region : regions) {
      if (!solidStyle(palette, region->getStyle())) {
        error = QObject::tr("Cadmium supports solid vector fill styles only.");
        return false;
      }
    }
    const double extent = std::max(bounds.getLx(), bounds.getLy());
    if (!(extent > 0.0)) {
      error = QObject::tr("The vector drawing has no measurable extent.");
      return false;
    }
    const double scale = (canvasSize - 2.0 * border) / extent;
    const TAffine transform =
        TTranslation(canvasSize / 2.0, canvasSize / 2.0) * TScale(scale) *
        TTranslation(-(bounds.x0 + bounds.x1) / 2.0,
                     -(bounds.y0 + bounds.y1) / 2.0);
    TOfflineGL gl(TDimension(canvasSize, canvasSize));
    CurrentContext context(gl);
    gl.clear(TPixel32(0, 0, 0, 0));
    TVectorRenderData lineData(TVectorRenderData::ProductionSettings(),
                              transform, TRect(), palette.getPointer());
    lineData.m_drawRegions = false;
    gl.draw(original, lineData);
    gl.flush();
    QImage lines = imageFromRaster(gl.getRaster());
    if (lines.isNull()) {
      error = QObject::tr("Unable to allocate the vector analysis image.");
      return false;
    }

    // This independent label pass follows native grouping and region nesting.
    // Encoded colors never enter the model's line-art image.
    const TVectorImageP mask = original->clone();
    TPaletteP labelPalette = new TPalette;
    labelPalette->setStyle(0, TPixel32(0, 0, 0, 0));
    for (UINT i = 0; i < mask->getStrokeCount(); ++i)
      mask->getStroke(i)->setStyle(0);
    const QVector<TRegion *> maskRegions = regionsOf(mask);
    if (maskRegions.size() != regions.size()) {
      error = QObject::tr("Vector regions changed while preparing analysis.");
      return false;
    }
    for (int i = 0; i < maskRegions.size(); ++i) {
      const int label = i + 1;
      const int style = labelPalette->addStyle(
          TPixel32(label & 255, (label >> 8) & 255, (label >> 16) & 255, 255));
      if (style < 0) {
        error = QObject::tr("Unable to allocate vector region labels.");
        return false;
      }
      maskRegions[i]->setStyle(style);
      maskRegions[i]->invalidateProp();
    }
    mask->setPalette(labelPalette.getPointer());
    gl.clear(TPixel32(0, 0, 0, 0));
    TVectorRenderData maskData(TVectorRenderData::ProductionSettings(),
                              transform, TRect(), labelPalette.getPointer());
    maskData.m_antiAliasing = false;
    maskData.m_regionAntialias = false;
    gl.draw(mask, maskData);
    gl.flush();
    const QImage labelImage = imageFromRaster(gl.getRaster());
    if (labelImage.isNull()) {
      error = QObject::tr("Unable to allocate vector region labels.");
      return false;
    }
    QVector<int> labels(canvasSize * canvasSize, 0);
    QVector<int> dense(regions.size() + 1, -1);
    dense[0] = 0;
    QVector<int> nativeIndices(1, -1);
    QVector<int> styles(1, 0);
    QVector<QColor> colors(1, QColor(0, 0, 0, 0));
    for (int y = 0; y < canvasSize; ++y) {
      const uchar *row = labelImage.constScanLine(y);
      for (int x = 0; x < canvasSize; ++x) {
        const uchar *pixel = row + 4 * x;
        const int raw = pixel[3] ? pixel[0] + (pixel[1] << 8) +
                                     (pixel[2] << 16) : 0;
        if (raw < 0 || raw > regions.size()) {
          error = QObject::tr("The renderer returned an invalid region label.");
          return false;
        }
        if (dense[raw] < 0) {
          dense[raw] = styles.size();
          const int nativeIndex = raw - 1;
          const int style = regions[nativeIndex]->getStyle();
          const TPixel32 color = palette->getStyle(style)->getMainColor();
          nativeIndices.append(nativeIndex);
          styles.append(style);
          colors.append(QColor(color.r, color.g, color.b, color.m));
        }
        labels[y * canvasSize + x] = dense[raw];
      }
    }
    if (styles.size() == 1) {
      error = QObject::tr("No native vector fill region is visible at the analysis scale.");
      return false;
    }
    Frame result;
    result.kind = Kind::Vector;
    result.original = TImageP(original.getPointer());
    result.palette = palette;
    result.lines = lines;
    result.labels = labels;
    result.regionStyles = styles;
    result.regionColors = colors;
    result.nativeRegionIndices = nativeIndices;
    const int omitted = regions.size() - (nativeIndices.size() - 1);
    if (omitted > 0) {
      result.warnings.append(QObject::tr(
          "%1 native vector region(s) are hidden or too small at the analysis "
          "scale. They are unresolved and will remain unchanged.").arg(omitted));
    }
    result.fingerprint = fingerprintOf(original, regions);
    frame = result;
    return true;
  } catch (const std::exception &) {
    error = QObject::tr("Unable to prepare native vector regions for Cadmium.");
  } catch (...) {
    error = QObject::tr("Unable to render the vector analysis image.");
  }
  return false;
}

TImageP applyVector(const Frame &frame, const QVector<int> &styleIds,
                   const QVector<QColor> &colors, bool repaint, QString &error) {
  error.clear();
  const TVectorImageP original(frame.original);
  if (frame.kind != Kind::Vector || !original || !frame.palette ||
      styleIds.size() != frame.regionStyles.size() ||
      frame.nativeRegionIndices.size() != styleIds.size() ||
      (!colors.isEmpty() && colors.size() != styleIds.size())) {
    error = QObject::tr("The vector paint proposal is incomplete.");
    return TImageP();
  }
  const QVector<TRegion *> originalRegions = regionsOf(original);
  if (fingerprintOf(original, originalRegions) != frame.fingerprint) {
    error = QObject::tr("The vector snapshot changed; generate a new proposal.");
    return TImageP();
  }
  for (int label = 1; label < styleIds.size(); ++label) {
    const int index = frame.nativeRegionIndices[label];
    const int style = styleIds[label];
    if (index < 0 || index >= originalRegions.size() ||
        (style != -1 && style != -100 && !solidStyle(frame.palette, style))) {
      error = QObject::tr("The vector proposal contains an invalid region or fill style.");
      return TImageP();
    }
  }
  const TVectorImageP result = original->clone();
  const QVector<TRegion *> regions = regionsOf(result);
  if (regions.size() != originalRegions.size()) {
    error = QObject::tr("Vector topology changed; generate a new proposal.");
    return TImageP();
  }
  for (int label = 1; label < styleIds.size(); ++label) {
    const int style = styleIds[label];
    TRegion *region = regions[frame.nativeRegionIndices[label]];
    if (style < 0 || (!repaint && region->getStyle() != 0)) continue;
    region->setStyle(style);
    region->invalidateProp();
  }
  return TImageP(result.getPointer());
}
}  // namespace Cadmium
