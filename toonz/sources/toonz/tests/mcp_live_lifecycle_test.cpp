#include "tests/mcp_live_lifecycle_test.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocalSocket>
#include <QMainWindow>
#include <QProcess>
#include <QThread>
#include <QtEndian>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "automation_build_identity.h"
#include "automation_live.h"
#include "automation_live_protocol.h"
#include "automation_live_security.h"
#include "automation_live_startup.h"
#include "tapp.h"
#include "tdoubleparam.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "tundo.h"

namespace {

using GroundzeroAutomation::Error;
namespace Live = GroundzeroAutomation::Live;

const QString instance = "00000000-0000-4000-8000-0000000000b2";

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// Ends the process when a guarded GUI wait does not return, so a wait cycle
// between the GUI and the transport fails the run instead of hanging it.
class Watchdog final {
public:
  Watchdog(const char* label, int milliseconds)
      : m_thread([this, label, milliseconds]() {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_done.wait_for(lock, std::chrono::milliseconds(milliseconds),
                            [this]() { return m_finished; }))
          return;
        std::fprintf(stderr,
                     "mcp-live-lifecycle: %s did not return within %d ms: "
                     "the GUI thread waited on a blocked transport\n",
                     label, milliseconds);
        std::fflush(stderr);
        std::_Exit(3);
      }) {}
  ~Watchdog() {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_finished = true;
    }
    m_done.notify_all();
    m_thread.join();
  }
  Watchdog(const Watchdog&)            = delete;
  Watchdog& operator=(const Watchdog&) = delete;

private:
  std::mutex m_mutex;
  std::condition_variable m_done;
  bool m_finished = false;
  std::thread m_thread;
};

QByteArray readFrame(QLocalSocket& socket) {
  QByteArray bytes;
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 3000) {
    bytes += socket.readAll();
    if (bytes.size() >= 4) {
      const quint32 size = qFromBigEndian<quint32>(
          reinterpret_cast<const uchar*>(bytes.constData()));
      if (bytes.size() >= int(size) + 4) return bytes.mid(4, int(size));
    }
    if (socket.state() != QLocalSocket::ConnectedState) break;
    socket.waitForReadyRead(50);
  }
  throw std::runtime_error("The live endpoint did not answer");
}

void writeFrame(QLocalSocket& socket, const QJsonObject& message) {
  const QByteArray frame =
      Live::frameMessage(message, Live::maximumRequestBytes);
  require(socket.write(frame) == frame.size(), "Cannot write a live frame");
  while (socket.bytesToWrite())
    require(socket.waitForBytesWritten(1000), "A live frame was not sent");
}

// An authenticated raw client on a plain thread, as the connector would be.
// It sends one queued request and then waits for Stop to close it.
class Client final {
public:
  explicit Client(QString path) : m_path(std::move(path)) {}
  ~Client() { join(); }
  void start() {
    m_thread = std::thread([this]() { run(); });
  }
  void join() {
    if (m_thread.joinable()) m_thread.join();
  }
  bool waitSent(int milliseconds) const {
    QElapsedTimer timer;
    timer.start();
    while (!m_sent && !m_finished && timer.elapsed() < milliseconds)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return m_sent;
  }
  std::string failure() const { return m_failure; }

private:
  void run() {
    try {
      exchange();
    } catch (const std::exception& error) {
      m_failure = error.what();
    }
    m_finished = true;
  }
  void exchange() {
    const auto session = Live::SecuritySession::read(m_path);
    QLocalSocket socket;
    socket.connectToServer(session->endpoint());
    require(socket.waitForConnected(3000), "Cannot reach the live endpoint");
    writeFrame(socket, {{"protocol", Live::liveProtocol},
                        {"build", GroundzeroAutomation::buildIdentity()},
                        {"instance_id", session->instanceId()},
                        {"client_id", "00000000-0000-4000-8000-0000000000c1"},
                        {"token", QString::fromLatin1(session->token())}});
    const QJsonObject hello =
        Live::parseMessage(readFrame(socket), Live::maximumResponseBytes);
    require(hello.value("instance_id") == session->instanceId(),
            "The live endpoint did not authenticate the client");
    writeFrame(socket, {{"id", "queued"},
                        {"method", "live.inspect"},
                        {"params", QJsonObject{{"scene_epoch", "1"}}}});
    m_sent = true;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000 &&
           socket.state() == QLocalSocket::ConnectedState) {
      socket.waitForReadyRead(50);
      socket.readAll();
    }
    require(socket.state() != QLocalSocket::ConnectedState,
            "Stop did not close an authenticated connection");
  }
  QString m_path;
  std::thread m_thread;
  std::atomic<bool> m_sent{false};
  std::atomic<bool> m_finished{false};
  std::string m_failure;
};

TDoubleParam* cameraX() {
  auto* sheet = TApp::instance()->getCurrentScene()->getScene()->getXsheet();
  auto* tree  = sheet->getStageObjectTree();
  return tree->getStageObject(tree->getCurrentCameraId(), false)
      ->getParam(TStageObject::T_X);
}

QJsonObject cameraEdit(Live::SceneState& state) {
  const QString epoch =
      Live::refreshScene(state).value("scene_epoch").toString();
  const QJsonObject snapshot =
      Live::inspectScene(state, {{"scene_epoch", epoch}});
  for (const QJsonValue& item : snapshot.value("editable_targets").toArray()) {
    const QJsonObject target = item.toObject();
    if (target.value("tool") != "live.stage.set_key_value" ||
        target.value("channel") != "x")
      continue;
    return {{"instance_id", instance},
            {"scene_epoch", epoch},
            {"operation_id", "00000000-0000-4000-8000-0000000000d1"},
            {"expected_fingerprint", target.value("expected_fingerprint")},
            {"object_id", target.value("object_id")},
            {"channel", "x"},
            {"frame", 10},
            {"value", 9},
            {"unit", "inch"}};
  }
  throw std::runtime_error("The lifecycle fixture has no editable stage key");
}

// Every native live entry point refuses a call from another thread before it
// reads or changes the scene, and Start refuses before creating any file.
void offGuiThread(QMainWindow* window, const QString& directory) {
  auto state             = Live::createSceneState(instance);
  const QJsonObject edit = cameraEdit(*state);
  const double value     = cameraX()->getValue(10);
  const int history      = TUndoManager::manager()->getHistoryCount();
  const QString path     = directory + "/off-thread.json";
  Live::LiveUi ui(*qApp, *window, Live::Options{"edit", path});
  const std::vector<std::function<void()>> calls{
      [&]() { Live::createSceneState(instance); },
      [&]() { Live::refreshScene(*state); },
      [&]() { Live::inspectScene(*state, {{"scene_epoch", "1"}}); },
      [&]() { Live::editStage(*state, edit); }, [&]() { ui.start(); }};
  std::vector<QString> codes;
  std::thread caller([&]() {
    for (const auto& call : calls) {
      try {
        call();
        codes.push_back("executed");
      } catch (const Error& error) {
        codes.push_back(error.code);
      } catch (...) {
        codes.push_back("unexpected");
      }
    }
  });
  caller.join();
  require(codes == std::vector<QString>(calls.size(), "gui_busy"),
          "A live call from another thread was not refused as gui_busy");
  require(cameraX()->getValue(10) == value &&
              TUndoManager::manager()->getHistoryCount() == history,
          "A refused off-thread live call changed the scene or history");
  require(!ui.running() && !QFile::exists(path),
          "An off-thread Start created a live session");
}

// Stop and Start block the GUI thread on the transport thread. The transport
// only posts to the GUI and never waits for it, so a Stop issued while the
// transport has just posted a request returns. Were the post a blocking call,
// the watchdog would end the run.
void waitGraph(QMainWindow* window, const QString& directory) {
  for (int cycle = 0; cycle < 3; ++cycle) {
    const QString path = directory + "/wait-graph.json";
    Live::LiveUi ui(*qApp, *window, Live::Options{"edit", path});
    {
      Watchdog guard("Start", 10000);
      ui.start();
    }
    Client client(path);
    client.start();
    require(client.waitSent(5000), "The wait-graph client did not send");
    QThread::msleep(100);
    {
      Watchdog guard("Stop", 10000);
      ui.stop();
    }
    client.join();
    require(client.failure().empty(), client.failure().c_str());
    require(!QFile::exists(path), "Stop left the live session file");
  }
}

QString lsof(const QStringList& selection) {
  QProcess process;
  QStringList arguments{"-nP", "-a", "-p",
                        QString::number(QCoreApplication::applicationPid())};
  process.start("lsof", arguments + selection);
  require(process.waitForFinished(10000), "lsof did not finish");
  require(process.exitStatus() == QProcess::NormalExit &&
              (process.exitCode() == 0 || process.exitCode() == 1),
          "lsof could not list this process's sockets");
  return QString::fromLocal8Bit(process.readAllStandardOutput());
}

// The live endpoint is a private local socket; the editor opens no TCP
// listener while live. Listing the endpoint proves lsof saw this process.
void noTcpListener(QMainWindow* window, const QString& directory) {
#ifdef Q_OS_WIN
  Q_UNUSED(window);
  Q_UNUSED(directory);
#else
  const QString path = directory + "/listeners.json";
  Live::LiveUi ui(*qApp, *window, Live::Options{"edit", path});
  ui.start();
  const QString endpoint = Live::SecuritySession::read(path)->endpoint();
  require(lsof({"-U"}).contains(QFileInfo(endpoint).fileName()),
          "lsof did not list the live endpoint of this process");
  require(lsof({"-iTCP", "-sTCP:LISTEN"}).trimmed().isEmpty(),
          "The editor has a TCP listener while live");
  ui.stop();
#endif
}

}  // namespace

void runNativeLiveLifecycleTests(QMainWindow* window,
                                 const QString& directory) {
  offGuiThread(window, directory);
  waitGraph(window, directory);
  noTcpListener(window, directory);
  require(QDir(directory).isEmpty(),
          "The lifecycle checks left files in the grant directory");
}
