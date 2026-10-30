#pragma once

#include "tcommon.h"
#include <QImage>
#include <QString>
#include <atomic>
#include <memory>

#undef DVAPI
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

namespace NeuralStyle {
// Construct, call, stop and destroy a session on the same thread. It owns one CPU worker and one
// reference embedding. No artwork leaves the local machine. A failed/cancelled
// operation discards the entire process, so the next call starts cleanly.
class DVAPI Session final {
  struct Impl;
  std::unique_ptr<Impl> m_impl;
public:
  // Directory must come from the trusted application/component installer, never
  // a scene or arbitrary downloaded manifest. Checksums detect corruption, not
  // authenticity of an attacker-replaced executable and manifest together.
  explicit Session(const QString &packageDirectory);
  ~Session();
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  // Exact model-size opaque SDR sRGB inputs; resampling/compositing belong to
  // the caller so preview and final render share the same preprocessing.
  bool style(const QImage &reference256, const QImage &content384,
             QImage &result, QString &error,
             const std::atomic_bool *cancel = nullptr);
  void stop();
};
}  // namespace NeuralStyle
