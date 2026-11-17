#include "automation.h"
#include "automation_dispatch.h"

#include "tenv.h"
#include "texception.h"
#include "thirdparty.h"
#include "timagecache.h"
#include "tiio_std.h"
#include "tnzimage.h"
#include "tnzsound.h"
#include "tpalette.h"
#include "tsystem.h"
#include "tthread.h"
#include "toonz/preferences.h"
#include "toonz/studiopalette.h"
#include "toonz/toonzfolders.h"

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
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <iostream>

#ifdef BUILD_UI_PORT_TESTS
#include "automation_production.h"
#include "tests/automation_animation_test.h"
#endif

#ifdef _WIN32
#include <io.h>
#include <qt_windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

DV_IMPORT_API void initStdFx();
DV_IMPORT_API void initColorFx();

namespace GroundzeroAutomation {
namespace {

constexpr int maximumLineBytes = 8 * 1024 * 1024;

struct Options {
  QString workspace;
  QStringList inputRoots;
  QString mediaToolsDirectory;
#ifdef BUILD_UI_PORT_TESTS
  bool testDomains = false;
#endif
};

QStringList workerArguments(int argc, char* argv[]) {
  QStringList arguments;
#ifdef _WIN32
  Q_UNUSED(argc);
  Q_UNUSED(argv);
  int count             = 0;
  auto* nativeArguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!nativeArguments)
    throw Error("invalid_argument", "Could not read the worker command line.");
  for (int index = 0; index < count; ++index)
    arguments.append(QString::fromWCharArray(nativeArguments[index]));
  LocalFree(nativeArguments);
#else
  for (int index = 0; index < argc; ++index)
    arguments.append(QString::fromLocal8Bit(argv[index]));
#endif
  return arguments;
}

QString canonicalWorkerRoot(const QString& path) {
  const QFileInfo directory(path);
  if (!directory.isAbsolute() || !directory.isDir() || directory.isSymLink())
    throw Error("invalid_argument",
                "Worker roots must be existing absolute directories.");
  const QString canonical = directory.canonicalFilePath();
  if (canonical.isEmpty())
    throw Error("invalid_argument", "Could not resolve a worker root.");
  return canonical;
}

void addWorkerRoot(Options& options, const QString& key, const QString& path) {
  const QString canonical = canonicalWorkerRoot(path);
  if (key == "--input-root") {
    if (options.inputRoots.size() >= 16)
      throw Error("invalid_argument", "At most 16 input roots are supported.");
    options.inputRoots.append(canonical);
  } else {
    if (!options.workspace.isEmpty())
      throw Error("invalid_argument", "Specify --workspace exactly once.");
    options.workspace = canonical;
  }
}

void validateMediaExecutable(const QString& directory, const QString& name) {
  const QString path = QDir(directory).filePath(name);
  const QFileInfo executable(path);
  if (!executable.isFile() || executable.isSymLink() ||
      !executable.isExecutable() || executable.canonicalFilePath() != path)
    throw Error("invalid_argument",
                "--media-tools requires regular executable ffmpeg and ffprobe "
                "files directly inside its directory; links are not admitted.",
                {{"helper", name}});
}

void addMediaTools(Options& options, const QString& path) {
  if (!options.mediaToolsDirectory.isEmpty())
    throw Error("invalid_argument", "Specify --media-tools at most once.");
  QString directory;
  try {
    directory = canonicalWorkerRoot(path);
  } catch (const Error& error) {
    throw Error("invalid_argument",
                "Invalid --media-tools directory: " + error.message);
  }
  if (QDir::cleanPath(QDir::fromNativeSeparators(path)) != directory)
    throw Error("invalid_argument",
                "--media-tools must name its canonical directory without "
                "symlink components.");
#ifdef _WIN32
  validateMediaExecutable(directory, "ffmpeg.exe");
  validateMediaExecutable(directory, "ffprobe.exe");
#else
  validateMediaExecutable(directory, "ffmpeg");
  validateMediaExecutable(directory, "ffprobe");
#endif
  options.mediaToolsDirectory = directory;
}

void addOption(Options& options, const QString& key, const QString& value) {
  if (key == "--media-tools")
    addMediaTools(options, value);
  else
    addWorkerRoot(options, key, value);
}

bool valueOption(const QString& key) {
  return key == "--workspace" || key == "--input-root" ||
         key == "--media-tools";
}

Options parseOptions(const QStringList& arguments) {
  Options options;
  for (int index = 2; index < arguments.size(); ++index) {
    const QString key = arguments[index];
#ifdef BUILD_UI_PORT_TESTS
    if (key == "--test-domains") {
      options.testDomains = true;
      continue;
    }
#endif
    if (!valueOption(key) || index + 1 >= arguments.size())
      throw Error("invalid_argument",
                  "Worker accepts --workspace DIR, repeated --input-root DIR, "
                  "and optional --media-tools DIR.");
    addOption(options, key, arguments[++index]);
  }
  if (options.workspace.isEmpty())
    throw Error("invalid_argument",
                "Automation worker requires --workspace DIR.");
  return options;
}

void establishMediaContainment(const Options& options) {
#ifdef _WIN32
  if (options.mediaToolsDirectory.isEmpty()) return;
  const HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job)
    throw Error("media_containment_failed",
                "Cannot create the worker media job.");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  const bool configured =
      SetHandleInformation(job, HANDLE_FLAG_INHERIT, 0) &&
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                              sizeof(limits));
  if (!configured || !AssignProcessToJobObject(job, GetCurrentProcess())) {
    const DWORD error = GetLastError();
    CloseHandle(job);
    throw Error("media_containment_failed",
                "Cannot contain native media helpers in the worker job.",
                {{"windows_error", QString::number(error)}});
  }
  // The sole non-inheritable handle lives until OS process teardown. Closing
  // it on this stack would terminate the worker itself, which is a job member.
#else
  Q_UNUSED(options);
#endif
}

int reserveProtocolOutput() {
  std::fflush(stdout);
#ifdef _WIN32
  const int descriptor = _dup(_fileno(stdout));
  if (descriptor < 0 || _dup2(_fileno(stderr), _fileno(stdout)) < 0)
    throw Error("io_error", "Could not reserve the worker protocol output.");
  const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
  if (!SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)) {
    _close(descriptor);
    throw Error("io_error", "Could not isolate the worker protocol handle.");
  }
  _setmode(descriptor, _O_BINARY);
  _setmode(_fileno(stdin), _O_BINARY);
#else
  const int descriptor = dup(fileno(stdout));
  if (descriptor < 0 || dup2(fileno(stderr), fileno(stdout)) < 0)
    throw Error("io_error", "Could not reserve the worker protocol output.");
  if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
    close(descriptor);
    throw Error("io_error",
                "Could not isolate the worker protocol descriptor.");
  }
#endif
  return descriptor;
}

QString createRuntimeDirectory(const QString& workspace) {
  const QString path = QDir(workspace).filePath(
      "runtime_" + QUuid::createUuid().toString(QUuid::WithoutBraces));
  if (!QDir().mkpath(path))
    throw Error("io_error",
                "Could not create the private worker runtime directory.");
  for (const QString& name : {QString("settings"), QString("cache/ffmpeg"),
                              QString("portablestuff")})
    if (!QDir(path).mkpath(name))
      throw Error("io_error", "Could not initialize private worker storage.");
  return path;
}

void initializeNative(const QString& runtime, const char* executable,
                      const Options& options) {
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
  // An empty preference would probe adjacent helpers during initImageIo.
  // The fresh runtime has no helpers, so omitted configuration stays disabled.
  ThirdParty::setFFmpegDir(options.mediaToolsDirectory.isEmpty()
                               ? runtime
                               : options.mediaToolsDirectory);
  if (!options.mediaToolsDirectory.isEmpty()) ThirdParty::setFFmpegTimeout(60);
  const QString cache = canonicalWorkerRoot(QDir(runtime).filePath("cache"));
  ToonzFolder::setCacheRootFolder(TFilePath(cache.toStdWString()));
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
  using Stop = std::function<void()>;
  InputThread(QApplication& application, Consumer consumer, Stop stop)
      : m_application(application),
        m_consumer(std::move(consumer)),
        m_stop(std::move(stop)) {}
  int exitCode() const { return m_exitCode; }

protected:
  void run() override {
    QByteArray line;
    int next = 0;
    while ((next = std::fgetc(stdin)) != EOF) {
      if (next == '\n') {
        if (!deliver(line, false)) {
          requestStop();
          return;
        }
        line.clear();
      } else {
        line.append(static_cast<char>(next));
        if (line.size() > maximumLineBytes) {
          m_exitCode = 2;
          deliver({}, true);
          requestStop();
          return;
        }
      }
    }
    if (std::ferror(stdin)) {
      m_exitCode = 2;
      std::cerr << "automation-worker: protocol input failed\n";
    } else if (!line.isEmpty()) {
      deliver(line, false);
    }
    requestStop();
  }

private:
  void requestStop() {
    QMetaObject::invokeMethod(&m_application, m_stop, Qt::QueuedConnection);
  }

  bool deliver(const QByteArray& line, bool tooLarge) {
    bool keepReading = false;
    QMetaObject::invokeMethod(
        &m_application,
        [this, line, tooLarge, &keepReading]() {
          keepReading = m_consumer(line, tooLarge);
        },
        Qt::BlockingQueuedConnection);
    if (!keepReading) m_exitCode = 2;
    return keepReading;
  }
  QApplication& m_application;
  Consumer m_consumer;
  Stop m_stop;
  int m_exitCode = 0;
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

#ifdef BUILD_UI_PORT_TESTS
int domainTestResult(Context& context) {
  try {
    runAutomationFxTests(context);
    runAutomationFxAuthoringTests(context);
    runAutomationGroupTests(context);
    runAutomationAnimationTests();
    runAutomationProductionTests(context);
    runAutomationMediaProcessTests(context);
    return 0;
  } catch (const Error& error) {
    std::cerr << "automation-worker: domain tests: "
              << error.code.toStdString() << ": "
              << error.message.toStdString() << '\n';
  } catch (const TException& error) {
    std::cerr << "automation-worker: domain tests: "
              << QString::fromStdWString(error.getMessage()).toStdString()
              << '\n';
  } catch (const std::exception& error) {
    std::cerr << "automation-worker: domain tests: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "automation-worker: domain tests: unknown native exception\n";
  }
  return 2;
}

int executeDomainTests(QApplication& application, Context& context) {
  bool completed = false;
  const bool queued = QMetaObject::invokeMethod(
      &application,
      [&application, &context, &completed] {
        const int result = domainTestResult(context);
        completed = true;
        application.exit(result);
      },
      Qt::QueuedConnection);
  if (!queued)
    throw Error("internal_error", "Could not queue native domain tests.");
  const int result = application.exec();
  TImageCache::instance()->clear(true);
  if (!completed)
    throw Error("internal_error",
                "The application exited before native domain tests finished.");
  return result;
}
#endif

int executeWorker(QApplication& application, QFile& output,
                  const Options& options) {
  Context context(options.workspace, options.inputRoots);
  context.mediaToolsDirectory = options.mediaToolsDirectory;
#ifdef BUILD_UI_PORT_TESTS
  if (options.testDomains) return executeDomainTests(application, context);
#endif
  Dispatcher dispatcher(context);
  InputThread reader(
      application,
      [&dispatcher, &output](const QByteArray& line, bool tooLarge) {
        return writeResponse(output, processLine(dispatcher, line, tooLarge));
      },
      [&application, &context]() {
        // A native writer can be inside a nested event loop. Set its abort
        // token before quit unwinds that loop into a blocking writer join.
        requestRenderCancellation(context);
        application.quit();
      });
  reader.start();
  const int result = application.exec();
  reader.wait();
  shutdownRender(context);
  TImageCache::instance()->clear(true);
  return result != 0 ? result : reader.exitCode();
}

}  // namespace

int runWorker(int argc, char* argv[]) {
  try {
    const Options options = parseOptions(workerArguments(argc, argv));
    establishMediaContainment(options);
#ifndef _WIN32
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
      throw Error("io_error",
                  "Could not configure worker pipe failure handling.");
#endif
    QFile output;
    if (!output.open(reserveProtocolOutput(), QIODevice::WriteOnly,
                     QFileDevice::AutoCloseHandle))
      throw Error("io_error", "Could not open the worker protocol stream.");
    const QString runtime = createRuntimeDirectory(options.workspace);
    if (!QDir::setCurrent(runtime))
      throw Error("io_error", "Could not enter the private worker runtime directory.");
    int applicationArgc   = 1;
#ifdef _WIN32
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
#endif
    QApplication application(applicationArgc, argv);
    std::setlocale(LC_NUMERIC, "C");
    QObject mainScope(&application);
    mainScope.setObjectName("mainScope");
    application.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("GroundzeroAutomation");
    QCoreApplication::setApplicationName("GroundzeroAutomationWorker");
    const QByteArray executable = QFile::encodeName(QCoreApplication::applicationFilePath());
    initializeNative(runtime, executable.constData(), options);
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
