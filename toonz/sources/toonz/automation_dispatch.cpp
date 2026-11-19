#include "automation_dispatch.h"
#include "automation_effects.h"
#include "automation_grants.h"
#include "automation_production.h"
#include "automation_projects.h"
#include "automation_scene_check.h"
#include "automation_assistance.h"
#include "automation_text.h"
#include "automation_path.h"
#include "automation_audio.h"
#include "automation_hook.h"
#include "automation_files.h"
#include "automation_intake.h"
#include "automation_jobs.h"
#include "automation_journal.h"
#include "automation_level_lifecycle.h"
#include "automation_rig.h"
#include "automation_scene_host.h"
#include "automation_shared.h"
#include "automation_storyboard.h"
#include "automation_lipsync.h"
#include "automation_runtime.h"

#include "texception.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QScopedValueRollback>

#include <algorithm>
#include <cmath>
#include <limits>

namespace GroundzeroAutomation {
namespace {

constexpr int MaximumCapabilityPage   = 64;
constexpr int MaximumCapabilityOffset = 4096;
constexpr int DefaultCapabilityPage   = 16;
constexpr qint64 maximumReceiptBytes = 64 * 1024 * 1024;
constexpr qint64 maximumResultBytes  = 8 * 1024 * 1024 - 1024;
// One reserved first cancellation per retained job: two lanes of 32.
constexpr int maximumControlReceipts = 2 * JobsPerLane;
constexpr qint64 maximumControlBytes = 16 * 1024;
// The error envelope of every outputSchema bounds messages to 2048 characters.
constexpr int maximumErrorMessage   = 2048;
constexpr int maximumDescribedTools = 16;

qint64 receiptSize(const QJsonObject& response) {
  return QJsonDocument(response).toJson(QJsonDocument::Compact).size();
}

void checkResultSize(const QJsonObject& result, bool control) {
  const qint64 limit = control ? maximumControlBytes : maximumResultBytes;
  if (receiptSize({{"result", result}}) <= limit) return;
  throw Error(
      "response_too_large",
      "The operation completed but its result exceeds the receipt limit.",
      {{"operation_id", result.value("operation_id")},
       {"outcome", "completed"},
       {"document_id", result.value("document_id")},
       {"revision", result.value("revision")},
       {"job_id", result.value("job_id")}});
}

Error boundedControlError(const Error& error, const QString& operationId,
                          const QString& controlJob) {
  if (controlJob.isEmpty() ||
      receiptSize({{"error", errorObject(error)}}) <= maximumControlBytes)
    return error;
  return Error(
      "partially_applied",
      "Cancellation began but its diagnostic exceeded the receipt limit.",
      {{"operation_id", operationId},
       {"job_id", controlJob},
       {"outcome", "unknown"},
       {"diagnostic_truncated", true}});
}

void reject(const QString& path, const QString& reason) {
  throw Error("invalid_params", path + ": " + reason);
}

bool hasType(const QJsonValue& value, const QString& type) {
  static const QHash<QString, QJsonValue::Type> types = {
      {"object", QJsonValue::Object}, {"array", QJsonValue::Array},
      {"string", QJsonValue::String}, {"boolean", QJsonValue::Bool},
      {"null", QJsonValue::Null},     {"number", QJsonValue::Double},
      {"integer", QJsonValue::Double}};
  auto expected = types.constFind(type);
  if (expected == types.constEnd() || value.type() != expected.value())
    return false;
  if (!value.isDouble()) return true;
  if (!std::isfinite(value.toDouble())) return false;
  return type != "integer" || std::floor(value.toDouble()) == value.toDouble();
}

void validate(const QJsonValue& value, const QJsonObject& schema,
              const QString& path, int depth);

void validateAlternatives(const QJsonValue& value,
                          const QJsonArray& alternatives, const QString& path,
                          int depth) {
  int matches = 0;
  for (const QJsonValue& candidate : alternatives) {
    try {
      validate(value, candidate.toObject(), path, depth + 1);
      ++matches;
    } catch (const Error& error) {
      if (error.code != "invalid_params") throw;
    }
  }
  if (matches != 1)
    reject(path, "Value must match exactly one supported shape.");
}

void validateObject(const QJsonObject& value, const QJsonObject& schema,
                    const QString& path, int depth) {
  const QJsonObject properties = schema.value("properties").toObject();
  for (const QJsonValue& key : schema.value("required").toArray())
    if (!value.contains(key.toString()))
      reject(path, "Missing field: " + key.toString());
  for (auto it = value.constBegin(); it != value.constEnd(); ++it) {
    if (!properties.contains(it.key())) {
      if (schema.value("additionalProperties") == QJsonValue(false))
        reject(path, "Unknown field: " + it.key());
      continue;
    }
    validate(it.value(), properties.value(it.key()).toObject(),
             path + "." + it.key(), depth + 1);
  }
}

void validateArray(const QJsonArray& value, const QJsonObject& schema,
                   const QString& path, int depth) {
  if (schema.contains("minItems") &&
      value.size() < schema.value("minItems").toInt())
    reject(path, "Too few array items.");
  if (schema.contains("maxItems") &&
      value.size() > schema.value("maxItems").toInt())
    reject(path, "Too many array items.");
  if (!schema.value("items").isObject()) return;
  for (int index = 0; index < value.size(); ++index)
    validate(value.at(index), schema.value("items").toObject(),
             path + "[" + QString::number(index) + "]", depth + 1);
}

void validateChoices(const QJsonValue& value, const QJsonObject& schema,
                     const QString& path) {
  if (schema.contains("type") &&
      !hasType(value, schema.value("type").toString()))
    reject(path, "Expected type " + schema.value("type").toString());
  if (schema.contains("enum") &&
      !schema.value("enum").toArray().contains(value))
    reject(path, "Value is not in the supported enumeration.");
  if (schema.contains("const") && schema.value("const") != value)
    reject(path, "Value does not match the required constant.");
}

int unicodeLength(const QString& value, const QString& path) {
  int size = 0;
  for (int index = 0; index < value.size(); ++index) {
    const QChar unit = value.at(index);
    if (unit.isHighSurrogate()) {
      if (++index == value.size() || !value.at(index).isLowSurrogate())
        reject(path, "String contains an unpaired Unicode surrogate.");
    } else if (unit.isLowSurrogate()) {
      reject(path, "String contains an unpaired Unicode surrogate.");
    }
    ++size;
  }
  return size;
}

// The dispatcher validates on one thread, so the cache needs no lock.
const QRegularExpression& wholeStringPattern(const QString& pattern) {
  static QHash<QString, QRegularExpression> compiled;
  auto found = compiled.find(pattern);
  if (found == compiled.end()) {
    const QRegularExpression expression(
        QRegularExpression::anchoredPattern(pattern));
    if (!expression.isValid())
      throw Error("internal_error",
                  "A published schema pattern is not a valid expression.");
    found = compiled.insert(pattern, expression);
  }
  return found.value();
}

void validateString(const QString& value, const QJsonObject& schema,
                    const QString& path) {
  const int size = unicodeLength(value, path);
  if (schema.contains("minLength") && size < schema.value("minLength").toInt())
    reject(path, "String is too short.");
  if (schema.contains("maxLength") && size > schema.value("maxLength").toInt())
    reject(path, "String is too long.");
  if (schema.contains("pattern") &&
      !wholeStringPattern(schema.value("pattern").toString())
           .match(value)
           .hasMatch())
    reject(path, "String does not match its published pattern.");
}

void validateNumber(double value, const QJsonObject& schema,
                    const QString& path) {
  if (schema.contains("minimum") && value < schema.value("minimum").toDouble())
    reject(path, "Number is below the supported minimum.");
  if (schema.contains("maximum") && value > schema.value("maximum").toDouble())
    reject(path, "Number is above the supported maximum.");
}

void validate(const QJsonValue& value, const QJsonObject& schema,
              const QString& path, int depth) {
  if (depth > 32) reject(path, "JSON nesting exceeds the supported limit.");
  if (schema.contains("oneOf"))
    validateAlternatives(value, schema.value("oneOf").toArray(), path, depth);
  validateChoices(value, schema, path);
  if (value.isObject()) validateObject(value.toObject(), schema, path, depth);
  if (value.isArray()) validateArray(value.toArray(), schema, path, depth);
  if (value.isString()) validateString(value.toString(), schema, path);
  if (value.isDouble()) validateNumber(value.toDouble(), schema, path);
}

// The effect class of a writing tool: publication when it writes outside the
// worker's private folders, job when it starts a job, memory otherwise.
QString effectClass(const QJsonObject& descriptor) {
  const QJsonObject effects = descriptor.value("effects").toObject();
  const QJsonArray files    = effects.value("files").toArray();
  if (files.contains("writes_caller_path") ||
      files.contains("writes_shared_roots") ||
      files.contains("writes_output_roots"))
    return "publication";
  return effects.value("job") == "starts" ? "job" : "memory";
}

QJsonObject asObject(const QJsonValue& value) {
  return value.isObject() ? value.toObject() : QJsonObject{{"data", value}};
}

Error nativeError(const std::exception& error) {
  return Error("native_error", QString::fromUtf8(error.what()));
}

void validateRequestFields(const QJsonObject& request) {
  for (const QString& key : request.keys())
    if (key != "id" && key != "method" && key != "params")
      throw Error("invalid_request", "Unknown request field: " + key);
}

void validateRequest(const QJsonObject& request) {
  const QJsonValue id = request.value("id");
  if (!id.isString() || id.toString().isEmpty() || id.toString().size() > 128 ||
      !request.value("method").isString() ||
      !request.value("params").isObject())
    throw Error("invalid_request",
                "Requests require a string id, method, and object params.");
  validateRequestFields(request);
}

QJsonValue callHandler(QJsonValue (*handler)(Context&, const QString&,
                                             const QJsonObject&),
                       Context& context, const QString& method,
                       const QJsonObject& params) {
  try {
    return handler(context, method, params);
  } catch (const Error&) {
    throw;
  } catch (const TException& error) {
    throw Error("native_error", QString::fromStdWString(error.getMessage()));
  } catch (const std::exception& error) {
    throw nativeError(error);
  } catch (...) {
    throw Error("native_error",
                "The native operation raised an unrecognized exception.");
  }
}

const QMap<QString, QString>& toolsetDescriptions() {
  static const QMap<QString, QString> descriptions = {
      {"core",
       "Scenes, levels, drawings, palettes, animation, motion paths, FX, "
       "macros, text, inspection, assistance, rendering and packages."},
      {"audio",
       "Sound sources, sound columns, clips, column volume and exact offline "
       "WAV mixdown."},
      {"rigging",
       "Level hooks, object handles and pivots, additive offset layers, and "
       "rig inspection and placement evaluation."},
      {"shared-library",
       "Read-only discovery of granted shared roots: studio palettes, "
       "Library templates and pose sets, with identities, revisions, "
       "dependencies and thumbnails."},
      {"fx-finishing",
       "Static FX finishing recipes whose steps compose light, blur, "
       "stylize, grade and transition effects with the core FX tools."},
      {"storyboard",
       "Storyboard panels: spans, captions, notes, colors and the drawing "
       "each panel shows."},
      {"lipsync",
       "Lip-sync timing import from Papagayo and Rhubarb files and exact "
       "application to drawing columns."},
      {"sheet",
       "Cell ranges of level columns: set, clear, remove, insert, copy, "
       "retime and replace exposures."},
      {"intake",
       "Image intake: preflight of PNG files and sequences, import with an "
       "explicit copy or link choice, and import provenance."}};
  return descriptions;
}

QString toolsetDescription(const QString& toolset) {
  const auto found = toolsetDescriptions().constFind(toolset);
  if (found == toolsetDescriptions().constEnd())
    throw Error("internal_error",
                "A native operation names an undescribed toolset.");
  return found.value();
}

QJsonObject catalogEntry(const QJsonArray& catalog, const QString& name) {
  for (const QJsonValue& value : catalog)
    if (value.toObject().value("name") == name) return value.toObject();
  return {};
}

QJsonObject describeTools(const QJsonArray& catalog,
                          const QJsonObject& params) {
  validate(
      params,
      catalogEntry(catalog, "tool.describe").value("inputSchema").toObject(),
      "params", 0);
  QJsonArray tools;
  QJsonArray unknown;
  for (const QJsonValue& name : params.value("names").toArray()) {
    const QJsonObject tool = catalogEntry(catalog, name.toString());
    if (tool.isEmpty()) {
      unknown.append(name);
      continue;
    }
    tools.append(QJsonObject{{"name", name},
                             {"toolset", tool.value("toolset")},
                             {"effects", tool.value("effects")},
                             {"errors", tool.value("errors")},
                             {"outputSchema", tool.value("outputSchema")}});
  }
  if (!unknown.isEmpty())
    throw Error("unknown_tool", "No tool of this worker has these names.",
                {{"names", unknown}});
  return {{"tools", tools}};
}

}  // namespace

QStringList toolsetNames() { return toolsetDescriptions().keys(); }

void checkResult(const QJsonObject& descriptor, const QJsonValue& result) {
  const QJsonObject schema = descriptor.value("outputSchema").toObject();
  if (schema.isEmpty()) return;
  QJsonObject content = asObject(result);
  content.remove("image");
  try {
    validate(content, schema.value("oneOf").toArray().first().toObject(),
             "result", 0);
  } catch (const Error& error) {
    if (error.code != "invalid_params") throw;
    throw Error("invalid_result", descriptor.value("name").toString() +
                                      " returned a result outside its "
                                      "outputSchema: " +
                                      error.message);
  }
}

void requirePlainInputSchema(const QJsonObject& descriptor) {
  const QString name       = descriptor.value("name").toString();
  const QJsonObject schema = descriptor.value("inputSchema").toObject();
  if (schema.value("type").toString() != "object")
    throw Error("internal_error",
                "The input schema of " + name + " must be a plain object.");
  for (const char* keyword : {"oneOf", "anyOf", "allOf", "not", "$ref"})
    if (schema.contains(keyword))
      throw Error("internal_error",
                  "The input schema of " + name + " has " +
                      QString::fromLatin1(keyword) +
                      " at its root; describe the alternatives with a "
                      "discriminating property and check them in the handler.");
}

QJsonObject errorObject(const Error& error) {
  QString message = error.message;
  if (message.size() > maximumErrorMessage) {
    message.truncate(maximumErrorMessage - 3);
    if (message.at(message.size() - 1).isHighSurrogate()) message.chop(1);
    message += "...";
  }
  return {
      {"code", error.code}, {"message", message}, {"details", error.details}};
}

Dispatcher::Dispatcher(Context& context) : m_context(context) {
  addWorkerOperation(withOutputSchema(
      operation(
          "automation.capabilities",
          "Inspect this worker's protocol, toolsets and available native "
          "operations. The tool descriptors come a page at a time: repeat with "
          "next_offset until it is null. A descriptor carries its effects, "
          "error codes and output schema only with declarations: true.",
          {{"limit", QJsonObject{{"type", "integer"},
                                 {"minimum", 1},
                                 {"maximum", MaximumCapabilityPage}}},
           {"offset", QJsonObject{{"type", "integer"},
                                  {"minimum", 0},
                                  {"maximum", MaximumCapabilityOffset}}},
           {"declarations", QJsonObject{{"type", "boolean"}}}},
          {}),
      capabilitiesResultSchema()));
  addWorkerOperation(withOutputSchema(
      operation("automation.list_documents",
                "List detached documents and their current revisions.", {}, {}),
      documentListResultSchema()));
  addWorkerOperation(withOutputSchema(
      operation(
          "tool.describe",
          "Return the full declaration of up to 16 tools: effects, every "
          "error code, and the output schema of the structured result.",
          {{"names", QJsonObject{{"type", "array"},
                                 {"minItems", 1},
                                 {"maxItems", maximumDescribedTools},
                                 {"items", QJsonObject{{"type", "string"},
                                                       {"minLength", 1},
                                                       {"maxLength", 128}}}}}},
          {"names"}),
      toolDescriptionResultSchema(maximumDescribedTools)));
  add(sceneOperations(), executeScene, "core");
  add(sheetOperations(), executeSheet, "sheet");
  add(levelLifecycleOperations(), executeLevelLifecycle, "sheet");
  add(sceneAccessOperations(), executeSceneAccess, "core");
  add(projectWriteOperations(), executeProjectWrite, "core");
  add(sceneInventoryOperations(), executeSceneInventory, "core");
  add(fxOperations(), executeFx, "core");
  add(fxRecipeOperations(), executeFx, "fx-finishing");
  add(drawingOperations(), executeDrawing, "core");
  add(svgExportOperations(), executeSvgExport, "core");
  add(intakeOperations(), executeIntake, "intake");
  add(renderOperations(), executeRender, "core");
  add(runtimeOperations(), executeRuntime, "core");
  add(jobOperations(), executeJobs, "core");
  add(persistenceOperations(), executePersistence, "core");
  add(journalOperations(), executeJournal, "core");
  add(productionOperations(), executeProduction, "core");
  add(sceneCheckOperations(), executeSceneCheck, "core");
  add(textOperations(), executeText, "core");
  add(pathOperations(), executePath, "core");
  add(audioOperations(), executeAudio, "audio");
  add(rigOperations(), executeRig, "rigging");
  add(hookOperations(), executeHook, "rigging");
  add(sharedOperations(), executeShared, "shared-library");
  add(storyboardOperations(), executeStoryboard, "storyboard");
  add(lipsyncOperations(), executeLipsync, "lipsync");
  add(
      assistanceOperations(),
      [](Context&, const QString& method, const QJsonObject& params) {
        return executeAssistance(method, params);
      },
      "core");
  m_context.toolsets = m_toolsets.keys();
  m_authorityDigest  = authorityReport(m_context).value("digest").toString();
  m_instance         = jobRegistry(m_context).instance();
}

void Dispatcher::addWorkerOperation(QJsonObject descriptor) {
  requirePlainInputSchema(descriptor);
  descriptor = declaredTool(descriptor);
  descriptor.insert("toolset", "core");
  m_toolsets["core"].append(descriptor.value("name").toString());
  m_catalog.append(descriptor);
}

QJsonArray Dispatcher::toolsetCatalog() const {
  QJsonArray toolsets;
  for (auto it = m_toolsets.constBegin(); it != m_toolsets.constEnd(); ++it)
    toolsets.append(
        QJsonObject{{"name", it.key()},
                    {"description", toolsetDescription(it.key())},
                    {"tools", QJsonArray::fromStringList(it.value())}});
  return toolsets;
}

void Dispatcher::add(const QJsonArray& operations, Handler handler,
                     const QString& toolset) {
  toolsetDescription(toolset);
  for (const QJsonValue& value : operations) {
    QJsonObject descriptor = value.toObject();
    const QString name     = descriptor.value("name").toString();
    // A tool the effect table declares takes its row, and with it its grants.
    if (descriptor.contains("effects") && declaredToolNames().contains(name))
      throw Error(
          "internal_error",
          "Tool " + name +
              " brings its own effects; its row in "
              "toolDeclarations (automation_effects.cpp) declares them.");
    if (!descriptor.contains("effects")) descriptor = declaredTool(descriptor);
    if (name.isEmpty() || m_entries.contains(name))
      throw Error(
          "internal_error",
          "The native operation catalog contains a duplicate or empty name.");
    if (name == "automation.capabilities" ||
        name == "automation.list_documents" || name == "tool.describe")
      throw Error("internal_error",
                  "A native domain cannot replace a worker operation.");
    requirePlainInputSchema(descriptor);
    descriptor.insert("toolset", toolset);
    m_entries.insert(name, {descriptor, handler});
    m_toolsets[toolset].append(name);
    m_catalog.append(descriptor);
  }
}

QJsonObject Dispatcher::dispatch(const QJsonObject& request) {
  const QJsonValue requestId = request.value("id");
  const QJsonValue id =
      requestId.isString() ? requestId : QJsonValue(QJsonValue::Null);
  try {
    validateRequest(request);
    if (m_busy)
      throw Error("worker_busy",
                  "The worker is already executing an operation.");
    QScopedValueRollback<bool> busy(m_busy, true);
    return {{"id", id},
            {"result", execute(request.value("method").toString(),
                               request.value("params").toObject())}};
  } catch (const Error& error) {
    return {{"id", id}, {"error", errorObject(error)}};
  } catch (const std::exception& error) {
    return {{"id", id}, {"error", errorObject(nativeError(error))}};
  }
}

QJsonValue Dispatcher::execute(const QString& method,
                               const QJsonObject& params) {
  if (method == "tool.describe") return describeTools(m_catalog, params);
  if (method == "automation.capabilities") return capabilities(params);
  if (method == "automation.list_documents") {
    if (!params.isEmpty())
      throw Error("invalid_params", "This operation accepts no parameters.");
    return QJsonObject{{"documents", documentRows(m_context)}};
  }
  if (!m_context.sceneHostDocument.isEmpty() &&
      method.startsWith("scene_host."))
    return callHandler(executeSceneHost, m_context, method, params);
  auto found = m_entries.constFind(method);
  if (found == m_entries.constEnd())
    throw Error("method_not_found",
                "The requested operation is not available.");
  const Entry& entry = found.value();
  requireGrants(m_context, entry.descriptor, params);
  refuseReadOnlyEarly(m_context, method, params, entry.descriptor);
  const bool readOnly = entry.descriptor.value("annotations")
                            .toObject()
                            .value("readOnlyHint")
                            .toBool();
  if (!readOnly) {
    recordJobOutcomes();
    return executeWrite(entry, method, params);
  }
  validate(params, entry.descriptor.value("inputSchema").toObject(), "params",
           0);
  const QJsonValue result = invoke(entry, method, params);
#ifdef BUILD_UI_PORT_TESTS
  checkResult(entry.descriptor, result);
#endif
  return result;
}

QJsonValue Dispatcher::invoke(const Entry& entry, const QString& method,
                              const QJsonObject& params) {
  QJsonValue routed;
  if (routeToSceneHost(m_context, method, params, entry.descriptor, routed))
    return routed;
  return callHandler(entry.handler, m_context, method, params);
}

// The receipt key: the arguments digest covers {method, params}; the
// fingerprint also covers the launch authority. The worker instance is
// recorded beside them, never hashed, so a later worker can match a retry.
Dispatcher::Receipt Dispatcher::receiptKey(const QString& method,
                                           const QJsonObject& params) const {
  const auto digest = [&](const QJsonObject& key) {
    return QCryptographicHash::hash(
               QJsonDocument(key).toJson(QJsonDocument::Compact),
               QCryptographicHash::Sha256)
        .toHex();
  };
  const QJsonObject request{{"method", method}, {"params", params}};
  QJsonObject bound = request;
  bound.insert("authority_digest", m_authorityDigest);
  return {digest(bound), digest(request), {}};
}

QJsonObject Dispatcher::capabilities(const QJsonObject& params) const {
  for (auto it = params.begin(); it != params.end(); ++it)
    if (it.key() != "limit" && it.key() != "offset" &&
        it.key() != "declarations")
      throw Error("invalid_params",
                  "automation.capabilities accepts only limit, offset and "
                  "declarations.");
  const QJsonValue declared = params.value("declarations");
  if (!declared.isUndefined() && !declared.isBool())
    throw Error("invalid_params", "declarations must be a boolean.");
  const int offset =
      integerParam(params, "offset", 0, MaximumCapabilityOffset, 0);
  const int limit = integerParam(params, "limit", 1, MaximumCapabilityPage,
                                 DefaultCapabilityPage);
  if (offset > m_catalog.size())
    throw Error("invalid_params", "offset exceeds the tool count.");
  const int end = std::min(offset + limit, int(m_catalog.size()));
  QJsonArray page;
  for (int index = offset; index < end; ++index) {
    QJsonObject descriptor = m_catalog.at(index).toObject();
    // The default page stays small enough to read; the declarations are
    // several times the size of the descriptors.
    if (!declared.toBool(false))
      for (const char* field : {"effects", "errors", "outputSchema"})
        descriptor.remove(QLatin1String(field));
    page.append(descriptor);
  }
  return QJsonObject{
      {"protocol_version", 4},
      {"worker", workerIdentity(m_context)},
      {"workspace", m_context.workspace()},
      {"tools", page},
      {"tool_count", int(m_catalog.size())},
      {"next_offset", end < m_catalog.size() ? QJsonValue(end) : QJsonValue()},
      {"toolsets", toolsetCatalog()},
      {"media_tools_configured", !m_context.mediaToolsDirectory.isEmpty()},
      {"platform", runtimePlatform()},
      {"isolation", "private_process"},
      {"read_only_scenes", readOnlyCapabilities(m_context)},
      {"journal", journalCapability(m_context)},
      {"authority", authorityReport(m_context)}};
}

QJsonObject Dispatcher::replayReceipt(const Receipt& receipt,
                                      const Receipt& key) const {
  if (receipt.fingerprint != key.fingerprint && receipt.request == key.request)
    throw Error("operation_conflict",
                "operation_id was used under a different launch authority; "
                "use a new operation_id.",
                {{"reason", "grant_changed"}});
  if (receipt.fingerprint != key.fingerprint)
    throw Error("operation_conflict",
                "operation_id was already used for a different request.");
  const QJsonObject response = receipt.response;
  if (response.contains("error")) {
    const QJsonObject error = response.value("error").toObject();
    throw Error(error.value("code").toString(),
                error.value("message").toString(),
                error.value("details").toObject());
  }
  return response.value("result").toObject();
}

QJsonObject Dispatcher::executeWrite(const Entry& entry, const QString& method,
                                     const QJsonObject& params) {
  const QString operationId = requiredString(params, "operation_id");
  if (unicodeLength(operationId, "params.operation_id") > 128)
    throw Error("invalid_params",
                "operation_id must contain at most 128 characters.");
  Receipt key = receiptKey(method, params);
  auto cached = m_receipts.constFind(operationId);
  if (cached != m_receipts.constEnd())
    return replayReceipt(cached.value(), key);
  QScopedValueRollback<QString> current(m_context.writeOperation, operationId);
  Journal* journal = workspaceJournal(m_context);
  if (journal) {
    journal->refresh();
    if (const JournalOperation* recorded = journal->find(operationId))
      return replayJournal(*journal, *recorded, key);
    if (!journal->available())
      throw Error("journal_unavailable",
                  "The workspace journal cannot be written, so no write is "
                  "admitted; inspect journal.status.",
                  {{"reason", journal->unavailableReason()}});
  }
  const QString jobId = params.value("job_id").toString();
  const bool control = method == "job.cancel" && !m_controlJobs.contains(jobId);
  const QString controlJob = control ? jobId : QString();
  checkReceiptCapacity(control);
  validate(params, entry.descriptor.value("inputSchema").toObject(), "params",
           0);
  const quint64 revision   = checkRevision(params);
  const QString writeClass = effectClass(entry.descriptor);
  if (journal && !admit(*journal, method, params, key, writeClass))
    return replayJournal(*journal, *journal->find(operationId), key);
  m_context.resetMutationStarted();
  QScopedValueRollback<QString> writing(m_context.writeClass, writeClass);
  try {
    QJsonObject result = applyWrite(entry, method, params, revision);
    checkResultSize(result, control);
#ifdef BUILD_UI_PORT_TESTS
    checkResult(entry.descriptor, result);
#endif
    key.response = {{"result", result}};
    recordOutcome(journal, operationId, key.response);
    rememberReceipt(operationId, key, controlJob);
    if (writeClass == "job") recordJobStart(journal, operationId, result);
    writeFaultFires(m_context, "journal:outcome");
    return result;
  } catch (const Error& error) {
    if (m_context.mutationStarted() || m_context.failureRecorded()) {
      const Error recorded =
          boundedControlError(error, operationId, controlJob);
      key.response = {{"error", errorObject(recorded)}};
      recordOutcome(journal, operationId, key.response);
      rememberReceipt(operationId, key, controlJob);
      throw recorded;
    }
    if (journal) journal->release(operationId);
    throw;
  }
}

void Dispatcher::recordJobStart(Journal* journal, const QString& operationId,
                                const QJsonObject& result) {
  if (!journal || !result.value("job_id").isString()) return;
  QJsonObject record = result;
  record.remove("receipt");
  journal->recordJob({{"job_id", result.value("job_id")},
                      {"operation_id", operationId},
                      {"record", record}});
}

void Dispatcher::recordJobOutcomes() {
  Journal* journal = workspaceJournal(m_context);
  if (!journal || !journal->available() || !m_context.jobs) return;
  for (const auto& job : m_context.jobs->jobs())
    if (job->terminal()) recordJobEnd(*journal, job->id(), job->record());
  // A scene host's job ends where the worker last observed it: a forwarded
  // job.status or job.list, a capacity check, or the loss of the host.
  const QHash<QString, QJsonObject> hosted = observedHostJobs(m_context);
  for (auto it = hosted.constBegin(); it != hosted.constEnd(); ++it)
    if (it.value().value("terminal").toBool())
      recordJobEnd(*journal, it.key(), it.value());
}

void Dispatcher::idle() {
  recordJobOutcomes();
  if (Journal* journal = workspaceJournal(m_context)) journal->maintain();
}

void Dispatcher::recordJobEnd(Journal& journal, const QString& id,
                              const QJsonObject& record) {
  if (m_journaledJobs.contains(id)) return;
  m_journaledJobs.insert(id);
  const QJsonObject* admitted = journal.job(id);
  if (!admitted) return;
  writeFaultFires(m_context, "journal:job-terminal");
  journal.recordJob({{"job_id", id},
                     {"operation_id", admitted->value("operation_id")},
                     {"record", record}});
}

bool Dispatcher::admit(Journal& journal, const QString& method,
                       const QJsonObject& params, const Receipt& key,
                       const QString& writeClass) {
  writeFaultFires(m_context, "journal:before-admit");
  const QJsonValue document = params.value("document_id");
  const QJsonValue expected = params.value("expected_revision");
  const bool admitted       = journal.admit(
      {{"operation_id", params.value("operation_id")},
       {"arguments_sha256", QString::fromLatin1(key.request)},
       {"authority_digest", m_authorityDigest},
       {"method", method},
       {"document_id", document.isString() ? document : QJsonValue()},
       {"expected_revision", expected.isString() ? expected : QJsonValue()},
       {"effect_class", writeClass}});
  if (admitted) writeFaultFires(m_context, "journal:admit");
  return admitted;
}

// The reply waits for the outcome on disk. When it cannot be recorded the
// change still stands and is returned; the journal is then unavailable to
// this worker, and a later worker reconciles the operation as interrupted.
void Dispatcher::recordOutcome(Journal* journal, const QString& operationId,
                               const QJsonObject& response) {
  writeFaultFires(m_context, "journal:handler");
  if (!journal) return;
  const QJsonObject result = response.value("result").toObject();
  const QJsonObject error  = response.value("error").toObject();
  const QJsonValue revision =
      result.contains("revision")
          ? result.value("revision")
          : error.value("details").toObject().value("revision");
  const QJsonValue job = result.value("job_id");
  journal->outcome(
      operationId, response,
      {{"code", error.isEmpty() ? QJsonValue() : error.value("code")},
       {"revision", revision.isString() ? revision : QJsonValue()},
       {"job_id", job.isString() ? job : QJsonValue()}});
}

// An exact retry of an operation the journal holds but this worker's memory
// does not: the recorded result or error, never a second execution.
QJsonObject Dispatcher::replayJournal(Journal& journal,
                                      const JournalOperation& operation,
                                      const Receipt& key) {
  if (operation.argumentsSha256 != QString::fromLatin1(key.request))
    throw Error("operation_conflict",
                "operation_id was already used for a different request.");
  if (operation.authorityDigest != m_authorityDigest)
    throw Error("operation_conflict",
                "operation_id was used under a different launch authority; "
                "use a new operation_id.",
                {{"reason", "grant_changed"}});
  const QJsonObject recordedBy{
      {"worker_instance_id", operation.writer},
      {"current", operation.writer == m_instance},
      {"alive", journal.writerAlive(operation.writer)}};
  if (!operation.stubState.isEmpty())
    throw Error("receipt_expired",
                "Only a summary of this operation is kept; inspect the "
                "document or files it named and use a new operation_id.",
                {{"state", operation.stubState},
                 {"method", operation.method},
                 {"document_id", operation.documentId.isEmpty()
                                     ? QJsonValue()
                                     : QJsonValue(operation.documentId)}});
  if (!operation.outcome.isEmpty()) return recordedResponse(journal, operation);
  if (recordedBy.value("alive").toBool())
    throw Error("operation_in_progress",
                "Another worker admitted this operation and has not recorded "
                "its outcome; ask operation.status later.",
                {{"recorded_by", recordedBy}});
  const JournalOperation copy  = operation;
  const JournalVerdict verdict = journal.reconcile(copy);
  throw Error("operation_interrupted",
              "The worker that ran this operation ended before recording its "
              "outcome; it is never run again. Inspect what the verdict names "
              "and use a new operation_id.",
              {{"verdict", verdict}, {"recorded_by", recordedBy}});
}

QJsonObject Dispatcher::recordedResponse(Journal& journal,
                                         const JournalOperation& operation) {
  const QJsonObject response = journal.response(operation);
  const QJsonObject receipt{
      {"source", "journal"},
      {"recorded_by_worker", operation.writer},
      {"recorded_at_ms", operation.outcome.value("t")},
      {"handles_expired", operation.writer != m_instance}};
  if (response.contains("error")) {
    const QJsonObject error = response.value("error").toObject();
    QJsonObject details     = error.value("details").toObject();
    details.insert("receipt", receipt);
    throw Error(error.value("code").toString(),
                error.value("message").toString(), details);
  }
  QJsonObject result = response.value("result").toObject();
  result.insert("receipt", receipt);
  return result;
}

void Dispatcher::checkReceiptCapacity(bool control) const {
  // The reserve holds one first cancellation per retained job; an unknown id's
  // cancellation is refused without a receipt and takes no slot.
  if (control) {
    const int retained = jobRegistry(m_context).retainedCount();
    if (m_controlJobs.size() < std::min(retained, maximumControlReceipts))
      return;
  } else if (m_receipts.size() - m_controlJobs.size() < 2048 &&
             m_receiptBytes <= maximumReceiptBytes - maximumResultBytes) {
    return;
  }
  throw Error("resource_limit",
              "The worker receipt limit was reached; start a new worker.");
}

void Dispatcher::rememberReceipt(const QString& operationId,
                                 const Receipt& receipt,
                                 const QString& controlJob) {
  m_receipts.insert(operationId, receipt);
  if (controlJob.isEmpty())
    m_receiptBytes += receiptSize(receipt.response);
  else
    m_controlJobs.insert(controlJob);
}

void Dispatcher::finishRevision(QJsonObject& result, const QString& documentId,
                                quint64 revision) {
  if (documentId.isEmpty()) return;
  try {
    Document& document = m_context.document(documentId);
    document.revision  = revision + 1;
  } catch (const Error& error) {
    if (error.code != "document_not_found") throw;
  }
  result.insert("document_id", documentId);
  result.insert("revision", QString::number(revision + 1));
}

quint64 Dispatcher::checkRevision(const QJsonObject& params) const {
  const QString documentId = params.value("document_id").toString();
  if (documentId.isEmpty()) return 0;
  const Document& document = m_context.document(documentId);
  if (requiredString(params, "expected_revision") != document.revisionText())
    throw Error(
        "revision_conflict", "The document changed; inspect it before writing.",
        {{"document_id", documentId}, {"revision", document.revisionText()}});
  if (document.revision == std::numeric_limits<quint64>::max())
    throw Error("resource_limit", "The document revision limit was reached.");
  return document.revision;
}

QJsonObject Dispatcher::applyWrite(const Entry& entry, const QString& method,
                                   const QJsonObject& params,
                                   quint64 revision) {
  const QString documentId   = params.value("document_id").toString();
  const bool mutatesDocument = advancesRevision(entry.descriptor);
  try {
    QJsonObject result     = asObject(invoke(entry, method, params));
    const QString resultId = documentId.isEmpty()
                                 ? result.value("document_id").toString()
                                 : documentId;
    if (mutatesDocument && m_context.mutationStarted()) {
      finishRevision(result, resultId, revision);
    } else if (!documentId.isEmpty()) {
      result.insert("document_id", documentId);
      result.insert("revision", QString::number(revision));
    }
    result.insert("operation_id", params.value("operation_id"));
    return result;
  } catch (const Error& error) {
    if (!m_context.mutationStarted()) throw;
    QJsonObject details = error.details;
    details.insert("cause_code", error.code);
    details.insert("operation_id", params.value("operation_id"));
    if (mutatesDocument) finishRevision(details, documentId, revision);
    throw Error("partially_applied",
                "The operation failed after mutation began: " + error.message,
                details);
  }
}

}  // namespace GroundzeroAutomation
