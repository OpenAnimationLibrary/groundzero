#include "automation_dispatch.h"
#include "automation_drawing.h"
#include "automation_effects.h"
#include "automation_grants.h"
#include "automation_jobs.h"

#include <QCryptographicHash>
#include <QJsonDocument>

#include <functional>
#include <iostream>
#include <stdexcept>

namespace GroundzeroAutomation {

// Test access to the receipts and receipt keys of a dispatcher.
struct DispatcherProbe {
  static QHash<QString, Dispatcher::Receipt>& receipts(Dispatcher& dispatcher) {
    return dispatcher.m_receipts;
  }
  static Dispatcher::Receipt key(const Dispatcher& dispatcher,
                                 const QString& method,
                                 const QJsonObject& params) {
    return dispatcher.receiptKey(method, params);
  }
  static void add(Dispatcher& dispatcher, const QJsonArray& operations,
                  Dispatcher::Handler handler, const QString& toolset) {
    dispatcher.add(operations, handler, toolset);
  }
  static void setAuthorityDigest(Dispatcher& dispatcher,
                                 const QString& digest) {
    dispatcher.m_authorityDigest = digest;
  }
};

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

const ToolEffects fixtureRead{"read", "none", {}, "none", {}, false};
const ToolEffects fixtureWrite{"document_write", "none", {}, "none", {}, false};

QJsonArray unicodeOperations() {
  return {declaredTool(
      operation("fixture.unicode", "Private Unicode length boundary",
                {{"value",
                  QJsonObject{
                      {"type", "string"}, {"minLength", 2}, {"maxLength", 2}}}},
                {"value"}, false),
      fixtureRead)};
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
  return {declaredTool(
      operation("fixture.receipt", "Private receipt boundary", {}, {}, true),
      fixtureWrite)};
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
  require(settleJobs(context, 60000),
          "Native writers did not close after cancellation");
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
  QJsonObject response = executeJobs(context, method, params).toObject();
  response.insert("failure", QJsonObject{{"code", "padding"},
                                         {"message", QString(2048, QChar(1))}});
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
  executeJobs(context, method, params);
  throw Error("native_error", "Cancellation failed after it began.",
              {{"diagnostic", QString(32 * 1024, QChar(1))}});
}

QJsonValue oversizedResult(Context& context, const QString& method,
                           const QJsonObject& params) {
  QJsonObject response = executeJobs(context, method, params).toObject();
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
  return {declaredTool(
      operation("fixture.pattern", "Private pattern boundary",
                {{"suffix",
                  QJsonObject{{"type", "string"}, {"pattern", "^[a-z]{0,8}$"}}},
                 {"revision", QJsonObject{{"type", "string"},
                                          {"pattern", "^(0|[1-9][0-9]*)$"}}}},
                {"suffix"}, false),
      fixtureRead)};
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

QJsonArray allTools(Dispatcher& dispatcher) {
  QJsonArray tools;
  int offset = 0;
  for (;;) {
    const QJsonObject page =
        result(dispatcher, "automation.capabilities",
               {{"offset", offset}, {"declarations", true}});
    for (const QJsonValue& tool : page.value("tools").toArray())
      tools.append(tool);
    require(page.contains("next_offset"),
            "A catalog page always reports next_offset");
    if (page.value("next_offset").isNull()) return tools;
    require(page.value("next_offset").toInt() > offset,
            "Catalog paging must advance");
    offset = page.value("next_offset").toInt();
  }
}

void catalogPaging(Context& context) {
  Dispatcher dispatcher(context);
  const QJsonObject first =
      result(dispatcher, "automation.capabilities", {{"limit", 3}});
  require(first.value("tools").toArray().size() == 3 &&
              first.value("next_offset").toInt() == 3,
          "A catalog page holds limit tools and points at the next one");
  require(first.value("tool_count").toInt() == allTools(dispatcher).size(),
          "tool_count is the whole catalog");
  const QJsonObject last =
      result(dispatcher, "automation.capabilities",
             {{"offset", first.value("tool_count").toInt() - 1}, {"limit", 5}});
  require(last.value("tools").toArray().size() == 1 &&
              last.value("next_offset").isNull(),
          "The last page ends with a null next_offset");
  const QJsonObject defaults =
      result(dispatcher, "automation.capabilities", {});
  require(defaults.value("tools").toArray().size() == 16,
          "The default catalog page is 16 tools");
  for (const QJsonObject& params :
       {QJsonObject{{"limit", 0}}, QJsonObject{{"limit", 65}},
        QJsonObject{{"offset", -1}}, QJsonObject{{"offset", 4000}},
        QJsonObject{{"offset", 4097}}, QJsonObject{{"pad", 1}}}) {
    const QJsonObject response =
        call(dispatcher, "automation.capabilities", params);
    require(
        response.value("error").toObject().value("code") == "invalid_params",
        "An invalid catalog page request is refused");
  }
}

void plainInputSchemas(Context& context) {
  Dispatcher dispatcher(context);
  for (const QJsonValue& tool : allTools(dispatcher))
    requirePlainInputSchema(tool.toObject());
  for (const char* keyword : {"oneOf", "anyOf", "allOf", "not", "$ref"}) {
    QJsonObject schema{{"type", "object"}, {keyword, QJsonArray{}}};
    bool refused = false;
    try {
      requirePlainInputSchema(
          {{"name", "invented.tool"}, {"inputSchema", schema}});
    } catch (const Error& error) {
      refused = error.code == "internal_error";
    }
    require(refused, "A root combinator in an input schema must be refused");
  }
  bool refused = false;
  try {
    requirePlainInputSchema({{"name", "invented.tool"},
                             {"inputSchema", QJsonObject{{"type", "array"}}}});
  } catch (const Error&) {
    refused = true;
  }
  require(refused, "A non-object input schema root must be refused");
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
  const QJsonArray tools = allTools(dispatcher);
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

QJsonObject catalogEntry(const QJsonObject& capabilities, const QString& name) {
  for (const QJsonValue& value : capabilities.value("tools").toArray())
    if (value.toObject().value("name") == name) return value.toObject();
  throw std::runtime_error("A worker operation is missing from the catalog");
}

bool refusedAs(const QJsonObject& descriptor, const QString& code,
               const QString& phrase) {
  try {
    declaredTool(descriptor);
  } catch (const Error& error) {
    return error.code == code && error.message.contains(phrase);
  }
  return false;
}

void effectDeclarations(Context& context) {
  Dispatcher dispatcher(context);
  const QJsonObject page   = result(dispatcher, "automation.capabilities", {});
  QJsonObject capabilities = page;
  capabilities.insert("tools", allTools(dispatcher));
  checkResult(catalogEntry(capabilities, "automation.capabilities"), page);
  for (const QJsonValue& lean : page.value("tools").toArray())
    require(!lean.toObject().contains("effects") &&
                !lean.toObject().contains("errors") &&
                !lean.toObject().contains("outputSchema"),
            "A default catalog page carries no declarations");
  checkResult(catalogEntry(capabilities, "automation.list_documents"),
              result(dispatcher, "automation.list_documents", {}));
  const QJsonObject described =
      result(dispatcher, "tool.describe",
             {{"names", QJsonArray{"scene.summary", "tool.describe"}}});
  checkResult(catalogEntry(capabilities, "tool.describe"), described);
  const QJsonObject summary =
      described.value("tools").toArray().first().toObject();
  require(
      summary.value("name") == "scene.summary" &&
          summary.value("outputSchema") ==
              catalogEntry(capabilities, "scene.summary").value("outputSchema"),
      "tool.describe did not return the catalog's declaration");
  rejected(dispatcher, "tool.describe",
           {{"names", QJsonArray{"scene.summary", "no.such.tool"}}},
           "unknown_tool");
  rejected(dispatcher, "tool.describe", {{"names", QJsonArray{}}},
           "invalid_params");
  QSet<QString> names;
  for (const QJsonValue& value : capabilities.value("tools").toArray()) {
    const QJsonObject tool = value.toObject();
    names.insert(tool.value("name").toString());
    require(!tool.contains("mutates_document"),
            "Effect declarations replace mutates_document");
  }
  for (const QString& name : declaredToolNames())
    require(names.contains(name),
            "An effect declaration names no registered tool; remove its row");
  for (const QString& name : outputSchemaNames())
    require(names.contains(name),
            "An output schema names no registered tool; remove its entry");
  require(refusedAs(operation("fixture.undeclared", "Private", {}, {}),
                    "internal_error", "add a row for it"),
          "A tool without an effect declaration was registered");
  require(refusedAs(operation("scene.summary", "Private", {}, {}, true),
                    "internal_error", "needs a writing class"),
          "A read declaration was accepted for a tool with receipts");
  require(refusedAs(operation("automation.capabilities", "Private", {}, {}),
                    "internal_error", "has no outputSchema"),
          "A tool without an output schema was registered");
  require(refusedAs(withOutputSchema(operation("scene.summary", "Private",
                                               {{"document_id", QJsonObject{}}},
                                               {"document_id"}),
                                     QJsonObject{{"type", "object"}}),
                    "internal_error", "keep one"),
          "A tool published two output schemas");
}

QJsonValue strayField(Context&, const QString&, const QJsonObject&) {
  return QJsonObject{{"value", 1}, {"stray", true}};
}

QJsonArray outputOperations() {
  const QJsonObject success{
      {"type", "object"},
      {"additionalProperties", false},
      {"required", QJsonArray{"value"}},
      {"properties",
       QJsonObject{{"value", QJsonObject{{"type", "integer"}}},
                   {"operation_id", QJsonObject{{"type", "string"}}}}}};
  return {
      declaredTool(withOutputSchema(operation("fixture.output.read",
                                              "Private output", {}, {}),
                                    success),
                   fixtureRead),
      declaredTool(withOutputSchema(operation("fixture.output.write",
                                              "Private output", {}, {}, true),
                                    success),
                   fixtureWrite)};
}

void outputSchemas(Dispatcher& dispatcher) {
  rejected(dispatcher, "fixture.output.read", {}, "invalid_result");
  rejected(dispatcher, "fixture.output.write", {{"operation_id", "stray"}},
           "invalid_result");
  const QJsonObject descriptor = outputOperations().first().toObject();
  checkResult(descriptor, QJsonObject{{"value", 2}, {"image", "png"}});
  bool refused = false;
  try {
    checkResult(descriptor, QJsonObject{{"value", 2.5}});
  } catch (const Error& error) {
    refused = error.code == "invalid_result";
  }
  require(refused, "A result outside its output schema was accepted");
  const QString rocket = QString::fromUtf8("\xf0\x9f\x9a\x80");
  const QString message =
      errorObject(
          Error("native_error", QString(2044, QChar('a')) + rocket.repeated(4)))
          .value("message")
          .toString();
  require(
      message.size() <= 2048 && QString::fromUtf8(message.toUtf8()) == message,
      "An error message exceeded the envelope or split a character");
}

QJsonObject refusal(Dispatcher& dispatcher, const QString& method,
                    const QJsonObject& params) {
  return call(dispatcher, method, params).value("error").toObject();
}

void requireDenied(Dispatcher& dispatcher, const QString& method,
                   const QJsonObject& params, const QStringList& missing,
                   const QString& domain) {
  const QJsonObject error   = refusal(dispatcher, method, params);
  const QJsonObject details = error.value("details").toObject();
  require(error.value("code") == "grant_denied" &&
              details.value("tool") == method &&
              details.value("missing") == QJsonArray::fromStringList(missing) &&
              details.value("domain") == domain,
          "A call outside the session's grants was not refused with "
          "grant_denied naming the missing grant and the tool's domain");
}

QJsonValue echoWrite(Context&, const QString&, const QJsonObject&) {
  return QJsonObject{};
}

QJsonArray grantFixtureOperations() {
  return {declaredTool(
      operation("fixture.grant", "Private receipt binding",
                {{"value", QJsonObject{{"type", "integer"}}}}, {}, true),
      fixtureWrite)};
}

void grantOrder(Context& context) {
  const Authority launched = context.authority;
  context.authority        = {{"inspect"}, {"core"}};
  Dispatcher dispatcher(context);
  const QJsonArray documents = context.documents();
  rejected(dispatcher, "no.such.tool", {}, "method_not_found");
  requireDenied(dispatcher, "scene.create", {{"operation_id", "denied"}},
                {"draft"}, "core");
  requireDenied(dispatcher, "scene.save_copy", {}, {"file"}, "core");
  requireDenied(dispatcher, "render.submit", {{"stray", true}}, {"job"},
                "core");
  requireDenied(dispatcher, "audio.inspect", {}, {}, "audio");
  auto& receipts    = DispatcherProbe::receipts(dispatcher);
  auto replayed     = DispatcherProbe::key(dispatcher, "scene.create",
                                           {{"operation_id", "replayed"}});
  replayed.response = {{"result", QJsonObject{{"document_id", "replayed"}}}};
  receipts.insert("replayed", replayed);
  requireDenied(dispatcher, "scene.create", {{"operation_id", "replayed"}},
                {"draft"}, "core");
  requireDenied(dispatcher, "scene.create", {{"operation_id", "denied"}},
                {"draft"}, "core");
  require(receipts.size() == 1 && !receipts.contains("denied") &&
              context.documents() == documents,
          "A denied call recorded a receipt or changed a document");
  context.authority = {{"draft", "inspect"}, {"core"}};
  const QString document =
      result(dispatcher, "scene.create",
             {{"name", "Granted"}, {"operation_id", "denied"}})
          .value("document_id")
          .toString();
  context.authority = {{"inspect"}, {"core"}};
  requireDenied(dispatcher, "scene.discard",
                {{"document_id", document}, {"operation_id", "discard"}},
                {"draft"}, "core");
  context.authority = launched;
  context.removeDocument(document);
  std::cout << "automation-dispatch-test: grant order PASS" << std::endl;
}

void receiptBindsAuthority(Context& context) {
  Dispatcher first(context);
  Dispatcher second(context);
  DispatcherProbe::add(first, grantFixtureOperations(), echoWrite, "core");
  DispatcherProbe::add(second, grantFixtureOperations(), echoWrite, "core");
  DispatcherProbe::setAuthorityDigest(second, QString(64, QChar('0')));
  const QJsonObject params{{"operation_id", "bound"}, {"value", 1}};
  const auto key   = DispatcherProbe::key(first, "fixture.grant", params);
  const auto other = DispatcherProbe::key(second, "fixture.grant", params);
  require(key.fingerprint != other.fingerprint && key.request == other.request,
          "The receipt fingerprint does not bind the launch authority");
  const QJsonObject original = result(first, "fixture.grant", params);
  require(result(first, "fixture.grant", params) == original,
          "A replay under the same authority did not return its receipt");
  DispatcherProbe::receipts(second).insert(
      "bound", DispatcherProbe::receipts(first).value("bound"));
  const QJsonObject changed = refusal(second, "fixture.grant", params);
  require(changed.value("code") == "operation_conflict" &&
              changed.value("details").toObject().value("reason") ==
                  "grant_changed",
          "A receipt replayed under another authority was not refused as "
          "grant_changed");
  QJsonObject different = params;
  different.insert("value", 2);
  const QJsonObject conflict = refusal(second, "fixture.grant", different);
  require(conflict.value("code") == "operation_conflict" &&
              !conflict.value("details").toObject().contains("reason"),
          "A different request was reported as a changed grant");
  std::cout << "automation-dispatch-test: receipt binds authority PASS"
            << std::endl;
}

Dispatcher* busyDispatcher = nullptr;

QJsonValue nestedCalls(Context&, const QString&, const QJsonObject&) {
  return QJsonObject{{"read", call(*busyDispatcher, "fixture.busy.read", {})},
                     {"write", call(*busyDispatcher, "fixture.busy.write",
                                    {{"operation_id", "nested-write"}})}};
}

QJsonValue idleAnswer(Context&, const QString&, const QJsonObject&) {
  return QJsonObject{{"idle", true}};
}

void busyWorkerRefusesNestedRequests(Context& context) {
  Dispatcher dispatcher(context);
  DispatcherProbe::add(dispatcher,
                       {declaredTool(operation("fixture.busy.outer",
                                               "Private busy outer", {}, {}),
                                     fixtureRead)},
                       nestedCalls, "core");
  DispatcherProbe::add(
      dispatcher,
      {declaredTool(operation("fixture.busy.read", "Private busy read", {}, {}),
                    fixtureRead),
       declaredTool(
           operation("fixture.busy.write", "Private busy write", {}, {}, true),
           fixtureWrite)},
      idleAnswer, "core");
  busyDispatcher          = &dispatcher;
  const QJsonObject outer = result(dispatcher, "fixture.busy.outer", {});
  for (const char* key : {"read", "write"}) {
    const QJsonObject nested = outer.value(key).toObject();
    require(!nested.contains("result") &&
                nested.value("error").toObject().value("code") == "worker_busy",
            "A request that arrived while an operation ran was not refused "
            "as worker_busy");
  }
  require(!DispatcherProbe::receipts(dispatcher).contains("nested-write"),
          "A request refused as worker_busy recorded a receipt");
  const QJsonObject after   = result(dispatcher, "fixture.busy.read", {});
  const QJsonObject written = result(dispatcher, "fixture.busy.write",
                                     {{"operation_id", "nested-write"}});
  require(after.value("idle").toBool() && written.value("idle").toBool() &&
              DispatcherProbe::receipts(dispatcher).contains("nested-write"),
          "The dispatcher stayed busy or lost the refused request's identity");
  busyDispatcher = nullptr;
  std::cout << "automation-dispatch-test: busy worker PASS" << std::endl;
}

bool refusedLaunch(const std::function<void()>& parse) {
  try {
    parse();
  } catch (const Error& error) {
    return error.code == "invalid_argument";
  }
  return false;
}

QString digestOf(Context& context, const Authority& authority) {
  const Authority launched = context.authority;
  context.authority        = authority;
  QJsonObject report       = authorityReport(context);
  context.authority        = launched;
  const QString digest     = report.take("digest").toString();
  require(digest == QString::fromLatin1(
                        QCryptographicHash::hash(QJsonDocument(report).toJson(
                                                     QJsonDocument::Compact),
                                                 QCryptographicHash::Sha256)
                            .toHex()),
          "The authority digest is not the SHA-256 of its compact JSON");
  return digest;
}

void authorityDigestIsCanonical(Context& context) {
  const QStringList names = toolsetNames();
  const Authority forward{parseGrants("inspect,draft,job"),
                          parseDomains("core,audio", names)};
  const Authority backward{parseGrants("job,draft,inspect"),
                           parseDomains("audio", names)};
  require(digestOf(context, forward) == digestOf(context, backward),
          "Option order or the implied core toolset changed the digest");
  require(digestOf(context, {parseGrants("inspect"), forward.domains}) !=
              digestOf(context, forward),
          "Different grants produced the same authority digest");
  require(parseDomains("all", names) == parseDomains(names.join(','), names),
          "--domains all does not name every toolset");
  for (const QString& value :
       {QString("draft"), QString("inspect,inspect"), QString("inspect,live"),
        QString("inspect,"), QString()})
    require(refusedLaunch([&] { parseGrants(value); }),
            "An invalid --grants value was accepted");
  for (const QString& value : {QString("all,core"), QString("core,core"),
                               QString("core,sound"), QString()})
    require(refusedLaunch([&] { parseDomains(value, names); }),
            "An invalid --domains value was accepted");
  std::cout << "automation-dispatch-test: authority digest PASS" << std::endl;
}

void effectsCannotBeRedeclared(Context& context) {
  Dispatcher dispatcher(context);
  bool refused = false;
  try {
    DispatcherProbe::add(
        dispatcher,
        {declaredTool(operation("audio.mixdown", "Private", {}, {}, true),
                      fixtureWrite)},
        echoWrite, "audio");
  } catch (const Error& error) {
    refused = error.code == "internal_error" &&
              error.message.contains("brings its own effects");
  }
  require(refused, "A declared tool was registered with weaker effects");
}

}  // namespace

void runAutomationDispatcherTests(Context& context) {
  grantOrder(context);
  receiptBindsAuthority(context);
  busyWorkerRefusesNestedRequests(context);
  authorityDigestIsCanonical(context);
  effectsCannotBeRedeclared(context);
  documentLimit(context);
  toolsetCatalog(context);
  effectDeclarations(context);
  {
    Dispatcher dispatcher(context);
    dispatcher.add(outputOperations(), strayField, "core");
    outputSchemas(dispatcher);
  }
  catalogPaging(context);
  plainInputSchemas(context);
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
