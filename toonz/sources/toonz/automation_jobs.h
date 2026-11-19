#pragma once

#include "automation.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMutex>

#include <deque>
#include <memory>
#include <vector>

namespace GroundzeroAutomation {

// Jobs run in one of two lanes; each lane admits one active job at a time
// and retains at most JobsPerLane jobs per worker, scene hosts included.
enum class JobLane { Render, Analysis };

constexpr int JobsPerLane      = 32;
constexpr int ExpiredJobIds    = 256;
constexpr int CancelGraceMs    = 30000;
constexpr int JobIdMaximumSize = 80;
// What a worker remembers of jobs released by job.discard; older ids are
// answered from the workspace journal while it holds them.
constexpr int DiscardedJobIds      = 1024;
constexpr int DiscardedArtifactIds = 8192;
// Releasable jobs a retention refusal names, oldest first.
constexpr int ReleasableListed = 8;

// One job of any kind. Every method except interrupt() runs on the
// application thread.
class Job {
public:
  virtual ~Job()                       = default;
  virtual QString id() const           = 0;
  virtual JobLane lane() const         = 0;
  virtual QString documentId() const   = 0;
  virtual bool terminal() const        = 0;
  virtual qint64 reservedBytes() const = 0;
  // The job record (contract definitions.job_record).
  virtual QJsonObject record() const = 0;
  // Ask the job to stop; a finished job records the request and is unchanged.
  virtual void cancel() = 0;
  // Ask the job's native work to stop at once, from any thread. The request is
  // recorded as made at `requestedMs` when cancel() runs on the application
  // thread.
  virtual void interrupt(qint64 requestedMs) = 0;
  // job.result for this job's kind.
  virtual QJsonValue result(Context& context, const QJsonObject& params) = 0;
  // job.discard of this terminal job: remove the files it keeps for its
  // result, its claim on them first, and return their bytes. Everything else
  // it holds goes with the object. Throws io_error when a file stays.
  virtual qint64 removeOutputs() { return 0; }
  // The artifacts job.result and artifact.read serve for this job.
  virtual QStringList artifactIds() const { return {}; }
};

// The worker's job families: its own jobs and what it knows of its hosts'.
class JobRegistry final {
public:
  explicit JobRegistry(QString instance, qint64 epochMs);

  const QString& instance() const { return m_instance; }
  // Milliseconds on the worker's monotonic clock since the worker started.
  qint64 elapsedMs() const;
  QString newJobId() const;
  QString newArtifactId() const;

  // Refuse an analysis job before anything is created when the lane is busy
  // or full. `hostActive` and `hostRetained` count the lane's jobs that run in
  // scene hosts. Render admission is the render checks' (lane, retention and
  // storage), which read active(), retained() and reservedBytes().
  void admitAnalysis(const QString& hostActive, int hostRetained) const;
  void add(std::shared_ptr<Job> job);
  // Record when the worker first saw a job id, its own or a scene host's.
  void note(const QString& id);
  // Every listed job id, newest first.
  QStringList newestFirst() const;
  // Retained jobs of this worker and its hosts.
  int retainedCount() const { return m_order.size(); }
  bool retains(const QString& id) const { return m_order.contains(id); }
  qint64 epochMs() const { return m_epochMs; }
  std::shared_ptr<Job> find(const QString& id) const;
  // The job, or unknown_job saying why the id is not a retained job here.
  std::shared_ptr<Job> require(const QString& id) const;
  [[noreturn]] void refuseUnknown(const QString& id) const;
  // Remember ids whose read-only document was discarded with its host.
  void expire(const QStringList& ids);
  // Forget a job released by job.discard (this process's, or a scene host's
  // the worker only lists) and remember what its id and its artifacts' ids
  // answer from now on: `tombstone` is the job_discarded details.
  void discard(const QString& id, const QJsonObject& tombstone,
               const QStringList& artifactIds);
  // The job_discarded details of a remembered discarded job, or empty.
  QJsonObject discarded(const QString& id) const;
  // The job id of a remembered discarded artifact, or empty.
  QString discardedArtifactJob(const QString& artifactId) const;
  // The details of a refusal at a lane's retention limit, from this
  // process's jobs and the last observed records of its scene hosts' jobs.
  QJsonObject retention(JobLane lane,
                        const QHash<QString, QJsonObject>& hosted) const;
  const std::vector<std::shared_ptr<Job>>& jobs() const { return m_jobs; }
  QString active(JobLane lane) const;
  int retained(JobLane lane) const;
  qint64 reservedBytes(JobLane lane) const;

  // Polls are answered off the application thread from published copies of
  // the records, so a busy application thread never delays them. Application
  // thread: refresh the copies of jobs whose record can still change: running
  // jobs, and finished ones published running or marked by unsettle().
  void publish();
  // A finished job's record changed (a late cancellation).
  void unsettle(const QString& id);
  // Any thread: job.status, or job.cancel when `cancel`, answered from the
  // published record; null when this process published no record for `id`.
  // A cancellation interrupts the job and queues cancel() for the application
  // thread; until it runs, published copies show the request.
  QJsonValue answerPoll(const QString& id, bool cancel);

private:
  struct Published final {
    QJsonObject record;
    std::weak_ptr<Job> job;
    qint64 interruptMs = -1;
    bool changed       = false;
  };
  QString m_instance;
  qint64 m_epochMs;
  std::vector<std::shared_ptr<Job>> m_jobs;
  QStringList m_order;
  std::deque<QString> m_expired;
  QHash<QString, QJsonObject> m_discarded;
  std::deque<QString> m_discardedOrder;
  QHash<QString, QString> m_discardedArtifacts;
  std::deque<QString> m_discardedArtifactOrder;
  QMutex m_publishedLock;
  QHash<QString, Published> m_published;
};

// The process's job registry: the worker's instance, or for a scene host the
// instance and clock its worker passed on the command line.
JobRegistry& jobRegistry(Context& context);
QString laneText(JobLane lane);
// The record of a scene-host job by id, or an empty object when the id does
// not belong to a host (automation_scene_access.cpp).
QJsonObject hostJobRecord(Context& context, const QString& id);
// The last record this worker observed of each of its scene hosts' jobs, by
// id; nothing is asked of the hosts (automation_scene_access.cpp).
QHash<QString, QJsonObject> observedHostJobs(Context& context);
// {instance_id, build} of the worker that owns this process's jobs.
QJsonObject workerIdentity(Context& context);
// The record fields every job kind shares.
struct JobCommon final {
  QString id;
  QString kind;
  JobLane lane = JobLane::Render;
  QString documentId;
  QString revision;
  QJsonObject request;
  QString snapshotManifest;
  QString sourceFingerprint;
  qint64 submittedMs   = 0;
  qint64 startedMs     = -1;
  qint64 endedMs       = -1;
  bool cancelRequested = false;
  qint64 cancelMs      = -1;
  QStringList forced;
  QJsonValue failure;
  bool stalled = false;
};
QJsonObject commonRecord(Context& context, const JobCommon& common,
                         const QString& status, bool terminal,
                         const QJsonObject& limits, const QJsonObject& progress,
                         const QJsonObject& outputs);
// The job.list row for one record.
QJsonObject jobSummary(const QJsonObject& record);

// Jobs one lane retains in this worker, its scene hosts' included.
int retainedJobs(Context& context, JobLane lane);
// The details of a refusal at a lane's retention limit: the limit, how many
// jobs the lane retains, and the oldest terminal ones job.discard releases.
QJsonObject retentionDetails(Context& context, JobLane lane);
// The message of that refusal, naming job.discard.
QString retentionMessage(JobLane lane);
// Remove a terminal job's private directory, through the pinned-directory
// functions, and return the bytes of the files it held. Throws io_error when
// something stays.
qint64 removeJobDirectory(const QString& directory);
// artifact.read of an artifact whose job was discarded: job_discarded.
// Returns when the artifact is not one this process remembers discarding.
void refuseDiscardedArtifact(Context& context, const QString& artifactId);
// After a scene host released one of its jobs: the worker forgets it and
// records the discard, as for its own jobs. `record` is the job's last
// record; returns the discard result as the worker reports it.
QJsonObject recordHostDiscard(Context& context, const QString& id,
                              QJsonObject record, QJsonObject result);

// Cancel every job this process runs, on both lanes.
void cancelAllJobs(Context& context);
// Cancel every job and run the event loop until none is active or budgetMs
// has passed; true when none is active. Jobs remove their unfinished output
// when they end.
bool settleJobs(Context& context, int budgetMs);

QJsonArray jobOperations();
QJsonValue executeJobs(Context& context, const QString& method,
                       const QJsonObject& params);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationJobsTests(Context& context);
#endif

}  // namespace GroundzeroAutomation
