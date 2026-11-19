#include "automation_journal.h"

#include "automation.h"

#include <algorithm>
#include <limits>

namespace GroundzeroAutomation {
namespace {

constexpr int MaximumListedOperations  = 64;
constexpr int DefaultListedOperations  = 32;
constexpr int MaximumScannedOperations = 262144;
constexpr double MaximumInteger        = 9007199254740991.0;

Journal& readableJournal(Context& context) {
  Journal* journal = workspaceJournal(context);
  if (!journal || !journal->readable())
    throw Error("journal_unavailable",
                "This worker has no readable workspace journal; inspect "
                "journal.status.",
                {{"reason", journal ? journal->unavailableReason()
                                    : QString("io_error")}});
  journal->refresh();
  return *journal;
}

QJsonValue optionalText(const QString& text) {
  return text.isEmpty() ? QJsonValue() : QJsonValue(text);
}

QJsonValue sequence(qint64 seq) {
  return seq > 0 ? QJsonValue(double(seq)) : QJsonValue();
}

// completed, failed, in_progress, interrupted or expired.
QString operationState(const Journal& journal,
                       const JournalOperation& operation) {
  if (!operation.stubState.isEmpty()) return "expired";
  if (!operation.outcome.isEmpty())
    return operation.outcome.value("state").toString();
  return journal.writerAlive(operation.writer) ? "in_progress" : "interrupted";
}

QString retryAnswer(const QString& state, bool authorityMatches) {
  if (!authorityMatches) return "returns_recorded_error";
  if (state == "completed") return "returns_recorded_result";
  if (state == "failed") return "returns_recorded_error";
  if (state == "in_progress") return "refused_in_progress";
  if (state == "interrupted") return "refused_interrupted";
  return state == "expired" ? "refused_expired" : "executes";
}

QJsonObject recordedBy(const Journal& journal, const QString& writer) {
  return {{"worker_instance_id", writer},
          {"current", writer == journal.instance()},
          {"alive", journal.writerAlive(writer)}};
}

QJsonValue outcomeSummary(const JournalOperation& operation) {
  const QJsonObject& outcome = operation.outcome;
  if (outcome.isEmpty()) return QJsonValue();
  return QJsonObject{{"code", outcome.value("code")},
                     {"revision", outcome.value("revision")},
                     {"job_id", outcome.value("job_id")},
                     {"receipt_bytes",
                      std::min(8388608.0, outcome.value("bytes").toDouble())}};
}

QJsonObject journalSummary(const Journal& journal) {
  return {{"state", journal.state()},
          {"horizon_seq", sequence(journal.horizonSeq())}};
}

QJsonObject operationStatus(Context& context, const QJsonObject& params) {
  const QString id      = requiredString(params, "operation_id");
  Journal& journal      = readableJournal(context);
  const auto* operation = journal.find(id);
  QJsonObject result{
      {"operation_id", id},
      {"state", journal.state() == "damaged" ? "unknown_damaged" : "not_found"},
      {"method", QJsonValue()},
      {"authority_matches", QJsonValue()},
      {"arguments_sha256", QJsonValue()},
      {"recorded_by", QJsonValue()},
      {"admitted_seq", QJsonValue()},
      {"recorded_at_ms", QJsonValue()},
      {"document_id", QJsonValue()},
      {"outcome", QJsonValue()},
      {"verdict", QJsonValue()},
      {"retry", "executes"},
      {"journal", journalSummary(journal)}};
  if (!operation) return result;
  const QString state = operationState(journal, *operation);
  const bool matches  = operation->authorityDigest == journal.authorityDigest();
  result.insert("state", state);
  result.insert("method", operation->method);
  result.insert("authority_matches", matches);
  result.insert("arguments_sha256", operation->argumentsSha256);
  result.insert("recorded_by", recordedBy(journal, operation->writer));
  result.insert("admitted_seq", double(operation->admittedSeq));
  result.insert("recorded_at_ms", double(operation->admittedAtMs));
  result.insert("document_id", optionalText(operation->documentId));
  result.insert("outcome", outcomeSummary(*operation));
  if (state == "interrupted")
    result.insert("verdict", journal.verdict(*operation));
  result.insert("retry", retryAnswer(state, matches));
  return result;
}

bool listed(const QString& filter, const QString& state) {
  if (filter == "any") return true;
  if (filter == "unresolved")
    return state == "in_progress" || state == "interrupted";
  return filter == state;
}

bool fromWorker(const QString& filter, bool current) {
  return filter == "any" || (filter == "current") == current;
}

QJsonObject operationRow(const Journal& journal,
                         const JournalOperation& operation,
                         const QString& state) {
  return {{"operation_id", operation.operationId},
          {"method", operation.method},
          {"state", state},
          {"recorded_by", recordedBy(journal, operation.writer)},
          {"admitted_seq", double(operation.admittedSeq)},
          {"recorded_at_ms", double(operation.admittedAtMs)},
          {"document_id", optionalText(operation.documentId)},
          {"verdict_kind", state == "interrupted"
                               ? journal.verdict(operation).value("kind")
                               : QJsonValue()},
          {"authority_matches",
           operation.authorityDigest == journal.authorityDigest()}};
}

QJsonObject listOperations(Context& context, const QJsonObject& params) {
  const QString stateFilter  = params.value("state").toString("unresolved");
  const QString workerFilter = params.value("worker").toString("any");
  const int limit = integerParam(params, "limit", 1, MaximumListedOperations,
                                 DefaultListedOperations);
  const double before = params.value("before_seq").toDouble(MaximumInteger + 1);
  Journal& journal    = readableJournal(context);
  QJsonArray rows;
  QJsonValue next;
  int scanned   = 0;
  bool complete = true;
  for (const JournalOperation* operation : journal.operationsNewestFirst()) {
    if (double(operation->admittedSeq) >= before) continue;
    if (scanned >= MaximumScannedOperations) {
      complete = false;
      break;
    }
    ++scanned;
    const QString state = operationState(journal, *operation);
    if (!listed(stateFilter, state) ||
        !fromWorker(workerFilter, operation->writer == journal.instance()))
      continue;
    if (rows.size() == limit) {
      next = rows.last().toObject().value("admitted_seq");
      break;
    }
    rows.append(operationRow(journal, *operation, state));
  }
  return {{"operations", rows},
          {"next_before_seq", next},
          {"scanned", scanned},
          {"complete", complete},
          {"horizon_seq", sequence(journal.horizonSeq())},
          {"journal_state", journal.state()}};
}

QJsonObject journalStatus(Context& context) {
  Journal* journal = workspaceJournal(context);
  if (journal) {
    journal->refresh();
    return journal->status();
  }
  return Journal::absentStatus();
}

QJsonObject stateSchema() {
  return {{"type", "string"},
          {"enum", QJsonArray{"unresolved", "failed", "completed", "any"}}};
}

}  // namespace

QJsonArray journalOperations() {
  const QJsonObject id{
      {"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
  const QJsonObject list{
      {"state", stateSchema()},
      {"worker", QJsonObject{{"type", "string"},
                             {"enum", QJsonArray{"current", "others", "any"}}}},
      {"before_seq",
       QJsonObject{
           {"type", "integer"}, {"minimum", 1}, {"maximum", MaximumInteger}}},
      {"limit", QJsonObject{{"type", "integer"},
                            {"minimum", 1},
                            {"maximum", MaximumListedOperations}}}};
  return {
      operation("operation.status",
                "Report what the workspace journal recorded for one "
                "operation_id: completed or failed, in progress in a live "
                "worker, interrupted by the loss of its worker (with the "
                "reconciliation verdict), expired or not found, and what an "
                "exact retry would do now. Never executes or writes anything.",
                {{"operation_id", id}}, {"operation_id"}),
      operation("journal.status",
                "Report the workspace journal's health: format, size against "
                "its limits, retention horizon, the workers that wrote it and "
                "whether they are alive, the last start-up recovery and any "
                "damage.",
                {}, {}),
      operation("operation.list",
                "List operations the workspace journal holds, newest first, "
                "by state (default unresolved: in progress or interrupted) and "
                "worker; page with next_before_seq until it is null.",
                list, {})};
}

QJsonValue executeJournal(Context& context, const QString& method,
                          const QJsonObject& params) {
  if (method == "operation.status") return operationStatus(context, params);
  if (method == "journal.status") return journalStatus(context);
  if (method == "operation.list") return listOperations(context, params);
  throw Error("method_not_found", "The journal operation is not available.");
}

}  // namespace GroundzeroAutomation
