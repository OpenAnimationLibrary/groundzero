#include "automation_live_state.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMutexLocker>

namespace GroundzeroAutomation {
namespace Live {
namespace {

QByteArray fingerprint(const QJsonObject& request) {
  return QCryptographicHash::hash(
      QJsonDocument(QJsonObject{{"method", request.value("method")},
                                {"params", request.value("params")}})
          .toJson(QJsonDocument::Compact),
      QCryptographicHash::Sha256);
}

QJsonObject failure(const QString& code, const QString& message,
                    const QJsonObject& details = {}) {
  return {{"error", liveError(Error(code, message, details))}};
}

}  // namespace

SharedState::SharedState(QString instanceId, QString mode)
    : m_instanceId(std::move(instanceId)), m_mode(std::move(mode)) {
  m_status = {{"instance_id", m_instanceId},
              {"mode", m_mode},
              {"scene_epoch", QJsonValue()},
              {"observation_serial", "0"},
              {"busy", true},
              {"busy_observation_cached", true},
              {"admission_observation_cached", true},
              {"write_admission", false},
              {"reasons", QJsonArray{"starting"}},
              {"limits", limits()}};
}

void SharedState::checkAvailable() const {
  if (m_stopped)
    throw Error(
        "gui_busy",
        "Live automation is stopped. Start a new explicitly enabled session.");
}

QJsonObject SharedState::replay(const std::shared_ptr<Ticket>& ticket) const {
  if (ticket->phase == Ticket::Phase::Terminal) return ticket->outcome;
  return {{"result", QJsonObject{{"instance_id", m_instanceId},
                                 {"operation_id", ticket->operationId},
                                 {"state", "pending"}}}};
}

QJsonObject SharedState::receipt(const QString& operationId) const {
  QJsonObject result{{"instance_id", m_instanceId},
                     {"operation_id", operationId},
                     {"state", "unknown"}};
  const auto found = m_receipts.constFind(operationId);
  if (found == m_receipts.constEnd()) return result;
  const Ticket& ticket = *found.value();
  if (ticket.phase != Ticket::Phase::Terminal) {
    result.insert("state", "pending");
  } else if (ticket.outcome.contains("result")) {
    result.insert("state", "completed");
    result.insert("result", ticket.outcome.value("result"));
  } else {
    const QJsonObject error = ticket.outcome.value("error").toObject();
    result.insert("state", error.value("code") == "partially_applied"
                               ? "partially_applied"
                               : "rejected");
    result.insert("error", error);
  }
  return result;
}

Admission SharedState::cachedRead(const QJsonObject& request) const {
  const QString method = request.value("method").toString();
  if (method == "live.status") return {{}, {{"result", m_status}}};
  if (method == "live.capabilities")
    return {{}, {{"result", capabilities(m_mode)}}};
  if (method == "live.receipt")
    return {{},
            {{"result", receipt(request.value("params")
                                    .toObject()
                                    .value("operation_id")
                                    .toString())}}};
  return {};
}

Admission SharedState::admit(const QString& clientId,
                             const QJsonObject& request, bool replyAvailable) {
  QMutexLocker lock(&m_mutex);
  checkAvailable();
  const QString method     = request.value("method").toString();
  const QJsonObject params = request.value("params").toObject();
  if (isWrite(method)) {
    const auto found =
        m_receipts.constFind(params.value("operation_id").toString());
    if (found != m_receipts.constEnd()) {
      if (found.value()->digest != fingerprint(request))
        throw Error(
            "operation_conflict",
            "This operation UUID has different arguments. Use a new UUID.");
      return {{}, replay(found.value())};
    }
  }
  validateParams(method, params);
  const Admission cached = cachedRead(request);
  if (!cached.immediate.isEmpty()) return cached;
  return enqueue(clientId, request, replyAvailable);
}

Admission SharedState::enqueue(const QString& clientId,
                               const QJsonObject& request,
                               bool replyAvailable) {
  const bool write = isWrite(request.value("method").toString());
  if (write && m_receipts.size() >= 2048)
    throw Error("resource_limit",
                "Live receipt capacity is full. Inspect state and start a new "
                "session.");
  auto ticket      = std::make_shared<Ticket>();
  ticket->clientId = clientId;
  ticket->operationId =
      request.value("params").toObject().value("operation_id").toString();
  ticket->request  = request;
  ticket->write    = write;
  ticket->digest   = fingerprint(request);
  ticket->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  if (write)
    m_receipts.insert(
        request.value("params").toObject().value("operation_id").toString(),
        ticket);
  checkAdmission(*ticket, replyAvailable);
  if (ticket->phase == Ticket::Phase::Terminal) return {{}, ticket->outcome};
  m_queued.append(ticket);
  ticket->counted = true;
  ++m_pendingTotal;
  ++m_pendingByClient[clientId];
  const bool wake = !m_wakeQueued;
  m_wakeQueued    = true;
  return {ticket, {}, wake};
}

void SharedState::checkAdmission(Ticket& ticket, bool replyAvailable) {
  if (ticket.write && m_mode != "edit")
    rejectTicket(
        ticket, "permission_denied",
        "This instance grants inspection only. Start an edit session.");
  else if (ticket.write && m_writesDisabled)
    rejectTicket(ticket, "unsupported_target",
                 "A partial live edit disabled writes. Inspect and restart.");
  else if (!replyAvailable || m_pendingTotal >= 8 ||
           m_pendingByClient.value(ticket.clientId) >= 4)
    rejectTicket(
        ticket, "resource_limit",
        "The live GUI queue is full. Retry with a new operation UUID.");
}

void SharedState::rejectTicket(Ticket& ticket, const QString& code,
                               const QString& message) {
  releaseCapacity(ticket);
  ticket.phase = Ticket::Phase::Terminal;
  ticket.outcome =
      failure(code, message, {{"operation_id", ticket.operationId}});
  ticket.request = {};
}

void SharedState::retireQueue(Ticket& ticket) {
  for (int index = 0; index < m_queued.size(); ++index) {
    if (m_queued.at(index).get() != &ticket) continue;
    m_queued.removeAt(index);
    return;
  }
}

void SharedState::releaseCapacity(Ticket& ticket) {
  if (!ticket.counted) return;
  ticket.counted = false;
  --m_pendingTotal;
  const int remaining = m_pendingByClient.value(ticket.clientId) - 1;
  if (remaining)
    m_pendingByClient[ticket.clientId] = remaining;
  else
    m_pendingByClient.remove(ticket.clientId);
}

void SharedState::expire(Ticket& ticket) {
  if (ticket.phase != Ticket::Phase::Queued ||
      std::chrono::steady_clock::now() < ticket.deadline)
    return;
  retireQueue(ticket);
  rejectTicket(ticket, "deadline_exceeded",
               "GUI admission expired before editing. Reinspect and retry.");
}

std::shared_ptr<Ticket> SharedState::takeNext() {
  QMutexLocker lock(&m_mutex);
  const auto queued = m_queued;
  for (const auto& ticket : queued) expire(*ticket);
  if (m_stopped || m_queued.isEmpty()) {
    m_wakeQueued = false;
    return {};
  }
  const auto ticket = m_queued.first();
  retireQueue(*ticket);
  ticket->phase = Ticket::Phase::Started;
  return ticket;
}

void SharedState::finish(const std::shared_ptr<Ticket>& ticket,
                         const QJsonObject& outcome) {
  const bool oversized =
      ticket->write &&
      QJsonDocument(outcome).toJson(QJsonDocument::Compact).size() >
          maximumReceiptBytes;
  QMutexLocker lock(&m_mutex);
  if (ticket->phase != Ticket::Phase::Started) return;
  releaseCapacity(*ticket);
  if (oversized) {
    m_writesDisabled = true;
    ticket->outcome  = failure(
        "partially_applied",
        "The live outcome exceeded reserved storage. Inspect editor state and "
        "restart.",
        {{"operation_id", ticket->operationId}, {"outcome", "unknown"}});
  } else
    ticket->outcome = outcome;
  ticket->phase   = Ticket::Phase::Terminal;
  ticket->request = {};
}

QJsonObject SharedState::poll(const std::shared_ptr<Ticket>& ticket) {
  QMutexLocker lock(&m_mutex);
  expire(*ticket);
  return ticket->phase == Ticket::Phase::Terminal ? ticket->outcome
                                                  : QJsonObject{};
}

void SharedState::disconnect(const QString& clientId) {
  QMutexLocker lock(&m_mutex);
  const auto pending = m_queued;
  for (const auto& ticket : pending) {
    if (ticket->clientId != clientId) continue;
    retireQueue(*ticket);
    rejectTicket(
        *ticket, "gui_busy",
        "The client disconnected before admission. Inspect its receipt.");
  }
}

void SharedState::revoke() {
  QMutexLocker lock(&m_mutex);
  m_stopped          = true;
  const auto pending = m_queued;
  for (const auto& ticket : pending) {
    retireQueue(*ticket);
    rejectTicket(
        *ticket, "gui_busy",
        "The live grant was revoked before admission. Start a new session.");
  }
}

void SharedState::updateStatus(QJsonObject status) {
  QMutexLocker lock(&m_mutex);
  status.insert("instance_id", m_instanceId);
  status.insert("mode", m_mode);
  status.insert("limits", limits());
  status.insert("busy_observation_cached", true);
  status.insert("admission_observation_cached", true);
  if (m_mode != "edit" || m_writesDisabled)
    status.insert("write_admission", false);
  m_status = std::move(status);
}

void SharedState::disableWrites() {
  QMutexLocker lock(&m_mutex);
  m_writesDisabled = true;
  m_status.insert("write_admission", false);
}

bool SharedState::writesDisabled() const {
  QMutexLocker lock(&m_mutex);
  return m_writesDisabled;
}

}  // namespace Live
}  // namespace GroundzeroAutomation
