#pragma once

// Shared immutable FFmpeg generations. All readers register a lifetime lease
// under the same short-lived gate used to retire generations. A failed lock is
// treated as busy, never as permission to remove files.
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QRunnable>
#include <QSaveFile>
#include <QSet>
#include <QThreadPool>
#include <QUuid>
#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef MEDIA_CACHE_TEST_HOOKS
#include <functional>
#endif

namespace MediaCache {
#ifdef MEDIA_CACHE_TEST_HOOKS
// Deterministic interleaving control for the protocol executable only. These
// hooks are absent from the application and image library builds.
struct TestHooks {
  std::function<void()> afterInventory, onManagementGateContention,
      afterProtectedGeneration;
};
inline TestHooks &testHooks() { static TestHooks hooks; return hooks; }
#endif
inline bool generationName(const QString &name) {
  static const QRegularExpression pattern("^import-v2-[0-9a-f]{64}$");
  return pattern.match(name).hasMatch();
}

class Gate {
  QLockFile m_lock;
public:
  explicit Gate(const QString &root) : m_lock(QDir(root).filePath(".manage.lock")) {
    m_lock.setStaleLockTime(0);
#ifdef MEDIA_CACHE_TEST_HOOKS
    if (m_lock.tryLock(0)) return;
    if (testHooks().onManagementGateContention)
      testHooks().onManagementGateContention();
#endif
    if (!m_lock.tryLock(5000))
      throw std::runtime_error("Media cache is busy or not writable");
  }
};

class Lease {
  std::unique_ptr<QLockFile> m_lock;
public:
  explicit Lease(const QString &base) {
    const QFileInfo info(base);
    if (!generationName(info.fileName()) || !QDir().mkpath(info.absolutePath()))
      throw std::runtime_error("Invalid media cache generation");
    Gate gate(info.absolutePath());
    m_lock.reset(new QLockFile(base + ".lease-" +
                              QUuid::createUuid().toString(QUuid::WithoutBraces)));
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0))
      throw std::runtime_error("Cannot protect media cache reader");
    // Last access is advisory eviction order, not cache validity or lock expiry.
    QFile access(base + ".access");
    if (access.open(QIODevice::WriteOnly | QIODevice::Truncate))
      access.write(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8());
  }
};

struct Entry {
  QString name, directory;
  qint64 bytes = 0;
  QDateTime accessed;
  bool active = false, complete = true;
};
struct Report {
  qint64 bytes = 0, activeBytes = 0, removedBytes = 0;
  int generations = 0, activeGenerations = 0, removedGenerations = 0;
  bool canceled = false;
  QStringList errors;
};
inline QFileInfoList retirements(const QString &root) {
  QFileInfoList result;
  static const QRegularExpression pattern(
      "^\\.retired-import-v2-[0-9a-f]{64}-[0-9a-f-]{36}$");
  for (const auto &info : QDir(root).entryInfoList(
           {".retired-*"}, QDir::Dirs | QDir::Hidden | QDir::NoSymLinks))
    if (pattern.match(info.fileName()).hasMatch()) result.append(info);
  return result;
}
inline bool canceled(const std::atomic<bool> *cancel) {
  return cancel && cancel->load();
}
inline qint64 directoryBytes(const QString &path,
                             const std::atomic<bool> *cancel = nullptr) {
  qint64 total = 0;
  QDirIterator it(path, QDir::Files | QDir::NoSymLinks | QDir::Hidden,
                  QDirIterator::Subdirectories);
  while (it.hasNext() && !canceled(cancel)) {
    it.next();
    total += it.fileInfo().size();
  }
  return total;
}
// Caller holds Gate. A dead process's QLockFile is recoverable by Qt; live
// processes never expire by age. This also protects older extraction workers.
inline bool active(const QString &root, const QString &name) {
  QDir dir(root);
  QStringList locks = dir.entryList({name + ".lease-*"}, QDir::Files | QDir::Hidden);
  locks.append(name + ".lock");
  for (const QString &file : locks) {
    const QString path = dir.filePath(file);
    if (QFileInfo(path).isSymLink()) return true;
    QLockFile probe(path);
    probe.setStaleLockTime(0);
    if (!probe.tryLock(0)) return true;
  }
  return false;
}
inline std::vector<Entry> inventory(const QString &root,
                                     const std::atomic<bool> *cancel = nullptr) {
  std::vector<Entry> entries;
  const QDir dir(root);
  if (!dir.exists()) return entries;
  const auto folders = dir.entryInfoList({"import-v2-*.frames", "import-v2-*.partial-*"},
                                         QDir::Dirs | QDir::NoSymLinks);
  static const QRegularExpression partial(
      "^(import-v2-[0-9a-f]{64})\\.partial-[A-Za-z0-9]{6}$");
  for (const auto &folder : folders) {
    if (canceled(cancel)) break;
    const auto match = partial.match(folder.fileName());
    const bool complete = folder.fileName().endsWith(".frames");
    const QString name = complete ? folder.fileName().chopped(7) : match.captured(1);
    if (!generationName(name)) continue;
    Entry entry;
    entry.name = name;
    entry.directory = folder.fileName();
    entry.complete = complete;
    entry.bytes = directoryBytes(folder.absoluteFilePath(), cancel);
    if (complete) entry.bytes += QFileInfo(dir.filePath(name + ".txt")).size();
    entry.accessed = QFileInfo(dir.filePath(name + ".access")).lastModified();
    if (!entry.accessed.isValid()) entry.accessed = folder.lastModified();
    { Gate gate(root); entry.active = active(root, name); }
    entries.push_back(entry);
  }
  return entries;
}
inline Report usage(const QString &root, const std::atomic<bool> *cancel = nullptr) {
  Report report;
  try {
    for (const auto &entry : inventory(root, cancel)) {
      report.bytes += entry.bytes;
      ++report.generations;
      if (entry.active) { report.activeBytes += entry.bytes; ++report.activeGenerations; }
    }
    for (const auto &entry : retirements(root))
      report.bytes += directoryBytes(entry.absoluteFilePath(), cancel);
  } catch (const std::exception &error) {
    report.errors.append(QString::fromUtf8(error.what()));
  }
  report.canceled = canceled(cancel);
  return report;
}
inline qint64 budget(const QString &root) {
  QFile file(QDir(root).filePath(".policy.json"));
  if (!file.open(QIODevice::ReadOnly)) return 0;
  bool valid = false;
  const qint64 value = QJsonDocument::fromJson(file.readAll()).object()
      .value("budgetBytes").toString().toLongLong(&valid);
  return valid && value > 0 ? value : 0;  // Zero disables automatic cleanup.
}
inline bool setBudget(const QString &root, qint64 bytes, QString *error) {
  try {
    if (bytes < 0 || !QDir().mkpath(root))
      throw std::runtime_error("Cannot create media cache settings");
    Gate gate(root);
    QSaveFile file(QDir(root).filePath(".policy.json"));
    const QByteArray data = QJsonDocument(QJsonObject{
        {"version", 1}, {"budgetBytes", QString::number(bytes)}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() ||
        !file.commit()) throw std::runtime_error("Cannot save media cache budget");
    return true;
  } catch (const std::exception &failure) {
    if (error) *error = QString::fromUtf8(failure.what());
    return false;
  }
}
// Retire under the gate, then delete only the retired directory. A new reader
// can recreate the original generation while deletion proceeds. Export scratch
// files, source movies and unknown directories are untouched. Partial extraction
// folders use the same base reader/writer locks and become eligible after a crash.
inline Report trim(const QString &root, qint64 target,
                   const std::atomic<bool> *cancel = nullptr) {
  Report report;
  if (target < 0) { report.errors.append("Invalid media cache budget"); return report; }
  if (!QDir(root).exists()) return report;
  // Serialize cleaners, not readers. Inventories must be taken after acquiring
  // this lock so another cleaner cannot make our budget accounting stale.
  QLockFile cleanup(QDir(root).filePath(".cleanup.lock"));
  cleanup.setStaleLockTime(0);
  QElapsedTimer waiting;
  waiting.start();
  while (!cleanup.tryLock(50)) {
    if (canceled(cancel)) { report.canceled = true; return report; }
    if (waiting.elapsed() >= 5000) {
      report.errors.append("Another media cache cleanup is still running");
      return report;
    }
  }
  try {
    auto entries = inventory(root, cancel);
#ifdef MEDIA_CACHE_TEST_HOOKS
    if (testHooks().afterInventory) testHooks().afterInventory();
#endif
    for (const auto &entry : entries) report.bytes += entry.bytes;
    report.generations = int(entries.size());
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
      return a.accessed == b.accessed ? a.name < b.name : a.accessed < b.accessed;
    });
    const QDir dir(root);
    // Retry a retirement interrupted by a crash or failed deletion. Each
    // retired directory has an independent process lock; it is never a reader
    // path and cannot be republished by extraction.
    for (const auto &entry : retirements(root)) {
      if (canceled(cancel)) break;
      const qint64 bytes = directoryBytes(entry.absoluteFilePath(), cancel);
      report.bytes += bytes;
      QLockFile removal(entry.absoluteFilePath() + ".remove-lock");
      removal.setStaleLockTime(0);
      if (!removal.tryLock(0)) continue;
      if (!QDir(entry.absoluteFilePath()).removeRecursively()) {
        report.errors.append("Cannot remove a retired media cache generation");
        continue;
      }
      report.bytes -= bytes;
      report.removedBytes += bytes;
      ++report.removedGenerations;
    }
    for (const auto &entry : entries) {
      if (canceled(cancel)) break;
      QString retired;
      {
        Gate gate(root);
        // Waiting for another reader's registration can outlive a cancellation.
        // Do not start a new retirement after acquiring the management gate.
        if (canceled(cancel)) break;
        if (active(root, entry.name)) {
          report.activeBytes += entry.bytes;
          ++report.activeGenerations;
#ifdef MEDIA_CACHE_TEST_HOOKS
          if (testHooks().afterProtectedGeneration) testHooks().afterProtectedGeneration();
#endif
          continue;
        }
        if (report.bytes <= target) continue;
        const QString source = dir.filePath(entry.directory);
        if (!QFileInfo(source).isDir() || QFileInfo(source).isSymLink()) continue;
        if (canceled(cancel)) break;
        retired = dir.filePath(".retired-" + entry.name + "-" +
                               QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!QDir().rename(source, retired)) {
          report.errors.append("Cannot retire cached generation " + entry.name);
          continue;
        }
        if (entry.complete) {
          for (const QString &suffix : {QString(".txt"), QString(".access")}) {
            const QString path = dir.filePath(entry.name + suffix);
            if (QFileInfo::exists(path) && !QFile::remove(path))
              report.errors.append("Cannot remove cached metadata " + entry.name);
          }
        }
      }
      // Finish a retirement even if cancellation arrives during deletion. It
      // can no longer be used; no subsequent generation is retired on cancel.
      QLockFile removal(retired + ".remove-lock");
      removal.setStaleLockTime(0);
      if (!removal.tryLock(0) || !QDir(retired).removeRecursively()) {
        report.errors.append("Cannot remove retired cached generation " + entry.name);
        continue;
      }
      report.bytes -= entry.bytes;
      report.removedBytes += entry.bytes;
      ++report.removedGenerations;
    }
  } catch (const std::exception &error) {
    report.errors.append(QString::fromUtf8(error.what()));
  }
  report.canceled = canceled(cancel);
  if (!report.canceled) {
    const Report current = usage(root, cancel);
    report.bytes = current.bytes;
    report.activeBytes = current.activeBytes;
    report.generations = current.generations;
    report.activeGenerations = current.activeGenerations;
    report.errors.append(current.errors);
    report.canceled = current.canceled;
  }
  return report;
}
inline Report enforceBudget(const QString &root) {
  const qint64 bytes = budget(root);
  return bytes > 0 ? trim(root, bytes) : Report();
}
// Reader destruction happens on interactive UI paths too. Coalesce automatic
// cleanup per root and run it at background priority; the manager can perform
// an explicitly requested cleanup independently under the same retirement gate.
inline void scheduleBudget(const QString &root) {
  if (QCoreApplication::closingDown() || budget(root) <= 0) return;
  struct Pending {
    QMutex mutex;
    QSet<QString> roots, again;
  };
  static auto pending = std::make_shared<Pending>();
  const auto state = pending;
  {
    QMutexLocker guard(&state->mutex);
    if (state->roots.contains(root)) { state->again.insert(root); return; }
    state->roots.insert(root);
  }
  try {
    QThreadPool::globalInstance()->start(QRunnable::create([root, state]() {
      for (;;) {
        try {
          const auto report = enforceBudget(root);
          for (const auto &error : report.errors)
            qWarning("Media cache automatic cleanup: %s", qPrintable(error));
        } catch (...) {
          qWarning("Media cache automatic cleanup failed");
        }
        QMutexLocker guard(&state->mutex);
        // A close that arrived during inventory may have released a lease we
        // just observed. Consume that request in a fresh pass before exiting.
        if (state->again.remove(root)) continue;
        state->roots.remove(root);
        break;
      }
    }), -1);
  } catch (...) {
    QMutexLocker guard(&state->mutex);
    state->roots.remove(root);
    state->again.remove(root);
    throw;
  }
}
}  // namespace MediaCache
