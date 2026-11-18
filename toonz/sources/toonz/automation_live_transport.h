#pragma once

#include "automation_live_state.h"

#include <QObject>
#include <QSet>

class QLocalServer;
class QLocalSocket;
class QTimer;
class QSocketNotifier;

namespace GroundzeroAutomation {
namespace Live {

class SecuritySession;

// The listener and every socket are exclusively owned by the transport thread.
class Transport final : public QObject {
public:
  using Post = std::function<void()>;
  Transport(std::shared_ptr<SharedState> state,
            std::shared_ptr<SecuritySession> security, Post post,
            std::function<void()> stopped);
  void start();
  bool stop();

private:
  struct Pending {
    QString id;
    std::shared_ptr<Ticket> ticket;
  };
  struct Connection {
    QLocalSocket* socket = nullptr;
    QByteArray bytes;
    QString clientId;
    bool refused = false;
    std::chrono::steady_clock::time_point authDeadline;
    QList<Pending> pending;
    QSet<QString> ids;
  };
  void accept();
  QLocalSocket* nextSocket();
  void validateGrant(const QJsonObject& message) const;
  void attach(QLocalSocket* socket);
  void receive(QLocalSocket* socket);
  bool consume(Connection& connection);
  void authenticate(Connection& connection, const QJsonObject& message);
  void request(Connection& connection, const QJsonObject& message);
  void send(Connection& connection, const QJsonObject& response);
  void sweep();
  void sweepConnection(Connection& connection);
  void disconnected(QLocalSocket* socket);
  int count(bool authenticated) const;

  std::shared_ptr<SharedState> m_state;
  std::shared_ptr<SecuritySession> m_security;
  Post m_post;
  std::function<void()> m_onStopped;
  bool m_stopped              = false;
  bool m_cleaned              = true;
  QLocalServer* m_server      = nullptr;
  QSocketNotifier* m_notifier = nullptr;
  QTimer* m_timer             = nullptr;
  QHash<QLocalSocket*, std::shared_ptr<Connection>> m_connections;
};

}  // namespace Live
}  // namespace GroundzeroAutomation
