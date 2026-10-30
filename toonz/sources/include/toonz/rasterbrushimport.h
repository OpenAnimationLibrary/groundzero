#pragma once
#include "toonz/rasterbrushpreset.h"
#include <atomic>
namespace RasterBrush {
struct ImportResult {
  std::vector<Record> records;
  QStringList diagnostics;
  bool complete = false;
};
// Versioned foreign-resource adapters. Native conversions do not confer
// source-engine semantic support or reference verification.
RASTERBRUSH_API ImportResult
importBrush(const QByteArray &bytes, const QString &format, QString &error,
            const std::atomic_bool *cancelled = nullptr,
            size_t maximumDecodedBytes        = 256u * 1024u * 1024u);
RASTERBRUSH_API ImportResult
importBrushFile(const QString &path, QString &error,
                const std::atomic_bool *cancelled = nullptr);
// Worker-process import contains decoder crashes and supports hard
// cancellation. The worker retains the current user's filesystem privileges.
RASTERBRUSH_API int runImportWorker(const QString &input,
                                    const QString &outputDirectory);
RASTERBRUSH_API ImportResult
importBrushProcess(const QString &executable, const QString &path,
                   QString &error, const std::atomic_bool *cancelled = nullptr);
}  // namespace RasterBrush
