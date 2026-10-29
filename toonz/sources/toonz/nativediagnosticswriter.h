#pragma once

#include <QDateTime>
#include <QString>
#include <memory>

namespace NativeDiagnostic {
enum class Severity { Debug, Info, Warning, Error };
enum class State { Off, Starting, Ready, Locked, Error, Stopped };

struct Record {
  QDateTime timestamp = QDateTime::currentDateTimeUtc();
  Severity severity = Severity::Info;
  QString source;
  QString text;
};

struct Status {
  State state = State::Off;
  QString detail;
  quint64 written = 0;
  quint64 dropped = 0;
  size_t queued = 0;
  size_t queuedBytes = 0;
  size_t pendingChanges = 0;
};

// Production defaults retain one 5 MiB file and three rotated backups.
// Smaller limits are injectable for deterministic CI tests.
struct Limits {
  size_t fileBytes = 5 * 1024 * 1024;
  size_t queueBytes = 1024 * 1024;
  size_t queueRecords = 1024;
  size_t recordBytes = 64 * 1024;
};

class Writer final {
public:
  explicit Writer(const Limits &limits = Limits());
  ~Writer();
  Writer(const Writer &) = delete;
  Writer &operator=(const Writer &) = delete;

  void enable(const QString &directory);
  void disable();
  void removeSavedLogs(const QString &directory);
  void setMinimumSeverity(Severity severity);
  void append(const Record &record);
  Status status() const;
  // Waits for queued operations, not a polling sleep. Intended for CI and
  // explicit shutdown; ordinary producers never wait for the disk.
  bool waitUntilIdle(int timeoutMilliseconds);
  void shutdown();

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};
}  // namespace NativeDiagnostic
