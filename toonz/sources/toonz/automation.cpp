#include "automation.h"
#include "automation_dispatch.h"

#include "tenv.h"
#include "texception.h"
#include "timagecache.h"
#include "tiio_std.h"
#include "tnzimage.h"
#include "tnzsound.h"
#include "tpalette.h"
#include "tsystem.h"
#include "tthread.h"
#include "toonz/preferences.h"
#include "toonz/studiopalette.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <QSettings>
#include <QThread>
#include <QUuid>

#include <clocale>
#include <cstdio>
#include <functional>
#include <iostream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

DV_IMPORT_API void initStdFx();
DV_IMPORT_API void initColorFx();

namespace GroundzeroAutomation {
#ifdef BUILD_UI_PORT_TESTS
void runAutomationFxTests(Context& context);
#endif
namespace {

constexpr int maximumLineBytes = 8 * 1024 * 1024;

struct Options {
  QString workspace;
  QStringList inputRoots;
#ifdef BUILD_UI_PORT_TESTS
  bool testDomains = false;
#endif
};

Options parseOptions(int argc, char* argv[]) {
  Options options;
  for (int index = 2; index < argc; ++index) {
    const QString key = QString::fromLocal8Bit(argv[index]);
#ifdef BUILD_UI_PORT_TESTS
    if (key == "--test-domains") {
      options.testDomains = true;
      continue;
    }
#endif
    if ((key != "--workspace" && key != "--input-root") || index + 1 >= argc)
      throw Error(
          "invalid_argument",
          "Worker accepts --workspace DIR and repeated --input-root DIR.");
    const QFileInfo directory(QString::fromLocal8Bit(argv[++index]));
    if (!directory.isAbsolute() || !directory.isDir() || directory.isSymLink())
      throw Error("invalid_argument",
                  "Worker roots must be existing absolute directories.");
    const QString canonical = directory.canonicalFilePath();
    if (canonical.isEmpty())
      throw Error("invalid_argument", "Could not resolve a worker root.");
    if (key == "--input-root") {
      if (options.inputRoots.size() >= 16)
        throw Error("invalid_argument",
                    "At most 16 input roots are supported.");
      options.inputRoots.append(canonical);
    } else {
      if (!options.workspace.isEmpty())
        throw Error("invalid_argument", "Specify --workspace exactly once.");
      options.workspace = canonical;
    }
  }
  if (options.workspace.isEmpty())
    throw Error("invalid_argument",
                "Automation worker requires --workspace DIR.");
  return options;
}

int reserveProtocolOutput() {
  std::fflush(stdout);
#ifdef _WIN32
  const int descriptor = _dup(_fileno(stdout));
  if (descriptor < 0 || _dup2(_fileno(stderr), _fileno(stdout)) < 0)
    throw Error("io_error", "Could not reserve the worker protocol output.");
  _setmode(descriptor, _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#else
  const int descriptor = dup(fileno(stdout));
  if (descriptor < 0 || dup2(fileno(stderr), fileno(stdout)) < 0)
    throw Error("io_error", "Could not reserve the worker protocol output.");
#endif
  return descriptor;
}

QString createRuntimeDirectory(const QString& workspace) {
  const QString path = QDir(workspace).filePath(
      "runtime_" + QUuid::createUuid().toString(QUuid::WithoutBraces));
  if (!QDir().mkpath(path))
    throw Error("io_error",
                "Could not create the private worker runtime directory.");
  for (const QString& name : {QString("settings"), QString("cache")})
    if (!QDir(path).mkpath(name))
      throw Error("io_error", "Could not initialize private worker storage.");
  return path;
}

void initializeNative(const QString& runtime, const char* executable) {
  QSettings::setDefaultFormat(QSettings::IniFormat);
  const QString settings = QDir(runtime).filePath("settings");
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings);
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settings);
  TEnv::setApplicationFileName(executable);
  TEnv::setRootVarName("TOONZROOT");
  TEnv::setSystemVarPrefix("TOONZ");
  TEnv::setArgPathValue("TOONZROOT", runtime.toStdString());
  for (const auto& entry : TEnv::getSystemPathMap()) {
    const QString path =
        QDir(runtime).filePath(QString::fromStdString(entry.second));
    if (!QDir().mkpath(path))
      throw Error("io_error",
                  "Could not initialize a native worker directory.");
    TEnv::setArgPathValue("TOONZ" + entry.first, path.toStdString());
  }
  StudioPalette::enable(false);
  Preferences::instance()->setValue(projectRoot, 8, false);
  Preferences::instance()->setValue(autosaveEnabled, false, false);
  Preferences::instance()->setValue(latestVersionCheckEnabled, false, false);
  TPalette::setRootDir(
      TFilePath(QDir(runtime).filePath("library").toStdWString()));
  Tiio::defineStd();
  initImageIo();
  initSoundIo();
  initStdFx();
  initColorFx();
  TThread::init();
  TSystem::hasMainLoop(true);
  TImageCache::instance()->setRootDir(
      TFilePath(QDir(runtime).filePath("cache").toStdWString()));
}

class InputThread final : public QThread {
public:
  using Consumer = std::function<bool(const QByteArray&, bool)>;
  InputThread(QApplication& application, Consumer consumer)
      : m_application(application), m_consumer(std::move(consumer)) {}

protected:
  void run() override {
    QByteArray line;
    int next = 0;
    while ((next = std::fgetc(stdin)) != EOF) {
      if (next == '\n') {
        if (!deliver(line, false)) {
          QMetaObject::invokeMethod(&m_application, "quit",
                                    Qt::QueuedConnection);
          return;
        }
        line.clear();
      } else {
        line.append(static_cast<char>(next));
        if (line.size() > maximumLineBytes) {
          deliver({}, true);
          QMetaObject::invokeMethod(&m_application, "quit",
                                    Qt::QueuedConnection);
          return;
        }
      }
    }
    if (!line.isEmpty()) deliver(line, false);
    QMetaObject::invokeMethod(&m_application, "quit", Qt::QueuedConnection);
  }

private:
  bool deliver(const QByteArray& line, bool tooLarge) {
    bool keepReading = false;
    QMetaObject::invokeMethod(
        &m_application,
        [this, line, tooLarge, &keepReading]() {
          keepReading = m_consumer(line, tooLarge);
        },
        Qt::BlockingQueuedConnection);
    return keepReading;
  }
  QApplication& m_application;
  Consumer m_consumer;
};

QJsonObject processLine(Dispatcher& dispatcher, const QByteArray& line,
                        bool tooLarge) {
  try {
    if (tooLarge)
      throw Error("request_too_large",
                  "Worker requests must not exceed 8 MiB.");
    if (QString::fromUtf8(line).toUtf8() != line)
      throw Error("parse_error", "Worker requests must contain valid UTF-8.");
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
      throw Error("parse_error", "Expected one JSON request object per line.");
    return dispatcher.dispatch(document.object());
  } catch (const Error& error) {
    return {{"id", QJsonValue::Null}, {"error", errorObject(error)}};
  }
}

bool writeResponse(QFile& output, QJsonObject response) {
  QByteArray bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
  if (bytes.size() > maximumLineBytes) {
    response = {{"id", response.value("id")},
                {"error", errorObject(Error("response_too_large",
                                            "The result exceeds 8 MiB; request "
                                            "a smaller range or artifact."))}};
    bytes    = QJsonDocument(response).toJson(QJsonDocument::Compact);
  }
  bytes.append('\n');
  if (output.write(bytes) != bytes.size() || !output.flush()) {
    std::cerr << "automation-worker: protocol output failed\n";
    return false;
  }
  return true;
}

int executeWorker(QApplication& application, QFile& output,
                  const Options& options) {
  Context context(options.workspace, options.inputRoots);
#ifdef BUILD_UI_PORT_TESTS
  if (options.testDomains) {
    runAutomationFxTests(context);
    TImageCache::instance()->clear(true);
    return 0;
  }
#endif
  Dispatcher dispatcher(context);
  InputThread reader(application, [&dispatcher, &output](const QByteArray& line,
                                                         bool tooLarge) {
    return writeResponse(output, processLine(dispatcher, line, tooLarge));
  });
  reader.start();
  const int result = application.exec();
  reader.wait();
  shutdownRender(context);
  TImageCache::instance()->clear(true);
  return result;
}

}  // namespace

int runWorker(int argc, char* argv[]) {
  try {
    const Options options = parseOptions(argc, argv);
    QFile output;
    if (!output.open(reserveProtocolOutput(), QIODevice::WriteOnly,
                     QFileDevice::AutoCloseHandle))
      throw Error("io_error", "Could not open the worker protocol stream.");
    const QString runtime = createRuntimeDirectory(options.workspace);
    int applicationArgc = 1;
    QApplication application(applicationArgc, argv);
    std::setlocale(LC_NUMERIC, "C");
    QObject mainScope(&application);
    mainScope.setObjectName("mainScope");
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("GroundzeroAutomation");
    QCoreApplication::setApplicationName("GroundzeroAutomationWorker");
    initializeNative(runtime, argv[0]);
    return executeWorker(application, output, options);
  } catch (const Error& error) {
    std::cerr << "automation-worker: " << error.code.toStdString() << ": "
              << error.message.toStdString() << '\n';
  } catch (const TException& error) {
    std::cerr << "automation-worker: "
              << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception& error) {
    std::cerr << "automation-worker: " << error.what() << '\n';
  }
  return 2;
}

}  // namespace GroundzeroAutomation
