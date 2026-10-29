// ProcessProgress::wait() runs an external tool with the interface live: it
// returns when the tool ends, stops it on Cancel and on a timeout, and does
// not wait for a tool that has already ended.
#include "processprogress.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QProgressDialog>
#include <QPushButton>
#include <QThread>
#include <QTimer>

#include <iostream>

namespace {
int failures = 0;

void check(bool condition, const char *message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << std::endl;
  ++failures;
}

// This executable, as the tool: it sleeps or exits with a code
void startChild(QProcess &process, const QString &mode) {
  process.start(QCoreApplication::applicationFilePath(), {mode});
  check(process.waitForStarted(10000), "the child did not start");
}

// Clicks Cancel, as the user would
void cancelLater(int ms) {
  QTimer::singleShot(ms, []() {
    for (QWidget *widget : QApplication::topLevelWidgets())
      if (auto *dialog = qobject_cast<QProgressDialog *>(widget))
        if (QPushButton *button = dialog->findChild<QPushButton *>())
          button->click();
  });
}
}  // namespace

int main(int argc, char *argv[]) {
  if (argc > 1 && QString(argv[1]) == "--sleep") {
    QThread::sleep(60);
    return 0;
  }
  if (argc > 1 && QString(argv[1]) == "--exit3") return 3;

  QApplication app(argc, argv);
  QElapsedTimer clock;

  {
    QProcess process;
    startChild(process, "--exit3");
    const auto result = ProcessProgress::wait(process, "exit", nullptr);
    check(result == ProcessProgress::Finished, "an ending tool was not waited");
    check(process.state() == QProcess::NotRunning, "the tool still runs");
    check(process.exitCode() == 3, "the exit code was lost");
  }
  {
    // Already ended before the wait: no event loop, no dialog
    QProcess process;
    startChild(process, "--exit3");
    process.waitForFinished(10000);
    clock.start();
    check(ProcessProgress::wait(process, "ended", nullptr) ==
              ProcessProgress::Finished,
          "an ended tool was not reported as finished");
    check(clock.elapsed() < 5000, "an ended tool was waited for");
  }
  {
    QProcess process;
    startChild(process, "--sleep");
    cancelLater(300);
    clock.start();
    const auto result = ProcessProgress::wait(process, "cancel", nullptr);
    check(result == ProcessProgress::Canceled, "Cancel was not reported");
    check(process.state() == QProcess::NotRunning,
          "a canceled tool was left running");
    check(clock.elapsed() < 20000, "Cancel did not stop the wait");
  }
  {
    QProcess process;
    startChild(process, "--sleep");
    clock.start();
    const auto result = ProcessProgress::wait(process, "timeout", nullptr, 300);
    check(result == ProcessProgress::TimedOut, "the timeout was not reported");
    check(process.state() == QProcess::NotRunning,
          "a timed out tool was left running");
    check(clock.elapsed() < 20000, "the timeout did not stop the wait");
  }

  if (failures) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }
  std::cout << "processprogress_test passed" << std::endl;
  return 0;
}
