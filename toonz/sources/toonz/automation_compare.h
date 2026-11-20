#pragma once

#include "automation.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QString>

#include <atomic>
#include <optional>
#include <vector>

namespace GroundzeroAutomation {

// One verified frame of a completed render job (automation_render.cpp).
struct RenderFrameRef final {
  QString artifactId;
  QString jobId;
  QString documentId;
  QString revision;
  QString path;
  QString sha256;
  QString profile;
  int frame    = 0;
  int width    = 0;
  int height   = 0;
  qint64 bytes = 0;
  QJsonObject effective;
};

// The frame an artifact id names, when a completed render job owns it.
std::optional<RenderFrameRef> renderFrameByArtifact(Context& context,
                                                    const QString& artifactId);
// Every frame of a completed render job: unknown_job when the id is not a
// retained job, unsupported_artifact_kind when it is not a render job,
// artifacts_unavailable when it has not completed.
std::vector<RenderFrameRef> completedRenderFrames(Context& context,
                                                  const QString& jobId);

// The comparison of two decoded straight RGBA8 images (contract w3-compare
// §5): every number is integer arithmetic or one correctly rounded binary64
// operation, so it is the same on every platform.
struct CompareInput final {
  const uchar* reference = nullptr;
  const uchar* candidate = nullptr;
  int width              = 0;
  int height             = 0;
  QRect region;
  bool premultiplied              = true;
  bool alpha                      = true;
  int threshold                   = 0;
  int gridColumns                 = 8;
  int gridRows                    = 8;
  int components                  = 16;
  const std::atomic<bool>* cancel = nullptr;
};

struct CompareOutput final {
  QJsonObject metrics;
  QJsonObject regions;
  qint64 changed  = 0;
  qint64 pixels   = 0;
  int maxAbsAlpha = 0;
  bool identical  = false;
  bool canceled   = false;
  // Per pixel of the region: the largest considered channel difference.
  std::vector<uchar> difference;
};

CompareOutput compareImages(const CompareInput& input);
// SHA-256 of "GZRGBA8\n", width and height as big-endian 32-bit integers and
// the straight RGBA bytes from the top row.
QString pixelSha256(const uchar* rgba, int width, int height);

QJsonArray compareOperations();
QJsonValue executeCompare(Context& context, const QString& method,
                          const QJsonObject& params);
// artifact.read for a review job's difference image or sheet; nullopt when
// no review job issued the id.
std::optional<QJsonValue> readReviewArtifact(Context& context,
                                             const QString& artifactId);
void sweepAbandonedReviews(Context& context);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationCompareTests();
#endif

}  // namespace GroundzeroAutomation
