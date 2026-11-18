#include "tests/mcp_live_disabled_test.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QProcess>
#include <QSaveFile>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>

#if defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <pthread.h>
#endif

#include "automation_live_startup.h"
#include "tapp.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheet.h"
#include "tundo.h"

namespace {

namespace Live = GroundzeroAutomation::Live;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QString fixturePath(const QString& name) {
  return QDir::current().filePath("mcp-live/" + name);
}

QJsonObject readConfiguration() {
  QFile file(fixturePath("config.json"));
  require(file.open(QIODevice::ReadOnly) && file.size() <= 4096,
          "The disabled-bridge fixture configuration is missing");
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  require(document.isObject(),
          "The disabled-bridge fixture configuration is not an object");
  return document.object();
}

// OS thread names; Qt names a started QThread after its object name.
QJsonArray threadNames() {
  QStringList names;
#if defined(Q_OS_MACOS)
  thread_act_array_t threads   = nullptr;
  mach_msg_type_number_t count = 0;
  require(task_threads(mach_task_self(), &threads, &count) == KERN_SUCCESS,
          "Cannot list this process's threads");
  for (mach_msg_type_number_t index = 0; index < count; ++index) {
    char name[64]          = {};
    const pthread_t thread = pthread_from_mach_thread_np(threads[index]);
    if (thread) pthread_getname_np(thread, name, sizeof(name));
    names.append(QString::fromUtf8(name));
    mach_port_deallocate(mach_task_self(), threads[index]);
  }
  vm_deallocate(mach_task_self(), vm_address_t(threads),
                count * sizeof(thread_act_t));
#elif defined(Q_OS_LINUX)
  for (const QString& task : QDir("/proc/self/task").entryList(QDir::Dirs)) {
    QFile comm("/proc/self/task/" + task + "/comm");
    if (comm.open(QIODevice::ReadOnly))
      names.append(QString::fromUtf8(comm.readAll()).trimmed());
  }
#endif
  names.sort();
  return QJsonArray::fromStringList(names);
}

QStringList lsof(const QStringList& selection) {
#ifdef Q_OS_WIN
  Q_UNUSED(selection);
  return {};
#else
  QProcess process;
  process.start("lsof", QStringList{"-nP", "-a", "-p",
                                    QString::number(qApp->applicationPid())} +
                            selection);
  require(process.waitForFinished(10000) &&
              process.exitStatus() == QProcess::NormalExit &&
              (process.exitCode() == 0 || process.exitCode() == 1),
          "lsof could not list this process's sockets");
  QStringList lines = QString::fromLocal8Bit(process.readAllStandardOutput())
                          .split('\n', Qt::SkipEmptyParts);
  if (!lines.isEmpty()) lines.removeFirst();
  return lines;
#endif
}

QJsonArray windows() {
  QStringList entries;
  for (QWidget* widget : QApplication::topLevelWidgets())
    entries.append(QString("%1|%2|%3")
                       .arg(widget->metaObject()->className(),
                            widget->objectName(),
                            widget->isVisible() ? "visible" : "hidden"));
  entries.sort();
  return QJsonArray::fromStringList(entries);
}

QJsonObject scene() {
  auto* handle  = TApp::instance()->getCurrentScene();
  auto* sheet   = handle->getScene()->getXsheet();
  auto* history = TUndoManager::manager();
  return {{"name", QString::fromStdWString(handle->getScene()->getSceneName())},
          {"dirty", handle->getDirtyFlag()},
          {"history_count", history->getHistoryCount()},
          {"history_index", history->getCurrentHistoryIndex()},
          {"columns", sheet->getColumnCount()},
          {"frames", sheet->getFrameCount()},
          {"fx", sheet->getFxDag()->getInternalFxs()->getFxCount()}};
}

int liveObjects(QMainWindow* window) {
  int count = 0;
  for (QObject* child : window->findChildren<QObject*>())
    if (child->objectName().startsWith("automationLive")) ++count;
  return count;
}

QJsonObject observe(QMainWindow* window, const QString& grants) {
  const QJsonArray threads = threadNames();
  const auto* bar          = window->findChild<QStatusBar*>();
  return {{"windows", windows()},
          {"status_bar_visible", bar && bar->isVisible()},
          {"live_objects", liveObjects(window)},
          {"bridge_threads", int(std::count(threads.begin(), threads.end(),
                                            QJsonValue("gz-live-bridge")))},
          {"threads", threads.size()},
          {"grant_files",
           QJsonArray::fromStringList(QDir(grants).entryList(
               QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden))},
          {"unix_sockets", lsof({"-U"}).size()},
          {"tcp_listeners",
           QJsonArray::fromStringList(lsof({"-iTCP", "-sTCP:LISTEN"}))},
          {"scene", scene()}};
}

// Nothing of the bridge exists in a launch without --automation-live.
void requireDisabled(const QJsonObject& state) {
  require(state.value("live_objects") == 0,
          "A disabled launch created live automation UI or timers");
  require(state.value("bridge_threads") == 0,
          "A disabled launch started the live transport thread");
  require(state.value("grant_files").toArray().isEmpty(),
          "A disabled launch created a live session file or socket");
  require(state.value("tcp_listeners").toArray().isEmpty(),
          "A disabled launch opened a TCP listener");
}

void writeEvidence(const QJsonObject& value) {
  QSaveFile file(fixturePath("disabled.json"));
  const QByteArray bytes = QJsonDocument(value).toJson(QJsonDocument::Indented);
  require(file.open(QIODevice::WriteOnly) &&
              file.write(bytes) == bytes.size() && file.commit(),
          "Cannot write the disabled-bridge evidence");
}

// The same observations see a running live session, and lose it again.
QJsonObject control(QMainWindow* window, const QString& grants,
                    QJsonObject& evidence) {
  const QJsonObject disabled = evidence.value("disabled").toObject();
  QJsonObject result;
  {
    Live::LiveUi ui(*qApp, *window,
                    Live::Options{"inspect", grants + "/control.json"});
    ui.start();
    const QJsonObject live = observe(window, grants);
    evidence.insert("live", live);
    writeEvidence(evidence);
    require(live.value("live_objects").toInt() > 0 &&
                live.value("bridge_threads") == 1 &&
                live.value("grant_files") == QJsonArray{"control.json"} &&
                live.value("unix_sockets").toInt() >
                    disabled.value("unix_sockets").toInt() &&
                live.value("tcp_listeners").toArray().isEmpty(),
            "The observations did not see a running live session");
    ui.stop();
    const QJsonObject stopped = observe(window, grants);
    for (const char* key :
         {"bridge_threads", "grant_files", "unix_sockets", "scene"})
      require(stopped.value(key) == disabled.value(key),
              "Stopping a live session left bridge state behind");
    result = {{"live", live}, {"stopped", stopped}};
  }
  const QJsonObject removed = observe(window, grants);
  for (const char* key : {"windows", "live_objects", "bridge_threads"})
    require(removed.value(key) == disabled.value(key),
            "Removing the live UI did not restore the disabled launch");
  result.insert("removed", removed);
  return result;
}

void run(QMainWindow* window) {
  const QJsonObject configuration = readConfiguration();
  const QString grants = configuration.value("grant_directory").toString();
  require(QDir::isAbsolutePath(grants) && QDir(grants).exists(),
          "The disabled-bridge grant directory is missing");
  const QJsonObject disabled = observe(window, grants);
  QJsonObject evidence{{"status", "failed"}, {"disabled", disabled}};
  writeEvidence(evidence);
  requireDisabled(disabled);
  if (configuration.value("control").toBool())
    evidence.insert("control", control(window, grants, evidence));
  evidence.insert("status", "passed");
  writeEvidence(evidence);
}

}  // namespace

void startMcpLiveDisabledTest(QMainWindow* window,
                              std::function<void(int)> completed) {
  QTimer::singleShot(0, window, [window, completed]() {
    int result = 1;
    try {
      run(window);
      result = 0;
    } catch (const std::exception& error) {
      std::cerr << "mcp-live-disabled: " << error.what() << std::endl;
    }
    std::cout << "mcp-live-disabled: " << (result ? "FAIL" : "PASS")
              << std::endl;
    completed(result);
  });
}
