#pragma once

#include "tcommon.h"
#include <QImage>
#include <QRectF>
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
struct Cancellation {
  const std::atomic_bool *flag;
  const int *renderFlag;
  Cancellation(const std::atomic_bool *flag = nullptr, const int *renderFlag = nullptr)
      : flag(flag), renderFlag(renderFlag) {}
  bool operator()() const { return (flag && flag->load()) || (renderFlag && *renderFlag); }
};
struct Profile {
  QImage reference; // Captured opaque 256-square SDR sRGB; scene-owned.
  QRectF crop{0, 0, 1, 1};
  QString sampleName, sampleDigest, maskName, maskDigest;
};
DVAPI bool capture(const QImage &reference, const QImage &mask, const QRectF &crop,
                   Profile &profile, QString &error);
DVAPI QString serialize(const Profile &profile);
DVAPI bool parse(const QString &text, Profile &profile, QString &error);
// Letterbox onto white before inference, retaining geometry. The returned
// rectangle identifies the content area of the fixed 384-square model canvas.
DVAPI QImage prepare(const QImage &source, QRect &contentArea, QString &error);
DVAPI QImage restoreSize(const QImage &styled384, const QSize &sourceSize);

// Construct, call, stop and destroy a session on the same thread. Each operation
// leases the application's single CPU worker; a batch retains models/embedding
// for its frames. Idle sessions own no process. No artwork leaves the machine.
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
             Cancellation cancel = {});
  bool styleBatch(const QImage &reference256, const std::vector<QImage> &content384,
                  std::vector<QImage> &results, QString &error,
                  Cancellation cancel = {});
  void stop();
};
}  // namespace NeuralStyle
