#include "automation.h"
#include "automation_dispatch.h"
#include "automation_files.h"
#include "automation_containment.h"
#include "automation_live_protocol.h"
#include "automation_ownership.h"
#include "automation_scene_host.h"
#include "automation_runtime.h"
#include "automation_shared_parse.h"

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
#include <QSemaphore>
#include <QSettings>
#include <QThread>
#include <QUuid>

#include <chrono>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

#ifdef BUILD_UI_PORT_TESTS
#include "automation_production.h"
#include "automation_assistance.h"
#include "automation_macro.h"
#include "automation_path.h"
#include "automation_audio.h"
#include "automation_rig.h"
#include "automation_shared.h"
#include "automation_storyboard.h"
#include "automation_lipsync.h"
#include "automation_text.h"
#include "automation_vector_opacity.h"
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

constexpr int maximumLineBytes      = 8 * 1024 * 1024;
constexpr int quitRetryMilliseconds = 50;

struct Options {
  QString workspace;
  QStringList inputRoots;
  QString mediaToolsDirectory;
  QMap<QString, SharedRootGrant> sharedRoots;
  QStringList sharedWrites;
  QString parseHelper;
  QString sceneHost;
#ifdef BUILD_UI_PORT_TESTS
  bool testDomains = false;
  int testRenderDeadlineMs = 0;
  QString childFault;
  QString renderFault;
  QString writeFault;
  QStringList pathSwap;
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

void addSharedRoot(Options& options, const QString& value) {
  const int separator = value.indexOf('=');
  const QString role  = value.left(separator);
  if (separator <= 0 || !sharedRootRoles().contains(role))
    throw Error("invalid_argument",
                "--shared-root requires ROLE=DIR with ROLE studio_palettes, "
                "project_palettes or library.");
  if (options.sharedRoots.contains(role))
    throw Error("invalid_argument",
                "Specify --shared-root at most once per role.");
  const QString path = value.mid(separator + 1);
  QString directory;
  try {
    directory = canonicalWorkerRoot(path);
  } catch (const Error& error) {
    throw Error("invalid_argument",
                "Invalid --shared-root directory: " + error.message);
  }
  if (QDir::cleanPath(QDir::fromNativeSeparators(path)) != directory)
    throw Error("invalid_argument",
                "--shared-root must name its canonical directory without "
                "symlink components.");
  options.sharedRoots.insert(role, {directory, false});
}

void addSharedWrite(Options& options, const QString& role) {
  if (!sharedRootRoles().contains(role) || options.sharedWrites.contains(role))
    throw Error("invalid_argument",
                "--shared-write names one shared root role at most once.");
  options.sharedWrites.append(role);
}

void addOption(Options& options, const QString& key, const QString& value) {
  if (key == "--media-tools")
    addMediaTools(options, value);
  else if (key == "--shared-root")
    addSharedRoot(options, value);
  else if (key == "--shared-write")
    addSharedWrite(options, value);
  else if (key == "--parse-helper")
    options.parseHelper = value;
  else if (key == "--scene-host")
    options.sceneHost = value;
  else
    addWorkerRoot(options, key, value);
}

bool valueOption(const QString& key) {
  return key == "--workspace" || key == "--input-root" ||
         key == "--media-tools" || key == "--shared-root" ||
         key == "--shared-write" || key == "--parse-helper" ||
         key == "--scene-host";
}

bool overlapping(const QString& first, const QString& second) {
  const auto beneath = [](const QString& path, const QString& root) {
    return path == root ||
           path.startsWith(root.endsWith('/') ? root : root + '/');
  };
  return beneath(first, second) || beneath(second, first);
}

void validateSharedRoots(Options& options) {
  for (const QString& role : options.sharedWrites) {
    if (!options.sharedRoots.contains(role))
      throw Error("invalid_argument",
                  "--shared-write requires --shared-root for the same role.");
    options.sharedRoots[role].writable = true;
  }
  const QStringList roles = options.sharedRoots.keys();
  for (int index = 0; index < roles.size(); ++index) {
    const QString path = options.sharedRoots.value(roles[index]).path;
    if (overlapping(path, options.workspace))
      throw Error("invalid_argument",
                  "Shared roots must not overlap the worker workspace.");
    for (int other = index + 1; other < roles.size(); ++other)
      if (overlapping(path, options.sharedRoots.value(roles[other]).path))
        throw Error("invalid_argument",
                    "Shared roots must not overlap one another.");
  }
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
    if (key == "--test-render-deadline-ms" && index + 1 < arguments.size()) {
      options.testRenderDeadlineMs = arguments[++index].toInt();
      continue;
    }
    if (key == "--test-child-fault" && index + 1 < arguments.size()) {
      options.childFault = arguments[++index];
      continue;
    }
    if (key == "--test-render-fault" && index + 1 < arguments.size()) {
      options.renderFault = arguments[++index];
      continue;
    }
    if (key == "--test-write-fault" && index + 1 < arguments.size()) {
      options.writeFault = arguments[++index];
      continue;
    }
    if (key == "--test-path-swap" && index + 3 < arguments.size()) {
      options.pathSwap = arguments.mid(index + 1, 3);
      index += 3;
      continue;
    }
#endif
    if (!valueOption(key) || index + 1 >= arguments.size())
      throw Error("invalid_argument",
                  "Worker accepts --workspace DIR, repeated --input-root DIR, "
                  "optional --media-tools DIR, repeated --shared-root "
                  "ROLE=DIR and repeated --shared-write ROLE.");
    addOption(options, key, arguments[++index]);
  }
  if (options.workspace.isEmpty())
    throw Error("invalid_argument",
                "Automation worker requires --workspace DIR.");
  if (!options.sceneHost.isEmpty() && (!options.mediaToolsDirectory.isEmpty() ||
                                       !options.sharedRoots.isEmpty()))
    throw Error("invalid_argument",
                "A scene host never receives media tools or shared roots.");
  validateSharedRoots(options);
  return options;
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
  void eventLoopReturned() { m_loopReturned.release(); }

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
  // The platform event loop can miss the single wake-up that quit() sends
  // from a queued call and then sleep until an unrelated event arrives.
  // Asking again until the loop has returned bounds that wait.
  void requestStop() {
    QMetaObject::invokeMethod(&m_application, m_stop, Qt::QueuedConnection);
    while (!m_loopReturned.tryAcquire(1, quitRetryMilliseconds))
      QMetaObject::invokeMethod(&m_application, &QCoreApplication::quit,
                                Qt::QueuedConnection);
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
  QSemaphore m_loopReturned;
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
    Live::rejectDuplicateKeys(line);
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
}  // namespace

void injectChildFault(const QString& fault) {
  if (fault == "crash") std::abort();
  if (fault == "hang")
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
  if (fault != "memory") return;
  // 4 GiB, past every child memory bound, then wait: a missing bound shows as
  // a timeout instead of exhausting the machine.
  constexpr size_t block = 64 * 1024 * 1024;
  std::vector<std::unique_ptr<char[]>> blocks;
  for (int index = 0; index < 64; ++index) {
    blocks.push_back(std::make_unique<char[]>(block));
    std::memset(blocks.back().get(), 1, block);
  }
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

namespace {

int domainTestResult(Context& context) {
  try {
    runAutomationFxTests(context);
    runAutomationFxAuthoringTests(context);
    runAutomationColorCompositeTests(context);
    runAutomationFxFinishingTests(context);
    runAutomationFxPreflightTests(context);
    runAutomationGroupTests(context);
    runAutomationVectorOpacityTests(context);
    runAutomationAnimationTests();
    runAutomationPathTests(context);
    runAutomationAudioTests(context);
    runAutomationRigTests(context);
    runAutomationSharedTests(context);
    runAutomationStoryboardTests(context);
    runAutomationLipsyncTests(context);
    runAutomationProductionTests(context);
    runAutomationAssistanceTests();
    runAutomationMacroTests(context);
    runAutomationTextTests(context);
    runAutomationMediaProcessTests(context);
    runAutomationSceneHostTests(context);
    runAutomationRuntimeTests(context);
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
                  const Options& options, bool helpersContained,
                  const QString& runtime) {
  Context context(options.workspace, options.inputRoots, helpersContained);
  context.mediaToolsDirectory = options.mediaToolsDirectory;
#ifdef BUILD_UI_PORT_TESTS
  if (options.testRenderDeadlineMs > 0)
    context.renderDeadlineMs = options.testRenderDeadlineMs;
  context.childFault = options.childFault;
  context.renderFault = options.renderFault;
  context.writeFault  = options.writeFault;
  if (!options.pathSwap.isEmpty())
    armPathSwap(options.pathSwap[0], options.pathSwap[1], options.pathSwap[2]);
#endif
  context.sharedRoots         = options.sharedRoots;
  context.runtimeDirectory    = runtime;
  if (!options.parseHelper.isEmpty())
    return runSharedParseHelper(context, options.parseHelper, output);
  if (!options.sceneHost.isEmpty()) {
    context.sceneHostDocument = options.sceneHost;
    startSceneHostWatchdog();
  }
#ifdef BUILD_UI_PORT_TESTS
  if (options.testDomains) return executeDomainTests(application, context);
#endif
  sweepAbandonedRenders(context);
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
  reader.eventLoopReturned();
  reader.wait();
  shutdownRender(context);
  TImageCache::instance()->clear(true);
  return result != 0 ? result : reader.exitCode();
}

}  // namespace

int runWorker(int argc, char* argv[]) {
  try {
    const Options options = parseOptions(workerArguments(argc, argv));
    // Parse helpers and scene hosts are children of a worker and share its
    // containment.
    const bool helpersContained = options.parseHelper.isEmpty() &&
                                  options.sceneHost.isEmpty() &&
                                  establishAutomationContainment();
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
    // Held until the process ends and left marked afterwards, so a later
    // worker can tell this directory from one that is still in use.
    static OwnershipClaim runtimeClaim(true);
    if (!runtimeClaim.acquire(runtime))
      throw Error("io_error",
                  "Could not claim the private worker runtime directory.");
    if (options.parseHelper.isEmpty() && options.sceneHost.isEmpty())
      OwnershipClaim::removeAbandoned(options.workspace, {"runtime_*"});
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
    return executeWorker(application, output, options, helpersContained,
                         runtime);
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
