#include "automation_live_transport.h"
#include "automation_live_security.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QTimer>

namespace GroundzeroAutomation {
namespace Live {
namespace {

quint32 frameSize(const QByteArray& bytes) {
  quint32 size = 0;
  for (int index = 0; index < 4; ++index)
    size = (size << 8) | static_cast<unsigned char>(bytes.at(index));
  return size;
}

void validateHello(const QJsonObject& message) {
  const QSet<QString> fields{"protocol", "instance_id", "client_id", "token"};
  if (message.size() != 4)
    throw Error("authentication_failed", "Invalid live handshake.");
  for (auto it = message.constBegin(); it != message.constEnd(); ++it)
    if (!fields.contains(it.key()) || !it.value().isString())
      throw Error("authentication_failed", "Invalid live handshake.");
  const QRegularExpression uuid(
      "^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
  if (!uuid.match(message.value("client_id").toString()).hasMatch())
    throw Error("authentication_failed", "Invalid live client identity.");
}

}  // namespace

Transport::Transport(std::shared_ptr<SharedState> state,
                     std::shared_ptr<SecuritySession> security, Post post,
                     std::function<void()> stopped)
    : m_state(std::move(state))
    , m_security(std::move(security))
    , m_post(std::move(post))
    , m_onStopped(std::move(stopped)) {}

void Transport::start() {
#ifdef _WIN32
  m_server = new QLocalServer(this);
  m_server->setSocketOptions(QLocalServer::UserAccessOption);
  m_server->setMaxPendingConnections(4);
  connect(m_server, &QLocalServer::newConnection, this, [this]() { accept(); });
  if (!m_server->listen(m_security->endpoint()))
    throw Error("authentication_failed",
                "Cannot create the private live endpoint. Use a new private "
                "directory.");
#else
  m_notifier = new QSocketNotifier(m_security->listenDescriptor(),
                                   QSocketNotifier::Read, this);
  connect(m_notifier, &QSocketNotifier::activated, this,
          [this]() { accept(); });
#endif
  m_security->publish();
  m_timer = new QTimer(this);
  m_timer->setInterval(10);
  connect(m_timer, &QTimer::timeout, this, [this]() { sweep(); });
  m_timer->start();
}

bool Transport::stop() {
  if (m_stopped) return m_cleaned;
  m_stopped = true;
  m_state->revoke();
  if (m_timer) m_timer->stop();
  delete m_notifier;
  m_notifier             = nullptr;
  const auto connections = m_connections;
  for (const auto& connection : connections) connection->socket->abort();
  m_connections.clear();
  if (m_server) m_server->close();
  m_cleaned = m_security->revoke();
  m_onStopped();
  return m_cleaned;
}

int Transport::count(bool authenticated) const {
  int total = 0;
  for (const auto& connection : m_connections)
    if ((!connection->clientId.isEmpty()) == authenticated) ++total;
  return total;
}

void Transport::accept() {
  if (m_stopped) return;
  for (int index = 0; index < 32; ++index) {
    QLocalSocket* socket = nullptr;
    try {
      socket = nextSocket();
      if (!socket) return;
      if (count(false) >= 2)
        throw Error("resource_limit", "Live authentication slots are full.");
      attach(socket);
    } catch (...) {
      if (!socket) {
        qWarning(
            "Live automation stopped after a listener failure. Start a new "
            "explicit session.");
        stop();
        return;
      }
      socket->abort();
      socket->deleteLater();
    }
  }
  QTimer::singleShot(0, this, [this]() { accept(); });
}

QLocalSocket* Transport::nextSocket() {
#ifdef _WIN32
  return m_server->nextPendingConnection();
#else
  const qintptr descriptor = m_security->acceptDescriptor();
  if (descriptor < 0) return nullptr;
  auto* socket = new QLocalSocket(this);
  if (socket->setSocketDescriptor(descriptor)) return socket;
  closeAcceptedDescriptor(descriptor);
  socket->deleteLater();
  throw Error("authentication_failed",
              "Cannot adopt the private live connection.");
#endif
}

void Transport::attach(QLocalSocket* socket) {
  auto connection    = std::make_shared<Connection>();
  connection->socket = socket;
  connection->authDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  socket->setReadBufferSize(maximumRequestBytes + 4);
  m_connections.insert(socket, connection);
  connect(socket, &QLocalSocket::readyRead, this,
          [this, socket]() { receive(socket); });
  connect(socket, &QLocalSocket::disconnected, this,
          [this, socket]() { disconnected(socket); });
  if (socket->bytesAvailable()) receive(socket);
}

void Transport::receive(QLocalSocket* socket) {
  const auto connection = m_connections.value(socket);
  if (!connection) return;
  try {
    for (int count = 0; count < 32; ++count) {
      const qint64 capacity =
          maximumRequestBytes + 4 - connection->bytes.size();
      connection->bytes.append(socket->read(capacity));
      if (!consume(*connection)) return;
      if (!m_connections.contains(socket)) return;
    }
    QTimer::singleShot(0, this, [this, socket]() { receive(socket); });
  } catch (...) {
    socket->abort();
  }
}

bool Transport::consume(Connection& connection) {
  if (connection.bytes.size() < 4) return false;
  const quint32 size = frameSize(connection.bytes);
  if (!size || size > static_cast<quint32>(maximumRequestBytes))
    throw Error("invalid_params", "The live frame length is invalid.");
  if (connection.bytes.size() < static_cast<int>(size) + 4) return false;
  if (connection.clientId.isEmpty())
    m_security->verifyClient(connection.socket->socketDescriptor());
  const QJsonObject message =
      parseMessage(connection.bytes.mid(4, size), maximumRequestBytes);
  connection.bytes.remove(0, static_cast<int>(size) + 4);
  if (connection.clientId.isEmpty())
    authenticate(connection, message);
  else
    request(connection, message);
  return true;
}

void Transport::authenticate(Connection& connection,
                             const QJsonObject& message) {
  validateHello(message);
  if (std::chrono::steady_clock::now() >= connection.authDeadline ||
      count(true) >= 2)
    throw Error("authentication_failed",
                "The live authentication window is closed.");
  validateGrant(message);
  const QString id = message.value("client_id").toString();
  for (const auto& other : m_connections)
    if (other.get() != &connection && other->clientId == id)
      throw Error("authentication_failed",
                  "The live client identity is already attached.");
  connection.clientId = id;
  send(connection, {{"instance_id", m_security->instanceId()},
                    {"client_id", id},
                    {"mode", m_security->mode()},
                    {"tools", operations()},
                    {"limits", limits()}});
}

void Transport::validateGrant(const QJsonObject& message) const {
  if (message.value("protocol") != "groundzero-live-v1" ||
      message.value("instance_id") != m_security->instanceId() ||
      !constantTimeTokenEquals(message.value("token").toString().toUtf8(),
                               m_security->token()))
    throw Error("authentication_failed",
                "The live grant does not match this instance.");
}

void Transport::request(Connection& connection, const QJsonObject& message) {
  validateRequest(message);
  const QString id = message.value("id").toString();
  if (connection.ids.contains(id))
    throw Error("invalid_params", "A transport request ID is already pending.");
  try {
    const Admission admission = m_state->admit(connection.clientId, message,
                                               connection.pending.size() < 4);
    if (!admission.immediate.isEmpty()) {
      QJsonObject response = admission.immediate;
      response.insert("id", id);
      send(connection, response);
      return;
    }
    connection.pending.append({id, admission.ticket});
    connection.ids.insert(id);
    if (admission.wake) m_post();
  } catch (const Error& error) {
    send(connection, {{"id", id}, {"error", liveError(error)}});
  }
}

void Transport::send(Connection& connection, const QJsonObject& response) {
  const QByteArray bytes = frameMessage(response, maximumResponseBytes);
  if (connection.socket->bytesToWrite() + bytes.size() >
      2 * maximumResponseBytes)
    throw Error("resource_limit", "The live client is not draining responses.");
  if (connection.socket->write(bytes) != bytes.size())
    connection.socket->abort();
}

void Transport::sweepConnection(Connection& connection) {
  if (connection.clientId.isEmpty() &&
      std::chrono::steady_clock::now() >= connection.authDeadline) {
    connection.socket->abort();
    return;
  }
  for (int index = connection.pending.size() - 1; index >= 0; --index) {
    const Pending item   = connection.pending.at(index);
    QJsonObject response = m_state->poll(item.ticket);
    if (response.isEmpty()) continue;
    response.insert("id", item.id);
    connection.ids.remove(item.id);
    connection.pending.removeAt(index);
    send(connection, response);
  }
}

void Transport::sweep() {
  const auto connections = m_connections;
  for (const auto& connection : connections) {
    try {
      sweepConnection(*connection);
    } catch (...) {
      connection->socket->abort();
    }
  }
}

void Transport::disconnected(QLocalSocket* socket) {
  const auto connection = m_connections.take(socket);
  if (connection && !connection->clientId.isEmpty())
    m_state->disconnect(connection->clientId);
  socket->deleteLater();
}

}  // namespace Live
}  // namespace GroundzeroAutomation
