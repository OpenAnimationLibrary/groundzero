#pragma once

#ifndef PYTHONCONSOLEPANEL_H
#define PYTHONCONSOLEPANEL_H

// The Python Console (built only with the CMake option WITH_PYTHON)

#include "pane.h"

#include <QList>
#include <QString>
#include <QTextEdit>

class QMimeData;

//=============================================================================
// PythonConsole
//-----------------------------------------------------------------------------

//! The interactive prompt of the Python engine (see toonz/pythonengine.h).
//! The lines run on the GUI thread: the application waits for them.
class PythonConsole final : public QTextEdit {
  Q_OBJECT

  QList<QString> m_commands;
  int m_commandIndex;
  QString m_currentCommand;
  QString m_prompt;  // ">>> ", or "... " in a multi-line statement

public:
  PythonConsole(QWidget *parent = 0);
  ~PythonConsole();

  //! Runs the line as if typed in the console
  void executeLine(const QString &line);
  //! Runs the Python file (see PythonEngine::runFile())
  void runFile(const QString &path);

protected:
  void keyPressEvent(QKeyEvent *e) override;
  bool canInsertFromMimeData(const QMimeData *source) const override;
  void insertFromMimeData(const QMimeData *source) override;

private:
  void onReturnKeyPress();
  void showPrompt();
  // the text typed after the prompt
  QString getCurrentLine() const;
  void setCurrentLine(const QString &line);

protected slots:
  void onOutput(int type, const QString &text);
  void onCursorPositionChanged();
};

//=============================================================================
// PythonConsolePanel
//-----------------------------------------------------------------------------

class PythonConsolePanel final : public TPanel {
  Q_OBJECT
  PythonConsole *m_console;

public:
  PythonConsolePanel(QWidget *parent       = 0,
                     Qt::WindowFlags flags = Qt::WindowFlags());
  ~PythonConsolePanel();

  void executeLine(const QString &line);
  void runFile(const QString &path);

public slots:
  void selectNone();
};

#endif  // PYTHONCONSOLEPANEL_H
