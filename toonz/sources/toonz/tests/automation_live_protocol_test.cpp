#include "automation_build_identity.h"
#include "automation_live_protocol.h"
#include "automation_live_state.h"

#include <QCoreApplication>
#include <QJsonDocument>

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace GroundzeroAutomation {
namespace Live {
namespace {

const QString instance = "00000000-0000-4000-8000-000000000001";

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void expectError(const std::function<void()>& operation, const QString& code) {
  try {
    operation();
  } catch (const Error& error) {
    require(error.code == code,
            "The live rejection used an unexpected error code");
    return;
  }
  throw std::runtime_error("The live operation did not reject invalid input");
}

QString operationId(int value) {
  return "00000000-0000-4000-8000-" +
         QString::number(value, 16).rightJustified(12, '0');
}

QJsonObject write(int value) {
  return {{"id", "request"},
          {"method", "live.stage.set_key_value"},
          {"params", QJsonObject{{"instance_id", instance},
                                 {"scene_epoch", "1"},
                                 {"operation_id", operationId(value)},
                                 {"expected_fingerprint", QString(64, '0')},
                                 {"object_id", "stage_fixture"},
                                 {"channel", "x"},
                                 {"frame", 0},
                                 {"value", 1},
                                 {"unit", "inch"}}}};
}

QJsonObject read(const QString& method, const QJsonObject& params = {}) {
  return {{"id", "read"}, {"method", method}, {"params", params}};
}

QJsonObject completed(const std::shared_ptr<Ticket>& ticket) {
  return {{"result", QJsonObject{{"instance_id", instance},
                                 {"operation_id", ticket->operationId},
                                 {"state", "completed"},
                                 {"changed", true}}}};
}

QJsonObject maximumOutcome(const std::shared_ptr<Ticket>& ticket) {
  QJsonObject outcome = completed(ticket);
  QJsonObject result  = outcome.value("result").toObject();
  result.insert("payload", "");
  outcome.insert("result", result);
  const int overhead =
      QJsonDocument(outcome).toJson(QJsonDocument::Compact).size();
  require(overhead < maximumReceiptBytes,
          "Receipt fixture has no payload capacity");
  result.insert("payload", QString(maximumReceiptBytes - overhead, 'x'));
  outcome.insert("result", result);
  return outcome;
}

void parserFailures() {
  const QList<QByteArray> invalid{"{\"x\":1,\"x\":2}",
                                  "{\"x\":1,\"\\u0078\":2}",
                                  "{\"x\":{\"nested\":1,\"nested\":2}}",
                                  "[]",
                                  "{}{}",
                                  "{\"x\":NaN}",
                                  "{\"x\":1e9999}",
                                  QByteArray("{\"x\":\"") + char(0xff) + "\"}"};
  for (const QByteArray& input : invalid)
    expectError([&]() { parseMessage(input, maximumRequestBytes); },
                "invalid_params");
  QByteArray deep("0");
  for (int index = 0; index < 20; ++index) deep = "{\"x\":" + deep + "}";
  expectError([&]() { parseMessage(deep, maximumRequestBytes); },
              "invalid_params");
}

void workerDuplicateKeys() {
  const QList<QByteArray> duplicated{
      "{\"x\":1,\"x\":2}", "{\"x\":1,\"\\u0078\":2}",
      "{\"a\":{\"x\":1},\"x\":{\"nested\":1,\"nested\":2}}",
      "{\"x\":[{\"y\":1},{\"z\":1,\"z\":1}]}",
      "{\"\\u00e9\":1,\"\xc3\xa9\":2}"};
  for (const QByteArray& input : duplicated) {
    try {
      rejectDuplicateKeys(input);
    } catch (const Error& error) {
      require(error.code == "invalid_params" &&
                  !error.message.contains("live capabilities"),
              "Worker duplicate-key refusal has the wrong code or advice");
      continue;
    }
    throw std::runtime_error("A duplicate JSON key was accepted: " +
                             std::string(input.constData()));
  }
  QByteArray deep("0");
  for (int index = 0; index < 100; ++index) deep = "{\"x\":" + deep + "}";
  const QList<QByteArray> distinct{"{\"x\":1,\"y\":2}",
                                   "{\"x\":{\"x\":1},\"y\":{\"x\":2}}",
                                   "{\"x\":[{\"y\":1},{\"y\":2}]}",
                                   "{\"x\":\"x\",\"y\":\"y\"}",
                                   "{\"x\":1,\"X\":2}",
                                   "{\"x\":1e-400}",
                                   deep};
  for (const QByteArray& input : distinct) rejectDuplicateKeys(input);
}

void liveIdentifiersAreWholeString() {
  const QJsonObject valid = write(3).value("params").toObject();
  const QString method    = "live.stage.set_key_value";
  validateParams(method, valid);
  for (const QString& field :
       {QString("instance_id"), QString("scene_epoch"), QString("operation_id"),
        QString("expected_fingerprint")}) {
    const QString original = valid.value(field).toString();
    for (const QString& broken :
         {original + "\n", original + "\r\n", "\n" + original,
          original.left(original.size() - 1) + "\n"}) {
      QJsonObject params = valid;
      params.insert(field, broken);
      expectError([&]() { validateParams(method, params); }, "invalid_params");
    }
  }
}

void parserBounds() {
  const QByteArray maximum =
      "{\"x\":\"" + QByteArray(maximumRequestBytes - 8, 'x') + "\"}";
  require(parseMessage(maximum, maximumRequestBytes).contains("x"),
          "Maximum frame was rejected");
  expectError([&]() { parseMessage(maximum + " ", maximumRequestBytes); },
              "invalid_params");
  const QJsonObject original{{"text", QString::fromUtf8("中文")},
                             {"finite", -0.25}};
  const QByteArray framed = frameMessage(original, maximumResponseBytes);
  quint32 size            = 0;
  for (int index = 0; index < 4; ++index)
    size = (size << 8) | static_cast<unsigned char>(framed.at(index));
  require(size == static_cast<quint32>(framed.size() - 4),
          "Framing length is not UTF-8 bytes");
  require(parseMessage(framed.mid(4), maximumResponseBytes) == original,
          "Framing changed the JSON object");
  expectError([&]() { frameMessage(original, 4); }, "resource_limit");
}

void schemas() {
  QJsonObject request = write(1);
  validateRequest(request);
  validateParams(request.value("method").toString(),
                 request.value("params").toObject());
  request.insert("extra", true);
  expectError([&]() { validateRequest(request); }, "invalid_params");
  QJsonObject params = write(2).value("params").toObject();
  params.insert("unit", "degree");
  expectError([&]() { validateParams("live.stage.set_key_value", params); },
              "invalid_params");
  params.insert("channel", "rotation");
  validateParams("live.stage.set_key_value", params);
  params.insert("revision", "1");
  expectError([&]() { validateParams("live.stage.set_key_value", params); },
              "invalid_params");
}

void plainSchemaRoots() {
  const QJsonArray tools = capabilities("edit").value("tools").toArray();
  require(tools.size() == 6, "The live catalog has six tools");
  for (const QJsonValue& tool : tools) {
    const QJsonObject schema = tool.toObject().value("inputSchema").toObject();
    require(schema.value("type") == "object",
            "A live input schema must be an object");
    for (const char* keyword : {"oneOf", "anyOf", "allOf", "not", "$ref"})
      require(!schema.contains(keyword),
              "A live input schema has a combinator at its root");
  }
}

void fxSchemas() {
  QJsonObject params = write(3).value("params").toObject();
  for (const char* field : {"object_id", "channel", "frame"})
    params.remove(field);
  params.insert("fx_id", "fx_fixture");
  params.insert("parameter", "value");
  params.insert("unit", "fxLength_native");
  params.insert("location", "default");
  validateParams("live.fx.set_scalar_value", params);
  params.insert("frame", 0);
  expectError([&]() { validateParams("live.fx.set_scalar_value", params); },
              "invalid_params");
  params.insert("location", "existing_key");
  validateParams("live.fx.set_scalar_value", params);
  params.remove("frame");
  expectError([&]() { validateParams("live.fx.set_scalar_value", params); },
              "invalid_params");
}

void sharedReplay() {
  SharedState state(instance, "edit");
  const QJsonObject request = write(4);
  const Admission admission = state.admit("client-a", request, true);
  require(admission.ticket && admission.wake,
          "First edit did not schedule one GUI wake");
  QJsonObject duplicate = request;
  duplicate.insert("id", "other-transport-id");
  const Admission pending = state.admit("client-b", duplicate, false);
  require(
      pending.immediate.value("result").toObject().value("state") == "pending",
      "Cross-client pending retry was not shared");
  require(state.takeNext() == admission.ticket,
          "GUI admission selected the wrong request");
  const QJsonObject outcome = completed(admission.ticket);
  state.finish(admission.ticket, outcome);
  require(state.admit("client-b", duplicate, false).immediate == outcome,
          "Cross-client exact replay changed the native outcome");
  QJsonObject changed = duplicate.value("params").toObject();
  changed.insert("scene_epoch", "2");
  duplicate.insert("params", changed);
  expectError([&]() { state.admit("client-b", duplicate, true); },
              "operation_conflict");
  const auto receipt = state.admit(
      "client-b", read("live.receipt", {{"operation_id", operationId(4)}}),
      false);
  require(receipt.immediate.value("result").toObject().value("result") ==
              outcome.value("result"),
          "Receipt lookup did not retain the original result");
}

void queueAndDeadline() {
  SharedState state(instance, "edit");
  QList<Admission> pending;
  for (int index = 0; index < 8; ++index) {
    pending.append(state.admit(index < 4 ? "client-a" : "client-b",
                               write(10 + index), true));
    require(bool(pending.last().ticket),
            "A bounded queue slot was unexpectedly rejected");
    require(pending.last().wake == (index == 0),
            "GUI wakeups were not coalesced");
  }
  const Admission full = state.admit("client-a", write(18), true);
  require(full.immediate.value("error").toObject().value("code") ==
              "resource_limit",
          "The GUI queue exceeded its global or per-client bound");
  const auto started = state.takeNext();
  state.disconnect("client-a");
  require(state.poll(started).isEmpty(),
          "Disconnect canceled an already-started edit");
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  require(state.poll(started).isEmpty(),
          "The admission timer expired an already-started edit");
  require(state.poll(pending.at(4).ticket)
                  .value("error")
                  .toObject()
                  .value("code") == "deadline_exceeded",
          "An unstarted edit survived its admission deadline");
  for (const Admission& item : pending) state.poll(item.ticket);
  const auto next = state.admit("client-b", write(19), true);
  require(next.ticket && !next.wake,
          "Expired requests created additional blocked-GUI wakeups");
  state.finish(started, completed(started));
  require(state.takeNext() == next.ticket,
          "GUI did not select the new unexpired request");
  state.finish(next.ticket, completed(next.ticket));
  require(!state.takeNext(), "The GUI queue retained terminal requests");
  require(state.admit("client-b", write(18), true).immediate == full.immediate,
          "A rejected queued edit was reapplied after capacity recovered");
}

void replyPressure() {
  SharedState state(instance, "edit");
  const auto rejected = state.admit("client-a", write(20), false);
  require(rejected.immediate.value("error").toObject().value("code") ==
              "resource_limit",
          "Undrained responses did not block another retained GUI result");
  require(
      state.admit("client-b", write(20), true).immediate == rejected.immediate,
      "Reply-pressure rejection was not retained");
  require(state.admit("client-a", read("live.status"), false)
              .immediate.contains("result"),
          "Reply pressure blocked cached status");
  require(state
              .admit("client-a",
                     read("live.receipt", {{"operation_id", operationId(20)}}),
                     false)
              .immediate.contains("result"),
          "Reply pressure blocked receipt lookup");
}

void capacity() {
  SharedState state(instance, "edit");
  QJsonObject oldest;
  qint64 retainedBytes = 0;
  for (int index = 0; index < 2048; ++index) {
    const auto admission = state.admit("client", write(100 + index), true);
    require(bool(admission.ticket), "Receipt capacity was exhausted too early");
    require(state.takeNext() == admission.ticket,
            "Receipt capacity was exhausted too early");
    const QJsonObject outcome = maximumOutcome(admission.ticket);
    retainedBytes +=
        QJsonDocument(outcome).toJson(QJsonDocument::Compact).size();
    state.finish(admission.ticket, outcome);
    require(state.poll(admission.ticket) == outcome,
            "A maximum-size receipt was not retained");
    if (!index) oldest = outcome;
  }
  require(retainedBytes == 8 * 1024 * 1024,
          "Receipt fixture did not fill the exact 8 MiB budget");
  expectError([&]() { state.admit("client", write(3000), true); },
              "resource_limit");
  require(state.admit("other-client", write(100), false).immediate == oldest,
          "Oldest receipt was evicted at capacity");
  require(state.admit("client", read("live.status"), false)
              .immediate.contains("result"),
          "Receipt exhaustion blocked cached reads");
}

void boundedOutcome() {
  SharedState state(instance, "edit");
  state.updateStatus({{"write_admission", true}});
  const auto admission = state.admit("client", write(4000), true);
  require(state.takeNext() == admission.ticket,
          "Oversized-outcome edit was not admitted");
  state.finish(admission.ticket, {{"result", QString(4096, QChar(1))}});
  const QJsonObject outcome = state.poll(admission.ticket);
  require(
      outcome.value("error").toObject().value("code") == "partially_applied",
      "An oversized native outcome did not fail closed");
  require(QJsonDocument(outcome).toJson(QJsonDocument::Compact).size() <=
              maximumReceiptBytes,
          "The fallback receipt exceeded its reserved byte capacity");
  require(state.writesDisabled(),
          "Oversized outcome left native writes enabled");
  const auto status = state.admit("client", read("live.status"), false);
  require(status.immediate.value("result").toObject().value(
              "write_admission") == QJsonValue(false),
          "A partial outcome left cached write admission enabled");
  const auto next = state.admit("client", write(4001), true);
  require(next.immediate.value("error").toObject().value("code") ==
              "unsupported_target",
          "A partial outcome did not disable future edits");
  require(state.admit("other-client", write(4000), false).immediate == outcome,
          "Bounded partial outcome changed on replay");
}

void permissionAndStop() {
  SharedState inspect(instance, "inspect");
  const auto denied = inspect.admit("client", write(5000), true);
  require(denied.immediate.value("error").toObject().value("code") ==
              "permission_denied",
          "Inspection mode admitted an edit");
  require(inspect.admit("other-client", write(5000), true).immediate ==
              denied.immediate,
          "Inspection-mode rejection was not shared");
  SharedState edit(instance, "edit");
  const auto active = edit.admit("client", write(5002), true);
  require(edit.takeNext() == active.ticket,
          "Stop fixture did not start its first request");
  const auto pending = edit.admit("client", write(5001), true);
  edit.revoke();
  require(edit.poll(pending.ticket).value("error").toObject().value("code") ==
              "gui_busy",
          "Stop did not reject unstarted work");
  require(edit.poll(active.ticket).isEmpty(),
          "Stop replaced an already-started outcome");
  const QJsonObject outcome = completed(active.ticket);
  edit.finish(active.ticket, outcome);
  require(edit.poll(active.ticket) == outcome,
          "Stop prevented finalizing the started outcome");
  require(!edit.takeNext(), "A revoked request reached GUI admission");
  expectError([&]() { edit.admit("client", read("live.status"), true); },
              "gui_busy");
}

void buildIdentitySchema() {
  const QJsonObject identity = buildIdentity();
  require(validBuildIdentity(identity) && identity == buildIdentity(),
          "The build identity is malformed or not stable");
  for (const auto& change :
       {std::pair<QString, QJsonValue>{"application", "other"},
        {"version", ""},
        {"version", QString(65, '1')},
        {"build_digest", QString(63, 'a')},
        {"build_digest", QString(64, 'A')},
        {"extra", 1}}) {
    QJsonObject changed = identity;
    changed.insert(change.first, change.second);
    require(!validBuildIdentity(changed), "A malformed build identity passed");
  }
}

void authenticationVersionChecks() {
  const QJsonObject valid{{"protocol", liveProtocol},
                          {"build", buildIdentity()},
                          {"instance_id", instance},
                          {"client_id", operationId(1)},
                          {"token", QString(64, 'a')}};
  require(handshakeMismatch(valid).isEmpty(),
          "A matching handshake was refused");
  QJsonObject foreignBuild = valid;
  QJsonObject build        = buildIdentity();
  build.insert("build_digest", QString(64, 'f'));
  foreignBuild.insert("build", build);
  QJsonObject previous = valid;
  previous.insert("protocol", "groundzero-live-v1");
  QJsonObject foreignWithToken = foreignBuild;
  foreignWithToken.insert("token", QString(64, 'b'));
  const QJsonObject editor = liveIdentity();
  for (const auto& entry :
       {std::pair<QJsonObject, QString>{foreignBuild, "build_mismatch"},
        {previous, "protocol_mismatch"},
        {foreignWithToken, "build_mismatch"}}) {
    const QJsonObject refused = handshakeMismatch(entry.first);
    const QJsonObject expected{{"code", entry.second},
                               {"details", QJsonObject{{"editor", editor}}}};
    require(refused == QJsonObject{{"error", expected}},
            "A foreign handshake was not refused with the editor identity");
  }
  SharedState state(instance, "inspect");
  for (const QString& method :
       QStringList{"live.status", "live.capabilities"}) {
    const QJsonObject result = state.admit("client", read(method), true)
                                   .immediate.value("result")
                                   .toObject();
    require(result.value("protocol") == QJsonValue(liveProtocol) &&
                result.value("build") == QJsonValue(buildIdentity()),
            "A cached live read omitted its protocol or build");
  }
}

}  // namespace

void runAutomationLiveProtocolTests() {
  const std::pair<const char*, void (*)()> cases[]{
      {"parser failures", parserFailures},
      {"worker duplicate keys", workerDuplicateKeys},
      {"live identifiers", liveIdentifiersAreWholeString},
      {"parser bounds", parserBounds},
      {"stage schemas", schemas},
      {"FX schemas", fxSchemas},
      {"plain schema roots", plainSchemaRoots},
      {"shared replay", sharedReplay},
      {"queue deadline and disconnect", queueAndDeadline},
      {"reply pressure", replyPressure},
      {"receipt capacity", capacity},
      {"bounded native outcome", boundedOutcome},
      {"permission and Stop", permissionAndStop},
      {"build identity schema", buildIdentitySchema},
      {"authentication version checks", authenticationVersionChecks}};
  for (const auto& test : cases) {
    test.second();
    std::cout << "automation-live-protocol-test: " << test.first << " PASS\n";
  }
}

}  // namespace Live
}  // namespace GroundzeroAutomation

#ifdef AUTOMATION_LIVE_PROTOCOL_STANDALONE
int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  try {
    GroundzeroAutomation::Live::runAutomationLiveProtocolTests();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "automation-live-protocol-test: " << error.what() << '\n';
    return 1;
  }
}
#endif
