#pragma once

#include "tcommon.h"
#include <QJsonObject>
#include <QString>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

#undef DVAPI
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

namespace NeuralStyle {

// Trusted detached-worker execution state; never serialized into a scene.
class DVAPI WorkerExecution final {
public:
  using Clock = std::chrono::steady_clock;
  const QString directory;
  const QString manifestSha256;
  const QJsonObject files;
  const QString cacheNamespace;
  const Clock::time_point deadline;

  WorkerExecution(QString packageDirectory, QString manifestHash,
                  QJsonObject identities, Clock::time_point expires);
  bool expired() const;
  bool verify(QString& error) const;
  void helperStarting() const;
  void helperClosed() const;
  // A helper of this execution was killed before it exited by itself.
  void helperKilled() const;
  bool helperWasKilled() const;
  bool helpersClosed() const;
  std::unique_lock<std::timed_mutex> lockCompute(
      const int* cancel = nullptr) const;
  [[noreturn]] void closureFailed() const;

private:
  mutable std::atomic_int m_helpers{0};
  mutable std::atomic_bool m_killed{false};
  mutable std::timed_mutex m_compute;
};

// Args: directory is the trusted app-adjacent package, deadline predates
// admission. Returns: a verified identity lease, or null with an actionable
// diagnostic.
DVAPI std::shared_ptr<const WorkerExecution> captureWorkerExecution(
    const QString& directory, WorkerExecution::Clock::time_point deadline,
    QString& error);

}  // namespace NeuralStyle
