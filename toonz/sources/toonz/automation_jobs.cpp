#include "automation_jobs.h"

#include "automation_build_identity.h"
#include "automation_files.h"
#include "automation_journal.h"
#include "automation_scene_host.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QMutexLocker>
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace GroundzeroAutomation {
namespace {

const QStringList& jobKinds() {
  static const QStringList kinds{
      "render_preview", "render_sequence", "neural_preview", "drawing_analysis",
      "scene_check",    "intake_import",   "scene_import",   "image_review"};
  return kinds;
}

QJsonValue optionalMs(qint64 value) {
  return value < 0 ? QJsonValue() : QJsonValue(double(value));
}

QJsonValue optionalText(const QString& value) {
  return value.isEmpty() ? QJsonValue() : QJsonValue(value);
}

bool stoppedEarly(const QString& status) {
  return status == "canceled" || status == "deadline_exceeded" ||
         status == "failed";
}

QJsonObject cancellationRecord(const JobCommon& common, const QString& status,
                               bool terminal) {
  QJsonValue effective;
  if (common.cancelRequested && terminal) effective = status == "canceled";
  return {{"requested", common.cancelRequested},
          {"requested_ms", optionalMs(common.cancelMs)},
          {"effective", effective}};
}

QJsonObject terminationRecord(const JobCommon& common, const QString& status,
                              bool terminal) {
  QJsonValue mode;
  if (terminal && stoppedEarly(status))
    mode = common.forced.isEmpty() ? "cooperative" : "forced";
  return {{"mode", mode},
          {"forced_processes", QJsonArray::fromStringList(common.forced)}};
}

QJsonObject jobIdSchema() {
  return {
      {"type", "string"}, {"minLength", 1}, {"maxLength", JobIdMaximumSize}};
}

QJsonObject boundedInteger(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject choice(const QStringList& values) {
  return {{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}};
}

// A document_id filter names a current document or one whose retained jobs
// outlived its discard.
void requireListedDocument(Context& context, const QString& documentId,
                           const QJsonArray& records) {
  try {
    context.document(documentId);
    return;
  } catch (const Error& error) {
    if (error.code != "document_not_found") throw;
  }
  for (const QJsonValue& record : records)
    if (record.toObject().value("document_id") == documentId) return;
  throw Error("document_not_found",
              "No current document or retained job has this document_id.",
              {{"document_id", documentId}});
}

bool listed(const QJsonObject& record, const QJsonObject& params) {
  if (params.contains("document_id") &&
      record.value("document_id") != params.value("document_id"))
    return false;
  if (params.contains("kind") && record.value("kind") != params.value("kind"))
    return false;
  const QString state = params.value("state").toString("any");
  const bool terminal = record.value("terminal").toBool();
  return state == "any" || (state == "terminal") == terminal;
}

// Records of every retained job, the worker's and its hosts', newest first.
QJsonArray retainedRecords(Context& context) {
  JobRegistry& registry = jobRegistry(context);
  QJsonArray records;
  for (const QString& id : registry.newestFirst()) {
    const auto job           = registry.find(id);
    const QJsonObject record = job ? job->record() : hostJobRecord(context, id);
    if (!record.isEmpty()) records.append(record);
  }
  return records;
}

bool discardedRecord(const QJsonObject& stored) {
  return stored.value("record").toObject().contains("discarded");
}

// Jobs of earlier or other workers that the journal holds, newest first;
// discarded jobs are no longer listed.
QJsonArray journalRecords(Context& context) {
  QJsonArray records;
  Journal* journal = workspaceJournal(context);
  if (!journal) return records;
  journal->refresh();
  JobRegistry& registry = jobRegistry(context);
  const QString own     = "job_" + registry.instance().left(8) + "_";
  for (const QString& id : journal->jobIds()) {
    const QJsonObject& stored = *journal->job(id);
    if (!id.startsWith(own) && !discardedRecord(stored))
      records.append(journal->jobView(stored));
  }
  return records;
}

QJsonValue listJobs(Context& context, const QJsonObject& params) {
  QJsonArray records = retainedRecords(context);
  for (const QJsonValue& record : journalRecords(context))
    records.append(record);
  if (params.contains("document_id"))
    requireListedDocument(context, requiredString(params, "document_id"),
                          records);
  QJsonArray matching;
  for (const QJsonValue& record : records)
    if (listed(record.toObject(), params))
      matching.append(jobSummary(record.toObject()));
  const int offset = integerParam(params, "offset", 0, 64, 0);
  const int limit  = integerParam(params, "limit", 1, 64, 32);
  QJsonArray page;
  for (int index = offset; index < matching.size() && page.size() < limit;
       ++index)
    page.append(matching.at(index));
  const int next = offset + page.size();
  return QJsonObject{
      {"jobs", page},
      {"total", matching.size()},
      {"offset", offset},
      {"next_offset", next < matching.size() ? QJsonValue(next) : QJsonValue()},
      {"worker", workerIdentity(context)},
      {"limits", QJsonObject{{"retained_render_jobs", JobsPerLane},
                             {"retained_analysis_jobs", JobsPerLane},
                             {"active_per_lane", 1}}}};
}

}  // namespace

JobRegistry::JobRegistry(QString instance, qint64 epochMs)
    : m_instance(std::move(instance)), m_epochMs(epochMs) {}

qint64 JobRegistry::elapsedMs() const {
  QElapsedTimer now;
  now.start();
  return std::max<qint64>(0, now.msecsSinceReference() - m_epochMs);
}

QString JobRegistry::newJobId() const {
  return "job_" + m_instance.left(8) + "_" +
         QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString JobRegistry::newArtifactId() const {
  return "artifact_" + m_instance.left(8) + "_" +
         QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void cancelAllJobs(Context& context) {
  if (!context.jobs) return;
  for (const auto& job : context.jobs->jobs())
    if (!job->terminal()) job->cancel();
}

namespace {

bool jobsSettled(const Context& context) {
  return !context.jobs || (context.jobs->active(JobLane::Render).isEmpty() &&
                           context.jobs->active(JobLane::Analysis).isEmpty());
}

}  // namespace

bool settleJobs(Context& context, int budgetMs) {
  cancelAllJobs(context);
  if (jobsSettled(context)) return true;
  QElapsedTimer elapsed;
  elapsed.start();
  QEventLoop drain;
  QTimer poll;
  QObject::connect(&poll, &QTimer::timeout, &drain,
                   [&context, &drain, &elapsed, budgetMs]() {
                     if (jobsSettled(context) || elapsed.elapsed() >= budgetMs)
                       drain.quit();
                   });
  poll.start(10);
  drain.exec(QEventLoop::ExcludeUserInputEvents);
  return jobsSettled(context);
}

QString JobRegistry::active(JobLane lane) const {
  for (const auto& job : m_jobs)
    if (job->lane() == lane && !job->terminal()) return job->id();
  return {};
}

int JobRegistry::retained(JobLane lane) const {
  return int(std::count_if(
      m_jobs.begin(), m_jobs.end(),
      [lane](const std::shared_ptr<Job>& job) { return job->lane() == lane; }));
}

qint64 JobRegistry::reservedBytes(JobLane lane) const {
  qint64 reserved = 0;
  for (const auto& job : m_jobs)
    if (job->lane() == lane) reserved += job->reservedBytes();
  return reserved;
}

namespace {

// A record as job.cancel leaves it once the request is recorded.
QJsonObject cancelView(QJsonObject record, qint64 requestedMs) {
  QJsonObject cancellation = record.value("cancellation").toObject();
  if (!cancellation.value("requested").toBool()) {
    cancellation.insert("requested", true);
    cancellation.insert("requested_ms", requestedMs);
  }
  if (record.value("terminal").toBool())
    cancellation.insert("effective", record.value("status") == "canceled");
  else
    record.insert("status", "cancellation_requested");
  record.insert("cancellation", cancellation);
  return record;
}

}  // namespace

void JobRegistry::unsettle(const QString& id) {
  QMutexLocker lock(&m_publishedLock);
  const auto found = m_published.find(id);
  if (found != m_published.end()) found->changed = true;
}

void JobRegistry::publish() {
  std::vector<std::pair<std::shared_ptr<Job>, QJsonObject>> records;
  {
    QMutexLocker lock(&m_publishedLock);
    for (const auto& job : m_jobs) {
      const auto found   = m_published.constFind(job->id());
      const bool settled = found != m_published.constEnd() &&
                           found->record.value("terminal").toBool() &&
                           found->interruptMs < 0 && !found->changed;
      if (!settled) records.emplace_back(job, QJsonObject());
    }
  }
  for (auto& entry : records) entry.second = entry.first->record();
  QMutexLocker lock(&m_publishedLock);
  for (auto& entry : records) {
    Published& published = m_published[entry.first->id()];
    published.job        = entry.first;
    published.record     = entry.second;
    published.changed    = false;
    if (published.interruptMs < 0) continue;
    if (entry.second.value("cancellation")
            .toObject()
            .value("requested")
            .toBool())
      published.interruptMs = -1;
    else
      published.record = cancelView(entry.second, published.interruptMs);
  }
}

QJsonValue JobRegistry::answerPoll(const QString& id, bool cancel) {
  QMutexLocker lock(&m_publishedLock);
  const auto found = m_published.find(id);
  if (found == m_published.end()) return QJsonValue();
  if (!cancel) return found->record;
  const std::shared_ptr<Job> job = found->job.lock();
  if (!job) return QJsonValue();
  const qint64 requestedMs = elapsedMs();
  if (found->interruptMs < 0 && !found->record.value("cancellation")
                                     .toObject()
                                     .value("requested")
                                     .toBool())
    found->interruptMs = requestedMs;
  job->interrupt(requestedMs);
  found->record = cancelView(found->record, requestedMs);
  QMetaObject::invokeMethod(
      QCoreApplication::instance(), [job]() { job->cancel(); },
      Qt::QueuedConnection);
  return found->record;
}

void JobRegistry::admitAnalysis(const QString& hostActive,
                                int hostRetained) const {
  const QString local = active(JobLane::Analysis);
  const QString busy  = local.isEmpty() ? hostActive : local;
  if (!busy.isEmpty())
    throw Error(
        "analysis_busy",
        "Wait for the active analysis job to finish or cancel it.",
        {{"active_job_id", busy}, {"lane", laneText(JobLane::Analysis)}});
  const int held = retained(JobLane::Analysis) + hostRetained;
  if (held < JobsPerLane) return;
  QJsonObject details = retention(JobLane::Analysis, {});
  details.insert("retained", held);
  throw Error("resource_limit", retentionMessage(JobLane::Analysis), details);
}

void JobRegistry::add(std::shared_ptr<Job> job) {
  note(job->id());
  m_jobs.push_back(std::move(job));
}

void JobRegistry::note(const QString& id) {
  if (!m_order.contains(id)) m_order.append(id);
}

QStringList JobRegistry::newestFirst() const {
  QStringList ids;
  for (auto it = m_order.crbegin(); it != m_order.crend(); ++it)
    ids.append(*it);
  return ids;
}

std::shared_ptr<Job> JobRegistry::find(const QString& id) const {
  for (const auto& job : m_jobs)
    if (job->id() == id) return job;
  return nullptr;
}

std::shared_ptr<Job> JobRegistry::require(const QString& id) const {
  if (auto job = find(id)) return job;
  refuseUnknown(id);
}

void JobRegistry::refuseUnknown(const QString& id) const {
  QString reason = "not_issued";
  if (!id.startsWith("job_" + m_instance.left(8) + "_"))
    reason = "other_worker";
  else if (std::find(m_expired.begin(), m_expired.end(), id) != m_expired.end())
    reason = "expired";
  throw Error("unknown_job",
              "This worker has no retained job with this id; list its jobs "
              "with job.list.",
              {{"reason", reason}, {"worker_instance_id", m_instance}});
}

void JobRegistry::expire(const QStringList& ids) {
  for (const QString& id : ids) {
    m_order.removeAll(id);
    m_expired.push_back(id);
    if (int(m_expired.size()) > ExpiredJobIds) m_expired.pop_front();
  }
}

void JobRegistry::discard(const QString& id, const QJsonObject& tombstone,
                          const QStringList& artifactIds) {
  m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(),
                              [&id](const std::shared_ptr<Job>& job) {
                                return job->id() == id;
                              }),
               m_jobs.end());
  m_order.removeAll(id);
  {
    QMutexLocker lock(&m_publishedLock);
    m_published.remove(id);
  }
  m_discarded.insert(id, tombstone);
  m_discardedOrder.push_back(id);
  while (int(m_discardedOrder.size()) > DiscardedJobIds) {
    m_discarded.remove(m_discardedOrder.front());
    m_discardedOrder.pop_front();
  }
  for (const QString& artifact : artifactIds) {
    m_discardedArtifacts.insert(artifact, id);
    m_discardedArtifactOrder.push_back(artifact);
  }
  while (int(m_discardedArtifactOrder.size()) > DiscardedArtifactIds) {
    m_discardedArtifacts.remove(m_discardedArtifactOrder.front());
    m_discardedArtifactOrder.pop_front();
  }
}

QJsonObject JobRegistry::discarded(const QString& id) const {
  return m_discarded.value(id);
}

QString JobRegistry::discardedArtifactJob(const QString& artifactId) const {
  return m_discardedArtifacts.value(artifactId);
}

JobRegistry& jobRegistry(Context& context) {
  if (!context.jobs) {
    if (context.workerInstance.isEmpty()) {
      context.workerInstance =
          QUuid::createUuid().toString(QUuid::WithoutBraces);
      QElapsedTimer epoch;
      epoch.start();
      context.workerEpochMs = epoch.msecsSinceReference();
    }
    context.jobs = std::make_shared<JobRegistry>(context.workerInstance,
                                                 context.workerEpochMs);
  }
  return *context.jobs;
}

QString laneText(JobLane lane) {
  return lane == JobLane::Render ? "render" : "analysis";
}

QJsonObject workerIdentity(Context& context) {
  return {{"instance_id", jobRegistry(context).instance()},
          {"build", buildIdentity()}};
}

QJsonObject commonRecord(Context& context, const JobCommon& common,
                         const QString& status, bool terminal,
                         const QJsonObject& limits, const QJsonObject& progress,
                         const QJsonObject& outputs) {
  return {
      {"job_id", common.id},
      {"kind", common.kind},
      {"lane", laneText(common.lane)},
      {"executor",
       context.sceneHostDocument.isEmpty() ? "worker" : "scene_host"},
      {"document_id", common.documentId},
      {"source_revision", common.revision},
      {"status", status},
      {"terminal", terminal},
      {"worker", workerIdentity(context)},
      {"plugin_policy", "builtin_only"},
      {"limits", limits},
      {"inputs",
       QJsonObject{
           {"request", common.request},
           {"snapshot_manifest_sha256", optionalText(common.snapshotManifest)},
           {"source_fingerprint", optionalText(common.sourceFingerprint)}}},
      {"progress", progress},
      {"cancellation", cancellationRecord(common, status, terminal)},
      {"termination", terminationRecord(common, status, terminal)},
      {"failure", common.failure},
      {"stalled", common.stalled},
      {"timing", QJsonObject{{"submitted_ms", double(common.submittedMs)},
                             {"started_ms", optionalMs(common.startedMs)},
                             {"ended_ms", optionalMs(common.endedMs)}}},
      {"outputs", outputs}};
}

QJsonObject jobSummary(const QJsonObject& record) {
  QJsonObject summary;
  for (const char* key : {"job_id", "kind", "lane", "document_id",
                          "source_revision", "status", "terminal", "timing"})
    summary.insert(key, record.value(key));
  summary.insert("observed_by", record.value("observed_by").toString("worker"));
  return summary;
}

QJsonArray jobOperations() {
  const QJsonObject job{{"job_id", jobIdSchema()}};
  const QJsonObject cancel = operation(
      "job.cancel",
      "Request cancellation of a job and return its record; poll job.status "
      "until terminal. A job that ends completed despite the request reports "
      "cancellation.effective false.",
      job, {"job_id"}, true);
  QJsonObject result = job;
  result.insert("offset", boundedInteger(0, 65536));
  result.insert("limit", boundedInteger(1, 256));
  const QJsonObject list{
      {"document_id",
       QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
      {"kind", choice(jobKinds())},
      {"state", choice({"active", "terminal", "any"})},
      {"offset", boundedInteger(0, 64)},
      {"limit", boundedInteger(1, 64)}};
  return {operation("job.status",
                    "Read one job's record: state, cancellation, termination, "
                    "failure, progress, limits, inputs, outputs and the worker "
                    "and build that own it.",
                    job, {"job_id"}),
          operation("job.list",
                    "List this worker's retained jobs, newest first, with "
                    "optional document, kind and state filters.",
                    list, {}),
          cancel,
          operation("job.result",
                    "Read a finished job's output: verified artifacts of a "
                    "completed render job.",
                    result, {"job_id"}),
          operation("job.discard",
                    "Release a finished job you no longer need: remove its "
                    "private files and artifacts and free its slot in the "
                    "lane. Only terminal jobs; later reads of the job or its "
                    "artifacts answer job_discarded.",
                    job, {"job_id"}, true)};
}

namespace {

// A job of an earlier or other worker of the workspace, answered from its
// last journal record: never resumed, cancelled, discarded or read for
// artifacts here.
QJsonValue journalJob(Context& context, const QString& method,
                      const QJsonObject& stored) {
  const QJsonObject record = workspaceJournal(context)->jobView(stored);
  if (method == "job.status") return record;
  if (method == "job.cancel" || method == "job.discard")
    jobRegistry(context).refuseUnknown(record.value("job_id").toString());
  const bool completed = record.value("status") == "completed";
  throw Error(completed ? "artifacts_unavailable" : "result_unavailable",
              "This job ran in an earlier worker of the workspace; its "
              "outputs are listed by digest in its record and cannot be read "
              "here.",
              {{"reason", "previous_worker"},
               {"status", record.value("status")},
               {"job_id", record.value("job_id")}});
}

[[noreturn]] void refuseDiscarded(const QJsonObject& details) {
  throw Error("job_discarded",
              "This job was released with job.discard: its record, result "
              "and artifacts are gone. Submit a new job if you need its "
              "output again.",
              details);
}

// The job_discarded details of a job released now, from its last record.
QJsonObject discardedDetails(Context& context, const QJsonObject& record) {
  return {{"job_id", record.value("job_id")},
          {"kind", record.value("kind")},
          {"final_status", record.value("status")},
          {"discarded_by_operation_id", optionalText(context.writeOperation)},
          {"worker_instance_id", jobRegistry(context).instance()}};
}

// The job_discarded details a journal record of a discarded job keeps.
QJsonObject journalDiscardedDetails(const QJsonObject& stored) {
  const QJsonObject record = stored.value("record").toObject();
  return {{"job_id", stored.value("job_id")},
          {"kind", record.value("kind")},
          {"final_status", record.value("status")},
          {"discarded_by_operation_id",
           record.value("discarded").toObject().value("operation_id")},
          {"worker_instance_id", stored.value("w")}};
}

// The discard as the journal keeps it, newest for its id: the summary of the
// job's terminal record (what job.list shows) with a discarded block, so a
// later worker neither lists nor serves the job and the journal's index keeps
// a few hundred bytes for it. Nothing when the journal never admitted the job.
void journalDiscard(Context& context, const QString& id,
                    const QJsonObject& terminal) {
  QJsonObject record = jobSummary(terminal);
  record.remove("observed_by");
  Journal* journal = workspaceJournal(context);
  if (!journal) return;
  const QJsonObject* stored = journal->job(id);
  if (!stored) return;
  const QJsonValue operation = stored->value("operation_id");
  record.insert(
      "discarded",
      QJsonObject{{"operation_id", optionalText(context.writeOperation)},
                  {"at_ms", double(jobRegistry(context).elapsedMs())}});
  journal->recordJob(
      {{"job_id", id}, {"operation_id", operation}, {"record", record}});
}

QJsonObject discardResult(Context& context, const QJsonObject& record,
                          qint64 freedBytes) {
  return {
      {"job_id", record.value("job_id")},
      {"kind", record.value("kind")},
      {"final_status", record.value("status")},
      {"discarded", true},
      {"freed_bytes", double(freedBytes)},
      {"retained_jobs",
       QJsonObject{{"render", retainedJobs(context, JobLane::Render)},
                   {"analysis", retainedJobs(context, JobLane::Analysis)}}}};
}

// job.discard of a job this process runs: refused while it runs; otherwise
// its files go first, then the registry forgets it and the journal records it.
QJsonValue discardJob(Context& context, const QString& id) {
  JobRegistry& registry    = jobRegistry(context);
  const auto job           = registry.require(id);
  const QJsonObject record = job->record();
  if (!job->terminal())
    throw Error("in_use",
                "The job is still running. Stop it with job.cancel, wait "
                "until job.status reports it terminal, then discard it.",
                {{"reason", "not_terminal"},
                 {"job_id", id},
                 {"status", record.value("status")},
                 {"next", "job.cancel"}});
  context.markMutationStarted();
  const qint64 freed = job->removeOutputs();
  registry.discard(id, discardedDetails(context, record), job->artifactIds());
  journalDiscard(context, id, record);
  return discardResult(context, record, freed);
}

bool ownJob(JobRegistry& registry, const QString& id) {
  return id.startsWith("job_" + registry.instance().left(8) + "_");
}

// A job the registry does not hold: a discarded one, or one of another
// worker of the workspace answered from the journal. Undefined when the
// registry answers (and refuses an unknown id with its reasons).
QJsonValue answeredWithoutRegistry(Context& context, const QString& method,
                                   const QString& id) {
  JobRegistry& registry        = jobRegistry(context);
  const QJsonObject remembered = registry.discarded(id);
  if (!remembered.isEmpty()) refuseDiscarded(remembered);
  Journal* journal = workspaceJournal(context);
  if (registry.find(id) || !journal) return QJsonValue(QJsonValue::Undefined);
  journal->refresh();
  const QJsonObject* stored = journal->job(id);
  if (!stored) return QJsonValue(QJsonValue::Undefined);
  if (discardedRecord(*stored))
    refuseDiscarded(journalDiscardedDetails(*stored));
  // This worker's own ids keep the registry's reasons (expired, not_issued).
  if (ownJob(registry, id)) return QJsonValue(QJsonValue::Undefined);
  return journalJob(context, method, *stored);
}

}  // namespace

int retainedJobs(Context& context, JobLane lane) {
  int retained       = jobRegistry(context).retained(lane);
  const QString name = laneText(lane);
  for (const QJsonObject& record : observedHostJobs(context))
    if (record.value("lane") == name) ++retained;
  return retained;
}

QJsonObject retentionDetails(Context& context, JobLane lane) {
  return jobRegistry(context).retention(lane, observedHostJobs(context));
}

QJsonObject JobRegistry::retention(
    JobLane lane, const QHash<QString, QJsonObject>& hosted) const {
  const QString name = laneText(lane);
  QJsonArray releasable;
  int retained = 0;
  int count    = 0;
  for (const QString& id : m_order) {
    const auto job             = find(id);
    const QJsonObject observed = job ? QJsonObject() : hosted.value(id);
    if (job ? job->lane() != lane : observed.value("lane") != name) continue;
    ++retained;
    if (job ? !job->terminal() : !observed.value("terminal").toBool()) continue;
    if (++count > ReleasableListed) continue;
    const QJsonObject record = job ? job->record() : observed;
    releasable.append(QJsonObject{
        {"job_id", id},
        {"kind", record.value("kind")},
        {"status", record.value("status")},
        {"document_id", record.value("document_id")},
        {"ended_ms", record.value("timing").toObject().value("ended_ms")}});
  }
  return {{"limit", "retained_" + name + "_jobs"},
          {"lane", name},
          {"max_retained_jobs", JobsPerLane},
          {"retained", retained},
          {"releasable", releasable},
          {"releasable_count", count},
          {"next", "job.discard"}};
}

QString retentionMessage(JobLane lane) {
  return "This worker retains " + QString::number(JobsPerLane) + " " +
         laneText(lane) +
         " jobs, its limit. Release finished jobs you no longer need with "
         "job.discard (details.releasable lists the oldest), then submit "
         "again.";
}

namespace {

qint64 treeBytes(const QString& directory) {
  QVector<EntryInfo> entries;
  if (listApprovedDirectory(directory, entries) != ListResult::Listed) return 0;
  qint64 bytes = 0;
  for (const EntryInfo& entry : entries)
    bytes += entry.isDir()    ? treeBytes(entry.absoluteFilePath())
             : entry.isFile() ? entry.size()
                              : 0;
  return bytes;
}

}  // namespace

qint64 removeJobDirectory(const QString& directory) {
  const qint64 bytes = treeBytes(directory);
  if (!removeApprovedTree(directory))
    throw Error("io_error",
                "Some of the job's files could not be removed; discard it "
                "again to remove the rest.",
                {{"phase", "remove_outputs"}});
  return bytes;
}

void refuseDiscardedArtifact(Context& context, const QString& artifactId) {
  JobRegistry& registry = jobRegistry(context);
  const QString job     = registry.discardedArtifactJob(artifactId);
  if (job.isEmpty()) return;
  QJsonObject details = registry.discarded(job);
  if (details.isEmpty())
    details = {{"job_id", job}, {"worker_instance_id", registry.instance()}};
  details.insert("artifact_id", artifactId);
  refuseDiscarded(details);
}

QJsonObject recordHostDiscard(Context& context, const QString& id,
                              QJsonObject record, QJsonObject result) {
  record.insert("status", result.value("final_status"));
  record.insert("terminal", true);
  jobRegistry(context).discard(id, discardedDetails(context, record), {});
  journalDiscard(context, id, record);
  result.insert(
      "retained_jobs",
      QJsonObject{{"render", retainedJobs(context, JobLane::Render)},
                  {"analysis", retainedJobs(context, JobLane::Analysis)}});
  return result;
}

std::shared_ptr<Job> retainedJobForResult(Context& context, const QString& id) {
  answeredWithoutRegistry(context, "job.result", id);
  return jobRegistry(context).require(id);
}

QJsonValue executeJobs(Context& context, const QString& method,
                       const QJsonObject& params) {
  if (method == "job.list") return listJobs(context, params);
  const QString id           = requiredString(params, "job_id");
  const QJsonValue elsewhere = answeredWithoutRegistry(context, method, id);
  if (!elsewhere.isUndefined()) return elsewhere;
  if (method == "job.discard") return discardJob(context, id);
  const auto job = jobRegistry(context).require(id);
  if (method == "job.status") return job->record();
  if (method == "job.result") return job->result(context, params);
  if (method != "job.cancel")
    throw Error("unknown_operation", "Unknown job operation.");
  if (!job->terminal()) context.markMutationStarted();
  job->cancel();
  jobRegistry(context).unsettle(job->id());
  return job->record();
}

}  // namespace GroundzeroAutomation
