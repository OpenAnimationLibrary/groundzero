#include "automation_live_protocol.h"

#include "automation_build_identity.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>

#include <cmath>
#include <limits>

namespace GroundzeroAutomation {
namespace Live {
namespace {

const QString liveAdvice = " Inspect the live capabilities and retry.";

void invalid(const QString& message) {
  throw Error("invalid_params", message + liveAdvice);
}

bool endsLiteral(char value) {
  return value == ',' || value == '}' || value == ']' || value <= ' ';
}

struct ShapeRules final {
  int maximumDepth;
  bool finiteNumbers;
  QString advice;
};

class JsonShape final {
public:
  JsonShape(const QByteArray& bytes, const ShapeRules& rules)
      : m_bytes(bytes), m_rules(rules) {}
  void validate() { value(1); }

private:
  [[noreturn]] void fail(const QString& message) const {
    throw Error("invalid_params", message + m_rules.advice);
  }

  void space() {
    while (m_offset < m_bytes.size() && m_bytes.at(m_offset) <= ' ') ++m_offset;
  }

  QByteArray stringToken() {
    const int begin = m_offset++;
    while (m_offset < m_bytes.size()) {
      const char next = m_bytes.at(m_offset++);
      if (next == '\\') {
        ++m_offset;
      } else if (next == '"') {
        return m_bytes.mid(begin, m_offset - begin);
      }
    }
    fail("An unterminated JSON string was received.");
  }

  QString key() {
    return QJsonDocument::fromJson("[" + stringToken() + "]")
        .array()
        .at(0)
        .toString();
  }

  void object(int depth) {
    ++m_offset;
    space();
    QSet<QString> keys;
    while (m_bytes.at(m_offset) != '}') {
      const QString name = key();
      if (keys.contains(name))
        fail("Duplicate JSON object keys are not allowed.");
      keys.insert(name);
      space();
      ++m_offset;
      value(depth + 1);
      space();
      if (m_bytes.at(m_offset) != ',') break;
      ++m_offset;
      space();
    }
    ++m_offset;
  }

  void array(int depth) {
    ++m_offset;
    space();
    while (m_bytes.at(m_offset) != ']') {
      value(depth + 1);
      space();
      if (m_bytes.at(m_offset) != ',') break;
      ++m_offset;
    }
    ++m_offset;
  }

  void literal() {
    const int begin = m_offset;
    while (m_offset < m_bytes.size()) {
      const char next = m_bytes.at(m_offset);
      if (endsLiteral(next)) break;
      ++m_offset;
    }
    const QByteArray token = m_bytes.mid(begin, m_offset - begin);
    if (!m_rules.finiteNumbers) return;
    if (token == "true" || token == "false" || token == "null") return;
    bool valid          = false;
    const double number = token.toDouble(&valid);
    if (!valid || !std::isfinite(number)) fail("JSON numbers must be finite.");
  }

  void value(int depth) {
    if (depth > m_rules.maximumDepth)
      fail("JSON nesting exceeds the live protocol limit.");
    space();
    switch (m_bytes.at(m_offset)) {
    case '{':
      object(depth);
      break;
    case '[':
      array(depth);
      break;
    case '"':
      stringToken();
      break;
    default:
      literal();
      break;
    }
  }

  const QByteArray& m_bytes;
  const ShapeRules m_rules;
  int m_offset = 0;
};

QJsonObject stringSchema(int maximum = 128) {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", maximum}};
}

QJsonObject patterned(const QString& pattern) {
  QJsonObject schema = stringSchema();
  schema.insert("pattern", pattern);
  return schema;
}

QJsonObject choices(const QJsonArray& values) {
  QJsonObject schema = stringSchema();
  schema.insert("enum", values);
  return schema;
}

QJsonObject numberSchema(double minimum, double maximum, bool integer = false) {
  return {{"type", integer ? "integer" : "number"},
          {"minimum", minimum},
          {"maximum", maximum}};
}

QJsonObject descriptor(const QString& name, const QString& description,
                       const QJsonObject& properties,
                       const QJsonArray& required, bool write = false) {
  return {{"name", name},
          {"description", description},
          {"inputSchema", QJsonObject{{"type", "object"},
                                      {"properties", properties},
                                      {"required", required},
                                      {"additionalProperties", false}}},
          {"annotations", QJsonObject{{"readOnlyHint", !write},
                                      {"destructiveHint", false},
                                      {"idempotentHint", true},
                                      {"openWorldHint", false}}}};
}

QJsonObject commonWrite() {
  const QString uuid =
      "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$";
  return {{"instance_id", patterned(uuid)},
          {"scene_epoch", patterned("^[0-9]+$")},
          {"operation_id", patterned(uuid)},
          {"expected_fingerprint", patterned("^[0-9a-f]{64}$")}};
}

// `rule` states in words the condition between arguments that the handler
// checks: MCP clients accept only a plain object at an input schema's root.
QJsonObject writeDescriptor(const QString& name, const QString& rule,
                            QJsonObject properties, QJsonArray required) {
  const QJsonObject common = commonWrite();
  for (auto it = common.constBegin(); it != common.constEnd(); ++it) {
    properties.insert(it.key(), it.value());
    required.append(it.key());
  }
  return descriptor(
      name,
      "Edit one qualified existing native scalar with one undo entry. " + rule,
      properties, required, true);
}

void checkString(const QString& value, const QJsonObject& schema) {
  if (value.isEmpty() || value.size() > schema.value("maxLength").toInt())
    invalid("A string exceeds its live schema bounds.");
  if (schema.contains("pattern") &&
      !QRegularExpression(QRegularExpression::anchoredPattern(
                              schema.value("pattern").toString()))
           .match(value)
           .hasMatch())
    invalid("A live identity or fingerprint has an invalid encoding.");
}

QJsonObject stageOperation() {
  return writeDescriptor(
      "live.stage.set_key_value",
      "Channels x and y take unit inch; rotation takes unit degree.",
      {{"object_id", stringSchema()},
       {"channel", choices({"x", "y", "rotation"})},
       {"frame", numberSchema(0, 999999, true)},
       {"value", numberSchema(-1000000, 1000000)},
       {"unit", choices({"inch", "degree"})}},
      {"object_id", "channel", "frame", "value", "unit"});
}

QJsonObject fxOperation() {
  return writeDescriptor(
      "live.fx.set_scalar_value",
      "frame is required with location existing_key and refused with default.",
      {{"fx_id", stringSchema()},
       {"parameter", choices({"value"})},
       {"location", choices({"default", "existing_key"})},
       {"frame", numberSchema(0, 999999, true)},
       {"value", numberSchema(0, 10000)},
       {"unit", choices({"fxLength_native"})}},
      {"fx_id", "parameter", "location", "value", "unit"});
}

void checkConditionalParams(const QString& method, const QJsonObject& params) {
  if (method == "live.fx.set_scalar_value" &&
      params.contains("frame") != (params.value("location") == "existing_key"))
    invalid("frame is required only for an existing FX key.");
  if (method == "live.stage.set_key_value") {
    const QString unit =
        params.value("channel") == "rotation" ? "degree" : "inch";
    if (params.value("unit") != unit)
      invalid("The stage channel requires its exact native unit.");
  }
}

void checkNumber(double value, const QJsonObject& schema) {
  if (!std::isfinite(value) || value < schema.value("minimum").toDouble() ||
      value > schema.value("maximum").toDouble())
    invalid("A numeric argument is outside the live schema bounds.");
  if (schema.value("type") == "integer" && std::floor(value) != value)
    invalid("The frame must be an integer.");
}

void checkValue(const QJsonValue& value, const QJsonObject& schema) {
  const QString type = schema.value("type").toString();
  if (type == "string") {
    if (!value.isString()) invalid("An argument must be a string.");
    checkString(value.toString(), schema);
  } else {
    if (!value.isDouble()) invalid("An argument must be a number.");
    checkNumber(value.toDouble(), schema);
  }
  if (schema.contains("enum") &&
      !schema.value("enum").toArray().contains(value))
    invalid("An argument is not one of the admitted values.");
}

void checkObject(const QJsonObject& params, const QJsonObject& schema) {
  const QJsonObject properties = schema.value("properties").toObject();
  for (const QJsonValue& required : schema.value("required").toArray())
    if (!params.contains(required.toString()))
      invalid("A required argument is missing.");
  for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
    if (!properties.contains(it.key()))
      invalid("Unknown argument: " + it.key());
    checkValue(it.value(), properties.value(it.key()).toObject());
  }
}

}  // namespace

QJsonObject parseMessage(const QByteArray& bytes, int maximumBytes) {
  if (bytes.isEmpty() || bytes.size() > maximumBytes)
    invalid("The live frame length is outside its fixed bound.");
  if (QString::fromUtf8(bytes).toUtf8() != bytes)
    invalid("Live frames require valid UTF-8.");
  QJsonParseError error;
  const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject())
    invalid("A live frame must contain exactly one valid JSON object.");
  JsonShape(bytes, {16, true, liveAdvice}).validate();
  return document.object();
}

void rejectDuplicateKeys(const QByteArray& bytes) {
  const QString advice = " Remove the repeated key and resend the request.";
  JsonShape(bytes, {std::numeric_limits<int>::max(), false, advice}).validate();
}

QByteArray frameMessage(const QJsonObject& value, int maximumBytes) {
  const QByteArray json = QJsonDocument(value).toJson(QJsonDocument::Compact);
  if (json.size() > maximumBytes)
    throw Error("resource_limit",
                "The live response exceeds its fixed bound. Narrow the scene.");
  const quint32 size = static_cast<quint32>(json.size());
  QByteArray frame(4, '\0');
  for (int index = 0; index < 4; ++index)
    frame[index] = static_cast<char>((size >> (24 - index * 8)) & 0xffU);
  frame.append(json);
  return frame;
}

QJsonObject limits() {
  return {{"authenticated_clients", 2},
          {"unauthenticated_sockets", 2},
          {"request_bytes", maximumRequestBytes},
          {"response_bytes", maximumResponseBytes},
          {"pending_gui_requests", 8},
          {"pending_per_client", 4},
          {"admission_deadline_ms", 2000},
          {"inspection_soft_deadline_ms", 25},
          {"stage_objects", 128},
          {"fx_nodes", 64},
          {"level_metadata_records", 256},
          {"root_columns", 64},
          {"exposure_entries", 4096},
          {"parameter_records", 256},
          {"curve_keys", 4096},
          {"handles_per_epoch", 1024},
          {"receipts", 2048},
          {"receipt_bytes", maximumReceiptBytes},
          {"receipt_storage_bytes", 8 * 1024 * 1024}};
}

QJsonArray operations() {
  return {
      descriptor("live.status",
                 "Read cached editor admission and scene identity.", {}, {}),
      descriptor("live.inspect",
                 "Inspect bounded native metadata and qualified edit targets.",
                 {{"scene_epoch", patterned("^[0-9]+$")}}, {"scene_epoch"}),
      stageOperation(),
      fxOperation(),
      descriptor("live.receipt",
                 "Read an app-owned operation receipt without native access.",
                 {{"operation_id", commonWrite().value("operation_id")}},
                 {"operation_id"}),
      descriptor("live.capabilities",
                 "Read this live session's exact scoped native catalog.", {},
                 {})};
}

bool isWrite(const QString& method) {
  return method == "live.stage.set_key_value" ||
         method == "live.fx.set_scalar_value";
}

void validateRequest(const QJsonObject& request) {
  const QSet<QString> fields{"id", "method", "params"};
  if (request.size() != 3)
    invalid("Requests require exactly id, method, and params.");
  for (const QString& key : request.keys())
    if (!fields.contains(key)) invalid("Unknown live request field.");
  if (!request.value("id").isString())
    invalid("The transport ID must be a string.");
  checkString(request.value("id").toString(), stringSchema(64));
  if (!request.value("method").isString() ||
      !request.value("params").isObject())
    invalid("The method and params fields have invalid types.");
}

void validateParams(const QString& method, const QJsonObject& params) {
  for (const QJsonValue& item : operations()) {
    const QJsonObject entry = item.toObject();
    if (entry.value("name") != method) continue;
    checkObject(params, entry.value("inputSchema").toObject());
    checkConditionalParams(method, params);
    return;
  }
  invalid("The requested method is not in the live catalog.");
}

QJsonObject capabilities(const QString& mode) {
  return {{"mode", mode},
          {"protocol", liveProtocol},
          {"build", buildIdentity()},
          {"tools", operations()},
          {"limits", limits()},
          {"profiles", QJsonArray{"qualified_target_dependencies_v1",
                                  "live_root_metadata_v1"}},
          {"fingerprint_exclusions",
           QJsonArray{
               "pixels", "vector_geometry", "palette_colors_and_animation",
               "asset_bytes", "asset_mtimes", "external_reload_contents",
               "render_caches", "arbitrary_plugin_state", "view_selections"}}};
}

QJsonObject liveError(const Error& error) {
  return {{"code", error.code},
          {"message", error.message.left(512)},
          {"details", error.details}};
}

QJsonObject liveIdentity() {
  return {{"protocol", liveProtocol}, {"build", buildIdentity()}};
}

QJsonObject handshakeMismatch(const QJsonObject& authentication) {
  QString code;
  if (authentication.value("protocol") != QJsonValue(liveProtocol))
    code = "protocol_mismatch";
  else if (authentication.value("build") != QJsonValue(buildIdentity()))
    code = "build_mismatch";
  if (code.isEmpty()) return {};
  return {{"error",
           QJsonObject{{"code", code},
                       {"details", QJsonObject{{"editor", liveIdentity()}}}}}};
}

}  // namespace Live
}  // namespace GroundzeroAutomation
