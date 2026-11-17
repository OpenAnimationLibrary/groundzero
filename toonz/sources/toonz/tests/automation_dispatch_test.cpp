#include "automation_dispatch.h"

#include <QJsonDocument>

#include <iostream>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QJsonObject call(Dispatcher& dispatcher, const QString& method,
                 const QJsonObject& params) {
  return dispatcher.dispatch(
      {{"id", "receipt-fixture"}, {"method", method}, {"params", params}});
}

QJsonObject result(Dispatcher& dispatcher, const QString& method,
                   const QJsonObject& params) {
  const QJsonObject response = call(dispatcher, method, params);
  require(response.contains("result"), "Receipt fixture operation failed");
  return response.value("result").toObject();
}

void rejected(Dispatcher& dispatcher, const QString& method,
              const QJsonObject& params, const QString& code) {
  require(call(dispatcher, method, params)
                  .value("error")
                  .toObject()
                  .value("code") == code,
          "Receipt fixture received an unexpected rejection");
}

QJsonValue echoString(Context&, const QString&, const QJsonObject& params) {
  return params;
}

QJsonArray unicodeOperations() {
  return {operation("fixture.unicode", "Private Unicode length boundary",
                    {{"value", QJsonObject{{"type", "string"},
                                          {"minLength", 2},
                                          {"maxLength", 2}}}},
                    {"value"}, false)};
}

void unicodeBoundaries(Dispatcher& dispatcher) {
  const QString rocket = QString::fromUtf8("\xf0\x9f\x9a\x80");
  const QString combining = QString::fromUtf8("e\xcc\x81");
  for (const QString& value : {QString("ab"), rocket + "a", combining}) {
    const QJsonObject params{{"value", value}};
    require(result(dispatcher, "fixture.unicode", params) == params,
            "Unicode scalar values did not survive schema validation");
  }
  for (const QString& value : {QString(), QString("a"), rocket,
                               rocket + "ab", QString(3, QChar('a'))})
    rejected(dispatcher, "fixture.unicode", {{"value", value}}, "invalid_params");
  for (const QString& value : {QString(QChar(0xd800)), QString(QChar(0xdc00)),
                               QString(QChar(0xd800)) + "a",
                               QString("a") + QChar(0xdc00),
                               QString(QChar(0xdc00)) + QChar(0xd800)})
    rejected(dispatcher, "fixture.unicode", {{"value", value}}, "invalid_params");
}

QJsonValue largeResult(Context&, const QString&, const QJsonObject&) {
  return QJsonObject{{"payload", QString(7 * 1024 * 1024, QChar('x'))}};
}

QJsonArray boundaryOperations() {
  QJsonObject descriptor =
      operation("fixture.receipt", "Private receipt boundary", {}, {}, true);
  descriptor.insert("mutates_document", false);
  return {descriptor};
}

void fillByteQuota(Dispatcher& dispatcher) {
  const QJsonObject first{{"operation_id", "payload-0"}};
  const QJsonObject original = call(dispatcher, "fixture.receipt", first);
  require(original.contains("result"), "First large receipt failed");
  int accepted = 1;
  for (; accepted < 16; ++accepted) {
    const QJsonObject response =
        call(dispatcher, "fixture.receipt",
             {{"operation_id", "payload-" + QString::number(accepted)}});
    if (response.contains("error")) {
      require(
          response.value("error").toObject().value("code") == "resource_limit",
          "Byte exhaustion returned the wrong error");
      break;
    }
  }
  require(accepted == 8 || accepted == 9,
          "Byte quota did not enforce the 64 MiB receipt bound");
  require(call(dispatcher, "fixture.receipt", first) == original,
          "Old large receipt changed after byte exhaustion");
  rejected(dispatcher, "scene.create", first, "operation_conflict");
}

QJsonObject startJob(Dispatcher& dispatcher) {
  const QJsonObject document = result(dispatcher, "scene.create",
                                      {{"operation_id", "create"},
                                       {"name", "Receipt bytes"},
                                       {"width_px", 32},
                                       {"height_px", 32},
                                       {"frame_rate", 24}});
  const QJsonObject job =
      result(dispatcher, "render.submit",
             {{"operation_id", "submit"},
              {"document_id", document.value("document_id")},
              {"expected_revision", document.value("revision")},
              {"start_frame", 0},
              {"end_frame", 119},
              {"width", 32},
              {"height", 32}});
  require(job.value("status") == "preparing" &&
              !job.value("writers_closed").toBool(),
          "Receipt boundary fixture did not create a pending native render");
  return job;
}

void rejectUnknownJobs(Dispatcher& dispatcher) {
  for (int index = 0; index < 40; ++index)
    rejected(dispatcher, "render.cancel",
             {{"operation_id", "unknown-" + QString::number(index)},
              {"job_id", "missing-" + QString::number(index)}},
             "unknown_job");
}

void verifyReplay(Dispatcher& dispatcher, const QJsonObject& params,
                  const QJsonObject& receipt) {
  require(call(dispatcher, "render.cancel", params) == receipt,
          "Cancellation receipt changed on replay");
  QJsonObject changed = params;
  changed.insert("job_id", "different-job");
  rejected(dispatcher, "render.cancel", changed, "operation_conflict");
  rejected(dispatcher, "scene.create",
           {{"operation_id", params.value("operation_id")}},
           "operation_conflict");
  changed = params;
  changed.insert("operation_id", "second-cancel-id");
  rejected(dispatcher, "render.cancel", changed, "resource_limit");
}

void closeJob(Context& context, Dispatcher& dispatcher,
              const QJsonObject& job) {
  const QJsonObject before =
      result(dispatcher, "render.status", {{"job_id", job.value("job_id")}});
  require(before.value("status") == "cancellation_requested" ||
              before.value("status") == "canceled",
          "The tested operation did not request native cancellation");
  shutdownRender(context);
  const QJsonObject status =
      result(dispatcher, "render.status", {{"job_id", job.value("job_id")}});
  require(status.value("status") == "canceled", "Native render did not cancel");
  require(status.value("writers_closed").toBool(),
          "Native writers survived cancellation");
  context.removeDocument(job.value("document_id").toString());
}

QString bytePressure(Context& context, Dispatcher& dispatcher) {
  const QJsonObject job = startJob(dispatcher);
  fillByteQuota(dispatcher);
  rejectUnknownJobs(dispatcher);
  const QJsonObject params{{"operation_id", "cancel"},
                           {"job_id", job.value("job_id")}};
  const QJsonObject receipt = call(dispatcher, "render.cancel", params);
  require(receipt.contains("result"),
          "Byte quota blocked the first native cancellation");
  closeJob(context, dispatcher, job);
  verifyReplay(dispatcher, params, receipt);
  return job.value("job_id").toString();
}

QJsonValue escapedCancellation(Context& context, const QString& method,
                               const QJsonObject& params) {
  QJsonObject response = executeRender(context, method, params).toObject();
  response.insert("error", QString(2048, QChar(1)));
  return response;
}

void terminalPressure(Dispatcher& dispatcher, const QString& jobId) {
  fillByteQuota(dispatcher);
  const QJsonObject params{{"operation_id", QString(128, QChar(1))},
                           {"job_id", jobId}};
  const QJsonObject receipt = call(dispatcher, "render.cancel", params);
  require(receipt.contains("result"),
          "Terminal first cancellation lost its reservation");
  const int bytes =
      QJsonDocument(receipt.value("result").toObject()).toJson().size();
  require(bytes > 12 * 1024 && bytes < 16 * 1024,
          "Escaped cancellation receipt did not exercise the serialized byte "
          "bound");
  verifyReplay(dispatcher, params, receipt);
}

QJsonValue oversizedCancellation(Context& context, const QString& method,
                                 const QJsonObject& params) {
  executeRender(context, method, params);
  throw Error("native_error", QString(32 * 1024, QChar(1)));
}

QJsonValue oversizedResult(Context& context, const QString& method,
                           const QJsonObject& params) {
  QJsonObject response = executeRender(context, method, params).toObject();
  response.insert("error", QString(32 * 1024, QChar(1)));
  return response;
}

void oversizedResultPressure(Context& context, Dispatcher& dispatcher) {
  const QJsonObject job = startJob(dispatcher);
  fillByteQuota(dispatcher);
  const QJsonObject params{{"operation_id", "cancel-result"},
                           {"job_id", job.value("job_id")}};
  const QJsonObject receipt = call(dispatcher, "render.cancel", params);
  const QJsonObject error   = receipt.value("error").toObject();
  require(error.value("code") == "response_too_large",
          "Oversized successful cancellation result was not bounded");
  const QJsonObject details = error.value("details").toObject();
  require(details.value("outcome") == "completed" &&
              details.value("job_id") == job.value("job_id"),
          "Oversized cancellation result lost its completion evidence");
  require(QJsonDocument(receipt).toJson().size() < 16 * 1024,
          "Oversized result fallback exceeded reserved receipt storage");
  closeJob(context, dispatcher, job);
  verifyReplay(dispatcher, params, receipt);
}

void partialCancellation(Context& context, Dispatcher& dispatcher) {
  const QJsonObject job = startJob(dispatcher);
  fillByteQuota(dispatcher);
  const QJsonObject params{{"operation_id", "cancel-partial"},
                           {"job_id", job.value("job_id")}};
  const QJsonObject receipt = call(dispatcher, "render.cancel", params);
  require(
      receipt.value("error").toObject().value("code") == "partially_applied",
      "Post-cancel failure lost its explicit partial outcome");
  const QJsonObject details =
      receipt.value("error").toObject().value("details").toObject();
  require(details.value("diagnostic_truncated").toBool() &&
              details.value("outcome") == "unknown",
          "Oversized cancellation diagnostic lost its explicit outcome");
  require(QJsonDocument(receipt).toJson().size() < 16 * 1024,
          "Post-cancel failure exceeded reserved receipt storage");
  closeJob(context, dispatcher, job);
  verifyReplay(dispatcher, params, receipt);
}

}  // namespace

void runAutomationDispatcherTests(Context& context) {
  {
    Dispatcher dispatcher(context);
    dispatcher.add(unicodeOperations(), echoString);
    unicodeBoundaries(dispatcher);
  }
  QString jobId;
  {
    Dispatcher dispatcher(context);
    dispatcher.add(boundaryOperations(), largeResult);
    dispatcher.m_entries["render.cancel"].handler = oversizedResult;
    oversizedResultPressure(context, dispatcher);
  }
  {
    Dispatcher dispatcher(context);
    dispatcher.add(boundaryOperations(), largeResult);
    jobId = bytePressure(context, dispatcher);
  }
  {
    Dispatcher dispatcher(context);
    dispatcher.add(boundaryOperations(), largeResult);
    dispatcher.m_entries["render.cancel"].handler = escapedCancellation;
    terminalPressure(dispatcher, jobId);
  }
  {
    Dispatcher dispatcher(context);
    dispatcher.add(boundaryOperations(), largeResult);
    dispatcher.m_entries["render.cancel"].handler = oversizedCancellation;
    partialCancellation(context, dispatcher);
  }
  std::cout << "automation-dispatch-test: byte-pressure cancellation PASS"
            << std::endl;
}

}  // namespace GroundzeroAutomation
