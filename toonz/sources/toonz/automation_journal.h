#pragma once

#include <QHash>
#include <QMap>
#include <QSet>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

namespace GroundzeroAutomation {

class Context;

// The workspace journal: the durable record of every write and job of every
// worker on one workspace, in <workspace>/.groundzero/journal. Appends are
// serialised by an OS file lock that the kernel releases when its holder dies;
// each is one write of complete, hash-verified JSON lines, flushed to the disk
// before the step that depends on it. Readers take no lock: they verify each
// line and treat an incomplete last line as not yet written.
//
// Only a worker opens it. A worker whose journal is unavailable refuses every
// write; reads still answer.

struct JournalLimits final {
  qint64 segmentBytes     = 16 * 1024 * 1024;
  int lineBytes           = 64 * 1024;
  int inlineResponseBytes = 32 * 1024;
  qint64 blobBytes        = 8 * 1024 * 1024;
  qint64 journalBytes     = qint64(1024) * 1024 * 1024;
  int fullReceipts        = 16384;
  int receiptStubs        = 262144;
  int packages            = 4096;
  qint64 packageBytes     = qint64(512) * 1024 * 1024;
  int jobs                = 4096;
  // Receipts the dispatcher keeps in memory; older ones the journal holds
  // are dropped and answered from it.
  int memoryReceipts      = 2048;
  int lockWaitMs          = 5000;
  int outcomeLockWaitMs   = 30000;
  // --test-journal-limits small: rotation, compaction and expiry in seconds.
  // (Not named small: the Windows headers define small as a macro.)
  static JournalLimits reduced();
  // The limits journal.status reports.
  QJsonObject report() const;
};

// What the journal holds for one operation id.
struct JournalOperation final {
  QString operationId;
  QString method;
  QString argumentsSha256;
  QString authorityDigest;
  QString documentId;
  QString effectClass;
  QString writer;
  qint64 admittedSeq  = 0;
  qint64 admittedAtMs = 0;
  // The stage, intent, outcome and reconcile records; empty when absent. An
  // outcome's inline response is not kept in memory: it is read back from
  // its line (segment and byte offset) when a retry needs it.
  QJsonObject stage;
  QJsonObject intent;
  // scene.open's pin on the package it copies from.
  QJsonObject pin;
  QJsonObject outcome;
  QString responseSegment;
  qint64 responseOffset = -1;
  QJsonObject reconcile;
  // Set for an operation compacted to a stub: its state then.
  QString stubState;
};

// A reconciliation verdict (contract section 4.2): {kind, reconciled,
// evidence {target, expected_sha256, found, job_id, outputs_present}}.
using JournalVerdict = QJsonObject;

class Journal final {
public:
  struct Start final {
    QString workspace;
    QString instance;
    QString runtimeName;
    QString authorityDigest;
    // The canonical input roots, which locations name by digest.
    QStringList inputRoots;
    // The granted output roots by name, with their canonical directories.
    QMap<QString, QString> outputRoots;
    // The output roots granted --output-modify.
    QStringList outputModify;
    // The shared build digest (runtime.inspect, the live handshake).
    QString buildDigest;
    JournalLimits limits;
    // Test fault points (journal:...) are read from it; may be null.
    const Context* faults = nullptr;
  };

  // Opens, creates or recovers the journal under its lock: a torn last line
  // is quarantined and cut off, a damaged segment is quarantined and
  // reported, operations and jobs of dead workers are reconciled, and this
  // worker's start is recorded. Never throws: a journal that cannot be used is
  // returned unavailable with its reason.
  static std::shared_ptr<Journal> open(const Start& start);
  ~Journal();
  Journal(const Journal&)            = delete;
  Journal& operator=(const Journal&) = delete;

  bool available() const { return m_reason.isEmpty(); }
  // Whether the journal was opened and read, even if it cannot be written.
  bool readable() const { return m_lockDescriptor >= 0; }
  // journal.status of a process without a journal.
  static QJsonObject absentStatus();
  // ok, damaged or unavailable.
  QString state() const;
  QString unavailableReason() const { return m_reason; }
  const QString& instance() const { return m_start.instance; }
  const QString& authorityDigest() const { return m_start.authorityDigest; }
  const JournalLimits& limits() const { return m_start.limits; }

  // Reads what other workers appended since the last read; takes no lock.
  void refresh();
  // The operation as last read, or null.
  const JournalOperation* find(const QString& operationId) const;
  // Whether the worker instance that wrote records is running.
  bool writerAlive(const QString& instance) const;
  // The full instance id of a worker whose id starts with `prefix`, or empty.
  QString writerWithPrefix(const QString& prefix) const;
  // The verdict for an admitted operation without outcome, from its effect
  // class and, for a publication, what is at its target now.
  JournalVerdict verdict(const JournalOperation& operation) const;
  // The absolute path of a location {root, path} in a root this worker
  // holds with the same canonical path, or empty.
  QString locatedPath(const QJsonObject& location) const;
  // The recorded response ({result} or {error}) of a completed or failed
  // operation; throws journal_unavailable when its blob is missing or altered.
  QJsonObject response(const JournalOperation& operation) const;

  // Appends and flushes an admit record, unless another worker admitted the
  // id meanwhile (false; nothing appended). Throws journal_busy when the lock
  // is not obtained within the admission wait, journal_unavailable when the
  // journal cannot be written.
  bool admit(const QJsonObject& fields);
  // An admitted operation refused before any change; not flushed.
  void release(const QString& operationId);
  // Appends and flushes the intent of a publication.
  void intent(const QJsonObject& fields);
  // Appends and flushes the stage record of a publication into an output
  // root, before its staging container exists.
  void stage(const QJsonObject& fields);
  // Appends and flushes a pin: an operation reading the package at a
  // location, which package.remove refuses to remove meanwhile.
  void pin(const QJsonObject& fields);
  // The live operation of another call that stages, publishes, removes or
  // reads the package at `location`, or null.
  const JournalOperation* pinning(const QJsonObject& location,
                                  const QString& except) const;
  // Appends and flushes the outcome of an admitted operation. On failure the
  // journal becomes unavailable for this worker and false is returned.
  bool outcome(const QString& operationId, const QJsonObject& response,
               const QJsonObject& summary);
  // Appends a reconcile record for an operation of a dead worker unless one
  // is already there; returns the verdict either way.
  JournalVerdict reconcile(const JournalOperation& operation);

  // Job records (newest record per job id).
  void recordJob(const QJsonObject& record);
  // The job record as job.status answers it from the journal: observed_by
  // journal, and status unknown with worker_loss when its worker died before
  // a terminal record.
  QJsonObject jobView(const QJsonObject& stored) const;
  const QJsonObject* job(const QString& jobId) const;
  QStringList jobIds() const;

  // Applies the retention limits to at most one sealed segment: one bounded
  // step, run between requests on the application thread.
  void maintain();

  // Package registrations (R3), newest first; an evicted one keeps an
  // expired mark.
  const QJsonObject* package(const QString& packageId) const;
  QStringList packageIds() const;
  // Appends and flushes a package registration.
  void registerPackage(const QJsonObject& fields);
  // A content-addressed payload: stored once under its SHA-256 (returned).
  QString storeBlob(const QByteArray& bytes);
  QByteArray readBlob(const QString& digest, qint64 bytes) const;

  // Operations, newest admission first, for operation.list.
  std::vector<const JournalOperation*> operationsNewestFirst() const;
  qint64 horizonSeq() const;

  // journal.status.
  QJsonObject status() const;

private:
  explicit Journal(Start start);
  struct Segment;
  struct Lock;
  void fail(const QString& reason);
  void recover();
  void scanSegment(Segment& segment, bool recovery);
  void applyRecord(const QJsonObject& record, Segment& segment);
  void apply(const QJsonObject& record);
  // The outcome record of an operation whose response was not kept in
  // memory, read back from its segment; empty when it is not there.
  QJsonObject outcomeLine(const JournalOperation& operation) const;
  // Quarantines and cuts off a torn last line (and, with verifyLast, a last
  // line whose hash is wrong); returns the bytes cut, or -1 on an I/O error.
  qint64 repairTail(Segment& segment, bool verifyLast);
  void quarantineSegment(Segment& segment, const std::vector<QByteArray>& good,
                         qint64 firstSeq, qint64 lastSeq);
  void reconcileDead();
  // Removes the abandoned staging container of an operation, in a root this
  // worker holds, keeping what its record does not name as residue.
  void removeStaging(const JournalOperation& operation);
  // Finishes an interrupted package removal whose trash lies in a root this
  // worker may modify.
  void finishRemoval(const JournalOperation& operation);
  // The canonical directory of a location's root when this worker holds it
  // (and may modify it, with `modify`), or empty.
  QString heldRoot(const QJsonObject& location, bool modify) const;
  struct Retention final {
    // operation id -> stub or forget
    QHash<QString, QString> operations;
    QSet<QString> jobs;
    QSet<QString> packages;
    bool empty() const;
  };
  Retention retention() const;
  void compactSegments();
  bool compactSegment(Segment& segment, const Retention& plan);
  QByteArray keptLine(const QByteArray& same, const QJsonObject& record,
                      const Retention& plan) const;
  void forgetCompacted(const Segment& segment, const Retention& plan);
  qint64 journalBytes() const;
  qint64 blobBytes() const;
  QJsonObject jobLoss(const QJsonObject& record) const;
  // Appends records under the lock; `check` runs after the tail is read
  // under the lock and returns false to append nothing.
  bool append(QList<QJsonObject> records, int waitMs, bool flush,
              const std::function<bool()>& check = {});
  QByteArray line(QJsonObject& record, qint64 seq) const;
  Segment& writableSegment(qint64 nextSeq, qint64 bytes);
  QString path(const QString& name) const;
  void listSegments();

  Start m_start;
  QString m_root;
  QString m_reason;
  int m_lockDescriptor = -1;
  std::vector<std::unique_ptr<Segment>> m_segments;
  QHash<QString, JournalOperation> m_operations;
  QHash<QString, QJsonObject> m_writers;
  QStringList m_writerOrder;
  QHash<QString, QJsonObject> m_jobs;
  QHash<QString, QJsonObject> m_packages;
  qint64 m_blobBytes         = 0;
  qint64 m_plannedSeq        = 0;
  int m_maintenance          = 0;
  int m_registeredByRecovery = 0;
  int m_removedContainers    = 0;
  QJsonArray m_stagingResidue;
  QJsonArray m_damage;
  QJsonArray m_quarantined;
  QJsonObject m_lastRecovery;
  // Where the record being applied starts: its segment and byte offset.
  QString m_applyingSegment;
  qint64 m_applyingOffset = -1;
  bool m_locked       = false;
  bool m_recovering   = false;
  qint64 m_lastSeq    = 0;
  qint64 m_runtimeBad = 0;
};

// The SHA-256 of a file, or of a folder as the sorted list of its files'
// relative paths, sizes and SHA-256s; empty when it cannot be read, holds a
// link or exceeds the package limits.
QString journalContentDigest(const QString& path);

// Before a publication's final rename: the durable intent naming its target
// and the digest of what will appear there. Nothing without a journal or
// outside a write.
void recordPublicationIntent(Context& context, const QString& staged,
                             const QString& target,
                             const QJsonObject& extra = {});
// The same for a target named by its location (an output root's). `extra`
// may name the tree's content_sha256 (else it is read from `staged`) and its
// manifest_name (else manifest.json, read from `staged`); an intent with a
// manifest_name is judged by its manifest's digest.
void recordPublicationIntentAt(Context& context, const QString& staged,
                               const QJsonObject& location,
                               const QJsonObject& extra);
// Before a staging container is made in an output root: the durable stage
// record {operation_id, location, container} with `extra`'s fields; a
// manifest_name among them names the manifest whose files recovery removes.
void recordPublicationStage(Context& context, const QJsonObject& location,
                            const QString& container, const QJsonObject& extra);

// Where a package or publication lies, with no absolute path:
// {root: workspace | input | an output root's name, path (root-relative),
// input_index, root_sha256 (of the canonical input or output root)}.
QJsonObject packageLocation(const Context& context, const QString& absolute);
// The location of `relative` in the granted output root `name`, its
// root_sha256 that of the root's canonical path.
QJsonObject outputLocation(const Context& context, const QString& name,
                           const QString& relative);
bool sameLocation(const QJsonObject& first, const QJsonObject& second);

// The worker's journal, or null outside a worker.
Journal* workspaceJournal(Context& context);
// automation.capabilities journal: {available, state, format_version, path}.
QJsonObject journalCapability(Context& context);

// The tools operation.status, journal.status and operation.list.
QJsonArray journalOperations();
QJsonValue executeJournal(Context& context, const QString& method,
                          const QJsonObject& params);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationJournalTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
