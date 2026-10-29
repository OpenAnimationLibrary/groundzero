#pragma once

#include "tcommon.h"
#include <QImage>
#include <QRectF>
#include <QString>
#include <array>
#include <atomic>

#undef DVAPI
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

namespace ColorMatch {
using Color = std::array<double, 3>;
struct Statistics {
  Color mean{{0, 0, 0}}, deviation{{0, 0, 0}};
};
struct Profile {
  Statistics source, reference;
  bool preserveLightness = true;
  double strength = 1;
};
// Straight SDR sRGB, normalized to [0,1]. Never modifies coverage.
DVAPI Color transform(const Color &rgb, const Profile &profile);
// Normalized top-left crop. Mask coverage = alpha * grayscale luminance.
// Images must already be bounded to 1024x1024 or smaller by the caller.
DVAPI bool analyze(const QImage &image, const QImage &mask, const QRectF &crop,
                   Statistics &result, QString &error,
                   const std::atomic_bool *cancel = nullptr);
DVAPI QString serialize(const Profile &profile);
DVAPI bool parse(const QString &text, Profile &profile, QString &error);
DVAPI QImage preview(const QImage &image, const Profile &profile,
                    const std::atomic_bool *cancel = nullptr);
DVAPI bool exportCube(const QString &path, const Profile &profile,
                      QString &error);
// Decode only local images; refuse excessive dimensions before decoding.
DVAPI QImage loadSample(const QString &path, QString &error);
}  // namespace ColorMatch
