#include "automation_dispatch.h"
#include "automation_drawing.h"

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

void unicodeOperationIds(Context& context, Dispatcher& dispatcher) {
  const QString rocket = QString::fromUtf8("\xf0\x9f\x9a\x80");
  const QString operationId = rocket.repeated(128);
  QJsonObject params{{"operation_id", operationId}, {"name", "Unicode receipt"},
                     {"width_px", 32}, {"height_px", 32}, {"frame_rate", 24}};
  const int before = context.documents().size();
  const auto created = result(dispatcher, "scene.create", params);
  require(created.value("operation_id") == operationId &&
              context.documents().size() == before + 1,
          "128 supplementary operation ID characters did not create exactly one scene");
  require(result(dispatcher, "scene.create", params) == created &&
              context.documents().size() == before + 1,
          "Unicode operation ID replay created a duplicate scene");
  params.insert("name", "Conflicting Unicode receipt");
  rejected(dispatcher, "scene.create", params, "operation_conflict");
  params.insert("operation_id", operationId + rocket);
  rejected(dispatcher, "scene.create", params, "invalid_params");
  require(context.documents().size() == before + 1,
          "Rejected Unicode operation IDs changed the document inventory");
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
    rejected(dispatcher, "job.cancel",
             {{"operation_id", "unknown-" + QString::number(index)},
              {"job_id", "missing-" + QString::number(index)}},
             "unknown_job");
}

void verifyReplay(Dispatcher& dispatcher, const QJsonObject& params,
                  const QJsonObject& receipt) {
  require(call(dispatcher, "job.cancel", params) == receipt,
          "Cancellation receipt changed on replay");
  QJsonObject changed = params;
  changed.insert("job_id", "different-job");
  rejected(dispatcher, "job.cancel", changed, "operation_conflict");
  rejected(dispatcher, "scene.create",
           {{"operation_id", params.value("operation_id")}},
           "operation_conflict");
  changed = params;
  changed.insert("operation_id", "second-cancel-id");
  rejected(dispatcher, "job.cancel", changed, "resource_limit");
}

void closeJob(Context& context, Dispatcher& dispatcher,
              const QJsonObject& job) {
  const QJsonObject before =
      result(dispatcher, "job.status", {{"job_id", job.value("job_id")}});
  require(before.value("status") == "cancellation_requested" ||
              before.value("status") == "canceled",
          "The tested operation did not request native cancellation");
  shutdownRender(context);
  const QJsonObject status =
      result(dispatcher, "job.status", {{"job_id", job.value("job_id")}});
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
  const QJsonObject receipt = call(dispatcher, "job.cancel", params);
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
  const QJsonObject receipt = call(dispatcher, "job.cancel", params);
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
  const QJsonObject receipt = call(dispatcher, "job.cancel", params);
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
  const QJsonObject receipt = call(dispatcher, "job.cancel", params);
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

QString domainToolset(const QString& name) {
  if (name.startsWith("audio.")) return "audio";
  if (name.startsWith("storyboard.")) return "storyboard";
  if (name.startsWith("lipsync.")) return "lipsync";
  if (name.startsWith("rig.") || name.startsWith("hook.") ||
      name.startsWith("offset_layer.") || name == "object.handles.set" ||
      name == "object.pivot.set")
    return "rigging";
  if (name == "shared.roots" || name.startsWith("studio_palette.") ||
      name.startsWith("library."))
    return "shared-library";
  if (name == "fx.recipe.list") return "fx-finishing";
  return "core";
}

bool patternHandlerRan = false;

QJsonValue patternHandler(Context&, const QString&, const QJsonObject&) {
  patternHandlerRan = true;
  return QJsonObject{};
}

QJsonArray patternOperations() {
  return {operation(
      "fixture.pattern", "Private pattern boundary",
      {{"suffix", QJsonObject{{"type", "string"}, {"pattern", "^[a-z]{0,8}$"}}},
       {"revision",
        QJsonObject{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]*)$"}}}},
      {"suffix"}, false)};
}

void patternBoundaries(Dispatcher& dispatcher) {
  for (const QString& suffix : {QString(), QString("a"), QString("abcdefgh")}) {
    patternHandlerRan = false;
    result(dispatcher, "fixture.pattern", {{"suffix", suffix}});
    require(patternHandlerRan, "A matching string did not reach its handler");
  }
  const QStringList invalid{"a\n", "a\r", "a\r\n",     "\na",
                            "\n",  "A",   "abcdefghi", "a b"};
  for (const QString& suffix : invalid) {
    patternHandlerRan = false;
    const auto response =
        call(dispatcher, "fixture.pattern", {{"suffix", suffix}});
    require(
        response.value("error").toObject().value("code") == "invalid_params",
        "A pattern violation was not an invalid_params refusal");
    require(!patternHandlerRan, "A pattern violation reached its handler");
  }
  for (const QString& revision :
       {QString("0\n"), QString("01"), QString("1\r\n")}) {
    patternHandlerRan = false;
    rejected(dispatcher, "fixture.pattern",
             {{"suffix", ""}, {"revision", revision}}, "invalid_params");
    require(!patternHandlerRan, "A revision violation reached its handler");
  }
  result(dispatcher, "fixture.pattern", {{"suffix", ""}, {"revision", "10"}});
}

void drawingSuffixHandlerIsWholeString(Context& context) {
  Dispatcher dispatcher(context);
  const auto document = result(dispatcher, "scene.create",
                               {{"operation_id", "pattern-scene"},
                                {"name", "Pattern"},
                                {"width_px", 32},
                                {"height_px", 32}});
  const QString id    = document.value("document_id").toString();
  const auto level = result(dispatcher, "level.create",
                            {{"operation_id", "pattern-level"},
                             {"document_id", id},
                             {"expected_revision", document.value("revision")},
                             {"name", "Pattern"},
                             {"format", "vector"}});
  for (const QString& suffix :
       {QString("a\n"), QString("\na"), QString("a\r\n")}) {
    const QJsonObject params{
        {"document_id", id},
        {"level_id", level.value("level_id")},
        {"drawing_id", QJsonObject{{"number", 1}, {"suffix", suffix}}}};
    bool refused = false;
    try {
      executeDrawing(context, "drawing.inspect", params);
    } catch (const Error& error) {
      refused = error.code == "invalid_params";
    }
    require(refused, "A drawing handler accepted a suffix with a line break");
  }
  context.removeDocument(id);
}

void toolsetCatalog(Context& context) {
  Dispatcher dispatcher(context);
  const QJsonObject capabilities =
      result(dispatcher, "automation.capabilities", {});
  require(capabilities.value("protocol_version").toInt() == 3,
          "The catalog reports protocol version 3");
  QHash<QString, QString> listed;
  for (const QJsonValue& value : capabilities.value("toolsets").toArray()) {
    const QJsonObject toolset = value.toObject();
    require(!toolset.value("description").toString().isEmpty(),
            "Every toolset needs a description");
    for (const QJsonValue& name : toolset.value("tools").toArray()) {
      require(!listed.contains(name.toString()),
              "A tool is listed in more than one toolset");
      listed.insert(name.toString(), toolset.value("name").toString());
    }
  }
  const QJsonArray tools = capabilities.value("tools").toArray();
  require(listed.size() == tools.size(),
          "Toolsets must list exactly the catalog's tools");
  for (const QJsonValue& value : tools) {
    const QJsonObject tool = value.toObject();
    const QString name     = tool.value("name").toString();
    const QString toolset  = tool.value("toolset").toString();
    require(toolset == domainToolset(name) && listed.value(name) == toolset,
            "Every current tool belongs to its domain's toolset");
  }
}

QString createdDocument(Dispatcher& dispatcher, int serial) {
  return result(dispatcher, "scene.create",
                {{"name", QString("limit-%1").arg(serial)},
                 {"operation_id", QString("limit-create-%1").arg(serial)}})
      .value("document_id")
      .toString();
}

void requireRefusedAtLimit(Context& context, Dispatcher& dispatcher,
                           const QString& method, const QJsonObject& params) {
  const QJsonArray before = context.documents();
  const QJsonObject response = call(dispatcher, method, params);
  const QJsonObject error    = response.value("error").toObject();
  require(error.value("code") == "resource_limit",
          "A refusal at the document limit must be resource_limit, not "
          "partially_applied");
  require(context.documents() == before,
          "A refusal at the document limit must change no document");
}

void documentLimit(Context& context) {
  Dispatcher dispatcher(context);
  QStringList created;
  const QString source = createdDocument(dispatcher, 0);
  created << source;
  const QJsonObject saved =
      result(dispatcher, "scene.save_copy",
             {{"document_id", source},
              {"expected_revision", "1"},
              {"operation_id", "limit-save"},
              {"directory", context.workspace() + "/limit-package"}});
  for (int serial = 1; context.documents().size() < 32; ++serial)
    created << createdDocument(dispatcher, serial);
  require(context.documents().size() == 32,
          "The document limit fixture must reach 32 documents");
  requireRefusedAtLimit(
      context, dispatcher, "scene.create",
      {{"name", "limit-over"}, {"operation_id", "limit-create-over"}});
  requireRefusedAtLimit(
      context, dispatcher, "scene.open",
      {{"path", saved.value("package_directory")},
       {"operation_id", "limit-open-over"}});
  for (const QString& id : created) context.removeDocument(id);
  std::cout << "automation-dispatch-test: document limit PASS" << std::endl;
}

}  // namespace

void runAutomationDispatcherTests(Context& context) {
  documentLimit(context);
  toolsetCatalog(context);
  {
    Dispatcher dispatcher(context);
    dispatcher.add(patternOperations(), patternHandler, "core");
    patternBoundaries(dispatcher);
  }
  drawingSuffixHandlerIsWholeString(context);
  {
    Dispatcher dispatcher(context);
    bool refused = false;
    try {
      dispatcher.add(unicodeOperations(), echoString, "undescribed");
    } catch (const Error& error) {
      refused = error.code == "internal_error";
    }
    require(refused, "An undescribed toolset must be refused at registration");
  }
  {
    Dispatcher dispatcher(context);
    dispatcher.add(unicodeOperations(), echoString, "core");
    unicodeBoundaries(dispatcher);
    unicodeOperationIds(context, dispatcher);
  }
  QString jobId;
  {
    Dispatcher dispatcher(context);
    dispatcher.add(boundaryOperations(), largeResult, "core");
    dispatcher.m_entries["job.cancel"].handler = oversizedResult;
    oversizedResultPressure(context, dispatcher);
  }
  {
    Dispatcher dispatcher(context);
    dispatcher.add(boundaryOperations(), largeResult, "core");
    jobId = bytePressure(context, dispatcher);
  }
  {
    Dispatcher dispatcher(context);
    dispatcher.add(boundaryOperations(), largeResult, "core");
    dispatcher.m_entries["job.cancel"].handler = escapedCancellation;
    terminalPressure(dispatcher, jobId);
  }
  {
    Dispatcher dispatcher(context);
    dispatcher.add(boundaryOperations(), largeResult, "core");
    dispatcher.m_entries["job.cancel"].handler = oversizedCancellation;
    partialCancellation(context, dispatcher);
  }
  std::cout << "automation-dispatch-test: byte-pressure cancellation PASS"
            << std::endl;
}

}  // namespace GroundzeroAutomation
