#pragma once

#ifndef CRASHHANDLER_INCLUDED
#define CRASHHANDLER_INCLUDED

#include "tcommon.h"
#include "tfilepath.h"

#include <QDialog>

class CrashHandler : public QDialog {
  Q_OBJECT;

  TFilePath m_crashFile;
  QString m_crashReport;
  bool m_previousSession;

public:
  CrashHandler(QWidget *parent, TFilePath crashFile, QString crashTxt,
               bool previousSession = false);

  void reject();

  static void install();
  // Unix: resolve the pending report path and the static report header, and
  // archive the report of a crash in the previous session. Call once the
  // application and Toonz environment are initialized.
  static void prepareReport();
  // Whether a report of a crash in the previous session is still to be shown
  static bool hasPendingReport();
  // Show the report of a crash in the previous session, if any
  static void showPendingReport(QWidget *parent = 0);
  static void reportProjectInfo(bool enableReport);
  static void attachParentWindow(QWidget *parent);
  static bool trigger(const QString reason, bool showDialog);
#ifdef ENABLE_CRASH_REPORTER_TEST
  static void triggerTestCrash();
#endif

public slots:
  void copyClipboard();
  void openWebpage();
  void openFolder();
};

#endif  // CRASHHANDLER_INCLUDED
