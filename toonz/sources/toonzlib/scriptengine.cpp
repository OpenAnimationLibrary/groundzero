

#include "toonz/scriptengine.h"
#include "toonz/scriptbinding.h"
#include "toonz/scriptbinding_files.h"
#include "trenderer.h"
#include "toonz/toonzfolders.h"
#include <QJSEngine>
#include <QJSValue>
#include <QCoreApplication>
#include <QEvent>
#include <QSemaphore>
#include <QFile>
#include <QTextStream>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QDeadlineTimer>
#include <QtGlobal>

#include <atomic>
#include <climits>

//=========================================================

namespace {

void sleepMs(unsigned long msec) {
  QMutex mutex;
  mutex.lock();
  QWaitCondition waitCondition;
  waitCondition.wait(&mutex, msec);
  mutex.unlock();
}

QString print(const QJSValue &arg, bool addQuotes) {
  if (arg.isArray()) {
    QString s   = "[";
    quint32 len = arg.property(QLatin1String("length")).toUInt();
    for (quint32 i = 0; i < len; ++i) {
      QJSValue item = arg.property(i);
      if (i > 0) s += ",";
      s += print(item, addQuotes);
    }
    s += "]";
    return s;
  } else if (arg.isBool()) {
    return arg.toBool() ? "true" : "false";
  } else if (arg.isString()) {
    if (addQuotes)
      return "\"" + arg.toString() + "\"";
    else
      return arg.toString();
  } else
    return TScriptBinding::toString(arg);
}

QString printArguments(const QJSValueList &args) {
  QString result;
  for (int i = 0; i < args.size(); ++i) {
    if (i > 0) result.append(" ");
    result.append(print(args[i], false));
  }
  return result;
}

QJSValue runFunction(QJSEngine *engine, const QJSValueList &args) {
  if (args.size() != 1) {
    engine->throwError("expected one parameter");
    return QJSValue();
  }
  TFilePath fp;
  if (!TScriptBinding::checkFilePath(engine, args[0], fp)) return QJSValue();
  if (!fp.isAbsolute()) {
    fp = ToonzFolder::getLibraryFolder() + "scripts" + fp;
  }

  QString fpStr = QString::fromStdWString(fp.getWideString());
  QFile file(fpStr);
  if (!file.open(QIODevice::ReadOnly)) {
    engine->throwError("can't read file " + fpStr);
    return QJSValue();
  }
  QTextStream in(&file);
  QString content = in.readAll();
  file.close();

  // The script is evaluated in the global scope: its global variables and
  // functions remain defined after run()
  QJSValue ret = engine->evaluate(content, fpStr);

  if (ret.isError()) {
    int line = ret.property("lineNumber").toInt();
    engine->throwError(QString("%1, at line %2 of %3")
                           .arg(ret.toString())
                           .arg(line)
                           .arg(fpStr));
    return QJSValue();
  }

  return ret;
}

}  // namespace

//=========================================================

// The thread running the scripts. The QJSEngine is created, used and
// destroyed only by this thread; the functions run here are queued to
// m_context, which lives in this thread.
class ScriptEngine::Executor {
public:
  QThread m_thread;
  QObject m_context;

  mutable QMutex m_mutex;
  QWaitCondition m_evaluationDone;
  bool m_evaluating;

  // set when the ScriptEngine is being destroyed: runOnMainThread() then
  // doesn't run its function any more
  std::atomic<bool> m_stopping;

  Executor() : m_evaluating(false), m_stopping(false) {
    m_context.moveToThread(&m_thread);
  }

  bool isScriptThread() const { return QThread::currentThread() == &m_thread; }

  // runs f on the script thread and waits for it. When called on the thread
  // of owner, the calls the script queues to owner (see runOnMainThread())
  // are delivered while waiting, so that the two threads can't wait for
  // each other.
  void runOnScriptThread(QObject *owner, const std::function<void()> &f) {
    if (isScriptThread())
      f();
    else if (QThread::currentThread() != owner->thread())
      QMetaObject::invokeMethod(&m_context, f, Qt::BlockingQueuedConnection);
    else {
      QSemaphore done;
      QMetaObject::invokeMethod(
          &m_context,
          [&f, &done]() {
            f();
            done.release();
          },
          Qt::QueuedConnection);
      while (!done.tryAcquire(1, 20))
        QCoreApplication::sendPostedEvents(owner, QEvent::MetaCall);
    }
  }
};

//=========================================================

ScriptEngine::ScriptEngine()
    : m_engine(0), m_executor(new Executor()), m_voidValue(0) {
  // I must call TRenderer::initialize(), because a script could cause a
  // rendering driven by a working thread
  TRenderer::initialize();

  // The engine is destroyed when the script thread ends (see ~ScriptEngine())
  connect(
      &m_executor->m_thread, &QThread::finished, &m_executor->m_context,
      [this]() {
        delete m_voidValue;
        m_voidValue = 0;
        delete m_engine;
        m_engine = 0;
      },
      Qt::DirectConnection);
  m_executor->m_thread.start();

  m_executor->runOnScriptThread(this, [this]() {
    m_engine          = new QJSEngine();
    QJSEngine &engine = *m_engine;

    TScriptBinding::bindAll(engine);

    // print(), warning() and the console results are not printed if they
    // are the void object. Being a child of the engine, it isn't garbage
    // collected.
    TScriptBinding::Void *voidObj = new TScriptBinding::Void();
    voidObj->setParent(m_engine);
    m_voidValue = new QJSValue(engine.newQObject(voidObj));
    engine.globalObject().setProperty("void", *m_voidValue);

    TScriptBinding::defineFunction(
        engine, "print", [this](QJSEngine *, const QJSValueList &args) {
          emitOutput(ScriptEngine::SimpleText, printArguments(args));
          sleepMs(50);
          return voidValue();
        });
    TScriptBinding::defineFunction(
        engine, "warning", [this](QJSEngine *, const QJSValueList &args) {
          emitOutput(ScriptEngine::Warning, printArguments(args));
          sleepMs(50);
          return voidValue();
        });
    TScriptBinding::defineFunction(engine, "run", runFunction);
  });
}

ScriptEngine::~ScriptEngine() {
  // stop the running script (if any), then the thread: when it ends it
  // destroys the engine. The script could be waiting for a call queued to
  // this thread (see runOnMainThread()): keep delivering them, while
  // m_stopping makes them return without running.
  m_executor->m_stopping = true;
  interrupt();
  m_executor->m_thread.quit();
  while (!m_executor->m_thread.wait(20))
    QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
  delete m_executor;
}

void ScriptEngine::defineFunction(const QString &name, const Function &f) {
  m_executor->runOnScriptThread(this, [this, name, f]() {
    TScriptBinding::defineFunction(*m_engine, name, f);
  });
}

void ScriptEngine::runOnMainThread(const std::function<void()> &f) {
  if (QThread::currentThread() == thread())
    f();
  else if (!m_executor->m_stopping)
    QMetaObject::invokeMethod(
        this,
        [this, &f]() {
          if (!m_executor->m_stopping) f();
        },
        Qt::BlockingQueuedConnection);
}

void ScriptEngine::evaluate(const QString &cmd) {
  {
    QMutexLocker locker(&m_executor->m_mutex);
    if (m_executor->m_evaluating) return;
    m_executor->m_evaluating = true;
  }

  QMetaObject::invokeMethod(
      &m_executor->m_context,
      [this, cmd]() {
        m_engine->collectGarbage();
        QJSValue result = m_engine->evaluate(cmd);
        if (m_engine->isInterrupted()) {
          // interrupt() was called: the evaluation has been aborted
        } else if (result.isError()) {
          emitOutput(ScriptEngine::SyntaxError, result.toString());
        } else if (result.isUndefined()) {
          emitOutput(ScriptEngine::UndefinedEvaluationResult, "undefined");
        } else {
          if (TScriptBinding::cast<TScriptBinding::Void>(result)) {
          } else {
            emitOutput(ScriptEngine::EvaluationResult, print(result, true));
          }
        }

        {
          QMutexLocker locker(&m_executor->m_mutex);
          m_engine->setInterrupted(false);
          m_executor->m_evaluating = false;
          m_executor->m_evaluationDone.wakeAll();
        }
        QMetaObject::invokeMethod(this, &ScriptEngine::onTerminated,
                                  Qt::QueuedConnection);
      },
      Qt::QueuedConnection);
}

bool ScriptEngine::wait(unsigned long time) {
  QDeadlineTimer deadline(QDeadlineTimer::Forever);
  if (time != ULONG_MAX) deadline.setRemainingTime(qint64(time));

  QMutexLocker locker(&m_executor->m_mutex);
  while (m_executor->m_evaluating) {
    if (!m_executor->m_evaluationDone.wait(&m_executor->m_mutex, deadline))
      return !m_executor->m_evaluating;
  }
  return true;
}

bool ScriptEngine::isEvaluating() const {
  QMutexLocker locker(&m_executor->m_mutex);
  return m_executor->m_evaluating;
}

void ScriptEngine::interrupt() {
  // setInterrupted() is thread safe. The flag is cleared at the end of the
  // evaluation (under the same mutex), so that it can't abort the next one.
  QMutexLocker locker(&m_executor->m_mutex);
  if (m_executor->m_evaluating && m_engine) m_engine->setInterrupted(true);
}

void ScriptEngine::onTerminated() {
  if (!m_executor->m_stopping) emit evaluationDone();
}
