#pragma once
#ifdef RASTERBRUSH_SANDBOX_TESTING
#include <QString>
#include <functional>
namespace RasterBrush {
// Qualification-only seam. Production builds contain neither storage nor calls.
using ImportCancellationObserver =
    std::function<void(const char *, const QString &, qint64)>;
void setImportCancellationObserver(ImportCancellationObserver observer);
void notifyImportCancellationObserver(const char *phase, const QString &path,
                                      qint64 processId);
}  // namespace RasterBrush
#endif
