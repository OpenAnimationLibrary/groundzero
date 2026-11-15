#include "uimemoryintegrationtest.h"

#include "custompanelmanager.h"
#include "histogrampopup.h"
#include "imageviewer.h"
#include "mainwindow.h"
#include "menubarcommandids.h"
#include "sceneviewer.h"
#include "tenv.h"
#ifndef _WIN32
#include "tsoundownershiptest.h"
#endif
#include "toonz/preferences.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/lutcalibrator.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/styleeditor.h"
#include "toonzqt/updatechecker.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLShader>
#include <QOpenGLShaderProgram>
#include <QPointer>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void writeFile(const QString& path, const QByteArray& contents) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly),
          "UI memory fixture could not open its input file");
  require(file.write(contents) == contents.size(),
          "UI memory fixture could not write its input file");
}

std::vector<QPointer<QMenu>> subMenus(QMenu& menu) {
  std::vector<QPointer<QMenu>> result;
  for (QMenu* child : menu.findChildren<QMenu*>()) result.push_back(child);
  return result;
}

void checkMenuCycles(QMenu& menu, const QString& command) {
  auto* manager = CustomPanelManager::instance();
  manager->loadCustomPanelEntries();
  QPointer<QAction> registered =
      CommandManager::instance()->getAction(command.toStdString().c_str());
  require(registered, "nested custom panel command was not registered");
  const int directCount =
      menu.findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly).size();
  const size_t count = subMenus(menu).size();
  require(count >= 2,
          "nested custom panel fixture created no submenu hierarchy");
  for (int cycle = 0; cycle < 5; ++cycle) {
    const auto previous = subMenus(menu);
    manager->loadCustomPanelEntries();
    for (const auto& retired : previous)
      require(retired.isNull(), "custom panel rebuild retained an old submenu");
    require(subMenus(menu).size() == count &&
                menu.findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly)
                        .size() == directCount,
            "custom panel submenu count grew after an identical rebuild");
    require(
        registered && CommandManager::instance()->getAction(
                          command.toStdString().c_str()) == registered.data(),
        "custom panel menu rebuild replaced its registered command action");
  }
}

void checkNestedMenus() {
  const QString root =
      (ToonzFolder::getMyModuleDir() + "custompanels").getQString();
  require(QDir().mkpath(root),
          "UI memory fixture has no custom panel directory");
  QTemporaryDir directory(QDir(root).filePath("native-memory-XXXXXX"));
  require(directory.isValid(), "UI memory fixture has no temporary directory");
  require(QDir().mkpath(directory.filePath("nested")),
          "UI memory fixture could not create its nested panel directory");
  writeFile(directory.filePath("nested/panel.ui"),
            "<ui version=\"4.0\"><class>Fixture</class>"
            "<widget class=\"QWidget\" name=\"Fixture\"/></ui>");
  const QString command = "MI_CustomPanel_" +
                          QFileInfo(directory.path()).fileName() +
                          "__nested__panel";
  auto* commands        = CommandManager::instance();
  require(!commands->getAction(command.toStdString().c_str()),
          "UI memory fixture command unexpectedly already exists");
  QAction* action = commands->getAction(MI_OpenCustomPanels);
  require(action && action->menu(), "native custom panel menu is unavailable");
  auto cleanup = [&]() {
    const bool removed = directory.remove();
    commands->setHandler(command.toStdString().c_str(), nullptr);
    delete commands->getAction(command.toStdString().c_str());
    CustomPanelManager::instance()->loadCustomPanelEntries();
    return removed;
  };
  try {
    checkMenuCycles(*action->menu(), command);
  } catch (...) {
    if (!cleanup())
      std::cerr << "UI memory fixture could not remove its temporary menu files"
                << std::endl;
    throw;
  }
  require(cleanup(),
          "UI memory fixture could not remove its temporary menu files");
}

void checkAboutCycles(MainWindow& window) {
  QAction* about = CommandManager::instance()->getAction(MI_About);
  require(about, "native About command is unavailable");
  const int originalCount = window.findChildren<QDialog*>().size();
  for (int cycle = 0; cycle < 5; ++cycle) {
    bool shown = false;
    QPointer<QDialog> dialog;
    std::vector<QPointer<QObject>> contents;
    QTimer response;
    QObject::connect(&response, &QTimer::timeout, &response, [&]() {
      auto* active = qobject_cast<QDialog*>(QApplication::activeModalWidget());
      if (!active) return;
      shown  = active->windowTitle() == MainWindow::tr("About Groundzero");
      dialog = active;
      for (QObject* child : active->findChildren<QObject*>())
        contents.push_back(child);
      response.stop();
      active->accept();
    });
    response.start(10);
    about->trigger();
    response.stop();
    const bool retired = dialog.isNull();
    if (shown && dialog) delete dialog.data();
    require(shown && retired, "closed About dialog remained alive");
    for (const auto& child : contents)
      require(child.isNull(), "closed About dialog retained a child object");
    require(window.findChildren<QDialog*>().size() == originalCount,
            "About dialog count grew after closing it");
  }
}

void checkUpdateCompletion(MainWindow& window, const QString& path,
                           bool expectedError) {
  QPointer<UpdateChecker> checker =
      new UpdateChecker(QUrl::fromLocalFile(path));
  struct Cleanup {
    QPointer<UpdateChecker> checker;
    ~Cleanup() { delete checker.data(); }
  } cleanup{checker};
  checker->setParent(&window);
  QPointer<QNetworkAccessManager> manager =
      checker->findChild<QNetworkAccessManager*>();
  require(manager, "local update checker has no owned network manager");
  bool completed = false;
  bool error     = !expectedError;
  QObject callbacks;
  QObject::connect(checker.data(), &UpdateChecker::done, &callbacks,
                   [&](bool failed) {
                     completed = true;
                     error     = failed;
                   });
  const auto connection =
      QObject::connect(checker.data(), SIGNAL(done(bool)), &window,
                       SLOT(onUpdateCheckerDone(bool)));
  require(static_cast<bool>(connection),
          "native update completion slot could not be connected");
  QElapsedTimer deadline;
  deadline.start();
  while ((!completed || checker) && deadline.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
  const bool retired = checker.isNull() && manager.isNull();
  require(completed && error == expectedError,
          "local update checker did not report the expected completion");
  require(retired, "completed update checker retained its network manager");
}

void checkOverlappingUpdates(MainWindow& window, const QString& path) {
  struct Cleanup {
    std::vector<QPointer<UpdateChecker>> checkers;
    ~Cleanup() {
      for (const auto& checker : checkers) delete checker.data();
    }
  } cleanup;
  std::vector<QPointer<QNetworkAccessManager>> managers;
  int completed          = 0;
  bool unexpectedSuccess = false;
  QObject callbacks;
  for (int i = 0; i < 2; ++i) {
    auto* checker = new UpdateChecker(QUrl::fromLocalFile(path));
    checker->setParent(&window);
    cleanup.checkers.push_back(checker);
    auto* manager = checker->findChild<QNetworkAccessManager*>();
    require(manager, "overlapping update checker has no network manager");
    managers.push_back(manager);
    QObject::connect(checker, &UpdateChecker::done, &callbacks,
                     [&](bool error) {
                       ++completed;
                       unexpectedSuccess |= !error;
                     });
    const auto connection = QObject::connect(
        checker, SIGNAL(done(bool)), &window, SLOT(onUpdateCheckerDone(bool)));
    require(static_cast<bool>(connection),
            "overlapping update completion slot could not be connected");
  }
  QElapsedTimer deadline;
  deadline.start();
  while (completed < 2 && deadline.elapsed() < 5000)
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  require(completed == 2 && !unexpectedSuccess,
          "overlapping local update checks did not complete independently");
  for (const auto& checker : cleanup.checkers)
    require(checker.isNull(),
            "overlapping update completion retained a checker");
  for (const auto& manager : managers)
    require(manager.isNull(),
            "overlapping update completion retained a manager");
}

void checkUpdateCycles(MainWindow& window) {
  QTemporaryDir directory;
  require(directory.isValid(),
          "UI memory fixture has no update input directory");
  writeFile(directory.filePath("malformed.txt"), "not a version");
  writeFile(directory.filePath("valid.txt"), "0.0.0");
  for (int cycle = 0; cycle < 5; ++cycle) {
    checkUpdateCompletion(window, directory.filePath("missing.txt"), true);
    checkUpdateCompletion(window, directory.filePath("malformed.txt"), true);
    checkUpdateCompletion(window, directory.filePath("valid.txt"), false);
    checkOverlappingUpdates(window, directory.filePath("missing.txt"));
  }
}

QSet<HistogramPopup*> histograms() {
  QSet<HistogramPopup*> result;
  for (QWidget* widget : QApplication::allWidgets()) {
    auto* histogram = qobject_cast<HistogramPopup*>(widget);
    if (histogram) result.insert(histogram);
  }
  return result;
}

void checkHistogramCycles() {
  const auto baseline = histograms();
  for (int cycle = 0; cycle < 8; ++cycle) {
    for (bool enabledAtCreation : {false, true}) {
      auto viewer =
          std::make_unique<ImageViewer>(nullptr, nullptr, enabledAtCreation);
      if (!enabledAtCreation) viewer->setHistogramEnable(true);
      const auto added = histograms() - baseline;
      require(added.size() == 1,
              "image viewer did not create exactly one histogram");
      QPointer<HistogramPopup> histogram = *added.begin();
      viewer.reset();
      const bool retired = histogram.isNull();
      delete histogram.data();
      require(retired && histograms() == baseline,
              "destroyed image viewer retained its histogram");
    }
  }
}

}  // namespace

class LutCalibratorLifetimeTest {
  static void checkWidget(std::unique_ptr<QWidget> widget,
                          const QSet<LutCalibrator*>& baseline) {
    const auto& registrations = LutManager::instance()->m_calibrators;
    require(registrations.size() == baseline.size() + 1,
            "calibrated widget did not register its calibrator");
    widget.reset();
    require(registrations == baseline,
            "destroyed calibrated widget retained its calibrator");
  }

  static void checkWidgetCycles() {
    auto* preferences = Preferences::instance();
    struct RestorePreference {
      bool enabled;
      ~RestorePreference() {
        Preferences::instance()->setValue(colorCalibrationEnabled, enabled,
                                          false);
      }
    } restore{preferences->isColorCalibrationEnabled()};
    const auto baseline = LutManager::instance()->m_calibrators;
    preferences->setValue(colorCalibrationEnabled, true, false);
    for (int cycle = 0; cycle < 8; ++cycle) {
      checkWidget(
          std::make_unique<StyleEditorGUI::HexagonalColorWheel>(nullptr),
          baseline);
      checkWidget(std::make_unique<ImageViewer>(nullptr, nullptr, false),
                  baseline);
      checkWidget(std::make_unique<SceneViewer>(nullptr), baseline);
    }
  }

  static void checkPartialCleanup() {
    QPointer<QOpenGLContext> previous = QOpenGLContext::currentContext();
    QSurface* previousSurface = previous ? previous->surface() : nullptr;
    QOpenGLContext context;
    context.setFormat(QSurfaceFormat::defaultFormat());
    require(context.create(),
            "calibration cleanup fixture could not create an OpenGL context");
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    require(surface.isValid() && context.makeCurrent(&surface),
            "calibration cleanup fixture could not make its context current");
    struct RestoreContext {
      QOpenGLContext& context;
      QPointer<QOpenGLContext> previous;
      QSurface* surface;
      ~RestoreContext() {
        context.doneCurrent();
        if (previous && previous->isValid() && surface)
          previous->makeCurrent(surface);
      }
    } restore{context, previous, previousSurface};
    for (int cycle = 0; cycle < 8; ++cycle) {
      LutCalibrator calibrator;
      QPointer<QOpenGLShader> vertex = new QOpenGLShader(QOpenGLShader::Vertex);
      QPointer<QOpenGLShader> fragment =
          new QOpenGLShader(QOpenGLShader::Fragment);
      QPointer<QOpenGLShaderProgram> program = new QOpenGLShaderProgram();
      calibrator.m_shader.vert               = vertex;
      calibrator.m_shader.frag               = fragment;
      calibrator.m_shader.program            = program;
      struct Cleanup {
        LutCalibrator& calibrator;
        QPointer<QOpenGLShader> vertex;
        QPointer<QOpenGLShader> fragment;
        QPointer<QOpenGLShaderProgram> program;
        ~Cleanup() {
          calibrator.cleanup();
          delete program.data();
          delete vertex.data();
          delete fragment.data();
        }
      } cleanup{calibrator, vertex, fragment, program};
      require(
          program->create(),
          "calibration cleanup fixture could not create its shader program");
      require(!calibrator.isValid(),
              "partial calibration fixture unexpectedly became valid");
      calibrator.cleanup();
      calibrator.cleanup();
      require(vertex.isNull() && fragment.isNull() && program.isNull(),
              "partial calibration cleanup retained shader objects");
      require(!calibrator.isValid() && !calibrator.isInitialized(),
              "calibration cleanup retained initialized state");
    }
  }

public:
  static void check() {
    checkWidgetCycles();
    checkPartialCleanup();
  }
};

void exerciseUiMemoryOwnership(QMainWindow* window) {
  auto* main = qobject_cast<MainWindow*>(window);
  require(main && TEnv::getIsPortable(),
          "UI memory fixture requires an isolated native window");
  require(!QApplication::activeModalWidget(),
          "UI memory fixture started with an unexpected modal");
  checkNestedMenus();
  checkAboutCycles(*main);
  checkUpdateCycles(*main);
#ifndef _WIN32
  QString soundFailure;
  if (!runSoundOutputOwnershipChecks(soundFailure))
    throw std::runtime_error(soundFailure.toStdString());
#endif
  checkHistogramCycles();
  LutCalibratorLifetimeTest::check();
  std::cerr << "native-ui-integration: checkpoint: UI repeated ownership "
               "checks passed"
            << std::endl;
}
