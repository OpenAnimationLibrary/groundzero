#pragma once

#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>

namespace GroundzeroAutomation {

struct MovieVerificationConfig final {
  QString mediaToolsDirectory;
  QString moviePath;
  int width           = 0;
  int height          = 0;
  int frameCount      = 0;
  double frameRate    = 0;
  qint64 maximumBytes = 0;
};

class MovieVerification {
public:
  virtual ~MovieVerification() = default;
  virtual void cancel()        = 0;
};

using MovieVerificationCompletion =
    std::function<void(QJsonObject metadata, QString error)>;

// Checks the closed, owned movie using a bounded asynchronous native probe.
// Completion is queued after the probe exits; keep the returned owner alive.
std::shared_ptr<MovieVerification> startMovieVerification(
    MovieVerificationConfig config, MovieVerificationCompletion completion);

}  // namespace GroundzeroAutomation
