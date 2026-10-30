#pragma once
#include "toonz/rasterbrushpreset.h"
namespace RasterBrush {
struct ImportResult {
  std::vector<Record> records;
  QStringList diagnostics;
  bool complete = false;
};
// Versioned foreign-resource adapters. Native conversions do not confer
// source-engine semantic support or reference verification.
RASTERBRUSH_API ImportResult importBrush(const QByteArray &bytes,
                                         const QString &format, QString &error);
RASTERBRUSH_API ImportResult importBrushFile(const QString &path,
                                             QString &error);
}  // namespace RasterBrush
