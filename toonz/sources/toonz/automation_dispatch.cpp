#include "automation_dispatch.h"

#include "texception.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QScopedValueRollback>

#include <cmath>
#include <limits>

namespace GroundzeroAutomation {
namespace {

constexpr qint64 maximumReceiptBytes = 64 * 1024 * 1024;
constexpr qint64 maximumResultBytes  = 8 * 1024 * 1024 - 1024;

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

void validateString(const QString& value, const QJsonObject& schema,
                    const QString& path) {
  const int size = value.size();
  if (schema.contains("minLength") && size < schema.value("minLength").toInt())
    reject(path, "String is too short.");
  if (schema.contains("maxLength") && size > schema.value("maxLength").toInt())
    reject(path, "String is too long.");
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

}  // namespace

QJsonObject errorObject(const Error& error) {
  return {{"code", error.code},
          {"message", error.message},
          {"details", error.details}};
}

Dispatcher::Dispatcher(Context& context) : m_context(context) {
  add(sceneOperations(), executeScene);
  add(fxOperations(), executeFx);
  add(drawingOperations(), executeDrawing);
  add(renderOperations(), executeRender);
  add(persistenceOperations(), executePersistence);
}

void Dispatcher::add(const QJsonArray& operations, Handler handler) {
  for (const QJsonValue& value : operations) {
    const QJsonObject descriptor = value.toObject();
    const QString name           = descriptor.value("name").toString();
    if (name.isEmpty() || m_entries.contains(name))
      throw Error(
          "internal_error",
          "The native operation catalog contains a duplicate or empty name.");
    m_entries.insert(name, {descriptor, handler});
    m_catalog.append(descriptor);
  }
}

QJsonObject Dispatcher::dispatch(const QJsonObject& request) {
  const QJsonValue requestId = request.value("id");
  const QJsonValue id =
      requestId.isString() ? requestId : QJsonValue(QJsonValue::Null);
  try {
    if (!requestId.isString() || requestId.toString().isEmpty() ||
        requestId.toString().size() > 128 ||
        !request.value("method").isString() ||
        !request.value("params").isObject())
      throw Error("invalid_request",
                  "Requests require a string id, method, and object params.");
    for (const QString& key : request.keys())
      if (key != "id" && key != "method" && key != "params")
        throw Error("invalid_request", "Unknown request field: " + key);
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
      return QJsonObject{{"protocol_version", 1},
                         {"workspace", m_context.workspace()},
                         {"tools", m_catalog},
                         {"isolation", "private_process"}};
    return QJsonObject{{"documents", m_context.documents()}};
  }
  auto found = m_entries.constFind(method);
  if (found == m_entries.constEnd())
    throw Error("method_not_found",
                "The requested operation is not available.");
  const Entry& entry  = found.value();
  const bool readOnly = entry.descriptor.value("annotations")
                            .toObject()
                            .value("readOnlyHint")
                            .toBool();
  if (!readOnly) return executeWrite(entry, method, params);
  validate(params, entry.descriptor.value("inputSchema").toObject(), "params",
           0);
  return callHandler(entry.handler, m_context, method, params);
}

QJsonObject Dispatcher::executeWrite(const Entry& entry, const QString& method,
                                     const QJsonObject& params) {
  const QString operationId = requiredString(params, "operation_id");
  if (operationId.size() > 128)
    throw Error("invalid_params",
                "operation_id must contain at most 128 characters.");
  const QByteArray fingerprint = QCryptographicHash::hash(
      QJsonDocument(QJsonObject{{"method", method}, {"params", params}})
          .toJson(QJsonDocument::Compact),
      QCryptographicHash::Sha256);
  auto cached = m_receipts.constFind(operationId);
  if (cached != m_receipts.constEnd()) {
    if (cached->fingerprint != fingerprint)
      throw Error("operation_conflict",
                  "operation_id was already used for a different request.");
    const QJsonObject response = cached->response;
    if (response.contains("error")) {
      const QJsonObject error = response.value("error").toObject();
      throw Error(error.value("code").toString(),
                  error.value("message").toString(),
                  error.value("details").toObject());
    }
    return response.value("result").toObject();
  }
  if (m_receipts.size() >= 2048 ||
      m_receiptBytes > maximumReceiptBytes - maximumResultBytes)
    throw Error("resource_limit",
                "The worker receipt limit was reached; start a new worker.");
  validate(params, entry.descriptor.value("inputSchema").toObject(), "params",
           0);
  m_context.resetMutationStarted();
  try {
    QJsonObject result = applyWrite(entry, method, params);
    const qint64 resultBytes =
        QJsonDocument(result).toJson(QJsonDocument::Compact).size();
    if (resultBytes > maximumResultBytes)
      throw Error("response_too_large",
                  "The operation completed but its result exceeds 8 MiB.",
                  {{"operation_id", operationId},
                   {"outcome", "completed"},
                   {"document_id", result.value("document_id")},
                   {"revision", result.value("revision")}});
    m_receipts.insert(operationId, {fingerprint, {{"result", result}}});
    m_receiptBytes += resultBytes;
    return result;
  } catch (const Error& error) {
    if (m_context.mutationStarted()) {
      m_receipts.insert(operationId,
                        {fingerprint, {{"error", errorObject(error)}}});
      m_receiptBytes += QJsonDocument(errorObject(error))
                            .toJson(QJsonDocument::Compact)
                            .size();
    }
    throw;
  }
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

QJsonObject Dispatcher::applyWrite(const Entry& entry, const QString& method,
                                   const QJsonObject& params) {
  const QString documentId = params.value("document_id").toString();
  quint64 revision         = 0;
  if (!documentId.isEmpty()) {
    const Document& document = m_context.document(documentId);
    revision                 = document.revision;
    if (requiredString(params, "expected_revision") != document.revisionText())
      throw Error(
          "revision_conflict",
          "The document changed; inspect it before writing.",
          {{"document_id", documentId}, {"revision", document.revisionText()}});
    if (revision == std::numeric_limits<quint64>::max())
      throw Error("resource_limit", "The document revision limit was reached.");
  }
  const bool mutatesDocument =
      entry.descriptor.value("mutates_document").toBool(true);
  try {
    QJsonObject result =
        asObject(callHandler(entry.handler, m_context, method, params));
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
