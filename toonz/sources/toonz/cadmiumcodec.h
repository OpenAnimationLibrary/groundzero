#pragma once

#include "cadmiumtypes.h"
#include <QBuffer>

namespace Cadmium {
inline QString imageUri(const QImage &image) {
  if (image.isNull()) return QString();
  QByteArray bytes;
  QBuffer buffer(&bytes);
  if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
    return QString();
  return QStringLiteral("data:image/png;base64,") +
         QString::fromLatin1(bytes.toBase64());
}

inline QString lineImageUri(const QImage &source) {
  if (source.isNull()) return QString();
  QImage image = source.convertToFormat(QImage::Format_RGBA8888);
  if (image.isNull()) return QString();
  for (int y = 0; y < image.height(); ++y) {
    uchar *row = image.scanLine(y);
    for (int x = 0; x < image.width(); ++x) {
      uchar *pixel = row + 4 * x;
      // Model image preparation also inspects RGB beneath transparent pixels.
      // Keep an explicit white canvas there while retaining line coverage.
      if (pixel[3] == 0) pixel[0] = pixel[1] = pixel[2] = 255;
    }
  }
  return imageUri(image);
}

inline QString labelUri(const Frame &frame) {
  const int width = frame.lines.width(), height = frame.lines.height();
  if (width <= 0 || height <= 0 || frame.regionColors.isEmpty() ||
      qint64(width) * height != frame.labels.size())
    return QString();
  QImage image(width, height, QImage::Format_RGBA8888);
  if (image.isNull()) return QString();
  for (int y = 0; y < height; ++y) {
    uchar *row = image.scanLine(y);
    for (int x = 0; x < width; ++x) {
      const int label = frame.labels[y * width + x];
      if (label < 0 || label >= frame.regionColors.size()) return QString();
      // The serving decoder reads PNG as BGRA, then packs those channels
      // most-significant first. Alpha carries the low byte, including zero.
      row[x * 4] = uchar((label >> 8) & 255);
      row[x * 4 + 1] = uchar((label >> 16) & 255);
      row[x * 4 + 2] = uchar((label >> 24) & 255);
      row[x * 4 + 3] = uchar(label & 255);
    }
  }
  return imageUri(image);
}
}
