#include "automation_jobs.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QTimer>

#include <functional>
#include <memory>
#include <iostream>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

const QString instance = "12345678-1234-4123-8123-123456789abc";

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

Error refused(const std::function<void()>& call) {
  try {
    call();
  } catch (const Error& error) {
    return error;
  }
  throw std::runtime_error("A job registry call was not refused");
}

class FakeJob final : public Job {
public:
  FakeJob(QString id, JobLane lane, bool terminal, qint64 bytes = 0)
      : m_id(std::move(id))
      , m_lane(lane)
      , m_terminal(terminal)
      , m_bytes(bytes) {}
  QString id() const override { return m_id; }
  JobLane lane() const override { return m_lane; }
  QString documentId() const override { return "document_test"; }
  bool terminal() const override { return m_terminal; }
  qint64 reservedBytes() const override { return m_bytes; }
  QJsonObject record() const override {
    return {
        {"job_id", m_id},
        {"status", m_terminal ? "completed" : "running"},
        {"terminal", m_terminal},
        {"cancellation",
         QJsonObject{{"requested", m_cancelMs >= 0},
                     {"requested_ms",
                      m_cancelMs >= 0 ? QJsonValue(m_cancelMs) : QJsonValue()},
                     {"effective", QJsonValue()}}}};
  }
  void cancel() override {
    if (m_cancelMs < 0) m_cancelMs = m_interruptMs >= 0 ? m_interruptMs : 1;
  }
  void interrupt(qint64 requestedMs) override {
    if (m_interruptMs < 0) m_interruptMs = requestedMs;
  }
  QJsonValue result(Context&, const QJsonObject&) override { return {}; }
  qint64 interruptMs() const { return m_interruptMs; }
  qint64 cancelMs() const { return m_cancelMs; }

private:
  QString m_id;
  JobLane m_lane;
  bool m_terminal;
  qint64 m_bytes;
  qint64 m_interruptMs = -1;
  qint64 m_cancelMs    = -1;
};

void identifiers() {
  JobRegistry registry(instance, 0);
  const QRegularExpression job(QRegularExpression::anchoredPattern(
      "job_12345678_[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-"
      "[0-9a-f]{12}"));
  const QString id = registry.newJobId();
  require(job.match(id).hasMatch() && id.size() <= JobIdMaximumSize,
          "A job id does not carry its worker prefix within 80 characters");
  require(registry.newJobId() != id, "Two job ids are equal");
  require(registry.newArtifactId().startsWith("artifact_12345678_"),
          "An artifact id does not carry its worker prefix");
}

void lanes() {
  JobRegistry registry(instance, 0);
  const QString render = registry.newJobId();
  registry.add(std::make_shared<FakeJob>(render, JobLane::Render, false));
  require(registry.active(JobLane::Render) == render &&
              registry.active(JobLane::Analysis).isEmpty(),
          "A running render occupied the wrong lane");
  registry.admitAnalysis({}, 0);
  registry.add(
      std::make_shared<FakeJob>(registry.newJobId(), JobLane::Analysis, false));
  const Error analysis = refused([&]() { registry.admitAnalysis({}, 0); });
  require(analysis.code == "analysis_busy" &&
              analysis.details.contains("active_job_id"),
          "A second analysis was admitted while one runs");
  const Error hosted =
      refused([&]() { JobRegistry(instance, 0).admitAnalysis("host", 0); });
  require(hosted.code == "analysis_busy" &&
              hosted.details.value("active_job_id") == "host",
          "A scene host's active analysis did not occupy the analysis lane");
}

void retention() {
  JobRegistry registry(instance, 0);
  for (int index = 0; index < JobsPerLane; ++index)
    registry.add(
        std::make_shared<FakeJob>(registry.newJobId(), JobLane::Render, true));
  require(registry.retained(JobLane::Render) == JobsPerLane &&
              registry.retained(JobLane::Analysis) == 0,
          "Retained render jobs were counted in the wrong lane");
  registry.admitAnalysis({}, 0);
  for (int index = 0; index < JobsPerLane; ++index)
    registry.add(std::make_shared<FakeJob>(registry.newJobId(),
                                           JobLane::Analysis, true));
  const Error full = refused([&]() { registry.admitAnalysis({}, 0); });
  require(full.code == "resource_limit" &&
              full.details.value("limit") == "retained_analysis_jobs",
          "A 33rd analysis job was admitted");
  JobRegistry hosted(instance, 0);
  require(refused([&]() { hosted.admitAnalysis({}, JobsPerLane); }).code ==
              "resource_limit",
          "Scene hosts' retained analysis jobs did not fill the lane");
  JobRegistry storage(instance, 0);
  storage.add(std::make_shared<FakeJob>(storage.newJobId(), JobLane::Render,
                                        true, 100 * 1024 * 1024));
  storage.add(std::make_shared<FakeJob>(storage.newJobId(), JobLane::Analysis,
                                        true, 7));
  require(storage.reservedBytes(JobLane::Render) == 100 * 1024 * 1024 &&
              storage.reservedBytes(JobLane::Analysis) == 7,
          "Reserved storage was summed across lanes");
}

QString reason(const JobRegistry& registry, const QString& id) {
  const Error error = refused([&]() { registry.require(id); });
  require(error.code == "unknown_job" &&
              error.details.value("worker_instance_id") == instance,
          "An unknown job id was refused with another code");
  return error.details.value("reason").toString();
}

void unknownIds() {
  JobRegistry registry(instance, 0);
  const QString issued = registry.newJobId();
  require(reason(registry, issued) == "not_issued",
          "A never-admitted id of this worker was not not_issued");
  require(reason(registry, "job_87654321_" + issued.mid(13)) == "other_worker",
          "Another worker's id was not other_worker");
  require(reason(registry, "unknown") == "other_worker",
          "An id without this worker's prefix was not other_worker");
  registry.note(issued);
  registry.expire({issued});
  require(
      reason(registry, issued) == "expired" && registry.newestFirst().isEmpty(),
      "An expired id was listed or not reported as expired");
  for (int index = 0; index < ExpiredJobIds; ++index)
    registry.expire({registry.newJobId()});
  require(reason(registry, issued) == "not_issued",
          "More than 256 expired ids were kept");
}

void order() {
  JobRegistry registry(instance, 0);
  const QString first  = registry.newJobId();
  const QString second = registry.newJobId();
  registry.add(std::make_shared<FakeJob>(first, JobLane::Render, true));
  registry.note(second);
  registry.note(first);
  require(registry.newestFirst() == QStringList({second, first}),
          "Jobs are not listed newest first in the order first seen");
}

QJsonObject recordFor(Context& context, const QString& status, bool terminal,
                      bool requested, const QStringList& forced = {}) {
  JobCommon common;
  common.id              = "job_test";
  common.kind            = "render_sequence";
  common.cancelRequested = requested;
  common.cancelMs        = requested ? 5 : -1;
  common.forced          = forced;
  return commonRecord(context, common, status, terminal, {}, {}, {});
}

void cancellationAndTermination(Context& context) {
  const auto cancellation = [&](const QString& status, bool terminal,
                                bool requested) {
    return recordFor(context, status, terminal, requested)
        .value("cancellation")
        .toObject()
        .value("effective");
  };
  require(cancellation("running", false, false).isNull() &&
              cancellation("cancellation_requested", false, true).isNull() &&
              cancellation("canceled", true, true) == true &&
              cancellation("completed", true, true) == false,
          "cancellation.effective does not follow the outcome");
  const auto mode = [&](const QString& status, bool terminal,
                        const QStringList& forced) {
    return recordFor(context, status, terminal, false, forced)
        .value("termination")
        .toObject()
        .value("mode");
  };
  require(mode("running", false, {}).isNull() &&
              mode("completed", true, {}).isNull() &&
              mode("canceled", true, {}) == "cooperative" &&
              mode("failed", true, {"encoder"}) == "forced" &&
              mode("deadline_exceeded", true, {}) == "cooperative",
          "termination.mode does not follow the outcome and kills");
  const QJsonObject record = recordFor(context, "running", false, true);
  require(record.value("cancellation").toObject().value("requested_ms") == 5 &&
              record.value("plugin_policy") == "builtin_only" &&
              record.value("worker").toObject().value("instance_id") ==
                  jobRegistry(context).instance(),
          "The common record lost its request time, policy or worker");
}

}  // namespace

QString statusOf(const QJsonValue& record) {
  return record.toObject().value("status").toString();
}

bool requested(const QJsonValue& record) {
  return record.toObject()
      .value("cancellation")
      .toObject()
      .value("requested")
      .toBool();
}

void publishedPolls() {
  JobRegistry registry(instance, 0);
  const auto job =
      std::make_shared<FakeJob>(registry.newJobId(), JobLane::Render, false);
  registry.add(job);
  require(registry.answerPoll(job->id(), false).isNull(),
          "A poll was answered before the job's record was published");
  registry.publish();
  require(statusOf(registry.answerPoll(job->id(), false)) == "running",
          "A published record was not answered off the application thread");
  require(registry.answerPoll("job_12345678_unknown", true).isNull(),
          "A cancellation of an unpublished job was answered off-thread");
  const QJsonValue canceled = registry.answerPoll(job->id(), true);
  require(statusOf(canceled) == "cancellation_requested" &&
              requested(canceled) && job->interruptMs() >= 0,
          "An off-thread cancellation did not interrupt the job at once");
  registry.publish();
  require(statusOf(registry.answerPoll(job->id(), false)) ==
                  "cancellation_requested" &&
              job->cancelMs() < 0,
          "Publishing before the queued cancel ran lost the request");
  QCoreApplication::processEvents();
  require(job->cancelMs() == job->interruptMs(),
          "The queued cancel did not record the off-thread request time");
  registry.publish();
  require(requested(registry.answerPoll(job->id(), false)),
          "The job's own record did not replace the request view");
}

// Ends one event-loop turn after it is canceled, as a job whose writers
// close asynchronously does; a stubborn one never ends.
class ClosingJob final : public Job {
public:
  ClosingJob(QString id, JobLane lane, bool stubborn)
      : m_id(std::move(id)), m_lane(lane), m_stubborn(stubborn) {}
  QString id() const override { return m_id; }
  JobLane lane() const override { return m_lane; }
  QString documentId() const override { return "document_test"; }
  bool terminal() const override { return *m_terminal; }
  qint64 reservedBytes() const override { return 0; }
  QJsonObject record() const override { return {{"job_id", m_id}}; }
  void cancel() override {
    canceled = true;
    if (cancelMs < 0) cancelMs = interruptMs >= 0 ? interruptMs : 1000;
    if (m_stubborn) return;
    const auto terminal = m_terminal;
    QTimer::singleShot(30, QCoreApplication::instance(),
                       [terminal]() { *terminal = true; });
  }
  void interrupt(qint64 requestedMs) override {
    if (interruptMs < 0) interruptMs = requestedMs;
  }
  QJsonValue result(Context&, const QJsonObject&) override { return {}; }
  bool canceled      = false;
  qint64 interruptMs = -1;
  qint64 cancelMs    = -1;

private:
  QString m_id;
  JobLane m_lane;
  bool m_stubborn;
  std::shared_ptr<bool> m_terminal = std::make_shared<bool>(false);
};

void shutdownSettlement(Context& context) {
  const auto saved      = context.jobs;
  context.jobs          = std::make_shared<JobRegistry>(instance, 0);
  JobRegistry& registry = *context.jobs;
  auto render =
      std::make_shared<ClosingJob>(registry.newJobId(), JobLane::Render, false);
  auto analysis = std::make_shared<ClosingJob>(registry.newJobId(),
                                               JobLane::Analysis, false);
  registry.add(render);
  registry.add(analysis);
  registry.add(
      std::make_shared<FakeJob>(registry.newJobId(), JobLane::Render, true));
  registry.publish();
  require(!registry.answerPoll(render->id(), true).isNull() &&
              render->interruptMs >= 0,
          "A poll cancellation did not interrupt the job");
  require(settleJobs(context, 5000) && render->canceled && analysis->canceled &&
              render->terminal() && analysis->terminal(),
          "Shutdown did not end the jobs of both lanes through the registry");
  require(render->cancelMs == render->interruptMs,
          "A poll cancellation and the shutdown recorded two cancellations");
  auto stubborn = std::make_shared<ClosingJob>(registry.newJobId(),
                                               JobLane::Analysis, true);
  registry.add(stubborn);
  QElapsedTimer elapsed;
  elapsed.start();
  require(!settleJobs(context, 100) && stubborn->canceled &&
              elapsed.elapsed() < 2000,
          "Shutdown waited past its budget for a job that does not end");
  context.jobs = saved;
}

void runAutomationJobsTests(Context& context) {
  const std::pair<const char*, std::function<void()>> cases[]{
      {"identifiers", identifiers},
      {"lanes", lanes},
      {"retention", retention},
      {"unknown ids", unknownIds},
      {"order", order},
      {"published polls", publishedPolls},
      {"cancellation and termination",
       [&context]() { cancellationAndTermination(context); }},
      {"shutdown settlement", [&context]() { shutdownSettlement(context); }}};
  for (const auto& test : cases) {
    test.second();
    std::cout << "automation-jobs-test: " << test.first << " PASS\n";
  }
}

}  // namespace GroundzeroAutomation
