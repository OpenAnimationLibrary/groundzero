#include "iconlifecycleintegrationtest.h"
#include "nativeuiintegrationtest.h"
#include "tapp.h"
#include "iocommand.h"

#include "menubarcommandids.h"
#include "toonzqt/menubarcommand.h"
#include "toonz/onionskinmask.h"
#include "toonz/levelset.h"
#include "toonz/levelproperties.h"
#include "toonz/stage.h"
#include "toonz/stage2.h"
#include "toonz/stageplayer.h"
#include "toonz/stagevisitor.h"
#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonzqt/icongenerator.h"
#include "tstroke.h"
#include "tpalette.h"
#include "tvectorimage.h"
#include "texception.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QThreadStorage>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

class RecordingVisitor final : public Stage::Visitor {
public:
  std::set<int> columns;
  explicit RecordingVisitor(const ImagePainter::VisualSettings &settings)
      : Stage::Visitor(settings) {}
  void onImage(const Stage::Player &player) override {
    columns.insert(player.m_column);
  }
  void onRasterImage(TRasterImage *, const Stage::Player &player) override {
    onImage(player);
  }
  void enableMask() override {}
  void disableMask() override {}
  void beginMask() override {}
  void endMask() override {}
};
}  // namespace

namespace {
void waitUntil(const std::function<bool()> &ready, const char *message) {
  QElapsedTimer deadline;
  deadline.start();
  while (!ready() && deadline.elapsed() < 10000)
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
  require(ready(), message);
}

struct TaskGate {
  std::mutex mutex;
  std::condition_variable condition;
  quint64 target = 0;
  bool armed = false, entered = false, released = false, expired = false;
  bool throwOnEntry = false;
  std::set<quint64> enteredTokens;

  void arm(quint64 token) {
    std::lock_guard<std::mutex> lock(mutex);
    target = token;
    armed = true;
    condition.notify_all();
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    armed = released = true;
    condition.notify_all();
  }
  void run(quint64 token) {
    std::unique_lock<std::mutex> lock(mutex);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    if (!condition.wait_until(lock, deadline, [&]() { return armed; })) {
      expired = true;
      throw std::runtime_error("Icon task test was never armed");
    }
    enteredTokens.insert(token);
    if (!target) target = token;
    if (token != target) return;
    entered = true;
    if (throwOnEntry) throw std::runtime_error("Injected native icon task failure");
    if (!condition.wait_until(lock, deadline, [&]() { return released; })) {
      expired = true;
      throw std::runtime_error("Icon drain did not service the GUI release event");
    }
  }
  bool hasEntered() {
    std::lock_guard<std::mutex> lock(mutex);
    return entered;
  }
  quint64 enteredToken() {
    std::lock_guard<std::mutex> lock(mutex);
    return target;
  }
};

struct ReleaseGate {
  std::shared_ptr<TaskGate> gate;
  ~ReleaseGate() { gate->release(); }
};

struct ThreadLifetime {
  std::atomic<int> created{0}, destroyed{0};
};
struct ThreadSentinel {
  std::shared_ptr<ThreadLifetime> state;
  explicit ThreadSentinel(std::shared_ptr<ThreadLifetime> value) : state(std::move(value)) {
    ++state->created;
  }
  ~ThreadSentinel() { ++state->destroyed; }
};

TThread::RunnableP taskFor(const std::vector<TThread::RunnableP> &before,
                         const std::vector<TThread::RunnableP> &after) {
  for (const auto &task : after)
    if (std::find(before.begin(), before.end(), task) == before.end()) return task;
  throw std::runtime_error("Native icon request did not retain its actual task");
}

void expireSceneRetirements(TSceneHandle *handle) {
  // Deliver the actual retirement timer path deterministically, without a
  // three-second sleep or bypassing its destruction/generation semantics.
  const auto timers = handle->findChildren<QTimer *>(
      "sceneRetirement", Qt::FindDirectChildrenOnly);
  for (auto *timer : timers) {
    QPointer<QTimer> live = timer;
    require(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
            "Cannot deliver native scene retirement timer");
    if (live) QCoreApplication::sendPostedEvents(live, QEvent::DeferredDelete);
    require(!live, "Native scene retirement timer did not finish");
  }
}

void sceneRetentionChecks() {
  QPointer<TXshLevel> retiredLevel, currentLevel;
  std::shared_ptr<void> pin;
  ToonzScene *current = nullptr;
  {
    auto handle = std::unique_ptr<TSceneHandle>(new TSceneHandle);
    auto *retired = new ToonzScene;
    retiredLevel = retired->createNewLevel(PLI_XSHLEVEL, L"Retained source");
    handle->setScene(retired);
    pin = handle->retainScene();
    current = new ToonzScene;
    currentLevel = current->createNewLevel(PLI_XSHLEVEL, L"Current source");
    handle->setScene(current);
    expireSceneRetirements(handle.get());
    require(retiredLevel && retired->getLevelSet()->getLevelCount() == 1,
            "Retirement deleted a scene while a reader retained it");
    handle.reset();
    require(retiredLevel && currentLevel,
            "Destroying the handle invalidated retained/current scene ownership");
  }
  pin.reset();
  require(!retiredLevel && currentLevel,
          "Releasing a retained scene after handle destruction lost ownership");
  delete current;
  require(!currentLevel, "Current scene ownership was unexpectedly transferred");

  // A final worker release is queued to the GUI. Reinstating the source before
  // that delivery must find the same authoritative retention record.
  ToonzScene stackScene;
  QPointer<TXshLevel> stackLevel =
      stackScene.createNewLevel(PLI_XSHLEVEL, L"Stack scene remains owned");
  {
    TSceneHandle handle;
    handle.setScene(&stackScene);
    auto workerPin = handle.retainScene();
    auto *other = new ToonzScene;
    QPointer<TXshLevel> otherLevel = other->createNewLevel(PLI_XSHLEVEL, L"Other source");
    handle.setScene(other);
    expireSceneRetirements(&handle);
    std::thread worker([lease = std::move(workerPin)]() mutable { lease.reset(); });
    worker.join();
    handle.setScene(&stackScene);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    expireSceneRetirements(&handle);
    require(stackLevel && !otherLevel && handle.getScene() == &stackScene,
            "Queued reader release or stale retirement deleted a reinstated scene");
  }
  require(stackLevel && stackScene.getLevelSet()->getLevelCount() == 1,
          "Handle destruction deleted its non-owned current stack scene");
  std::cout << "icon-lifecycle-native: scene retention and reinstatement PASS" << std::endl;
}

bool closeThroughNativePrompt(QMainWindow *window, const QString &choice,
                              const char *phase) {
  QObject context;
  QTimer poll, deadline;
  bool queued = false, clicked = false;
  bool dispatched = false, returned = false, closed = false;
  std::exception_ptr closeFailure;
  QString failure;
  std::cout << "icon-lifecycle-native: Quit " << phase << " begin" << std::endl;
  QObject::connect(&poll, &QTimer::timeout, &context, [&]() {
    if (queued || clicked || !failure.isEmpty()) return;
    queued = true;
    QTimer::singleShot(0, &context, [&]() {
      queued = false;
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog) return;
      bool scenePrompt = false;
      for (auto *label : dialog->findChildren<QLabel *>())
        scenePrompt |= label->text().contains("the current scene has been modified.");
      if (!scenePrompt) {
        failure = "Unexpected dialog during native Quit: " +
            QString::fromLatin1(dialog->metaObject()->className()).left(100);
        dialog->reject();
        return;
      }
      for (auto *button : dialog->findChildren<QPushButton *>())
        if (button->isVisible() && button->isEnabled() &&
            QString(button->text()).remove('&') == choice) {
          clicked = true;
          button->click();
          return;
        }
    });
  });
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &context, [&]() {
    failure = "Native Quit did not expose the required scene decision";
    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
      dialog->reject();
  });
  poll.start(1);
  deadline.start(10000);
  // This fixture runs after the application's original exec() has returned.
  // An accepted real close calls qApp->quit(), setting Qt's thread quit flag.
  // A second QDialog::exec() outside an application exec boundary would then
  // return immediately without servicing its decision button. Exercise every
  // real close inside a fresh public application loop so Qt performs its own
  // entry/exit cleanup; never reset private Qt state or bypass closeEvent().
  QTimer::singleShot(0, &context, [&]() {
    dispatched = true;
    try {
      closed = window->close();
      returned = true;
    } catch (...) {
      closeFailure = std::current_exception();
    }
    // Cancel leaves the window open and does not quit. Finish only this test
    // loop after the actual close call returns; accepted Quit has already
    // performed the product's own terminal shutdown and quit request.
    QApplication::exit(0);
  });
  const int loopResult = QApplication::exec();
  poll.stop();
  deadline.stop();
  if (closeFailure) {
    try {
      std::rethrow_exception(closeFailure);
    } catch (const TException &error) {
      const QString detail = QString::fromStdWString(error.getMessage()).left(1024)
                                 .replace('\n', ' ').replace('\r', ' ');
      throw std::runtime_error((QString("Native Quit %1 threw a Toonz exception: %2")
          .arg(QString::fromLatin1(phase), detail)).toStdString());
    } catch (const std::exception &error) {
      const QString detail = QString::fromUtf8(error.what()).left(1024)
                                 .replace('\n', ' ').replace('\r', ' ');
      throw std::runtime_error((QString("Native Quit %1 threw a standard exception: %2")
          .arg(QString::fromLatin1(phase), detail)).toStdString());
    } catch (...) {
      throw std::runtime_error((QString("Native Quit %1 threw an unknown exception")
          .arg(QString::fromLatin1(phase))).toStdString());
    }
  }
  require(loopResult == 0 && dispatched && returned,
          "Native Quit application loop did not dispatch and complete its actual close");
  require(failure.isEmpty(), qPrintable(failure));
  require(clicked, "Native Quit did not deliver its actual decision button");
  std::cout << "icon-lifecycle-native: Quit " << phase
            << " decision returned closed=" << closed << std::endl;
  return closed;
}

using Completions = std::map<quint64, std::vector<bool>>;

TXshSimpleLevelP makeTransitionLevel(ToonzScene *scene, const std::wstring &name) {
  TXshSimpleLevelP level = scene->createNewLevel(PLI_XSHLEVEL, name)->getSimpleLevel();
  require(level, "Cannot create native scene-transition vector level");
  for (int frame = 1; frame <= 3; ++frame) {
    TVectorImageP image = new TVectorImage;
    image->setPalette(level->getPalette());
    auto *stroke = new TStroke(std::vector<TThickPoint>{
        TThickPoint(-30, -10, 6), TThickPoint(0, 20, 6), TThickPoint(30, -10, 6)});
    stroke->setStyle(1);
    image->addStroke(stroke);
    level->setFrame(TFrameId(frame), image);
  }
  require(scene->getXsheet()->setCell(0, 0, TXshCell(level.getPointer(), TFrameId(1))),
          "Cannot expose native scene-transition artwork");
  scene->getCurrentCamera()->setRes(TDimension(80, 60));
  scene->getCurrentCamera()->setSize(TDimensionD(2.0, 1.5));
  level->getProperties()->setDirtyFlag(false);
  level->getPalette()->setDirtyFlag(false);
  return level;
}

bool hasInk(const QImage &image) {
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) {
      const QRgb pixel = image.pixel(x, y);
      if (qAlpha(pixel) && qRed(pixel) < 200 && qGreen(pixel) < 200 && qBlue(pixel) < 200)
        return true;
    }
  return false;
}

void successfulIcon(IconGenerator *icons, TXshSimpleLevel *level, Completions &completions) {
  icons->remove(level, TFrameId(1));
  const quint64 token = icons->requestIcon(level, TFrameId(1));
  require(token, "Scene transition did not reopen native icon admission");
  waitUntil([&]() { return completions.count(token) != 0; },
            "Native icon generation did not finish after the scene decision");
  require(completions[token] == std::vector<bool>{true} &&
              hasInk(icons->getIcon(level, TFrameId(1), true, true).toImage()),
          "Scene transition did not resume visible native icon generation exactly once");
}

QByteArray fileBytes(const TFilePath &path) {
  QFile file(QString::fromStdWString(path.getWideString()));
  const bool opened = file.open(QIODevice::ReadOnly);
  require(opened, qPrintable(QString("Cannot read native scene-transition fixture file: %1 (%2)")
      .arg(file.fileName().left(512), file.errorString().left(160))));
  const QByteArray bytes = file.readAll();
  require(file.error() == QFileDevice::NoError,
          qPrintable(QString("Native scene-transition fixture read failed: %1 (%2)")
              .arg(file.fileName().left(512), file.errorString().left(160))));
  return bytes;
}

void sceneDecision(const QString &operation, const QString &choice,
                   const std::function<void()> &run,
                   const std::function<void()> &beforeChoice) {
  QObject context;
  QTimer poll, deadline;
  bool queued = false, clicked = false;
  QString failure;
  QObject::connect(&poll, &QTimer::timeout, &context, [&]() {
    if (queued || clicked || !failure.isEmpty()) return;
    queued = true;
    QTimer::singleShot(0, &context, [&]() {
      queued = false;
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog) return;
      try {
        bool expected = false;
        for (auto *label : dialog->findChildren<QLabel *>())
          expected |= label->text().contains(operation) &&
                      label->text().contains("the current scene has been modified.");
        require(expected, "Unexpected native scene-transition decision dialog");
        for (auto *button : dialog->findChildren<QPushButton *>())
          if (button->isVisible() && button->isEnabled() &&
              QString(button->text()).remove('&') == choice) {
            beforeChoice();
            clicked = true;
            button->click();
            return;
          }
      } catch (const std::exception &error) {
        failure = QString::fromUtf8(error.what());
        dialog->reject();
      } catch (...) {
        failure = "Unexpected native scene-transition decision callback exception";
        dialog->reject();
      }
    });
  });
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &context, [&]() {
    failure = "Native " + operation + " did not finish its required " + choice + " decision";
    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
  });
  poll.start(1);
  deadline.start(10000);
  run();
  require(failure.isEmpty(), qPrintable(failure));
  require(clicked, "Native scene transition did not deliver the actual decision button");
}

void sceneTransitionChecks() {
  auto *app = TApp::instance();
  auto *icons = IconGenerator::instance();
  const auto project = TProjectManager::instance()->getCurrentProject();
  require(bool(project), "Native scene-transition fixture has no current project");
  const QString scenesFolder = QString::fromStdWString(project->getFolder("scenes", true).getWideString());
  require(!scenesFolder.isEmpty() && QDir().mkpath(scenesFolder),
          "Cannot prepare native scene-transition project folder");
  QTemporaryDir directory(QDir(scenesFolder).filePath("icon-transition-XXXXXX"));
  require(directory.isValid(), "Cannot prepare native scene-transition source directory");
  const TFilePath loadPath(directory.filePath("load-source.tnz").toStdWString());
  const TFilePath drawingPath(directory.filePath("load-source.pli").toStdWString());
  {
    ToonzScene saved;
    saved.setProject(project);
    saved.setScenePath(loadPath);
    auto level = makeTransitionLevel(&saved, L"Loaded icon source");
    level->setPath(drawingPath, true);
    // makeTransitionLevel clears dirty flags for the later modal checks. This
    // newly authored saved input must explicitly enter the native writer;
    // saveSimpleLevel intentionally skips clean drawing/palette data.
    level->getProperties()->setDirtyFlag(true);
    level->getPalette()->setDirtyFlag(true);
    level->save(drawingPath);
    saved.saveCopy(loadPath);

    require(!fileBytes(drawingPath).isEmpty() && !fileBytes(loadPath).isEmpty(),
            "Native scene-transition save did not materialize both input files");
    ToonzScene reopened;
    reopened.setProject(project);
    reopened.load(loadPath);
    auto *reopenedBase = reopened.getLevelSet()->getLevel(L"Loaded icon source");
    auto *reopenedLevel = reopenedBase ? reopenedBase->getSimpleLevel() : nullptr;
    require(reopenedLevel && reopened.getLevelSet()->getLevelCount() == 1 &&
                reopenedLevel->getFrameCount() == 3 &&
                reopened.getXsheet()->getCell(0, 0) == TXshCell(reopenedLevel, TFrameId(1)) &&
                reopened.getCurrentCamera()->getRes() == TDimension(80, 60),
            "Native scene-transition saved scene did not reopen its level/exposure/camera");
    for (int frame = 1; frame <= 3; ++frame) {
      const TVectorImageP original = level->getFrame(TFrameId(frame), false);
      const TVectorImageP image = reopenedLevel->getFrame(TFrameId(frame), false);
      require(original && image && image->getStrokeCount() == 1 &&
                  image->getPalette() == reopenedLevel->getPalette() &&
                  image->getStroke(0)->getStyle() == original->getStroke(0)->getStyle() &&
                  image->getStroke(0)->getControlPointCount() == 3,
              "Native scene-transition PLI did not reopen its authored drawing");
      for (int point = 0; point < 3; ++point)
        require(image->getStroke(0)->getControlPoint(point) ==
                    original->getStroke(0)->getControlPoint(point),
                "Native scene-transition PLI changed authored control points");
    }
  }
  const auto loadProject = TProjectManager::instance()->loadSceneProject(loadPath);
  require(loadProject && loadProject->isCurrent(),
          "Native scene-transition source would require an unrelated project decision");
  const QByteArray savedScene = fileBytes(loadPath), savedDrawing = fileBytes(drawingPath);
  require(!savedScene.isEmpty() && !savedDrawing.isEmpty(),
          "Native scene-transition source files were empty");

  enum Operation { NewScene, CheckedLoad, UncheckedLoad };
  const std::vector<std::pair<Operation, bool>> cases{
      {NewScene, false}, {CheckedLoad, false}, {NewScene, true},
      {CheckedLoad, true}, {UncheckedLoad, true}};
  // QThreadStorage owners must outlive the dedicated workers that use them,
  // including the worker intentionally left running after a canceled decision.
  std::vector<std::shared_ptr<QThreadStorage<ThreadSentinel *>>> retainedStorages;
  std::vector<std::shared_ptr<ThreadLifetime>> retainedLifetimes;
  struct FailureDrain {
    IconGenerator *icons;
    bool complete = false;
    ~FailureDrain() {
      if (!complete) {
        icons->setTaskTestHook({});
        icons->shutdown();
      }
    }
  } failureDrain{icons};
  for (const auto &test : cases) {
    const Operation operation = test.first;
    const bool accepted = test.second;
    const QString name = operation == NewScene ? "New Scene" : "Load Scene";
    std::cout << "icon-lifecycle-native: " << name.toStdString() << ' '
              << (operation == UncheckedLoad ? "unchecked" : accepted ? "accepted" : "Cancel")
              << std::endl;
    icons->setTaskTestHook({});
    icons->clearRequests();
    waitUntil([&]() { return icons->lifecycleTestTasks().empty(); },
              "Earlier icons did not finish before a scene-transition case");
    QString resetError;
    const bool reset = resetNativeUiIntegrationScene(&resetError);
    require(reset, qPrintable(resetError));
    auto *source = app->getCurrentScene()->getScene();
    const auto sourcePin = app->getCurrentScene()->retainScene();
    auto level = makeTransitionLevel(source, L"Active transition source");
    const auto sourceImage = level->getFrame(TFrameId(1), false);
    const TFilePath sourcePath = source->getScenePath();
    const int history = TUndoManager::manager()->getHistoryCount();
    const int historyIndex = TUndoManager::manager()->getCurrentHistoryIndex();
    app->getCurrentScene()->setDirtyFlag(true);
    auto sourceUnchanged = [&]() {
      return app->getCurrentScene()->getScene() == source &&
          source->getScenePath() == sourcePath && source->getLevelSet()->getLevelCount() == 1 &&
          source->getLevelSet()->getLevel(level->getName()) == level.getPointer() &&
          source->getXsheet()->getCell(0, 0) == TXshCell(level.getPointer(), TFrameId(1)) &&
          level->getFrameCount() == 3 && level->getFrame(TFrameId(1), false) == sourceImage &&
          app->getCurrentScene()->getDirtyFlag() &&
          TUndoManager::manager()->getHistoryCount() == history &&
          TUndoManager::manager()->getCurrentHistoryIndex() == historyIndex;
    };
    Completions completions;
    int pausedPublications = 0, resumedRefreshes = 0;
    bool commandReturned = false;
    QObject context;
    QObject::connect(icons, &IconGenerator::iconRequestFinished, &context,
                     [&](quint64 token, bool generated) { completions[token].push_back(generated); });
    QObject::connect(icons, &IconGenerator::iconGenerated, &context, [&]() {
      if (icons->lifecycleTestPaused()) ++pausedPublications;
      else if (commandReturned && !IoCmd::isSceneTransitionInProgress()) ++resumedRefreshes;
    });
    const auto gate = std::make_shared<TaskGate>();
    const auto lifetime = std::make_shared<ThreadLifetime>();
    const auto storage = std::make_shared<QThreadStorage<ThreadSentinel *>>();
    retainedStorages.push_back(storage);
    retainedLifetimes.push_back(lifetime);
    ReleaseGate release{gate};
    gate->arm(0);
    icons->setTaskTestHook([gate, lifetime, storage](quint64 token) {
      if (!storage->hasLocalData()) storage->setLocalData(new ThreadSentinel(lifetime));
      gate->run(token);
    });
    icons->removeXsheetFrameIcons(source->getXsheet());
    icons->getXsheetFrameIcon(source->getXsheet(), 0, TDimension(80, 60), false);
    waitUntil([&]() { return gate->hasEntered(); },
              "Native scene-transition Xsheet renderer did not enter its worker");
    const quint64 active = gate->enteredToken();
    icons->remove(level.getPointer(), TFrameId(2));
    const quint64 queued = icons->requestIcon(level.getPointer(), TFrameId(2));
    require(active && queued && active != queued && icons->lifecycleTestTasks().size() >= 2,
            "Native scene transition did not retain active and queued renderers");
    auto refusedNestedTransitions = [&]() {
      IoCmd::newScene();
      const bool checked = IoCmd::loadScene(loadPath, false, true);
      const bool unchecked = IoCmd::loadScene(loadPath, false, false);
      const bool direct = IoCmd::saveSceneIfNeeded("Queued Change Project");
      QCloseEvent close;
      QApplication::sendEvent(app->getMainWindow(), &close);
      require(IoCmd::isSceneTransitionInProgress() && !checked && !unchecked && !direct &&
                  !close.isAccepted() && sourceUnchanged(),
              "Nested scene operation mutated a source still owned by its outer decision");
    };
    QString callbackFailure;
    bool pauseObserved = false, watchdogExpired = false;
    QTimer paused, watchdog;
    QObject::connect(&paused, &QTimer::timeout, &context, [&]() {
      if (!icons->lifecycleTestPaused() || pauseObserved) return;
      pauseObserved = true;
      try {
        require(accepted && sourceUnchanged() && !icons->isShutdownComplete() &&
                    lifetime->created.load() == 1 && !lifetime->destroyed.load(),
                "Native scene transition paused after clearing its active renderer source");
        refusedNestedTransitions();
        require(!icons->pauseForSceneChange(),
                "Reentrant native pause reported quiescence while its worker was still active");
        require(icons->requestIcon(level.getPointer(), TFrameId(3)) == 0,
                "Paused native scene transition admitted another icon request");
      } catch (const std::exception &error) {
        callbackFailure = QString::fromUtf8(error.what());
      } catch (...) {
        callbackFailure = "Unexpected native scene-transition pause callback exception";
      }
      // Admitted renderers already own their copied hook. The resumed executor
      // may receive a normal GUI refresh before this command returns.
      icons->setTaskTestHook({});
      gate->release();
    });
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &context, [&]() {
      watchdogExpired = true;
      gate->release();
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
    });
    paused.start(1);
    watchdog.start(10000);
    bool loaded = false;
    auto run = [&]() {
      if (operation == NewScene) IoCmd::newScene();
      else loaded = IoCmd::loadScene(loadPath, false, operation == CheckedLoad);
    };
    if (operation == UncheckedLoad) run();
    else sceneDecision(name, accepted ? "Discard Changes" : "Cancel", run, [&]() {
      require(!icons->lifecycleTestPaused() && !icons->isShutdownComplete() &&
                  sourceUnchanged() && !completions.count(active) &&
                  !completions.count(queued) && !lifetime->destroyed.load(),
              "A pending scene decision prematurely retired its active native worker");
      refusedNestedTransitions();
    });
    commandReturned = true;
    paused.stop();
    watchdog.stop();
    require(callbackFailure.isEmpty(), qPrintable(callbackFailure));
    require(!watchdogExpired && !icons->lifecycleTestPaused() && !icons->isShutdownComplete() &&
                !IoCmd::isSceneTransitionInProgress() && pausedPublications == 0,
            "Native scene transition failed to restore admission or published while paused");
    if (!accepted) {
      require(!pauseObserved && !loaded && sourceUnchanged() &&
                  !completions.count(active) && !completions.count(queued) &&
                  !lifetime->destroyed.load(),
              "Canceling a native scene transition changed its source or retired its worker");
      gate->release();
      waitUntil([&]() { return completions.count(active) && completions.count(queued); },
                "Canceled scene transition did not retain its admitted icon work");
      require(completions[active] == std::vector<bool>{true} &&
                  completions[queued] == std::vector<bool>{true},
              "Canceled scene transition lost or duplicated admitted native results");
      require(hasInk(icons->getXsheetFrameIcon(source->getXsheet(), 0, TDimension(80, 60), true).toImage()),
              "Canceled scene transition's retained Xsheet renderer produced no artwork");
    } else {
      require(pauseObserved && (operation == NewScene || loaded) &&
                  app->getCurrentScene()->getScene() != source &&
                  completions[active] == std::vector<bool>{false} &&
                  completions[queued] == std::vector<bool>{false} &&
                  lifetime->created.load() == 1 && lifetime->destroyed.load() == 1 &&
                  icons->getIcon(level.getPointer(), TFrameId(2), true, true).isNull(),
              "Accepted scene transition did not retire old renderers before replacing their source");
      std::lock_guard<std::mutex> lock(gate->mutex);
      require(!gate->expired && gate->enteredTokens.count(active) == 1 &&
                  !gate->enteredTokens.count(queued),
              "Accepted scene transition ran queued old-source work");
    }
    icons->setTaskTestHook({});
    if (accepted) {
      waitUntil([&]() { return resumedRefreshes != 0; },
                "Accepted scene transition did not repaint after resuming native icon admission");
      require(pausedPublications == 0 && completions[active] == std::vector<bool>{false} &&
                  completions[queued] == std::vector<bool>{false},
              "Post-resume repaint fabricated a completion or published while paused");
    }
    auto *current = app->getCurrentScene()->getScene();
    if (accepted && operation != NewScene) {
      require(current->getScenePath() == loadPath &&
                  current->getLevelSet()->getLevelCount() == 1,
              "Native Load did not reopen the actual saved scene");
      auto *loadedLevel = current->getLevelSet()->getLevel(L"Loaded icon source");
      require(loadedLevel, "Native Load lost its persisted level name");
      level = loadedLevel->getSimpleLevel();
      require(level && level->getFrameCount() == 3 &&
                  current->getXsheet()->getCell(0, 0) == TXshCell(level.getPointer(), TFrameId(1)),
              "Native Load lost its persisted vector exposure");
    } else if (accepted) {
      require(current->getLevelSet()->getLevelCount() == 0,
              "Accepted native New did not create an empty scene");
      level = makeTransitionLevel(current, L"New scene icon source");
    }
    successfulIcon(icons, level.getPointer(), completions);
    require(fileBytes(loadPath) == savedScene && fileBytes(drawingPath) == savedDrawing,
            "Scene-transition acceptance changed its saved source files");
  }
  icons->setTaskTestHook({});
  icons->clearRequests();
  waitUntil([&]() { return icons->lifecycleTestTasks().empty(); },
            "Scene-transition icons did not finish before final fixture reset");
  QString error;
  const bool reset = resetNativeUiIntegrationScene(&error);
  require(reset, qPrintable(error));
  for (const auto &lifetime : retainedLifetimes)
    require(lifetime->created.load() == 1 && lifetime->destroyed.load() == 1,
            "Scene-transition fixture outlived an unretired native thread-storage sentinel");
  failureDrain.complete = true;
  std::cout << "icon-lifecycle-native: canceled/accepted New and checked/unchecked Load PASS" << std::endl;
}
}  // namespace

void runIconLifecycleNativeChecks(NativeIconQuitScenario scenario) {
  const bool preserveReplacement = scenario == NativeIconQuitScenario::PreserveReplacement;
  auto *icons = IconGenerator::instance();
  require(!icons->isShutdownComplete(),
          "Native icon lifecycle checks began after terminal shutdown");
  auto *app = TApp::instance();
  try {
    sceneRetentionChecks();
    sceneTransitionChecks();
    waitUntil([&]() { return icons->lifecycleTestTasks().empty(); },
              "Earlier native icon requests did not finish before lifecycle checks");
    QString resetError;
    const bool reset = resetNativeUiIntegrationScene(&resetError);
    require(reset, qPrintable(resetError));
    ToonzScene *source = app->getCurrentScene()->getScene();
    QPointer<QMainWindow> window = app->getMainWindow();
    require(source && window && window->isVisible(),
            "Native Quit lifecycle fixture has no visible application window");
    TXshSimpleLevelP level = source->createNewLevel(PLI_XSHLEVEL, L"Icon lifecycle fixture")
                                ->getSimpleLevel();
    require(level, "Cannot create native icon lifecycle vector level");
    for (int frame = 1; frame <= 5; ++frame) {
      TVectorImageP image = new TVectorImage;
      image->setPalette(level->getPalette());
      auto *stroke = new TStroke(std::vector<TThickPoint>{
          TThickPoint(-30, -10, 6), TThickPoint(0, 20, 6), TThickPoint(30, -10, 6)});
      stroke->setStyle(1);
      image->addStroke(stroke);
      level->setFrame(TFrameId(frame), image);
    }
    level->getProperties()->setDirtyFlag(false);
    level->getPalette()->setDirtyFlag(false);
    app->getCurrentScene()->setDirtyFlag(true);
    const int levelCount = source->getLevelSet()->getLevelCount();
    require(!closeThroughNativePrompt(window, "Cancel", "Cancel") && window && window->isVisible() &&
                !icons->isShutdownComplete() && app->getCurrentScene()->getScene() == source &&
                source->getLevelSet()->getLevelCount() == levelCount &&
                source->getLevelSet()->getLevel(level->getName()) == level.getPointer() &&
                app->getCurrentScene()->getDirtyFlag(),
            "Canceling actual Quit stopped icons or cleared the scene");
    icons->clearRequests();
    waitUntil([&]() { return icons->lifecycleTestTasks().empty(); },
              "Canceled Quit left earlier icon requests unsettled");
    std::map<quint64, std::vector<bool>> completions;
    int publications = 0;
    QObject connectionContext;
    QObject::connect(icons, &IconGenerator::iconRequestFinished, &connectionContext,
                     [&](quint64 token, bool generated) { completions[token].push_back(generated); });
    QObject::connect(icons, &IconGenerator::iconGenerated, &connectionContext,
                     [&]() { ++publications; });

    // A real successful renderer establishes that this fixture is capable of
    // producing a visible native vector icon before publication is disabled.
    icons->remove(level.getPointer(), TFrameId(1));
    const auto beforeNormal = icons->lifecycleTestTasks();
    const quint64 normal = icons->requestIcon(level.getPointer(), TFrameId(1));
    require(normal != 0, "Native lifecycle success request was not admitted");
    const auto normalTask = taskFor(beforeNormal, icons->lifecycleTestTasks());
    waitUntil([&]() { return completions.count(normal) != 0; },
              "Native lifecycle success request did not finish");
    require(completions[normal] == std::vector<bool>{true},
            "Native lifecycle success request did not publish exactly once");
    const QImage visible = icons->getIcon(level.getPointer(), TFrameId(1), true, true).toImage();
    bool ink = false;
    for (int y = 0; y < visible.height() && !ink; ++y)
      for (int x = 0; x < visible.width(); ++x) {
        const QRgb pixel = visible.pixel(x, y);
        if (qAlpha(pixel) && qRed(pixel) < 200 && qGreen(pixel) < 200 && qBlue(pixel) < 200) {
          ink = true;
          break;
        }
      }
    require(!visible.isNull() && ink,
            "Native lifecycle fixture rendered no visible vector artwork");
    icons->onTerminated(normalTask);  // Deliberately late, already completed.

    const auto failureGate = std::make_shared<TaskGate>();
    failureGate->throwOnEntry = true;
    ReleaseGate releaseFailure{failureGate};
    icons->setTaskTestHook([failureGate](quint64 token) { failureGate->run(token); });
    const auto beforeFailure = icons->lifecycleTestTasks();
    const quint64 failed = icons->requestIcon(level.getPointer(), TFrameId(2));
    failureGate->arm(failed);
    require(failed != 0, "Native lifecycle failure request was not admitted");
    const auto failedTask = taskFor(beforeFailure, icons->lifecycleTestTasks());
    waitUntil([&]() { return completions.count(failed) != 0; },
              "Native lifecycle exception did not resolve its request");
    require(completions[failed] == std::vector<bool>{false} &&
                icons->getIcon(level.getPointer(), TFrameId(2), true, true).isNull(),
            "Failed native icon request published a result or completed twice");
    icons->onTerminated(failedTask);

    // An admitted Xsheet renderer can belong to a scene replaced BEFORE Quit.
    // Expire its real retirement timer while the worker is still held; only
    // the renderer's admission lease may keep this source alive.
    {
      ToonzScene replacementScene;
      TSceneHandle handle;
      auto *oldScene = new ToonzScene;
      oldScene->getCurrentCamera()->setRes(TDimension(80, 60));
      QPointer<TXshSimpleLevel> oldLevel = oldScene->createNewLevel(
          PLI_XSHLEVEL, L"Older admitted Xsheet source")->getSimpleLevel();
      TVectorImageP image = new TVectorImage;
      image->setPalette(oldLevel->getPalette());
      auto *stroke = new TStroke(std::vector<TThickPoint>{
          TThickPoint(-10, 0, 3), TThickPoint(0, 5, 3), TThickPoint(10, 0, 3)});
      stroke->setStyle(1);
      image->addStroke(stroke);
      oldLevel->setFrame(TFrameId(1), image);
      require(oldScene->getXsheet()->setCell(0, 0, TXshCell(oldLevel.data(), TFrameId(1))),
              "Cannot expose older admitted Xsheet source");
      handle.setScene(oldScene);
      const auto oldGate = std::make_shared<TaskGate>();
      ReleaseGate releaseOld{oldGate};
      oldGate->arm(0);
      icons->setTaskTestHook([oldGate](quint64 token) { oldGate->run(token); });
      icons->getXsheetFrameIcon(oldScene->getXsheet(), 0, TDimension(80, 60), false);
      waitUntil([&]() { return oldGate->hasEntered(); },
                "Older native Xsheet renderer did not enter its real worker");
      const quint64 oldRequest = oldGate->enteredToken();
      handle.setScene(&replacementScene);
      expireSceneRetirements(&handle);
      require(oldLevel && oldScene->getXsheet()->getCell(0, 0).m_level.getPointer() == oldLevel.data(),
              "Retirement invalidated an already-admitted Xsheet renderer");
      oldGate->release();
      waitUntil([&]() { return completions.count(oldRequest) != 0; },
                "Older admitted Xsheet renderer did not complete");
      require(completions[oldRequest] == std::vector<bool>{true},
              "Retained older Xsheet source failed native rendering");
      waitUntil([&]() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return oldLevel.isNull();
      }, "Completed Xsheet renderer did not release its retired source");
      icons->setTaskTestHook({});
    }
    std::cout << "icon-lifecycle-native: already-retired Xsheet reader PASS" << std::endl;
    // Rendering/settings probes need the live application. No scene or worker
    // is restarted after the terminal MainWindow close below.
    runIconWorkerSettingsNativeChecks();

    QTemporaryDir loadDirectory;
    require(loadDirectory.isValid(), "Cannot prepare native shutdown load source");
    const TFilePath loadPath((loadDirectory.path() + "/queued-load.tnz").toStdWString());
    {
      ToonzScene loadSource;
      loadSource.saveCopy(loadPath);
    }
    require(QFileInfo(QString::fromStdWString(loadPath.getWideString())).isFile(),
            "Native queued Load fixture did not save its real scene");
    const TFilePath originalPath = source->getScenePath();
    const int originalHistory = TUndoManager::manager()->getHistoryCount();
    const int originalHistoryIndex = TUndoManager::manager()->getCurrentHistoryIndex();

    std::unique_ptr<ToonzScene> preparedReplacement;
    QPointer<TXshLevel> replacementLevel;
    if (preserveReplacement) {
      preparedReplacement.reset(new ToonzScene);
      TProjectManager::instance()->initializeScene(preparedReplacement.get());
      replacementLevel = preparedReplacement->createNewLevel(
          PLI_XSHLEVEL, L"Unreviewed shutdown replacement");
      require(replacementLevel && !preparedReplacement->getScenePath().isEmpty() &&
                  preparedReplacement->getProject()->isCurrent(),
              "Native Quit replacement lacks a valid initialized scene/project");
    }

    const auto gate = std::make_shared<TaskGate>();
    const auto threadLifetime = std::make_shared<ThreadLifetime>();
    const auto threadStorage = std::make_shared<QThreadStorage<ThreadSentinel *>>();
    ReleaseGate releaseActive{gate};
    icons->setTaskTestHook([gate, threadLifetime, threadStorage](quint64 token) {
      if (!threadStorage->hasLocalData())
        threadStorage->setLocalData(new ThreadSentinel(threadLifetime));
      gate->run(token);
    });
    const quint64 active = icons->requestIcon(level.getPointer(), TFrameId(3));
    gate->arm(active);
    require(active != 0, "Native lifecycle active request was not admitted");
    waitUntil([&]() { return gate->hasEntered(); },
              "Native icon worker did not reach its real execution boundary");
    icons->remove(level.getPointer(), TFrameId(3));
    const quint64 replacement = icons->requestIcon(level.getPointer(), TFrameId(3));
    const quint64 queued = icons->requestIcon(level.getPointer(), TFrameId(4));
    require(replacement && queued && active != replacement && replacement != queued,
            "Native lifecycle fixture did not queue distinct superseded generations");
    const auto outstanding = icons->lifecycleTestTasks();
    require(outstanding.size() >= 3,
            "Native lifecycle tracking omitted a superseded active task");
    publications = 0;
    bool releaseDelivered = false, refusedDuringDrain = false, watchdogExpired = false;
    bool nestedDrainComplete = false, sourceAliveDuringDrain = false;
    bool commandsRefused = false, replacementRetained = false;
    QString queuedFailure;
    ToonzScene *replacementDuringDrain = nullptr;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &connectionContext, [&]() {
      watchdogExpired = true;
      gate->release();  // Failure remains safe: never leave a paused worker.
    });
    watchdog.start(10000);
    QTimer releaseWhenStopping;
    QObject::connect(&releaseWhenStopping, &QTimer::timeout, &connectionContext, [&]() {
      // The real Quit prompt also pumps events. Do not release the worker until
      // the accepted-close path has actually closed icon request admission.
      if (!icons->lifecycleTestStopping() || releaseDelivered) return;
      releaseDelivered = true;
      try {
      IoCmd::newScene();
      const bool checkedLoad = IoCmd::loadScene(loadPath, false, true);
      const bool uncheckedLoad = IoCmd::loadScene(loadPath, false, false);
      const bool directClear = IoCmd::saveSceneIfNeeded("Queued Change Project");
      QCloseEvent nestedClose;
      QApplication::sendEvent(window, &nestedClose);
      commandsRefused = IoCmd::isSceneTransitionInProgress() && !checkedLoad &&
          !uncheckedLoad && !directClear && !nestedClose.isAccepted() && window->isVisible() &&
          app->getCurrentScene()->getScene() == source &&
          source->getScenePath() == originalPath && app->getCurrentScene()->getDirtyFlag() &&
          TUndoManager::manager()->getHistoryCount() == originalHistory &&
          TUndoManager::manager()->getCurrentHistoryIndex() == originalHistoryIndex;

      // A low-level replacement can bypass command entrypoints. Its old scene
      // timer must not invalidate the blocked source reader or clear this new,
      // unreviewed scene when the accepted Quit resumes.
      if (preserveReplacement) {
        replacementDuringDrain = preparedReplacement.release();
        app->getCurrentScene()->setScene(replacementDuringDrain);
        app->getCurrentScene()->setDirtyFlag(true);
        expireSceneRetirements(app->getCurrentScene());
        replacementRetained = app->getCurrentScene()->getScene() == replacementDuringDrain &&
            replacementDuringDrain->getLevelSet()->getLevelCount() == 1;
      }
      sourceAliveDuringDrain = source->getLevelSet()->getLevelCount() == levelCount &&
          source->getLevelSet()->getLevel(level->getName()) == level.getPointer() &&
          level->getFrameCount() == 5;
      refusedDuringDrain = !icons->isShutdownComplete() &&
          icons->requestIcon(level.getPointer(), TFrameId(5)) == 0;
      gate->release();
      // A queued close/quit callback can reenter the drain while its outer
      // event loop is servicing GUI work. Neither call may return early.
      icons->shutdown();
      nestedDrainComplete = icons->isShutdownComplete() &&
          threadLifetime->created.load() == 1 && threadLifetime->destroyed.load() == 1;
      } catch (const TException &error) {
        queuedFailure = "Queued native Quit Toonz exception: " +
            QString::fromStdWString(error.getMessage()).left(1024)
                .replace('\n', ' ').replace('\r', ' ');
        gate->release();
      } catch (const std::exception &error) {
        queuedFailure = QString::fromUtf8(error.what());
        gate->release();
      } catch (...) {
        queuedFailure = "Unexpected queued shutdown fixture exception";
        gate->release();
      }
    });
    releaseWhenStopping.start(1);
    const bool closed = closeThroughNativePrompt(window, "Discard Changes",
        preserveReplacement ? "Discard with replacement" : "Discard reviewed scene");
    releaseWhenStopping.stop();
    watchdog.stop();
    icons->setTaskTestHook({});
    require(queuedFailure.isEmpty(), qPrintable(queuedFailure));
    require(closed && window && !window->isVisible() && sourceAliveDuringDrain && commandsRefused,
            "Accepted Quit lost its real close, retained source or reentry protection");
    if (preserveReplacement)
      require(replacementRetained && replacementLevel &&
                  app->getCurrentScene()->getScene() == replacementDuringDrain &&
                  replacementDuringDrain->getLevelSet()->getLevelCount() == 1 &&
                  app->getCurrentScene()->getDirtyFlag(),
              "Accepted Quit cleared an unreviewed replacement");
    else
      require(app->getCurrentScene()->getScene() == source &&
                  source->getLevelSet()->getLevelCount() == 0 &&
                  !app->getCurrentScene()->getDirtyFlag(),
              "Explicitly reviewed scene did not clear after native worker retirement");
    require(releaseDelivered && refusedDuringDrain && nestedDrainComplete && !watchdogExpired,
            "Native icon drain did not close admission and service its GUI release");
    require(icons->isShutdownComplete() && icons->lifecycleTestTasks().empty(),
            "Native icon shutdown returned before all tasks and workers retired");
    require(threadLifetime->created.load() == 1 && threadLifetime->destroyed.load() == 1,
            "Native icon shutdown returned before actual worker TLS cleanup");
    {
      std::lock_guard<std::mutex> lock(gate->mutex);
      require(!gate->expired && gate->enteredTokens.count(active) == 1 &&
                  !gate->enteredTokens.count(replacement) && !gate->enteredTokens.count(queued),
              "Native icon drain ran queued work or lost the active worker");
    }
    for (const quint64 token : {active, replacement, queued})
      require(completions[token] == std::vector<bool>{false},
              "Native icon drain lost or duplicated an accepted request completion");
    require(publications == 0 &&
                icons->getIcon(level.getPointer(), TFrameId(3), true, true).isNull() &&
                icons->getIcon(level.getPointer(), TFrameId(4), true, true).isNull() &&
                icons->requestIcon(level.getPointer(), TFrameId(5)) == 0,
            "Retired native icons published late data or admitted new work");
    for (const auto &task : outstanding) icons->onTerminated(task);
    icons->shutdown();
    require(icons->isShutdownComplete() && icons->lifecycleTestTasks().empty() &&
                publications == 0,
            "Repeated native shutdown or late termination revived icon work");
    std::cout << (preserveReplacement
        ? "icon-lifecycle-native: terminal replacement PASS"
        : "icon-lifecycle-native: terminal reviewed-clear PASS") << std::endl;
    std::cout << "icon-lifecycle-native: PASS (real success/error, active and superseded tasks, queued cancellation, no late publication, worker retirement)"
              << std::endl;
  } catch (...) {
    icons->setTaskTestHook({});
    icons->shutdown();
    throw;
  }
}

void runIconWorkerSettingsNativeChecks() {
  // These are detached native scene objects. The current application scene,
  // its selection and undo history remain available for ordinary teardown.
  ToonzScene source;
  auto *sheet = source.getXsheet();
  require(sheet, "Icon worker settings fixture has no native sheet");
  for (int column = 0; column < 2; ++column) {
    auto *level = source.createNewLevel(
        PLI_XSHLEVEL, column ? L"Worker right" : L"Worker left")->getSimpleLevel();
    require(level, "Icon worker settings fixture has no vector level");
    TVectorImageP image = new TVectorImage;
    image->setPalette(level->getPalette());
    const std::vector<TThickPoint> points{
        TThickPoint(-20, column * 20, 3),
        TThickPoint(0, column * 20, 3),
        TThickPoint(20, column * 20, 3)};
    auto *stroke = new TStroke(points);
    stroke->setStyle(1);
    image->addStroke(stroke);
    level->setFrame(TFrameId(1), image);
    require(sheet->setCell(0, column, TXshCell(level, TFrameId(1))),
            "Icon worker settings fixture cannot expose its drawing");
  }

  QPointer<QAction> cameraTest =
      CommandManager::instance()->getAction(MI_CameraTest);
  require(cameraTest && cameraTest->isCheckable(),
          "Native Camera Test action is unavailable");
  const bool original = cameraTest->isChecked();
  const QSignalBlocker blocked(cameraTest);
  struct RestoreAction {
    QPointer<QAction> action;
    bool checked;
    ~RestoreAction() { if (action) action->setChecked(checked); }
  } restore{cameraTest, original};
  cameraTest->setChecked(true);
  require(CameraTestCheck::instance()->isEnabled(),
          "Worker settings oracle did not enable the real GUI Camera Test");

  std::set<int> defaults, explicitOff, explicitOn;
  std::exception_ptr failure;
  std::thread worker([&]() {
    try {
      ImagePainter::VisualSettings settings;
      RecordingVisitor ordinary(settings);
      // This is the exact convenience path used by ToonzScene::renderFrame.
      // Historically it read the live GUI QAction inside StageBuilder.
      Stage::visit(ordinary, &source, sheet, 0);
      defaults = ordinary.columns;

      OnionSkinMask onion;
      Stage::VisitArgs args;
      args.m_scene = &source;
      args.m_xsh = sheet;
      args.m_osm = &onion;
      args.m_col = 1;
      RecordingVisitor off(settings);
      Stage::visit(off, args);
      explicitOff = off.columns;

      args.m_cameraTest = true;
      RecordingVisitor on(settings);
      Stage::visit(on, args);
      explicitOn = on.columns;
    } catch (...) {
      failure = std::current_exception();
    }
  });
  worker.join();
  if (failure) std::rethrow_exception(failure);
  require(defaults == std::set<int>({0, 1}) && explicitOff == defaults,
          "Background Stage visit inherited a GUI Camera Test setting");
  require(explicitOn == std::set<int>({1}),
          "Explicit Camera Test snapshot lost its selected-column behavior");
  std::cout << "icon-lifecycle-native: worker Stage settings PASS"
            << std::endl;
}
