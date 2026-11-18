#include "automation_jobs.h"

#include "automation_build_identity.h"
#include "automation_scene_host.h"

#include <QUuid>

#include <algorithm>

namespace GroundzeroAutomation {
namespace {

const QStringList& jobKinds() {
  static const QStringList kinds{"render_preview", "render_sequence",
                                 "neural_preview", "drawing_analysis",
                                 "scene_check"};
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

QJsonValue listJobs(Context& context, const QJsonObject& params) {
  const QJsonArray records = retainedRecords(context);
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

void JobRegistry::admitAnalysis(const QString& hostActive,
                                int hostRetained) const {
  const QString local = active(JobLane::Analysis);
  const QString busy  = local.isEmpty() ? hostActive : local;
  if (!busy.isEmpty())
    throw Error("analysis_busy",
                "Wait for the active analysis job to finish or cancel it.",
                {{"active_job_id", busy}});
  if (retained(JobLane::Analysis) + hostRetained >= JobsPerLane)
    throw Error("resource_limit",
                "This worker has reached its analysis job limit.",
                {{"limit", "retained_analysis_jobs"}});
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
  summary.insert("observed_by", "worker");
  return summary;
}

QJsonArray jobOperations() {
  const QJsonObject job{{"job_id", jobIdSchema()}};
  QJsonObject cancel = operation(
      "job.cancel",
      "Request cancellation of a job and return its record; poll job.status "
      "until terminal. A job that ends completed despite the request reports "
      "cancellation.effective false.",
      job, {"job_id"}, true);
  cancel.insert("mutates_document", false);
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
                    result, {"job_id"})};
}

QJsonValue executeJobs(Context& context, const QString& method,
                       const QJsonObject& params) {
  if (method == "job.list") return listJobs(context, params);
  const auto job =
      jobRegistry(context).require(requiredString(params, "job_id"));
  if (method == "job.status") return job->record();
  if (method == "job.result") return job->result(context, params);
  if (method != "job.cancel")
    throw Error("unknown_operation", "Unknown job operation.");
  if (!job->terminal()) context.markMutationStarted();
  job->cancel();
  return job->record();
}

}  // namespace GroundzeroAutomation
