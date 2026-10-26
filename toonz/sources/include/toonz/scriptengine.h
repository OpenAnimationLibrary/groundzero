#pragma once

#ifndef SCRIPTENGINE_H
#define SCRIPTENGINE_H

// #define SCRIPTENGINE_TEST

#include "tcommon.h"
#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

#include <climits>
#include <QObject>
#include <QMetaType>
#include <QList>

#include <functional>

class Foo;
class QJSValue;
class QJSEngine;

class DVAPI ScriptCommand {
public:
  ScriptCommand() {}
  virtual ~ScriptCommand() {}

  virtual void execute() = 0;
};

//! Runs the toonz scripts (Script Console, "Run Script", -script command
//! line option).
/*!
  The JavaScript engine (a QJSEngine) lives in a worker thread owned by
  ScriptEngine: it is created there, every evaluation runs there and it is
  destroyed there, so that long scripts don't freeze the GUI. The output
  signal is emitted from that thread; evaluationDone() is emitted on the
  thread of the ScriptEngine.
*/
class DVAPI ScriptEngine final : public QObject {
  Q_OBJECT
  QJSEngine *m_engine;
  class Executor;
  friend class Executor;
  Executor *m_executor;
  QJSValue *m_voidValue;

public:
  //! A native script function. It is called on the script thread with the
  //! script arguments. To make the script throw, call engine->throwError()
  //! and return QJSValue().
  typedef std::function<QJSValue(QJSEngine *engine,
                                 const QList<QJSValue> &args)>
      Function;

  ScriptEngine();
  ~ScriptEngine();

  void evaluate(const QString &cmd);
  void interrupt();
  bool isEvaluating() const;
  bool wait(unsigned long time = ULONG_MAX);

  enum OutputType {
    SimpleText,
    Warning,
    ExecutionError,
    SyntaxError,
    EvaluationResult,
    UndefinedEvaluationResult
  };
  void emitOutput(OutputType type, const QString &value) {
    emit output((int)type, value);
  }

  //! Defines (or replaces) the global script function \b name. If a script
  //! is being evaluated, waits for it to finish.
  void defineFunction(const QString &name, const Function &f);

  //! Runs f on the thread of this ScriptEngine (the GUI thread) and waits
  //! for it to return. To be used by script functions that touch the GUI;
  //! f must not use the QJSEngine. While the ScriptEngine is being
  //! destroyed, f is not run.
  void runOnMainThread(const std::function<void()> &f);

  //! The value returned by functions, like print(), whose result must not
  //! be printed in the console.
  const QJSValue &voidValue() const { return *m_voidValue; }

protected slots:
  void onTerminated();

signals:
  void evaluationDone();
  void output(int type, const QString &);
};

Q_DECLARE_METATYPE(ScriptEngine *)

#endif
