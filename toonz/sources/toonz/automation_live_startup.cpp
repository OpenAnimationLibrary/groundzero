#include "automation_live_startup.h"

#include "automation_build_identity.h"

#include "iocommand.h"
#include "tapp.h"
#include "tools/toolhandle.h"
#include "tundo.h"

#include <QAction>
#include <QDir>
#include <QEvent>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QToolButton>

#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace GroundzeroAutomation {
namespace Live {
namespace {

thread_local int eventDepth = 0;

bool hasLiveLaunchOptions(int argc, char* argv[]) {
  for (int index = 1; index < argc; ++index) {
    const QString argument = QString::fromLocal8Bit(argv[index]);
    if (argument == "--automation-live" ||
        argument == "--automation-live-session")
      return true;
  }
  return false;
}

QStringList launchArguments(int argc, char* argv[]) {
  QStringList arguments;
#ifdef _WIN32
  Q_UNUSED(argv);
  int count = 0;
  std::unique_ptr<wchar_t*, decltype(&LocalFree)> native(
      CommandLineToArgvW(GetCommandLineW(), &count), &LocalFree);
  if (!native || count != argc)
    throw Error("invalid_argument",
                "Could not read the native GUI command line.");
  for (int index = 0; index < count; ++index)
    arguments.append(QString::fromWCharArray(native.get()[index]));
#else
  for (int index = 0; index < argc; ++index)
    arguments.append(QString::fromLocal8Bit(argv[index]));
#endif
  return arguments;
}

QString optionValue(const QStringList& arguments, int& index) {
  if (++index >= arguments.size())
    throw Error("invalid_argument", "Live automation options require a value.");
  const QString value = arguments[index];
  if (value.isEmpty())
    throw Error("invalid_argument",
                "Live automation option values cannot be empty.");
  return value;
}

bool unsupportedLaunchArgument(const QString& argument) {
  return argument.startsWith("--test-") ||
         argument.startsWith("--automation-") ||
         argument.endsWith(".toonzscript", Qt::CaseInsensitive);
}

void validateSessionPath(const QString& path) {
  if (path.isEmpty() || !QDir::isAbsolutePath(path))
    throw Error(
        "invalid_argument",
        "--automation-live-session requires an absolute new session path.");
}

void validateLaunch(const Options& options, const QStringList& remaining) {
  if (options.mode.isEmpty() && options.sessionPath.isEmpty()) return;
  if (options.mode != "inspect" && options.mode != "edit")
    throw Error("invalid_argument",
                "--automation-live requires inspect or edit.");
  validateSessionPath(options.sessionPath);
  for (const QString& argument : remaining) {
    if (unsupportedLaunchArgument(argument))
      throw Error(
          "invalid_argument",
          "Live automation cannot run with scripts, workers, or test modes.");
  }
}

void appendNativeBusyReasons(QStringList& reasons) {
  TApp* app = TApp::instance();
  if (app->isApplicationStarting()) reasons.append("starting");
  if (app->isSaveInProgress()) reasons.append("saving");
  if (IoCmd::isSceneTransitionInProgress()) reasons.append("scene_transition");
  if (app->getCurrentTool()->isToolBusy()) reasons.append("interactive_tool");
  if (TUndoManager::manager()->isBusy()) reasons.append("native_history");
}

QStringList currentBusyReasons() {
  QStringList reasons;
  appendNativeBusyReasons(reasons);
  if (QCoreApplication::closingDown()) reasons.append("shutting_down");
  if (QApplication::activeModalWidget() || QApplication::activePopupWidget())
    reasons.append("modal_or_popup");
  if (QThread::currentThread()->loopLevel() != 1 || eventDepth > 1)
    reasons.append("nested_event_dispatch");
  return reasons;
}

}  // namespace

bool Application::notify(QObject* receiver, QEvent* event) {
  QScopedValueRollback<int> dispatch(eventDepth, eventDepth + 1);
  return QApplication::notify(receiver, event);
}

Options parseLiveOptions(int& argc, char* argv[]) {
  if (!hasLiveLaunchOptions(argc, argv)) return {};
  Options options;
  const QStringList arguments = launchArguments(argc, argv);
  QStringList remaining;
  int kept = 1;
  for (int index = 1; index < argc; ++index) {
    const QString argument = arguments[index];
    if (argument == "--automation-live") {
      if (!options.mode.isEmpty())
        throw Error("invalid_argument", "--automation-live must appear once.");
      options.mode = optionValue(arguments, index);
    } else if (argument == "--automation-live-session") {
      if (!options.sessionPath.isEmpty())
        throw Error("invalid_argument",
                    "--automation-live-session must appear once.");
      options.sessionPath = optionValue(arguments, index);
    } else {
      remaining.append(argument);
      argv[kept++] = argv[index];
    }
  }
  validateLaunch(options, remaining);
  argc       = kept;
  argv[argc] = nullptr;
  return options;
}

class LiveUi::Impl final {
public:
  Impl(QApplication& application, QMainWindow& window, Options options)
      : mode(options.mode), service(std::move(options), currentBusyReasons) {
    prefetchBuildIdentity();
    label = new QLabel(&window);
    label->setObjectName("automationLiveStatus");
    label->setAccessibleName(QObject::tr("Live automation status"));
    action = new QAction(QObject::tr("Stop Live Automation"), &window);
    action->setObjectName("automationLiveStop");
    action->setEnabled(false);
    button = new QToolButton(&window);
    button->setDefaultAction(action);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setFocusPolicy(Qt::StrongFocus);
    window.statusBar()->addPermanentWidget(label);
    window.statusBar()->addPermanentWidget(button);
    window.statusBar()->show();
    QObject::connect(action, &QAction::triggered, label, [this]() { stop(); });
    QObject::connect(&application, &QCoreApplication::aboutToQuit, label,
                     [this]() { stop(); });
    observer = new QTimer(label);
    observer->setObjectName("automationLiveObserver");
    observer->setInterval(250);
    QObject::connect(observer, &QTimer::timeout, label, [this]() {
      if (!service.running()) {
        observer->stop();
        showStopped();
      }
    });
    showStopped();
  }

  ~Impl() {
    stop();
    delete button;
    delete label;
    delete action;
  }

  void showStopped() {
    if (label) label->setText(QObject::tr("Live automation: Stopped"));
    if (action) action->setEnabled(false);
  }

  void start() {
    service.start();
    if (label)
      label->setText(mode == "edit" ? QObject::tr("Live automation: Edit")
                                    : QObject::tr("Live automation: Inspect"));
    if (action) action->setEnabled(true);
    observer->start();
  }

  void stop() {
    service.stop();
    if (observer) observer->stop();
    showStopped();
  }

  QString mode;
  Service service;
  QPointer<QLabel> label;
  QPointer<QToolButton> button;
  QPointer<QAction> action;
  QPointer<QTimer> observer;
};

LiveUi::LiveUi(QApplication& application, QMainWindow& window, Options options)
    : m_impl(std::make_unique<Impl>(application, window, std::move(options))) {}

LiveUi::~LiveUi() = default;

void LiveUi::start() { m_impl->start(); }

void LiveUi::stop() { m_impl->stop(); }

bool LiveUi::running() const { return m_impl->service.running(); }

}  // namespace Live
}  // namespace GroundzeroAutomation
