#include "nativediagnosticswriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <stdexcept>

namespace NativeDiagnostic {
namespace {
QString fileName(int index) {
  return index ? QString("diagnostics.%1.log").arg(index)
               : QString("diagnostics.log");
}

spdlog::filename_t nativeFileName(const QString &path) {
#ifdef SPDLOG_WCHAR_FILENAMES
  return path.toStdWString();
#else
  return path.toUtf8().toStdString();
#endif
}

const char *severityName(Severity severity) {
  switch (severity) {
  case Severity::Debug: return "debug";
  case Severity::Info: return "info";
  case Severity::Warning: return "warning";
  case Severity::Error: return "error";
  }
  return "info";
}

spdlog::level::level_enum logLevel(Severity severity) {
  switch (severity) {
  case Severity::Debug: return spdlog::level::debug;
  case Severity::Info: return spdlog::level::info;
  case Severity::Warning: return spdlog::level::warn;
  case Severity::Error: return spdlog::level::err;
  }
  return spdlog::level::info;
}
}  // namespace

class Writer::Impl {
public:
  enum class Kind { Reconcile, Delete, Record };
  struct Task {
    Kind kind;
    QString directory;
    QByteArray data;
    Severity severity = Severity::Info;
  };
  Limits limits;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::condition_variable idle;
  std::deque<Task> tasks;
  std::thread worker;
  Status current;
  Severity minimum = Severity::Info;
  bool desiredEnabled = false;
  bool accepting = false;
  bool stopping = false;
  bool working = false;
  bool workingRecord = false;
  bool workerExited = false;
  bool reconcileQueued = false;
  bool deleteQueued = false;
  QString desiredDirectory;
  quint64 pendingDropped = 0;
  std::unique_ptr<QLockFile> fileLock;
  std::shared_ptr<spdlog::logger> logger;
  QString activeDirectory;

  explicit Impl(const Limits &requested) : limits(requested) {
    limits.fileBytes = std::max(size_t(256), limits.fileBytes);
    limits.recordBytes = std::max(size_t(128),
        std::min(limits.recordBytes, limits.fileBytes - 1));
    limits.queueBytes = std::max(limits.recordBytes, limits.queueBytes);
    limits.queueRecords = std::max(size_t(1), limits.queueRecords);
  }

  void startLocked() {
    if (worker.joinable() && workerExited) worker.join();
    if (worker.joinable()) return;
    workerExited = false;
    try {
      worker = std::thread([this] {
        try { run(); }
        catch (...) {
          close();
          std::lock_guard<std::mutex> guard(mutex);
          current.state = State::Error;
          current.detail = "The diagnostic writer stopped unexpectedly.";
          current.dropped += current.queued + (workingRecord ? 1 : 0);
          current.queued = current.queuedBytes = 0;
          tasks.clear();
          accepting = working = workingRecord = false;
          reconcileQueued = deleteQueued = false;
          workerExited = true;
          idle.notify_all();
          return;
        }
        std::lock_guard<std::mutex> guard(mutex);
        workerExited = true;
        idle.notify_all();
      });
    } catch (...) {
      current.state = State::Error;
      current.detail = "Cannot start the diagnostic writer.";
      accepting = false;
      current.dropped += current.queued;
      current.queued = current.queuedBytes = 0;
      tasks.clear();
      reconcileQueued = deleteQueued = false;
      idle.notify_all();
    }
  }

  void queueReconcileLocked() {
    if (!reconcileQueued) {
      tasks.push_back({Kind::Reconcile, {}, {}});
      reconcileQueued = true;
    }
  }

  void state(State next, const QString &detail = QString()) {
    std::lock_guard<std::mutex> guard(mutex);
    current.state = next;
    current.detail = detail;
    if (next == State::Error || next == State::Locked) accepting = false;
  }

  void close() {
    if (logger) {
      try { logger->flush(); } catch (...) {}
      logger.reset();
    }
    fileLock.reset();
    activeDirectory.clear();
  }

  bool acquire(const QString &directory) {
    QDir dir(directory);
    if (directory.isEmpty() || !dir.mkpath(".")) {
      state(State::Error, "Cannot create the diagnostic log folder.");
      return false;
    }
    for (int index = 0; index <= 3; ++index) {
      const QFileInfo file(dir.filePath(fileName(index)));
      if (file.isSymLink() || (file.exists() && !file.isFile())) {
        state(State::Error, "A diagnostic filename is occupied by another file type.");
        return false;
      }
    }
    auto lock = std::make_unique<QLockFile>(dir.filePath("diagnostics.lock"));
    // QLockFile detects a dead owner. Never steal a live process's log family
    // merely because that process has been open for a long time.
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
      state(lock->error() == QLockFile::LockFailedError ? State::Locked
                                                       : State::Error,
            "Cannot take ownership of the diagnostic log files.");
      return false;
    }
    activeDirectory = dir.absolutePath();
    fileLock = std::move(lock);
    return true;
  }

  void open(const QString &directory) {
    close();
    if (!acquire(directory)) return;
    try {
      // Adapt the source template's bounded rotating sink. Preserve the
      // captured event time rather than stamping when the worker drains it.
      auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          nativeFileName(QDir(activeDirectory).filePath(fileName(0))),
          limits.fileBytes, 3, false);
      logger = std::make_shared<spdlog::logger>("native-diagnostics", sink);
      logger->set_pattern("%v");
      logger->set_level(spdlog::level::trace);
      logger->flush_on(spdlog::level::warn);
      // Let the worker report errors through status, never through Qt or
      // spdlog's default stderr callback (which could expose private paths).
      logger->set_error_handler([](const std::string &) {
        throw std::runtime_error("Diagnostic write failed");
      });
      {
        std::lock_guard<std::mutex> guard(mutex);
        accepting = desiredEnabled && !stopping;
      }
      state(State::Ready);
    } catch (...) {
      close();
      state(State::Error, "Cannot open the diagnostic log file.");
    }
  }

  void erase(const QString &directory) {
    close();
    if (!acquire(directory)) return;
    bool ok = true;
    QDir dir(activeDirectory);
    for (int index = 0; index <= 3; ++index) {
      const QString path = dir.filePath(fileName(index));
      if (QFileInfo::exists(path) && !QFile::remove(path)) ok = false;
    }
    close();
    if (!ok) {
      state(State::Error, "Some diagnostic log files could not be removed.");
      return;
    }
    bool resume;
    {
      std::lock_guard<std::mutex> guard(mutex);
      resume = desiredEnabled && !stopping;
    }
    if (resume) open(directory);
    else state(State::Off);
  }

  void write(const Task &task) {
    if (!logger) {
      std::lock_guard<std::mutex> guard(mutex);
      ++current.dropped;
      return;
    }
    quint64 lost;
    {
      std::lock_guard<std::mutex> guard(mutex);
      lost = pendingDropped;
      pendingDropped = 0;
    }
    try {
      if (lost)
        logger->warn("[diagnostics] {} messages dropped before they could be written", lost);
      logger->log(logLevel(task.severity), "{}", task.data.constData());
      std::lock_guard<std::mutex> guard(mutex);
      ++current.written;
    } catch (...) {
      close();
      {
        std::lock_guard<std::mutex> guard(mutex);
        ++current.dropped;
      }
      state(State::Error, "Writing diagnostic logs failed. Check free space and permissions.");
    }
  }

  void run() {
    for (;;) {
      Task task;
      bool enableRequested = false;
      {
        std::unique_lock<std::mutex> guard(mutex);
        changed.wait(guard, [this] { return stopping || !tasks.empty(); });
        if (tasks.empty() && stopping) break;
        task = std::move(tasks.front());
        tasks.pop_front();
        if (task.kind == Kind::Reconcile) {
          reconcileQueued = false;
          task.directory = desiredDirectory;
          enableRequested = desiredEnabled;
        } else if (task.kind == Kind::Delete) {
          deleteQueued = false;
        } else {
          --current.queued;
          current.queuedBytes -= size_t(task.data.size());
        }
        working = true;
        workingRecord = task.kind == Kind::Record;
      }
      try {
        if (task.kind == Kind::Reconcile) {
          if (enableRequested) {
            if (logger && activeDirectory == QDir(task.directory).absolutePath()) {
              std::lock_guard<std::mutex> guard(mutex);
              accepting = desiredEnabled && !stopping;
              current.state = State::Ready;
            } else open(task.directory);
          } else { close(); state(State::Off); }
        } else if (task.kind == Kind::Delete) erase(task.directory);
        else write(task);
      } catch (...) {
        close();
        if (task.kind == Kind::Record) {
          std::lock_guard<std::mutex> guard(mutex);
          ++current.dropped;
        }
        state(State::Error, "A diagnostic operation could not be completed.");
      }
      // Flush before announcing idle, so both CI and explicit deletion see
      // completed writes. The disk remains exclusively owned by this thread.
      bool drained;
      {
        std::lock_guard<std::mutex> guard(mutex);
        drained = tasks.empty();
      }
      if (drained && logger) {
        try { logger->flush(); }
        catch (...) {
          close();
          state(State::Error, "Flushing diagnostic logs failed.");
        }
      }
      {
        std::lock_guard<std::mutex> guard(mutex);
        working = false;
        workingRecord = false;
        if (tasks.empty()) idle.notify_all();
      }
    }
    close();
    state(State::Stopped);
    idle.notify_all();
  }
};

Writer::Writer(const Limits &limits) : m_impl(new Impl(limits)) {}
Writer::~Writer() { shutdown(); }

void Writer::enable(const QString &directory) {
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  if (m_impl->stopping) return;
  m_impl->desiredEnabled = true;
  m_impl->desiredDirectory = directory;
  m_impl->accepting = true;
  m_impl->current.state = State::Starting;
  m_impl->queueReconcileLocked();
  m_impl->startLocked();
  m_impl->changed.notify_one();
}

void Writer::disable() {
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  if (m_impl->stopping) return;
  m_impl->desiredEnabled = false;
  m_impl->accepting = false;
  if (!m_impl->worker.joinable() || m_impl->workerExited) {
    m_impl->current.state = State::Off;
    m_impl->current.detail.clear();
    return;
  }
  m_impl->queueReconcileLocked();
  m_impl->changed.notify_one();
}

void Writer::removeSavedLogs(const QString &directory) {
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  if (m_impl->stopping) return;
  m_impl->accepting = false;
  if (!m_impl->deleteQueued) {
    m_impl->tasks.push_back({Impl::Kind::Delete, directory, {}});
    m_impl->deleteQueued = true;
  }
  m_impl->startLocked();
  m_impl->changed.notify_one();
}

void Writer::setMinimumSeverity(Severity severity) {
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  m_impl->minimum = severity;
}

void Writer::append(const Record &record) {
  {
    std::lock_guard<std::mutex> guard(m_impl->mutex);
    if (!m_impl->desiredEnabled || m_impl->stopping ||
        int(record.severity) < int(m_impl->minimum)) return;
  }
  // Bound conversion work before allocating a UTF-8 record. Embedded newlines
  // remain one logical event, with escaped controls preventing forged prefixes.
  const QString clipped = record.text.left(int(m_impl->limits.recordBytes));
  QString message = clipped;
  message.replace(QChar(0), QString("\\0"));
  message.replace('\r', QString("\\r"));
  message.replace('\n', QString("\\n"));
  const QString source = record.source.left(64).simplified();
  QByteArray data = QString("%1 [%2] [%3] %4")
      .arg(record.timestamp.toUTC().toString(Qt::ISODateWithMs),
           QString::fromLatin1(severityName(record.severity)), source, message)
      .toUtf8();
  const size_t maximum = m_impl->limits.recordBytes;
  if (size_t(data.size()) > maximum || clipped.size() != record.text.size()) {
    data.truncate(int(maximum - 16));
    // Avoid leaving a partial UTF-8 character at the truncation boundary.
    while (!data.isEmpty() && (uchar(data.back()) & 0xc0) == 0x80) data.chop(1);
    if (!data.isEmpty() && (uchar(data.back()) & 0x80)) data.chop(1);
    data += " [truncated]";
  }
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  if (!m_impl->desiredEnabled || m_impl->stopping ||
      int(record.severity) < int(m_impl->minimum)) return;
  if (!m_impl->accepting ||
      m_impl->current.queued >= m_impl->limits.queueRecords ||
      m_impl->current.queuedBytes + size_t(data.size()) > m_impl->limits.queueBytes) {
    ++m_impl->current.dropped;
    ++m_impl->pendingDropped;
    return;
  }
  ++m_impl->current.queued;
  m_impl->current.queuedBytes += size_t(data.size());
  m_impl->tasks.push_back({Impl::Kind::Record, {}, std::move(data), record.severity});
  m_impl->changed.notify_one();
}

Status Writer::status() const {
  std::lock_guard<std::mutex> guard(m_impl->mutex);
  Status result = m_impl->current;
  result.pendingChanges = m_impl->tasks.size() - result.queued;
  return result;
}

bool Writer::waitUntilIdle(int timeoutMilliseconds) {
  std::unique_lock<std::mutex> guard(m_impl->mutex);
  return m_impl->idle.wait_for(guard, std::chrono::milliseconds(timeoutMilliseconds),
      [this] { return m_impl->tasks.empty() && !m_impl->working; });
}

void Writer::shutdown() {
  {
    std::lock_guard<std::mutex> guard(m_impl->mutex);
    m_impl->desiredEnabled = false;
    m_impl->accepting = false;
    m_impl->stopping = true;
    m_impl->changed.notify_one();
  }
  if (m_impl->worker.joinable()) m_impl->worker.join();
}
}  // namespace NativeDiagnostic
