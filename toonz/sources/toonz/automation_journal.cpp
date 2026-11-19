#include "automation_journal.h"

#include "automation.h"
#include "automation_files.h"
#include "automation_ownership.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <qt_windows.h>

#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#ifdef __linux__
#include <sys/statfs.h>
#endif
#endif

namespace GroundzeroAutomation {
namespace {

constexpr int FormatVersion       = 1;
constexpr int RecordVersion       = 1;
constexpr int MaximumTreeFiles    = 8192;
constexpr qint64 MaximumTreeBytes = 256 * 1024 * 1024;
constexpr int MaximumReportRows   = 16;
constexpr int MaximumWriterRows   = 64;
const char* const JournalPath     = ".groundzero/journal";

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

QByteArray sha256Hex(const QByteArray& bytes) {
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QByteArray compact(const QJsonObject& object) {
  return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

qint64 integer(const QJsonObject& record, const char* key) {
  return qint64(record.value(QLatin1String(key)).toDouble());
}

QJsonValue optional(const QString& text) {
  return text.isEmpty() ? QJsonValue() : QJsonValue(text);
}

QString segmentName(qint64 firstSeq) {
  return QString("seg-%1.jsonl").arg(firstSeq, 20, 10, QChar('0'));
}

qint64 segmentFirstSeq(const QString& name) {
  return name.mid(4, 20).toLongLong();
}

bool fullyWritten(int descriptor, const QByteArray& bytes) {
  qint64 written = 0;
  while (written < bytes.size()) {
#ifdef _WIN32
    const int count = _write(descriptor, bytes.constData() + written,
                             unsigned(bytes.size() - written));
#else
    const ssize_t count = ::write(descriptor, bytes.constData() + written,
                                  size_t(bytes.size() - written));
    if (count < 0 && errno == EINTR) continue;
#endif
    if (count <= 0) return false;
    written += count;
  }
  return true;
}

// Data to the device and, on macOS, out of the drive's cache.
bool flushDescriptor(int descriptor) {
#ifdef _WIN32
  return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)));
#elif defined(__APPLE__)
  return fcntl(descriptor, F_FULLFSYNC) == 0 || fsync(descriptor) == 0;
#else
  return fdatasync(descriptor) == 0;
#endif
}

int openFile(const QString& path, int flags) {
#ifdef _WIN32
  return _wopen(reinterpret_cast<const wchar_t*>(path.utf16()),
                flags | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
#else
  return ::open(QFile::encodeName(path).constData(),
                flags | O_CLOEXEC | O_NOFOLLOW, 0600);
#endif
}

void closeFile(int descriptor) {
#ifdef _WIN32
  _close(descriptor);
#else
  ::close(descriptor);
#endif
}

// Renames over an existing file in one step, so a reader finds the old or the
// new content and never neither.
bool replaceFile(const QString& from, const QString& to) {
#ifdef _WIN32
  return MoveFileExW(reinterpret_cast<LPCWSTR>(from.utf16()),
                     reinterpret_cast<LPCWSTR>(to.utf16()),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
  return ::rename(QFile::encodeName(from).constData(),
                  QFile::encodeName(to).constData()) == 0;
#endif
}

void truncateFile(int descriptor, qint64 size) {
#ifdef _WIN32
  _chsize_s(descriptor, size);
#else
  if (::ftruncate(descriptor, off_t(size)) != 0) return;
#endif
}

bool lockNow(int descriptor) {
#ifdef _WIN32
  OVERLAPPED overlapped{};
  return LockFileEx(reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)),
                    LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1,
                    0, &overlapped);
#else
  return ::flock(descriptor, LOCK_EX | LOCK_NB) == 0;
#endif
}

void unlock(int descriptor) {
#ifdef _WIN32
  OVERLAPPED overlapped{};
  UnlockFileEx(reinterpret_cast<HANDLE>(_get_osfhandle(descriptor)), 0, 1, 0,
               &overlapped);
#else
  ::flock(descriptor, LOCK_UN);
#endif
}

// Why a volume cannot hold the journal, or empty. The journal needs advisory
// locks that the kernel of this machine releases when their holder dies, so a
// network file system is refused; every local one qualifies (probe P-DR-2:
// on macOS an flock is exclusive and released within a millisecond of
// SIGKILL on APFS, HFS+, exFAT and FAT alike).
QString volumeProblem(const QString& path) {
#ifdef _WIN32
  const QString root =
      QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath().left(3));
  const UINT type = GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
  if (type == DRIVE_REMOTE || type == DRIVE_UNKNOWN ||
      type == DRIVE_NO_ROOT_DIR)
    return "unsupported_volume";
  return {};
#else
  struct statfs info{};
  if (::statfs(QFile::encodeName(path).constData(), &info) != 0)
    return "io_error";
#ifdef __APPLE__
  if (!(info.f_flags & MNT_LOCAL)) return "unsupported_volume";
#else
  // NFS, SMB, CIFS, SMB2, Coda, AFS, Ceph, FUSE and 9P.
  static const std::vector<unsigned long> network{
      0x6969UL,     0x517BUL,     0xFF534D42UL, 0xFE534D42UL, 0x73757245UL,
      0x5346414FUL, 0x00C36400UL, 0x65735546UL, 0x01021997UL};
  if (std::find(network.begin(), network.end(), (unsigned long)info.f_type) !=
      network.end())
    return "unsupported_volume";
#endif
  return {};
#endif
}

QString contentDigest(const QString& path) {
  const QFileInfo root(path);
  if (root.isSymLink()) return {};
  const auto fileDigest = [](const QString& file) -> QByteArray {
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&input)) return {};
    return hash.result().toHex();
  };
  if (root.isFile()) return QString::fromLatin1(fileDigest(path));
  if (!root.isDir()) return {};
  QStringList lines;
  qint64 bytes = 0;
  QDirIterator entries(path, QDir::Files | QDir::Hidden | QDir::System,
                       QDirIterator::Subdirectories);
  while (entries.hasNext()) {
    const QFileInfo entry(entries.next());
    bytes += entry.size();
    if (entry.isSymLink() || lines.size() >= MaximumTreeFiles ||
        bytes > MaximumTreeBytes)
      return {};
    const QByteArray digest = fileDigest(entry.filePath());
    if (digest.isEmpty()) return {};
    lines.append(QDir(path).relativeFilePath(entry.filePath()) + '\0' +
                 QString::number(entry.size()) + '\0' +
                 QString::fromLatin1(digest));
  }
  lines.sort();
  return QString::fromLatin1(sha256Hex(lines.join('\n').toUtf8()));
}

bool validRuntimeName(const QString& name) {
  static const QRegularExpression pattern(
      "^runtime_[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
  return pattern.match(name).hasMatch();
}

bool faultFires(const Context* context, const char* point) {
  return context && writeFaultFires(*context, point);
}

bool faultArmed(const Context* context, const char* point) {
  return context && context->writeFaultArmed(point);
}

[[noreturn]] void unavailable(const QString& reason) {
  throw Error("journal_unavailable",
              "The workspace journal cannot be written, so no write is "
              "admitted; inspect journal.status.",
              {{"reason", reason}});
}

}  // namespace

JournalLimits JournalLimits::small() {
  JournalLimits limits;
  limits.segmentBytes = 64 * 1024;
  limits.fullReceipts = 16;
  limits.receiptStubs = 64;
  limits.packages     = 4;
  limits.jobs         = 8;
  return limits;
}

QJsonObject JournalLimits::report() const {
  return {{"segment_bytes", double(segmentBytes)},
          {"record_line_bytes", lineBytes},
          {"blob_bytes", double(blobBytes)},
          {"journal_bytes", double(journalBytes)},
          {"full_receipts", fullReceipts},
          {"receipt_stubs", receiptStubs},
          {"packages", packages},
          {"package_manifest_bytes", double(packageBytes)},
          {"jobs", jobs},
          {"lock_wait_ms", lockWaitMs},
          {"outcome_lock_wait_ms", outcomeLockWaitMs}};
}

struct Journal::Segment final {
  QString name;
  qint64 firstSeq = 0;
  // Bytes read so far and the highest sequence applied from this segment.
  qint64 offset  = 0;
  qint64 lastSeq = 0;
  qint64 size    = 0;
};

// The append lock, held for one append or for the whole recovery; an append
// inside the recovery uses the lock the recovery holds.
struct Journal::Lock final {
  Lock(Journal& journal, int waitMs, bool refuse) : m_journal(journal) {
    if (journal.m_locked) {
      m_held = true;
      return;
    }
    if (refuse || journal.m_lockDescriptor < 0) return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    for (int pause = 1;; pause = std::min(pause * 2, 20)) {
      if (lockNow(journal.m_lockDescriptor)) {
        m_held = m_owner = journal.m_locked = true;
        return;
      }
      if (std::chrono::steady_clock::now() >= deadline) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(pause));
    }
  }
  ~Lock() {
    if (!m_owner) return;
    m_journal.m_locked = false;
    unlock(m_journal.m_lockDescriptor);
  }
  Lock(const Lock&)            = delete;
  Lock& operator=(const Lock&) = delete;
  bool held() const { return m_held; }

private:
  Journal& m_journal;
  bool m_held  = false;
  bool m_owner = false;
};

Journal::Journal(Start start) : m_start(std::move(start)) {
  m_root = QDir(m_start.workspace).filePath(JournalPath);
}

Journal::~Journal() {
  if (m_lockDescriptor >= 0) closeFile(m_lockDescriptor);
}

QString Journal::path(const QString& name) const {
  return QDir(m_root).filePath(name);
}

void Journal::fail(const QString& reason) {
  if (m_reason.isEmpty()) m_reason = reason;
}

QString Journal::state() const {
  if (!available()) return "unavailable";
  return m_damage.isEmpty() && m_runtimeBad == 0 ? "ok" : "damaged";
}

namespace {

// Creates the FORMAT file once, or reads it; returns the failure reason.
QString checkFormat(const QString& path) {
  const QByteArray format =
      compact({{"format", "groundzero-journal"}, {"version", FormatVersion}});
  const int created = openFile(path, O_WRONLY | O_CREAT | O_EXCL);
  if (created >= 0) {
    const bool written =
        fullyWritten(created, format + '\n') && flushDescriptor(created);
    closeFile(created);
    if (!written) return "io_error";
  }
  // Another worker starting in the same workspace may have created the file
  // a moment ago and not written its line yet: wait for the line, briefly.
  QJsonObject read;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (;;) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return "io_error";
    const QByteArray line = file.read(4096);
    if (line.endsWith('\n')) {
      read = QJsonDocument::fromJson(line.trimmed()).object();
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) return "io_error";
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (read.value("format") != "groundzero-journal") return "io_error";
  return read.value("version").toInt() > FormatVersion ? "newer_format"
                                                       : QString();
}

}  // namespace

std::shared_ptr<Journal> Journal::open(const Start& start) {
  std::shared_ptr<Journal> journal(new Journal(start));
  journal->recover();
  return journal;
}

void Journal::listSegments() {
  const QStringList names =
      QDir(m_root).entryList({"seg-*.jsonl"}, QDir::Files, QDir::Name);
  for (const QString& name : names) {
    if (name.size() != 30) continue;
    const auto known =
        std::find_if(m_segments.begin(), m_segments.end(),
                     [&name](const std::unique_ptr<Segment>& segment) {
                       return segment->name == name;
                     });
    if (known != m_segments.end()) continue;
    auto segment      = std::make_unique<Segment>();
    segment->name     = name;
    segment->firstSeq = segmentFirstSeq(name);
    m_segments.push_back(std::move(segment));
  }
  std::sort(
      m_segments.begin(), m_segments.end(),
      [](const std::unique_ptr<Segment>& a, const std::unique_ptr<Segment>& b) {
        return a->firstSeq < b->firstSeq;
      });
}

void Journal::recover() {
  if (!QDir().mkpath(path("blobs")) || !QDir().mkpath(path("quarantine")))
    return fail("io_error");
  const QString problem = faultFires(m_start.faults, "journal:volume")
                              ? QString("unsupported_volume")
                              : volumeProblem(m_root);
  if (!problem.isEmpty()) return fail(problem);
  const QString format = checkFormat(path("FORMAT"));
  m_lockDescriptor     = openFile(path("lock"), O_RDWR | O_CREAT);
  if (m_lockDescriptor < 0) return fail("lock_unavailable");
  const Lock lock(*this, m_start.limits.lockWaitMs, false);
  if (!lock.held()) return fail("lock_unavailable");
  m_recovering = true;
  listSegments();
  qint64 truncated = 0;
  for (std::size_t index = 0; index < m_segments.size(); ++index) {
    Segment& segment = *m_segments[index];
    if (index + 1 == m_segments.size() && format.isEmpty())
      truncated = repairTail(segment, true);
    scanSegment(segment, format.isEmpty());
  }
  // A newer format is still read, but nothing is ever written to it.
  if (!format.isEmpty()) {
    m_recovering = false;
    return fail(format);
  }
  QList<QJsonObject> damage;
  for (const QJsonValue& row : m_quarantined) {
    QJsonObject record = row.toObject();
    record.insert("type", "damage");
    damage.append(record);
  }
  if (!damage.isEmpty()) append(damage, 0, true);
  m_lastRecovery = {
      {"by_worker", m_start.instance},
      {"truncated_tail_bytes", double(std::max<qint64>(0, truncated))},
      {"quarantined_segments", m_quarantined}};
  reconcileDead();
  compactSegments();
  append({QJsonObject{{"type", "worker_start"},
                      {"runtime", m_start.runtimeName},
                      {"executable_sha256", optional(m_start.executableSha256)},
                      {"authority_digest", m_start.authorityDigest}}},
         0, true);
  m_lastRecovery.insert("at_seq", double(m_lastSeq));
  m_recovering = false;
}

qint64 Journal::repairTail(Segment& segment, bool verifyLast) {
  QFile file(path(segment.name));
  if (!file.open(QIODevice::ReadWrite)) return -1;
  const qint64 size = file.size();
  if (size == 0) return 0;
  const qint64 from = std::max<qint64>(0, size - 2 * m_start.limits.lineBytes);
  if (!file.seek(from)) return -1;
  const QByteArray bytes = file.readAll();
  const bool complete    = bytes.endsWith('\n');
  if (complete && !verifyLast) return 0;
  const int end         = complete ? bytes.size() - 1 : bytes.size();
  const int start       = bytes.lastIndexOf('\n', end - 1) + 1;
  const QByteArray last = bytes.mid(start, end - start);
  if (complete && last.size() >= 66 && last.at(64) == ' ' &&
      sha256Hex(last.mid(65)) == last.left(64))
    return 0;
  // Bytes before the window would mean a line longer than any record.
  if (start == 0 && from > 0) return -1;
  QFile tail(
      path("quarantine/tail-" + QString::number(nowMs()) + "-" + segment.name));
  if (!tail.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      tail.write(bytes.mid(start)) != bytes.size() - start)
    return -1;
  tail.close();
  if (!file.resize(from + start)) return -1;
  file.flush();
  flushDescriptor(file.handle());
  if (segment.offset > from + start) segment.offset = from + start;
  segment.size = from + start;
  return bytes.size() - start;
}

void Journal::scanSegment(Segment& segment, bool recovery) {
  QFile file(path(segment.name));
  if (!file.open(QIODevice::ReadOnly)) return;
  if (file.size() < segment.offset) segment.offset = 0;
  if (!file.seek(segment.offset)) return;
  const QByteArray bytes = file.readAll();
  std::vector<QByteArray> good;
  qint64 first = 0;
  qint64 last  = 0;
  bool bad     = false;
  int position = 0;
  for (int end; (end = bytes.indexOf('\n', position)) >= 0;
       position = end + 1) {
    const QByteArray text = bytes.mid(position, end - position);
    const QByteArray json = text.mid(65);
    if (text.size() < 66 || text.at(64) != ' ' ||
        sha256Hex(json) != text.left(64)) {
      bad = true;
      continue;
    }
    good.push_back(text);
    const QJsonObject record = QJsonDocument::fromJson(json).object();
    const qint64 seq         = integer(record, "seq");
    if (seq > 0 && first == 0) first = seq;
    last = std::max(last, seq);
    applyRecord(record, segment);
  }
  segment.offset += position;
  segment.size = segment.offset;
  if (!bad) return;
  if (!recovery) {
    ++m_runtimeBad;
    return;
  }
  quarantineSegment(segment, good, first ? first : segment.firstSeq,
                    last ? last : segment.firstSeq);
}

void Journal::applyRecord(const QJsonObject& record, Segment& segment) {
  const qint64 seq = integer(record, "seq");
  if (record.value("type") == "segment") return;
  if (seq <= segment.lastSeq) return;
  segment.lastSeq = seq;
  m_lastSeq       = std::max(m_lastSeq, seq);
  if (record.value("v").toInt() > RecordVersion) {
    fail("newer_format");
    return;
  }
  apply(record);
}

void Journal::quarantineSegment(Segment& segment,
                                const std::vector<QByteArray>& good,
                                qint64 firstSeq, qint64 lastSeq) {
  const QString source = path(segment.name);
  const QString copy =
      path("quarantine/" + QString::number(nowMs()) + "-" + segment.name);
  const QString rewritten = source + ".rewrite";
  QFile output(rewritten);
  if (!QFile::copy(source, copy) ||
      !output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    return fail("io_error");
  for (const QByteArray& text : good) output.write(text + '\n');
  output.flush();
  flushDescriptor(output.handle());
  output.close();
  if (!replaceFile(rewritten, source)) return fail("io_error");
  flushDirectory(m_root);
  segment.offset = QFileInfo(source).size();
  segment.size   = segment.offset;
  const QJsonObject damage{{"segment", segment.name},
                           {"first_seq", double(firstSeq)},
                           {"last_seq", double(lastSeq)},
                           {"bytes", double(QFileInfo(copy).size())}};
  m_damage.append(damage);
  m_quarantined.append(damage);
}

void Journal::apply(const QJsonObject& record) {
  const QString type = record.value("type").toString();
  const QString id   = record.value("operation_id").toString();
  const QString w    = record.value("w").toString();
  if (type == "worker_start") {
    m_writers.insert(w, record);
    m_writerOrder.removeAll(w);
    m_writerOrder.append(w);
  } else if (type == "admit" || type == "stub") {
    JournalOperation operation;
    operation.operationId     = id;
    operation.method          = record.value("method").toString();
    operation.argumentsSha256 = record.value("arguments_sha256").toString();
    operation.authorityDigest = record.value("authority_digest").toString();
    operation.documentId      = record.value("document_id").toString();
    operation.effectClass     = record.value("effect_class").toString();
    operation.writer          = w;
    operation.admittedSeq     = integer(record, "seq");
    operation.admittedAtMs    = integer(record, "t");
    if (type == "stub") operation.stubState = record.value("state").toString();
    m_operations.insert(id, operation);
  } else if (type == "release" || type == "intent" || type == "outcome" ||
             type == "reconcile") {
    const auto found = m_operations.find(id);
    if (found == m_operations.end()) return;
    if (type == "release" && found->writer == w && found->outcome.isEmpty())
      m_operations.erase(found);
    else if (type == "intent" && found->writer == w)
      found->intent = record;
    else if (type == "outcome" && found->writer == w)
      found->outcome = record;
    else if (type == "reconcile" && found->reconcile.isEmpty())
      found->reconcile = record;
  } else if (type == "job") {
    m_jobs.insert(record.value("job_id").toString(), record);
  } else if (type == "damage") {
    bool known = false;
    for (const QJsonValue& row : m_damage)
      known =
          known || row.toObject().value("segment") == record.value("segment");
    if (!known)
      m_damage.append(QJsonObject{{"segment", record.value("segment")},
                                  {"first_seq", record.value("first_seq")},
                                  {"last_seq", record.value("last_seq")},
                                  {"bytes", record.value("bytes")}});
  }
}

void Journal::refresh() {
  if (m_lockDescriptor < 0) return;
  listSegments();
  for (const auto& segment : m_segments) {
    if (QFileInfo(path(segment->name)).size() == segment->offset) continue;
    scanSegment(*segment, false);
  }
}

QByteArray Journal::line(QJsonObject& record, qint64 seq) const {
  record.insert("v", RecordVersion);
  record.insert("seq", double(seq));
  record.insert("t", double(nowMs()));
  record.insert("w", m_start.instance);
  const QByteArray json = compact(record);
  return sha256Hex(json) + ' ' + json + '\n';
}

Journal::Segment& Journal::writableSegment(qint64 nextSeq, qint64 bytes) {
  if (!m_segments.empty() &&
      m_segments.back()->size + bytes <= m_start.limits.segmentBytes)
    return *m_segments.back();
  auto segment      = std::make_unique<Segment>();
  segment->name     = segmentName(nextSeq);
  segment->firstSeq = nextSeq;
  m_segments.push_back(std::move(segment));
  return *m_segments.back();
}

bool Journal::append(QList<QJsonObject> records, int waitMs, bool flush,
                     const std::function<bool()>& check) {
  if (!available()) return false;
  const bool test = !m_recovering;
  const Lock lock(*this, waitMs,
                  test && faultFires(m_start.faults, "journal:lock"));
  if (!lock.held()) {
    if (waitMs != m_start.limits.lockWaitMs) fail("lock_unavailable");
    return false;
  }
  // A worker killed mid-append leaves a torn line; it is cut off before
  // anything is appended after it.
  listSegments();
  if (!m_recovering && !m_segments.empty() &&
      repairTail(*m_segments.back(), false) < 0) {
    fail("io_error");
    return false;
  }
  refresh();
  if (check && !check()) return true;
  QByteArray bytes;
  for (int index = 0; index < records.size(); ++index)
    bytes += line(records[index], m_lastSeq + 1 + index);
  // A record over the line limit is refused alone; the journal stays usable.
  if (bytes.size() > qint64(m_start.limits.lineBytes) * records.size())
    return false;
  Segment& segment = writableSegment(m_lastSeq + 1, bytes.size());
  const bool fresh = segment.size == 0;
  if (fresh) {
    const QByteArray header =
        compact({{"type", "segment"},
                 {"generation", 1},
                 {"first_seq", double(segment.firstSeq)}});
    bytes.prepend(sha256Hex(header) + ' ' + header + '\n');
  }
  const int descriptor =
      openFile(path(segment.name), O_WRONLY | O_CREAT | O_APPEND);
  if (descriptor < 0) {
    fail("io_error");
    return false;
  }
  if (test && faultArmed(m_start.faults, "journal:torn")) {
    fullyWritten(descriptor, bytes.left(bytes.size() - 20));
    std::_Exit(WriteFaultCrashExitCode);
  }
  const bool written = fullyWritten(descriptor, bytes);
  // A failed flush may lose what was written; the test fault, on an outcome,
  // loses it all.
  const bool lost = test && records.front().value("type") == "outcome" &&
                    faultFires(m_start.faults, "journal:flush");
  if (lost) truncateFile(descriptor, segment.size);
  const bool flushed =
      written && (!flush || (!lost && flushDescriptor(descriptor)));
  closeFile(descriptor);
  if (fresh && flushed) flushDirectory(m_root);
  if (!written || !flushed) {
    fail(written ? "flush_failed" : "io_error");
    return false;
  }
  scanSegment(segment, false);
  return true;
}

bool Journal::admit(const QJsonObject& fields) {
  if (!available()) unavailable(m_reason);
  const QString id   = fields.value("operation_id").toString();
  QJsonObject record = fields;
  record.insert("type", "admit");
  bool admitted = false;
  const bool appended =
      append({record}, m_start.limits.lockWaitMs, true, [this, &id, &admitted] {
        admitted = !m_operations.contains(id);
        return admitted;
      });
  if (appended) return admitted;
  if (!available()) unavailable(m_reason);
  throw Error("journal_busy",
              "Another worker held the workspace journal for 5 s; nothing "
              "was admitted. Retry with the same operation_id.",
              {{"wait_ms", m_start.limits.lockWaitMs}});
}

void Journal::release(const QString& operationId) {
  append({QJsonObject{{"type", "release"}, {"operation_id", operationId}}},
         m_start.limits.outcomeLockWaitMs, false);
}

void Journal::intent(const QJsonObject& fields) {
  QJsonObject record = fields;
  record.insert("type", "intent");
  if (!append({record}, m_start.limits.outcomeLockWaitMs, true))
    unavailable(m_reason.isEmpty() ? "lock_unavailable" : m_reason);
}

bool Journal::outcome(const QString& operationId, const QJsonObject& response,
                      const QJsonObject& summary) {
  const QByteArray bytes = compact(response);
  QJsonObject record     = summary;
  record.insert("type", "outcome");
  record.insert("operation_id", operationId);
  record.insert("state", response.contains("error") ? "failed" : "completed");
  record.insert("bytes", double(bytes.size()));
  if (bytes.size() <= m_start.limits.inlineResponseBytes) {
    record.insert("response", response);
  } else {
    const QString blob = storeBlob(bytes);
    if (blob.isEmpty()) {
      fail("io_error");
      return false;
    }
    record.insert("blob", blob);
  }
  return append({record}, m_start.limits.outcomeLockWaitMs, true);
}

QString Journal::storeBlob(const QByteArray& bytes) {
  if (bytes.size() > m_start.limits.blobBytes) return {};
  const QString digest = QString::fromLatin1(sha256Hex(bytes));
  const QString target = path("blobs/" + digest);
  if (readBlob(digest, bytes.size()) == bytes) return digest;
  const QString staged =
      path("blobs/." + QUuid::createUuid().toString(QUuid::WithoutBraces));
  const int descriptor = openFile(staged, O_WRONLY | O_CREAT | O_EXCL);
  if (descriptor < 0) return {};
  const bool written =
      fullyWritten(descriptor, bytes) && flushDescriptor(descriptor);
  closeFile(descriptor);
  // Blobs are named by their content, so replacing one replaces equal bytes.
  if (!written || !replaceFile(staged, target)) {
    QFile::remove(staged);
    return {};
  }
  flushDirectory(path("blobs"));
  return digest;
}

QByteArray Journal::readBlob(const QString& digest, qint64 bytes) const {
  QFile file(path("blobs/" + digest));
  if (QFileInfo(file.fileName()).size() != bytes ||
      !file.open(QIODevice::ReadOnly))
    return {};
  const QByteArray content = file.readAll();
  return QString::fromLatin1(sha256Hex(content)) == digest ? content
                                                           : QByteArray();
}

QJsonObject Journal::response(const JournalOperation& operation) const {
  const QJsonObject& record = operation.outcome;
  if (record.contains("response")) return record.value("response").toObject();
  const QByteArray bytes =
      readBlob(record.value("blob").toString(), integer(record, "bytes"));
  if (bytes.isEmpty())
    throw Error("journal_unavailable",
                "The recorded result of this operation is missing or "
                "altered in the workspace journal.",
                {{"reason", "io_error"}});
  return QJsonDocument::fromJson(bytes).object();
}

const JournalOperation* Journal::find(const QString& operationId) const {
  const auto found = m_operations.constFind(operationId);
  return found == m_operations.constEnd() ? nullptr : &found.value();
}

bool Journal::writerAlive(const QString& instance) const {
  if (instance == m_start.instance) return true;
  const auto found = m_writers.constFind(instance);
  if (found == m_writers.constEnd()) return false;
  const QString runtime = found->value("runtime").toString();
  if (!validRuntimeName(runtime)) return false;
  const QString directory = QDir(m_start.workspace).filePath(runtime);
  return QFileInfo(directory).isDir() && !OwnershipClaim::abandoned(directory);
}

QString Journal::writerWithPrefix(const QString& prefix) const {
  for (const QString& writer : m_writerOrder)
    if (writer.startsWith(prefix)) return writer;
  return {};
}

JournalVerdict Journal::verdict(const JournalOperation& operation) const {
  if (!operation.reconcile.isEmpty()) {
    QJsonObject verdict = operation.reconcile.value("verdict").toObject();
    verdict.insert("reconciled", true);
    return verdict;
  }
  QJsonObject evidence{{"target", QJsonValue()},
                       {"expected_sha256", QJsonValue()},
                       {"found", "not_applicable"},
                       {"job_id", QJsonValue()},
                       {"outputs_present", QJsonValue()}};
  QString kind = "lost_with_worker";
  if (operation.effectClass == "job") {
    kind = "job_unknown";
  } else if (operation.effectClass == "publication") {
    kind = "not_published";
    if (!operation.intent.isEmpty()) {
      const QString target = operation.intent.value("target").toString();
      const QString expected =
          operation.intent.value("expected_sha256").toString();
      const QString absolute = QDir(m_start.workspace).filePath(target);
      evidence.insert("target", target);
      evidence.insert("expected_sha256", expected);
      const QFileInfo info(absolute);
      const bool present = info.exists() || info.isSymLink();
      const bool matches =
          present && contentDigest(absolute) ==
                         operation.intent.value("content_sha256").toString();
      evidence.insert("found", !present  ? "absent"
                               : matches ? "matches"
                                         : "differs");
      if (present) kind = matches ? "published" : "published_changed";
    }
  }
  return {{"kind", kind}, {"reconciled", false}, {"evidence", evidence}};
}

JournalVerdict Journal::reconcile(const JournalOperation& operation) {
  JournalVerdict computed = verdict(operation);
  if (computed.value("reconciled").toBool() || !available()) return computed;
  const QString id   = operation.operationId;
  QJsonObject stored = computed;
  stored.remove("reconciled");
  append({QJsonObject{{"type", "reconcile"},
                      {"operation_id", id},
                      {"writer", operation.writer},
                      {"verdict", stored}}},
         m_start.limits.outcomeLockWaitMs, true, [this, &id] {
           const JournalOperation* current = find(id);
           return current && current->outcome.isEmpty() &&
                  current->reconcile.isEmpty();
         });
  const JournalOperation* current = find(id);
  return current && !current->reconcile.isEmpty() ? verdict(*current)
                                                  : computed;
}

void Journal::reconcileDead() {
  std::vector<JournalOperation> interrupted;
  for (const JournalOperation& operation : m_operations)
    if (operation.outcome.isEmpty() && operation.reconcile.isEmpty() &&
        operation.stubState.isEmpty() && !writerAlive(operation.writer))
      interrupted.push_back(operation);
  for (const JournalOperation& operation : interrupted) reconcile(operation);
  const int operations = int(interrupted.size());
  std::vector<QJsonObject> lost;
  for (const QJsonObject& record : m_jobs)
    if (!record.value("record").toObject().value("terminal").toBool() &&
        !writerAlive(record.value("w").toString()))
      lost.push_back(record);
  for (const QJsonObject& record : lost) recordJob(jobLoss(record));
  const int jobs = int(lost.size());
  m_lastRecovery.insert("reconciled_operations", operations);
  m_lastRecovery.insert("reconciled_jobs", jobs);
  m_lastRecovery.insert("registered_packages", 0);
}

QJsonObject Journal::jobLoss(const QJsonObject& record) const {
  QJsonObject job = record.value("record").toObject();
  const QString directory =
      job.value("outputs").toObject().value("directory").toString();
  const bool present =
      !directory.isEmpty() &&
      QFileInfo(QDir(m_start.workspace).filePath(directory)).isDir();
  job.insert("status", "unknown");
  job.insert("terminal", true);
  job.insert("worker_loss",
             QJsonObject{{"cause", "worker_exit_unobserved"},
                         {"outputs_present", present},
                         {"worker_instance_id", record.value("w")}});
  return {{"job_id", record.value("job_id")},
          {"operation_id", record.value("operation_id")},
          {"writer", record.value("w")},
          {"record", job}};
}

void Journal::recordJob(const QJsonObject& fields) {
  QJsonObject record = fields;
  record.insert("type", "job");
  const QByteArray json = compact(record);
  if (json.size() > m_start.limits.lineBytes - 200) {
    QJsonObject job = record.value("record").toObject();
    job.remove("inputs");
    job.remove("effective");
    record.insert("record", job);
  }
  append({record}, m_start.limits.outcomeLockWaitMs, true);
}

QJsonObject Journal::jobView(const QJsonObject& stored) const {
  QJsonObject view = stored.value("record").toObject();
  if (!view.value("terminal").toBool() &&
      !writerAlive(stored.value("w").toString()))
    view = jobLoss(stored).value("record").toObject();
  view.insert("observed_by", "journal");
  return view;
}

const QJsonObject* Journal::job(const QString& jobId) const {
  const auto found = m_jobs.constFind(jobId);
  return found == m_jobs.constEnd() ? nullptr : &found.value();
}

QStringList Journal::jobIds() const {
  QStringList ids = m_jobs.keys();
  std::sort(ids.begin(), ids.end(), [this](const QString& a, const QString& b) {
    return integer(m_jobs.value(a), "seq") > integer(m_jobs.value(b), "seq");
  });
  return ids;
}

std::vector<const JournalOperation*> Journal::operationsNewestFirst() const {
  std::vector<const JournalOperation*> result;
  for (const JournalOperation& operation : m_operations)
    result.push_back(&operation);
  std::sort(result.begin(), result.end(),
            [](const JournalOperation* a, const JournalOperation* b) {
              return a->admittedSeq > b->admittedSeq;
            });
  return result;
}

qint64 Journal::horizonSeq() const {
  qint64 oldest = 0;
  for (const JournalOperation& operation : m_operations)
    if (oldest == 0 || operation.admittedSeq < oldest)
      oldest = operation.admittedSeq;
  if (oldest > 0) return oldest;
  return m_segments.empty() ? 0
                            : std::max<qint64>(1, m_segments.front()->firstSeq);
}

namespace {

QByteArray hashedLine(const QJsonObject& record) {
  const QByteArray json = compact(record);
  return sha256Hex(json) + ' ' + json + '\n';
}

// The stub an old, settled operation is compacted to: enough to refuse a
// retry with receipt_expired, nothing of its result.
QJsonObject stubRecord(const QJsonObject& admit,
                       const JournalOperation& operation) {
  QString state = operation.stubState;
  if (state.isEmpty())
    state = operation.outcome.isEmpty()
                ? QString("interrupted")
                : operation.outcome.value("state").toString();
  QJsonObject stub;
  for (const char* key :
       {"v", "seq", "t", "w", "operation_id", "arguments_sha256",
        "authority_digest", "method", "document_id", "effect_class"})
    stub.insert(key, admit.value(QLatin1String(key)));
  stub.insert("type", "stub");
  stub.insert("state", state);
  return stub;
}

}  // namespace

// Operations beyond the newest fullReceipts that are settled become stubs;
// stubs beyond the newest receiptStubs are forgotten. One sealed segment at a
// time is rewritten to a temporary file, flushed and renamed over itself.
void Journal::compactSegments() {
  std::vector<const JournalOperation*> operations = operationsNewestFirst();
  QHash<QString, QString> fate;
  int full  = 0;
  int stubs = 0;
  for (const JournalOperation* operation : operations) {
    const bool settled =
        !operation->outcome.isEmpty() || !operation->reconcile.isEmpty();
    if (operation->stubState.isEmpty() &&
        (++full <= m_start.limits.fullReceipts || !settled))
      continue;
    fate.insert(operation->operationId,
                ++stubs <= m_start.limits.receiptStubs ? "stub" : "forget");
  }
  if (fate.isEmpty()) return;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  for (std::size_t index = 0; index + 1 < m_segments.size(); ++index) {
    if (std::chrono::steady_clock::now() >= deadline) return;
    compactSegment(*m_segments[index], fate);
  }
}

void Journal::compactSegment(Segment& segment,
                             const QHash<QString, QString>& fate) {
  QFile file(path(segment.name));
  if (!file.open(QIODevice::ReadOnly)) return;
  const QList<QByteArray> lines = file.readAll().split('\n');
  file.close();
  QByteArray rewritten;
  bool changed = false;
  for (const QByteArray& text : lines) {
    if (text.isEmpty()) continue;
    QJsonObject record = QJsonDocument::fromJson(text.mid(65)).object();
    const QString id   = record.value("operation_id").toString();
    const QString type = record.value("type").toString();
    const QString kind = fate.value(id);
    const bool operationRecord =
        !id.isEmpty() && type != "job" && type != "package";
    // A stub already written stays as it is.
    if (kind.isEmpty() || !operationRecord ||
        (kind == "stub" && type == "stub")) {
      rewritten += text + '\n';
      continue;
    }
    changed = true;
    if (kind == "stub" && type == "admit")
      rewritten += hashedLine(stubRecord(record, *find(id)));
  }
  if (!changed) return;
  const QString staged = path(segment.name + ".compact");
  const int descriptor = openFile(staged, O_WRONLY | O_CREAT | O_TRUNC);
  if (descriptor < 0) return;
  const bool written =
      fullyWritten(descriptor, rewritten) && flushDescriptor(descriptor);
  closeFile(descriptor);
  faultFires(m_start.faults, "journal:compaction");
  if (!written || !replaceFile(staged, path(segment.name))) {
    QFile::remove(staged);
    return fail("io_error");
  }
  flushDirectory(m_root);
  segment.offset = segment.size = rewritten.size();
  for (auto it = fate.constBegin(); it != fate.constEnd(); ++it) {
    const auto found = m_operations.find(it.key());
    if (found == m_operations.end() || found->admittedSeq > segment.lastSeq)
      continue;
    if (it.value() == "forget") {
      m_operations.erase(found);
      continue;
    }
    found->stubState =
        stubRecord(QJsonObject{}, *found).value("state").toString();
    found->outcome = found->intent = found->reconcile = QJsonObject();
  }
}

QJsonObject Journal::status() const {
  qint64 segmentBytes = 0;
  for (const auto& segment : m_segments) segmentBytes += segment->size;
  qint64 blobBytes = 0;
  if (readable())
    for (const QFileInfo& blob :
         QDir(path("blobs")).entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
      blobBytes += blob.size();
  int full  = 0;
  int stubs = 0;
  for (const JournalOperation& operation : m_operations)
    ++(operation.stubState.isEmpty() ? full : stubs);
  QJsonArray writers;
  for (int index = m_writerOrder.size() - 1;
       index >= 0 && writers.size() < MaximumWriterRows; --index) {
    const QString writer = m_writerOrder.at(index);
    writers.append(QJsonObject{
        {"worker_instance_id", writer},
        {"current", writer == m_start.instance},
        {"alive", writerAlive(writer)},
        {"started_at_ms", double(integer(m_writers.value(writer), "t"))}});
  }
  QJsonArray damage;
  for (int row = 0; row < m_damage.size() && row < MaximumReportRows; ++row) {
    QJsonObject entry = m_damage.at(row).toObject();
    entry.remove("bytes");
    damage.append(entry);
  }
  const qint64 horizon = horizonSeq();
  QJsonObject recovery = m_lastRecovery;
  if (recovery.contains("at_seq")) {
    QJsonArray segments;
    for (const QJsonValue& row :
         recovery.value("quarantined_segments").toArray()) {
      QJsonObject entry = row.toObject();
      entry.insert("name", entry.take("segment"));
      if (segments.size() < MaximumReportRows) segments.append(entry);
    }
    recovery.insert("quarantined_segments", segments);
  }
  return {{"available", available()},
          {"state", state()},
          {"unavailable_reason", optional(m_reason)},
          {"format_version", FormatVersion},
          {"path", JournalPath},
          {"segments", int(m_segments.size())},
          {"segment_bytes", double(segmentBytes)},
          {"blob_bytes", double(blobBytes)},
          {"limits", m_start.limits.report()},
          {"retained", QJsonObject{{"full_receipts", full},
                                   {"receipt_stubs", stubs},
                                   {"packages", 0},
                                   {"jobs", m_jobs.size()}}},
          {"horizon", QJsonObject{{"seq", horizon ? QJsonValue(double(horizon))
                                                  : QJsonValue()},
                                  {"recorded_at_ms", QJsonValue()}}},
          {"writers", writers},
          {"writers_total", m_writerOrder.size()},
          {"last_recovery",
           recovery.contains("at_seq") ? QJsonValue(recovery) : QJsonValue()},
          {"damage", damage}};
}

Journal* workspaceJournal(Context& context) { return context.journal.get(); }

QJsonObject journalCapability(Context& context) {
  const Journal* journal = workspaceJournal(context);
  return {{"available", journal && journal->available()},
          {"state", journal ? journal->state() : QString("unavailable")},
          {"format_version", FormatVersion},
          {"path", JournalPath}};
}

QString journalContentDigest(const QString& path) {
  return contentDigest(path);
}

void recordPublicationIntent(Context& context, const QString& staged,
                             const QString& target) {
  Journal* journal = workspaceJournal(context);
  if (!journal || context.writeOperation.isEmpty()) return;
  const QString content   = contentDigest(staged);
  const QString manifest  = QFileInfo(staged).isDir()
                                ? contentDigest(staged + "/manifest.json")
                                : QString();
  const QString workspace = context.workspace();
  const QString relative  = target.startsWith(workspace + '/')
                                ? target.mid(workspace.size() + 1)
                                : target;
  journal->intent({{"operation_id", context.writeOperation},
                   {"target", relative},
                   {"expected_sha256", manifest.isEmpty() ? content : manifest},
                   {"content_sha256", content},
                   {"manifest_sha256", optional(manifest)}});
  writeFaultFires(context, "journal:intent");
}

QJsonObject Journal::absentStatus() {
  Journal absent(Start{});
  absent.fail("io_error");
  return absent.status();
}

}  // namespace GroundzeroAutomation
