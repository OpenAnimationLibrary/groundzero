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
// cancellation. Linux workers require Landlock ABI 3 and seccomp. Windows
// workers require an ephemeral zero-capability AppContainer plus Job limits.
// macOS uses an independently signed App Sandbox helper and descriptor
// transport. All platform boundaries require their denial/packaging
// qualification checks.
RASTERBRUSH_API int runImportWorker(const QString &input,
                                    const QString &outputDirectory);
#ifdef __APPLE__
// Only the signed helper uses this descriptor transport. The callback supports
// the same adversarial protocol fixtures in the separately signed CI helper.
RASTERBRUSH_API int runMacImportStream(const QString &name,
                                       const QString &forbidden,
                                       int (*worker)(const QString &,
                                                     const QString &));
#endif
RASTERBRUSH_API ImportResult
importBrushProcess(const QString &executable, const QString &path,
                   QString &error, const std::atomic_bool *cancelled = nullptr,
                   size_t maximumDecodedBytes = 256u * 1024u * 1024u);
}  // namespace RasterBrush
