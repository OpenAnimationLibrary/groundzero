#pragma once

#include "automation_live_protocol.h"

#include <chrono>
#include <mutex>

namespace GroundzeroAutomation {
namespace Live {

struct Ticket final {
  enum class Phase { Queued, Started, Terminal };
  QString clientId;
  QString operationId;
  QJsonObject request;
  QByteArray digest;
  std::chrono::steady_clock::time_point deadline;
  Phase phase = Phase::Queued;
  QJsonObject outcome;
  bool write   = false;
  bool counted = false;
};

struct Admission final {
  std::shared_ptr<Ticket> ticket;
  QJsonObject immediate;
  bool wake = false;
};

// Every member is protected by the mutex. Native callbacks never run under it.
class SharedState final {
public:
  SharedState(QString instanceId, QString mode);
  Admission admit(const QString& clientId, const QJsonObject& request,
                  bool replyAvailable);
  std::shared_ptr<Ticket> takeNext();
  void finish(const std::shared_ptr<Ticket>& ticket,
              const QJsonObject& outcome);
  QJsonObject poll(const std::shared_ptr<Ticket>& ticket);
  void disconnect(const QString& clientId);
  void revoke();
  void updateStatus(QJsonObject status);
  void disableWrites();
  bool writesDisabled() const;

private:
  QJsonObject replay(const std::shared_ptr<Ticket>& ticket) const;
  QJsonObject receipt(const QString& operationId) const;
  Admission cachedRead(const QJsonObject& request) const;
  void rejectTicket(Ticket& ticket, const QString& code,
                    const QString& message);
  void retireQueue(Ticket& ticket);
  void releaseCapacity(Ticket& ticket);
  void expire(Ticket& ticket);
  Admission enqueue(const QString& clientId, const QJsonObject& request,
                    bool replyAvailable);
  void checkAdmission(Ticket& ticket, bool replyAvailable);
  void checkAvailable() const;

  mutable std::mutex m_mutex;
  const QString m_instanceId;
  const QString m_mode;
  QHash<QString, std::shared_ptr<Ticket>> m_receipts;
  QList<std::shared_ptr<Ticket>> m_queued;
  QHash<QString, int> m_pendingByClient;
  int m_pendingTotal = 0;
  QJsonObject m_status;
  bool m_stopped        = false;
  bool m_writesDisabled = false;
  bool m_wakeQueued     = false;
};

}  // namespace Live
}  // namespace GroundzeroAutomation
