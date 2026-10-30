#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
namespace RasterBrush {
// No filesystem extraction; ZIP64, encryption, links and duplicate/colliding
// paths are rejected. Returns no members when any archive validation fails.
QMap<QString, QByteArray> brushArchive(const QByteArray &bytes, QString &error);
}  // namespace RasterBrush
