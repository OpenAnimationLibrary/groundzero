#include "nativediagnostics.h"
#include "diagnosticssettingswidget.h"
#include "toonzqt/tmessageviewer.h"
#include "tlogger.h"
#include "tmsgcore.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<int> handled{0};
void previous(QtMsgType, const QMessageLogContext &, const QString &) { ++handled; }
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
template <class Predicate> void wait(Predicate predicate) {
  QElapsedTimer timer; timer.start();
  while (!predicate() && timer.elapsed() < 10000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(1);
  }
  require(predicate(), "asynchronous diagnostic operation completed");
}
QByteArray read(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "log is readable after flush");
  return file.readAll();
}
void run() {
  QTemporaryDir temporary;
  require(temporary.isValid(), "temporary preferences available");
  const QString settings = temporary.path() + "/preferences.ini";
  const QString directory = temporary.path() + "/logs";
  const QString file = directory + "/diagnostics.log";
  QtMessageHandler saved = qInstallMessageHandler(previous);
  {
    NativeDiagnostics service;
    service.initialize(settings, directory);
    require(!service.isEnabled(), "fresh preferences disable disk logging");
    require(!QFileInfo::exists(directory), "off creates no log directory");
    DiagnosticsSettingsWidget first(nullptr, &service), second(nullptr, &service);
    auto *enabled = first.findChild<QCheckBox *>("EnableDiagnosticLogging");
    auto *otherEnabled = second.findChild<QCheckBox *>("EnableDiagnosticLogging");
    auto *history = first.findChild<QSpinBox *>("MessageHistoryLimit");
    auto *severity = first.findChild<QComboBox *>("DiagnosticSeverity");
    require(enabled && otherEnabled && history && severity, "GUI exposes complete diagnostic controls");
    history->setValue(500);
    require(TMessageRepository::instance()->historyLimit() == 500,
            "history control updates native repository");
    require(second.findChild<QSpinBox *>("MessageHistoryLimit")->value() == 500,
            "two settings views remain synchronized");
    enabled->setChecked(true);
    wait([&] { return service.status().state == NativeDiagnostic::State::Ready; });
    require(otherEnabled->isChecked(), "enable state synchronizes without duplicate handlers");
    TMessageViewer messages(nullptr), another(nullptr);
    messages.show(); another.show();
    QCoreApplication::processEvents();
    const quint64 before = service.status().written;
    require(TMsgCore::instance()->send(DVGui::INFORMATION, "application-once"),
            "normal native message transport remains active");
    const int oldHandled = handled;
    qWarning("qt-once");
    TLogger::instance()->addMessage(TLogger::Message(TLogger::Info, "native-once"));
    std::thread worker([] {
      TLogger::instance()->addMessage(TLogger::Message(TLogger::Warning, "worker-native-once"));
    });
    worker.join();
    wait([&] { return service.status().written >= before + 4; });
    require(handled == oldHandled + 1, "prior Qt handler called exactly once");
    service.setEnabled(false);
    wait([&] { return service.status().state == NativeDiagnostic::State::Off; });
    const QByteArray savedText = read(file);
    QByteArray text = savedText;
    text.replace("\r\n", "\n");
    for (const QByteArray &marker : {QByteArray("application-once"), QByteArray("qt-once"),
                                    QByteArray("native-once"), QByteArray("worker-native-once")}) {
      // Native's marker is also a substring of the worker marker; compare the
      // full event suffix to verify each native adapter delivered only once.
      require(text.count(QByteArray("] ") + marker + '\n') == 1,
              "each origin writes once independent of viewer count");
    }
    const int afterDisable = handled;
    qWarning("off-warning");
    require(handled == afterDisable + 1, "disable restores the previous handler");
    require(!read(file).contains("off-warning"), "off stops recording immediately");
    TMessageRepository::instance()->clear();
    require(read(file) == savedText, "clear history leaves diagnostic files unchanged");
    severity->setCurrentIndex(2);
    QSettings persisted(settings, QSettings::IniFormat);
    require(persisted.value("Diagnostics/HistoryLimit").toInt() == 500 &&
            persisted.value("Diagnostics/MinimumSeverity").toInt() == 2,
            "native preference values persist through GUI");
    service.setEnabled(true);
    wait([&] { return service.status().state == NativeDiagnostic::State::Ready; });
    service.shutdown();
  }
  {
    NativeDiagnostics restarted;
    restarted.initialize(settings, directory);
    require(restarted.isEnabled() && restarted.historyLimit() == 500 &&
            restarted.minimumSeverity() == 2, "restart restores chosen settings");
    wait([&] { return restarted.status().state == NativeDiagnostic::State::Ready; });
    restarted.setEnabled(false);
    wait([&] { return restarted.status().state == NativeDiagnostic::State::Off; });
    restarted.deleteSavedLogs();
    wait([&] { return !QFileInfo::exists(file); });
    restarted.shutdown();
  }
  qInstallMessageHandler(saved);
}
}  // namespace
int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication app(argc, argv);
  try { run(); std::cout << "native diagnostic service passed\n"; return 0; }
  catch (const std::exception &error) { qInstallMessageHandler(nullptr); std::cerr << error.what() << '\n'; return 1; }
}
