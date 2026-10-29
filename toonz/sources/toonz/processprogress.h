#pragma once

#ifndef PROCESSPROGRESS_H
#define PROCESSPROGRESS_H

// Waits for a started external tool while the interface keeps drawing, under
// a busy dialog whose Cancel stops the tool. A blocking waitForFinished()
// froze the whole window for as long as the tool ran, with no way out.

#include <QEventLoop>
#include <QObject>
#include <QProcess>
#include <QProgressDialog>
#include <QTimer>

namespace ProcessProgress {

enum Result { Finished, Canceled, TimedOut };

// A killed tool is waited for before returning. timeoutMs < 0: no limit.
inline Result wait(QProcess &process, const QString &label, QWidget *parent,
                   int timeoutMs = -1) {
  if (process.state() == QProcess::NotRunning) return Finished;

  QProgressDialog dialog(label, QObject::tr("Cancel"), 0, 0, parent);
  // Modal from the start: the loop below delivers input, and a second
  // command started meanwhile would run inside this one
  dialog.setWindowModality(Qt::ApplicationModal);
  dialog.setMinimumDuration(0);
  dialog.setAutoClose(false);
  dialog.setAutoReset(false);

  Result result = Finished;
  QEventLoop loop;
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&process, &QProcess::stateChanged, &loop,
                   [&loop](QProcess::ProcessState state) {
                     if (state == QProcess::NotRunning) loop.quit();
                   });
  QObject::connect(&dialog, &QProgressDialog::canceled, &loop,
                   [&loop, &result]() {
                     result = Canceled;
                     loop.quit();
                   });
  QObject::connect(&timer, &QTimer::timeout, &loop, [&loop, &result]() {
    result = TimedOut;
    loop.quit();
  });

  dialog.show();
  // The tool can end between the check above and the connections
  if (process.state() != QProcess::NotRunning) {
    if (timeoutMs >= 0) timer.start(timeoutMs);
    loop.exec();
  }
  if (process.state() != QProcess::NotRunning) {
    if (result == Finished) result = Canceled;
    process.kill();
    process.waitForFinished();
  }
  return result;
}

}  // namespace ProcessProgress

#endif
