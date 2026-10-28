// The embedded Python interpreter (built only with the CMake option
// WITH_PYTHON), see pythonengine.h

// must be the first includes (see pythonbinding.h)
#include "pythonbinding.h"
#pragma push_macro("slots")
#undef slots
#include <pybind11/embed.h>
#pragma pop_macro("slots")

#include "toonz/pythonengine.h"

// Qt includes
#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QStringList>
#include <QThread>
#include <QVariantMap>

#include <cassert>
#include <exception>
#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace {

// sys.stdout and sys.stderr
class OutputStream {
  int m_type;

public:
  explicit OutputStream(int type) : m_type(type) {}

  py::ssize_t write(const py::handle &text) {
    if (!PyUnicode_Check(text.ptr()))
      throw py::type_error("write() argument must be str");
    py::object bytes = py::reinterpret_steal<py::object>(
        PyUnicode_AsEncodedString(text.ptr(), "utf-8", "backslashreplace"));
    if (!bytes) throw py::error_already_set();
    QString s = QString::fromUtf8(PyBytes_AS_STRING(bytes.ptr()),
                                  (int)PyBytes_GET_SIZE(bytes.ptr()));
    if (!s.isEmpty())
      PythonEngine::instance()->emitOutput((PythonEngine::OutputType)m_type, s);
    return PyUnicode_GetLength(text.ptr());
  }
};

// Run once the interpreter is started, in a new namespace whose push(),
// reset() and run_file() are used by PythonEngine
const char *const bootstrapCode = R"PY(
import sys

# Don't import the modules of the current folder: before any other import, or
# a code.py or traceback.py there would run in place of the standard ones
sys.path[:] = [p for p in sys.path if p not in ('', '.')]

import builtins, code, os, traceback
import opentoonz

sys.stdout = opentoonz._OutputStream(0)
sys.stderr = opentoonz._OutputStream(1)
# the console can't read input()
sys.stdin = None

version = '%d.%d.%d' % sys.version_info[:3]

_console = code.InteractiveConsole(
    {'__name__': '__console__', '__doc__': None, 'opentoonz': opentoonz})


def push(line):
    try:
        return _console.push(line)
    except SystemExit:
        _console.resetbuffer()
        print('SystemExit (ignored: the console stays open)', file=sys.stderr)
        return False


def reset():
    _console.resetbuffer()


def run_file(path):
    path = os.path.abspath(path)
    folder = os.path.dirname(path)
    namespace = {'__name__': '__main__', '__file__': path,
                 '__builtins__': builtins}
    argv = sys.argv
    sys.argv = [path]
    sys.path.insert(0, folder)
    try:
        with open(path, 'rb') as f:
            source = f.read()
        exec(compile(source, path, 'exec'), namespace)
        return True
    except SystemExit as e:
        if e.code is None or e.code == 0:
            return True
        print('SystemExit: %s' % (e.code,), file=sys.stderr)
        return False
    except BaseException:
        info = sys.exc_info()
        # without the frame of run_file()
        traceback.print_exception(info[0], info[1], info[2].tb_next)
        return False
    finally:
        sys.argv = argv
        try:
            sys.path.remove(folder)
        except ValueError:
            pass
)PY";

//-----------------------------------------------------------------------------

QVariant toVariant(const py::handle &h) {
  if (h.is_none()) return QVariant();
  if (py::isinstance<py::bool_>(h)) return QVariant(h.cast<bool>());
  if (py::isinstance<py::int_>(h)) {
    long long value = PyLong_AsLongLong(h.ptr());
    if (value == -1 && PyErr_Occurred()) {
      // too big
      PyErr_Clear();
      return QVariant(QString::fromStdString(py::str(h).cast<std::string>()));
    }
    return QVariant((qlonglong)value);
  }
  if (py::isinstance<py::float_>(h)) return QVariant(h.cast<double>());
  if (py::isinstance<py::str>(h))
    return QVariant(QString::fromStdString(h.cast<std::string>()));
  if (py::isinstance<py::list>(h) || py::isinstance<py::tuple>(h)) {
    QVariantList list;
    for (py::handle item : h) list.append(toVariant(item));
    return list;
  }
  if (py::isinstance<py::dict>(h)) {
    QVariantMap map;
    for (auto item : h.cast<py::dict>())
      map.insert(
          QString::fromStdString(py::str(item.first).cast<std::string>()),
          toVariant(item.second));
    return map;
  }
  return QVariant(QString::fromStdString(py::str(h).cast<std::string>()));
}

py::object toPython(const QVariant &value) {
  if (!value.isValid() || value.isNull()) return py::none();
  switch (value.userType()) {
  case QMetaType::Bool:
    return py::bool_(value.toBool());
  case QMetaType::Int:
  case QMetaType::Short:
  case QMetaType::Long:
  case QMetaType::LongLong:
  case QMetaType::Char:
  case QMetaType::SChar:
    return py::int_((long long)value.toLongLong());
  case QMetaType::UInt:
  case QMetaType::UShort:
  case QMetaType::ULong:
  case QMetaType::ULongLong:
  case QMetaType::UChar:
    return py::int_((unsigned long long)value.toULongLong());
  case QMetaType::Double:
  case QMetaType::Float:
    return py::float_(value.toDouble());
  case QMetaType::QStringList: {
    py::list list;
    for (const QString &s : value.toStringList())
      list.append(py::str(s.toStdString()));
    return std::move(list);
  }
  case QMetaType::QVariantList: {
    py::list list;
    for (const QVariant &item : value.toList()) list.append(toPython(item));
    return std::move(list);
  }
  case QMetaType::QVariantMap: {
    py::dict dict;
    QVariantMap map = value.toMap();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
      dict[it.key().toStdString().c_str()] = toPython(it.value());
    return std::move(dict);
  }
  case QMetaType::QVariantHash: {
    py::dict dict;
    QVariantHash hash = value.toHash();
    for (auto it = hash.constBegin(); it != hash.constEnd(); ++it)
      dict[it.key().toStdString().c_str()] = toPython(it.value());
    return std::move(dict);
  }
  default:
    if (value.canConvert<QString>())
      return py::str(value.toString().toStdString());
    return py::none();
  }
}

// Defines opentoonz.name (the GIL must be held)
void addFunction(const QString &name, const PythonEngine::Function &f,
                 const QString &doc) {
  py::module_ module  = py::module_::import("opentoonz");
  std::string nameStr = name.toStdString();
  std::string docStr  = doc.toStdString();
  // not module.def(), which would add an overload to an existing function
  module.attr(nameStr.c_str()) = py::cpp_function(
      [f, nameStr](py::args args) {
        // The functions of the application use its UI: a thread that a script
        // started must not call them while the event loop runs
        QCoreApplication *app = QCoreApplication::instance();
        if (app && QThread::currentThread() != app->thread())
          throw std::runtime_error(
              nameStr + "() can only be called from the main thread");
        QVariantList list;
        for (py::handle arg : args) list.append(toVariant(arg));
        return toPython(f(list));
      },
      py::name(nameStr.c_str()), py::scope(module), docStr.c_str());
}

QString errorText(const std::exception &e) {
  return QString::fromUtf8(e.what()) + "\n";
}

}  // namespace

//=============================================================================

PYBIND11_EMBEDDED_MODULE(opentoonz, m) {
  m.doc() =
      "OpenToonz scripting: the Scene, Level and StageObject classes of the "
      "JavaScript Script Console, with the same methods. Frames are xsheet "
      "rows numbered from 0.";

  py::class_<OutputStream>(m, "_OutputStream")
      .def(py::init<int>())
      .def("write", &OutputStream::write)
      .def("flush", [](OutputStream &) {})
      .def("isatty", [](OutputStream &) { return false; })
      .def("writable", [](OutputStream &) { return true; })
      .def_property_readonly("encoding", [](OutputStream &) { return "utf-8"; })
      .def_property_readonly("errors",
                             [](OutputStream &) { return "backslashreplace"; });

  PythonBinding::bindClasses(m);
}

//=============================================================================

class PythonEngine::Imp {
public:
  struct PendingFunction {
    QString m_name;
    Function m_function;
    QString m_doc;
  };

  bool m_started, m_failed;
  QString m_error, m_version;
  QList<PendingFunction> m_pendingFunctions;

  // the functions of the bootstrap code (new references, never released: the
  // interpreter is never finalized)
  PyObject *m_push, *m_reset, *m_runFile;
  // the GUI thread state, while the GIL is released
  PyThreadState *m_threadState;

  Imp()
      : m_started(false)
      , m_failed(false)
      , m_push(0)
      , m_reset(0)
      , m_runFile(0)
      , m_threadState(0) {}
};

//-----------------------------------------------------------------------------

PythonEngine::PythonEngine() : m_imp(new Imp()) {}

// Never called: the engine and the interpreter live until the application
// exits (some extension modules don't support Py_Finalize())
PythonEngine::~PythonEngine() {}

//-----------------------------------------------------------------------------

PythonEngine *PythonEngine::instance() {
  static PythonEngine *engine = new PythonEngine();
  return engine;
}

//-----------------------------------------------------------------------------

bool PythonEngine::isStarted() const { return m_imp->m_started; }

QString PythonEngine::getVersion() const { return m_imp->m_version; }

//-----------------------------------------------------------------------------

bool PythonEngine::start() {
  if (m_imp->m_started) return true;
  if (m_imp->m_failed) {
    emitOutput(StdErr, tr("Python is not available: %1\n").arg(m_imp->m_error));
    return false;
  }
  assert(QThread::currentThread() == thread());

  try {
    // no signal handlers: Ctrl+C must keep its usual effect on the
    // application
#if defined(PYBIND11_PYCONFIG_SUPPORT_PY_VERSION_HEX) &&                       \
    PY_VERSION_HEX >= PYBIND11_PYCONFIG_SUPPORT_PY_VERSION_HEX
    // with a PyConfig a failed initialization (e.g. no standard library)
    // throws instead of aborting the application
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv              = 0;
    config.install_signal_handlers = 0;
    py::initialize_interpreter(&config);
#else
    py::initialize_interpreter(false);
#endif
  } catch (const std::exception &e) {
    m_imp->m_failed = true;
    m_imp->m_error  = QString::fromUtf8(e.what());
    emitOutput(StdErr,
               tr("Python could not be started: %1\n").arg(m_imp->m_error));
    return false;
  }

  // the GIL is held
  try {
    py::dict ns;
    py::exec(bootstrapCode, ns);
    m_imp->m_push    = py::object(ns["push"]).release().ptr();
    m_imp->m_reset   = py::object(ns["reset"]).release().ptr();
    m_imp->m_runFile = py::object(ns["run_file"]).release().ptr();
    m_imp->m_version =
        QString::fromStdString(py::object(ns["version"]).cast<std::string>());
    m_imp->m_started = true;
    for (const Imp::PendingFunction &f : m_imp->m_pendingFunctions)
      addFunction(f.m_name, f.m_function, f.m_doc);
    m_imp->m_pendingFunctions.clear();
  } catch (const std::exception &e) {
    // py::error_already_set included
    if (!m_imp->m_started) {
      m_imp->m_failed = true;
      m_imp->m_error  = QString::fromUtf8(e.what());
    }
    emitOutput(StdErr, tr("Python could not be started: %1\n")
                           .arg(QString::fromUtf8(e.what())));
  }

  // the scripts run with the GIL, but Python threads can run in between
  m_imp->m_threadState = PyEval_SaveThread();
  return m_imp->m_started;
}

//-----------------------------------------------------------------------------

bool PythonEngine::push(const QString &line) {
  if (!start()) return false;
  assert(QThread::currentThread() == thread());
  py::gil_scoped_acquire gil;
  try {
    return py::handle(m_imp->m_push)(line.toStdString()).cast<bool>();
  } catch (const std::exception &e) {
    emitOutput(StdErr, errorText(e));
  }
  return false;
}

//-----------------------------------------------------------------------------

void PythonEngine::resetInput() {
  if (!m_imp->m_started) return;
  py::gil_scoped_acquire gil;
  try {
    py::handle(m_imp->m_reset)();
  } catch (const std::exception &e) {
    emitOutput(StdErr, errorText(e));
  }
}

//-----------------------------------------------------------------------------

bool PythonEngine::runFile(const QString &path) {
  if (!start()) return false;
  assert(QThread::currentThread() == thread());
  py::gil_scoped_acquire gil;
  try {
    return py::handle(m_imp->m_runFile)(path.toStdString()).cast<bool>();
  } catch (const std::exception &e) {
    emitOutput(StdErr, errorText(e));
  }
  return false;
}

//-----------------------------------------------------------------------------

void PythonEngine::defineFunction(const QString &name, const Function &f,
                                  const QString &doc) {
  if (!m_imp->m_started) {
    for (Imp::PendingFunction &pending : m_imp->m_pendingFunctions)
      if (pending.m_name == name) {
        pending.m_function = f;
        pending.m_doc      = doc;
        return;
      }
    m_imp->m_pendingFunctions.append(Imp::PendingFunction{name, f, doc});
    return;
  }
  py::gil_scoped_acquire gil;
  try {
    addFunction(name, f, doc);
  } catch (const std::exception &e) {
    emitOutput(StdErr, errorText(e));
  }
}
