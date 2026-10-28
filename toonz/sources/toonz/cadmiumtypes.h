#pragma once

#include "timage.h"
#include "tpalette.h"
#include <QColor>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Cadmium {
enum class Kind { ToonzRaster, Vector, FullColor };
enum class RasterSource { TransparentLines, PaperScan, LinkedLines };
struct Frame {
  Kind kind = Kind::ToonzRaster;
  TImageP original;
  TPaletteP palette;
  QImage lines;
  QVector<int> labels;
  QVector<int> regionStyles;
  QVector<int> nativeRegionIndices;
  QVector<QColor> regionColors;
  QByteArray fingerprint;
  QStringList warnings;
  RasterSource rasterSource = RasterSource::TransparentLines;
  TImageP lineSource;
  bool extractionApproved = false;
  double paperThreshold = 0.92;
  double lineThreshold = 0.2;
  QImage protectionMask;
};
bool snapshotToonzRaster(const TImageP &, Frame &, QString &);
bool snapshotVector(const TImageP &, Frame &, QString &);
bool snapshotFullColor(const TImageP &, Frame &, QString &);
bool matchesFullColor(const TImageP &, const Frame &, const TImageP &, QString &);
bool matchesToonzRaster(const TImageP &, const Frame &, QString &);
bool matchesFullColor(const TImageP &, const Frame &, const Frame &, QString &);
TImageP applyToonzRaster(const Frame &, const QVector<int> &,
                        const QVector<QColor> &, bool, QString &);
TImageP applyVector(const Frame &, const QVector<int> &,
                   const QVector<QColor> &, bool, QString &);
TImageP applyFullColor(const Frame &, const QVector<int> &,
                      const QVector<QColor> &, bool, QString &);
}
