#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
namespace RasterBrush {
struct PngParts {
  QByteArray image;
  QMap<QString, QByteArray> text;
};
// Validates chunk framing/CRC and bounds metadata inflation before Qt sees PNG.
PngParts boundedPng(const QByteArray &bytes, QString &error);
}  // namespace RasterBrush
