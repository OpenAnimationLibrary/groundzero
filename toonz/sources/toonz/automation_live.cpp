#include "automation_live.h"
#include "automation_live_protocol.h"
#include "automation_live_security.h"
#include "automation_live_state.h"
#include "automation_live_transport.h"

#include "texception.h"

#include <QApplication>
#include <QDebug>
#include <QJsonDocument>
#include <QMetaObject>
#include <QScopedValueRollback>
#include <QThread>
#include <QTimer>

#include <exception>

namespace GroundzeroAutomation {
namespace Live {
namespace {

QJsonArray reasonsArray(const QStringList& reasons) {
  QJsonArray result;
  for (const QString& reason : reasons.mid(0, 16))
    result.append(reason.left(256));
  return result;
}

QJsonObject errorOutcome(const Error& error) {
  return {{"error", liveError(error)}};
}

QJsonObject errorContext(const Ticket& ticket, QJsonObject details) {
  if (!ticket.write) return details;
  details.insert("operation_id", ticket.operationId);
  const QJsonObject params = ticket.request.value("params").toObject();
  for (const char* key : {"object_id", "fx_id", "channel", "parameter"})
    if (params.contains(key)) details.insert(key, params.value(key));
  return details;
}

}  // namespace

class Service::Impl final : public QObject {
public:
  Impl(Options options, BusyProbe busyProbe)
      : m_options(std::move(options)), m_busyProbe(std::move(busyProbe)) {}

  void start() {
    requireGuiThread();
    if (m_started)
      throw Error("gui_busy",
                  "A live grant cannot restart. Launch a new editor instance.");
    if (m_options.mode != "inspect" && m_options.mode != "edit")
      throw Error("invalid_params",
                  "Choose the inspect or edit live launch mode.");
    m_started  = true;
    m_security = std::make_shared<SecuritySession>(m_options.sessionPath,
                                                   m_options.mode);
    m_state =
        std::make_shared<SharedState>(m_security->instanceId(), m_options.mode);
    m_scene     = createSceneState(m_security->instanceId());
    m_transport = new Transport(
        m_state, m_security,
        [this]() {
          QMetaObject::invokeMethod(
              this, [this]() { execute(); }, Qt::QueuedConnection);
        },
        [this]() {
          QMetaObject::invokeMethod(
              this, [this]() { stop(); }, Qt::QueuedConnection);
        });
    m_transport->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_transport, &QObject::deleteLater);
    m_thread.start();
    startTransport();
    m_running     = true;
    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(50);
    connect(m_statusTimer, &QTimer::timeout, this,
            [this]() { refreshStatus(); });
    m_statusTimer->start();
    refreshStatus();
  }

  void stop() noexcept {
    if (m_stopped) return;
    if (m_active) {
      m_stopRequested = true;
      m_running       = false;
      m_state->revoke();
      return;
    }
    m_stopped = true;
    m_running = false;
    if (m_statusTimer) m_statusTimer->stop();
    if (m_state) m_state->revoke();
    bool cleaned = true;
    if (m_thread.isRunning()) {
      QMetaObject::invokeMethod(
          m_transport, [this, &cleaned]() { cleaned = m_transport->stop(); },
          Qt::BlockingQueuedConnection);
      m_thread.quit();
      m_thread.wait();
      m_transport = nullptr;
    } else if (m_security) {
      cleaned = m_security->revoke();
    }
    m_scene.reset();
    m_state.reset();
    m_security.reset();
    if (!cleaned)
      qWarning(
          "Live automation was revoked, but grant cleanup failed. Remove the "
          "inactive grant before restarting.");
  }

  bool running() const { return m_running; }

private:
  void requireGuiThread() const {
    if (!qApp || QThread::currentThread() != qApp->thread())
      throw Error(
          "gui_busy",
          "The live service must run on the editor application thread.");
  }

  void startTransport() {
    std::exception_ptr failure;
    QMetaObject::invokeMethod(
        m_transport,
        [this, &failure]() {
          try {
            m_transport->start();
          } catch (...) {
            failure = std::current_exception();
          }
        },
        Qt::BlockingQueuedConnection);
    if (!failure) return;
    stop();
    std::rethrow_exception(failure);
  }

  QStringList busyReasons() const {
    QStringList reasons =
        m_busyProbe ? m_busyProbe() : QStringList{"missing_native_guards"};
    if (!m_running || m_stopped) reasons.append("inactive");
    if (m_active) reasons.append("live_commit");
    if (QThread::currentThread()->loopLevel() > 1)
      reasons.append("nested_event_loop");
    if (QApplication::activeModalWidget()) reasons.append("modal_widget");
    if (QApplication::activePopupWidget()) reasons.append("popup_widget");
    return reasons;
  }

  void refreshStatus() {
    if (!m_running) return;
    try {
      const QStringList busy = busyReasons();
      if (busy.isEmpty()) m_cachedDomain = refreshScene(*m_scene);
      QJsonObject status = m_cachedDomain;
      status.insert("busy", !busy.isEmpty());
      if (!busy.isEmpty()) {
        status.insert("write_admission", false);
        status.insert("reasons", reasonsArray(busy));
      }
      m_state->updateStatus(status);
    } catch (...) {
      QJsonObject status = m_cachedDomain;
      status.insert("busy", true);
      status.insert("write_admission", false);
      status.insert("reasons", QJsonArray{"native_status_unavailable"});
      m_state->updateStatus(status);
    }
  }

  QJsonObject edit(const QString& method, const QJsonObject& params) {
    if (m_state->writesDisabled())
      throw Error("unsupported_target",
                  "Live writes are disabled after a partial edit. Inspect and "
                  "restart.");
    if (params.value("instance_id") != m_security->instanceId())
      throw Error("scene_changed",
                  "The edit belongs to another live instance. Reconnect and "
                  "reinspect.");
    QJsonObject result = method == "live.stage.set_key_value"
                             ? editStage(*m_scene, params)
                             : editFx(*m_scene, params);
    result.insert("instance_id", m_security->instanceId());
    result.insert("scene_epoch", params.value("scene_epoch"));
    result.insert("operation_id", params.value("operation_id"));
    result.insert("state", "completed");
    return result;
  }

  QJsonObject invoke(const std::shared_ptr<Ticket>& ticket) {
    const QStringList busy = busyReasons();
    if (!busy.isEmpty())
      throw Error(
          "gui_busy",
          "The editor is busy. Reinspect after the native operation finishes.",
          {{"reasons", reasonsArray(busy)}});
    QScopedValueRollback<bool> active(m_active, true);
    refreshStatus();
    const QString method     = ticket->request.value("method").toString();
    const QJsonObject params = ticket->request.value("params").toObject();
    QJsonObject result =
        ticket->write ? edit(method, params) : inspectScene(*m_scene, params);
    if (!ticket->write) result.insert("instance_id", m_security->instanceId());
    return {{"result", result}};
  }

  QJsonObject failed(const std::shared_ptr<Ticket>& ticket,
                     const Error& cause) {
    QJsonObject details = errorContext(*ticket, cause.details);
    if (!ticket->write || !mutationStarted(*m_scene))
      return errorOutcome(Error(cause.code, cause.message, details));
    m_state->disableWrites();
    details.insert("cause_code", cause.code.left(128));
    details.insert("outcome", "unknown");
    return errorOutcome(Error("partially_applied",
                              "The native edit failed after mutation began. "
                              "Inspect editor state and restart.",
                              details));
  }

  QJsonObject executeNative(const std::shared_ptr<Ticket>& ticket) {
    try {
      return invoke(ticket);
    } catch (const Error& error) {
      return failed(ticket, error);
    } catch (const TException& error) {
      return failed(ticket, Error("unsupported_target",
                                  QString::fromStdWString(error.getMessage())));
    } catch (const std::exception& error) {
      return failed(
          ticket, Error("unsupported_target", QString::fromUtf8(error.what())));
    } catch (...) {
      return failed(ticket,
                    Error("unsupported_target",
                          "The native operation raised an unknown error."));
    }
  }

  QJsonObject bounded(const std::shared_ptr<Ticket>& ticket,
                      const QJsonObject& outcome) {
    const int maximum =
        ticket->write ? maximumReceiptBytes : maximumResponseBytes - 1024;
    if (QJsonDocument(outcome).toJson(QJsonDocument::Compact).size() <= maximum)
      return outcome;
    return failed(ticket, Error("resource_limit",
                                "The native live result exceeded its fixed "
                                "bound. Narrow the scene."));
  }

  void execute() {
    if (m_stopped || !m_state) return;
    const auto ticket = m_state->takeNext();
    if (!ticket) return;
    if (m_active) {
      m_state->finish(
          ticket, errorOutcome(Error("gui_busy",
                                     "Another live operation is committing. "
                                     "Reinspect before retrying.",
                                     {{"operation_id", ticket->operationId}})));
      return;
    }
    resetMutationStarted(*m_scene);
    m_state->finish(ticket, bounded(ticket, executeNative(ticket)));
    if (m_stopRequested) {
      stop();
      return;
    }
    refreshStatus();
    QTimer::singleShot(0, this, [this]() { execute(); });
  }

  Options m_options;
  BusyProbe m_busyProbe;
  std::shared_ptr<SecuritySession> m_security;
  std::shared_ptr<SharedState> m_state;
  std::shared_ptr<SceneState> m_scene;
  QJsonObject m_cachedDomain{{"scene_epoch", QJsonValue()},
                             {"observation_serial", "0"}};
  QThread m_thread;
  Transport* m_transport = nullptr;
  QTimer* m_statusTimer  = nullptr;
  bool m_started         = false;
  bool m_stopped         = false;
  bool m_running         = false;
  bool m_active          = false;
  bool m_stopRequested   = false;
};

Service::Service(Options options, BusyProbe busyProbe)
    : m_impl(new Impl(std::move(options), std::move(busyProbe))) {}

Service::~Service() { stop(); }

void Service::start() { m_impl->start(); }

void Service::stop() noexcept { m_impl->stop(); }

bool Service::running() const { return m_impl->running(); }

}  // namespace Live
}  // namespace GroundzeroAutomation
