#include "automation_dispatch.h"
#include "automation_production.h"
#include "automation_scene_check.h"
#include "automation_assistance.h"
#include "automation_text.h"
#include "automation_path.h"
#include "automation_audio.h"
#include "automation_hook.h"
#include "automation_jobs.h"
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

constexpr qint64 maximumReceiptBytes = 64 * 1024 * 1024;
constexpr qint64 maximumResultBytes  = 8 * 1024 * 1024 - 1024;
// One reserved first cancellation per retained job: two lanes of 32.
constexpr int maximumControlReceipts = 2 * JobsPerLane;
constexpr qint64 maximumControlBytes = 16 * 1024;

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

QString toolsetDescription(const QString& toolset) {
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
       "application to drawing columns."}};
  const auto found = descriptions.constFind(toolset);
  if (found == descriptions.constEnd())
    throw Error("internal_error",
                "A native operation names an undescribed toolset.");
  return found.value();
}

}  // namespace

QJsonObject errorObject(const Error& error) {
  return {{"code", error.code},
          {"message", error.message},
          {"details", error.details}};
}

Dispatcher::Dispatcher(Context& context) : m_context(context) {
  addWorkerOperation(
      operation("automation.capabilities",
                "Inspect this worker's protocol, toolsets and available native "
                "operations.",
                {}, {}));
  addWorkerOperation(operation(
      "automation.list_documents",
      "List detached documents and their current revisions.", {}, {}));
  add(sceneOperations(), executeScene, "core");
  add(sceneAccessOperations(), executeSceneAccess, "core");
  add(sceneInventoryOperations(), executeSceneInventory, "core");
  add(fxOperations(), executeFx, "core");
  add(fxRecipeOperations(), executeFx, "fx-finishing");
  add(drawingOperations(), executeDrawing, "core");
  add(renderOperations(), executeRender, "core");
  add(runtimeOperations(), executeRuntime, "core");
  add(jobOperations(), executeJobs, "core");
  add(persistenceOperations(), executePersistence, "core");
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
}

void Dispatcher::addWorkerOperation(QJsonObject descriptor) {
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
    if (name.isEmpty() || m_entries.contains(name))
      throw Error(
          "internal_error",
          "The native operation catalog contains a duplicate or empty name.");
    if (name == "automation.capabilities" ||
        name == "automation.list_documents")
      throw Error("internal_error",
                  "A native domain cannot replace a worker operation.");
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
  if (method == "automation.capabilities" ||
      method == "automation.list_documents") {
    if (!params.isEmpty())
      throw Error("invalid_params", "This operation accepts no parameters.");
    if (method == "automation.capabilities")
      return QJsonObject{
          {"protocol_version", 3},
          {"worker", workerIdentity(m_context)},
          {"workspace", m_context.workspace()},
          {"tools", m_catalog},
          {"toolsets", toolsetCatalog()},
          {"media_tools_configured", !m_context.mediaToolsDirectory.isEmpty()},
          {"platform", runtimePlatform()},
          {"isolation", "private_process"},
          {"read_only_scenes", readOnlyCapabilities(m_context)}};
    return QJsonObject{{"documents", documentRows(m_context)}};
  }
  if (!m_context.sceneHostDocument.isEmpty() &&
      method.startsWith("scene_host."))
    return callHandler(executeSceneHost, m_context, method, params);
  auto found = m_entries.constFind(method);
  if (found == m_entries.constEnd())
    throw Error("method_not_found",
                "The requested operation is not available.");
  const Entry& entry  = found.value();
  refuseReadOnlyEarly(m_context, method, params, entry.descriptor);
  const bool readOnly = entry.descriptor.value("annotations")
                            .toObject()
                            .value("readOnlyHint")
                            .toBool();
  if (!readOnly) return executeWrite(entry, method, params);
  validate(params, entry.descriptor.value("inputSchema").toObject(), "params",
           0);
  return invoke(entry, method, params);
}

QJsonValue Dispatcher::invoke(const Entry& entry, const QString& method,
                              const QJsonObject& params) {
  QJsonValue routed;
  if (routeToSceneHost(m_context, method, params, entry.descriptor, routed))
    return routed;
  return callHandler(entry.handler, m_context, method, params);
}

QJsonObject Dispatcher::replayReceipt(const Receipt& receipt,
                                      const QByteArray& fingerprint) const {
  if (receipt.fingerprint != fingerprint)
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
  const QByteArray fingerprint = QCryptographicHash::hash(
      QJsonDocument(QJsonObject{{"method", method}, {"params", params}})
          .toJson(QJsonDocument::Compact),
      QCryptographicHash::Sha256);
  auto cached = m_receipts.constFind(operationId);
  if (cached != m_receipts.constEnd())
    return replayReceipt(cached.value(), fingerprint);
  const QString jobId = params.value("job_id").toString();
  const bool control = method == "job.cancel" && !m_controlJobs.contains(jobId);
  const QString controlJob = control ? jobId : QString();
  checkReceiptCapacity(control);
  validate(params, entry.descriptor.value("inputSchema").toObject(), "params",
           0);
  m_context.resetMutationStarted();
  try {
    QJsonObject result = applyWrite(entry, method, params);
    checkResultSize(result, control);
    rememberReceipt(operationId, {fingerprint, {{"result", result}}},
                    controlJob);
    return result;
  } catch (const Error& error) {
    if (m_context.mutationStarted() || m_context.failureRecorded()) {
      const Error recorded =
          boundedControlError(error, operationId, controlJob);
      rememberReceipt(operationId,
                      {fingerprint, {{"error", errorObject(recorded)}}},
                      controlJob);
      throw recorded;
    }
    throw;
  }
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
                                   const QJsonObject& params) {
  const QString documentId = params.value("document_id").toString();
  const quint64 revision   = checkRevision(params);
  const bool mutatesDocument =
      entry.descriptor.value("mutates_document").toBool(true);
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
