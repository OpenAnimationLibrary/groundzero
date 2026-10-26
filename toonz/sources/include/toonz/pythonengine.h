#pragma once

#ifndef PYTHONENGINE_H
#define PYTHONENGINE_H

// Built only with the CMake option WITH_PYTHON. This header doesn't need the
// Python headers.

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

#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include <functional>

//! Runs the Python scripts (Python Console, "Run Python Script...").
/*!
  The embedded CPython interpreter is started on the first use (push(),
  runFile(), start()) and lives until the application exits. It must be used
  only from the GUI thread (the thread of the first instance() call): the
  scripts run there and block it while they run.

  The scripts can import the opentoonz module, which has the Image, Scene,
  Level and StageObject classes of the JavaScript binding (see
  pythonbinding.cpp), and the functions given to defineFunction().

  sys.stdout and sys.stderr are redirected to the output() signal (also when
  a Python thread writes to them: the signal is then emitted from that
  thread).
*/
class DVAPI PythonEngine final : public QObject {
  Q_OBJECT
  class Imp;
  Imp *m_imp;

  PythonEngine();
  ~PythonEngine();

public:
  enum OutputType {
    StdOut,  //!< written to sys.stdout (print(), expression values)
    StdErr   //!< written to sys.stderr (errors, tracebacks, warnings)
  };

  //! A native function of the opentoonz module. It gets the Python arguments
  //! converted to QVariant (None, bool, int, float, str, list, tuple, dict;
  //! other objects as their str()), and its result is converted back. To
  //! raise a Python exception, throw a std::exception: pybind11 converts it
  //! (e.g. std::invalid_argument to ValueError, std::runtime_error to
  //! RuntimeError).
  typedef std::function<QVariant(const QVariantList &args)> Function;

  //! The engine (created on the first call)
  static PythonEngine *instance();

  //! Starts the interpreter if needed. Returns false (and writes the reason
  //! to the output) if it can't be started.
  bool start();
  bool isStarted() const;

  //! The Python version, e.g. "3.12.3" ("" before start())
  QString getVersion() const;

  //! Gives a line typed in the console to the interactive interpreter (as the
  //! python prompt does): the statement runs when complete, and the values of
  //! the expressions are printed. Returns true if more lines are needed (the
  //! prompt is then "... ").
  bool push(const QString &line);
  //! Discards the lines of an incomplete statement given to push()
  void resetInput();

  //! Runs the Python file as the main module of a new namespace (as
  //! "python path" does, with sys.argv = [path] and its folder at the start
  //! of sys.path while it runs). Errors are printed as tracebacks to stderr.
  //! Returns false if the file raised an exception (SystemExit(0) and
  //! SystemExit(None) are a success).
  bool runFile(const QString &path);

  //! Defines (or replaces) the function opentoonz.name. Can be called before
  //! start(): the function is defined when the interpreter starts.
  void defineFunction(const QString &name, const Function &f,
                      const QString &doc = QString());

  //! Used by sys.stdout and sys.stderr
  void emitOutput(OutputType type, const QString &text) {
    emit output((int)type, text);
  }

signals:
  //! type is an OutputType. text is what was written (usually without line
  //! breaks between successive writes: print() writes its own "\n").
  void output(int type, const QString &text);
};

#endif  // PYTHONENGINE_H
