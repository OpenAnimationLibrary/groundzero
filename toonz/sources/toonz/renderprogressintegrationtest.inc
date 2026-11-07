// Included only by rendercommand.cpp in BUILD_UI_PORT_TESTS builds.
#include "renderprogressintegrationtest.h"
#include "nativeuiintegrationtest.h"
#include "toonz/txshcell.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "trasterimage.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QTemporaryDir>

#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <thread>

class RenderProgressNativeChecks final {
  class EventDeadline final {
    QTimer m_timer;

  public:
    EventDeadline() {
      m_timer.setSingleShot(true);
      QObject::connect(&m_timer, &QTimer::timeout, &m_timer, []() {
        qFatal("render progress fixture exceeded its 10-second event deadline");
      });
      m_timer.start(10000);
    }
  };

  static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
  }

  static void drainEvents() {
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
  }

  static void awaitRetirement(const QPointer<RenderListener>& listener) {
    EventDeadline nestedEvents;
    QElapsedTimer deadline;
    deadline.start();
    while (listener && deadline.elapsed() < 10000) drainEvents();
    require(!listener, "terminal render progress dialog was not destroyed");
  }

  template <typename Listener>
  static QList<Listener*> listeners() {
    QList<Listener*> result;
    for (QWidget* widget : QApplication::allWidgets())
      if (auto* listener = dynamic_cast<Listener*>(widget))
        result.push_back(listener);
    return result;
  }

  static void resetScene() {
    QString error;
    require(resetNativeUiIntegrationScene(&error),
            qPrintable(QString("render progress scene reset: %1").arg(error)));
  }

  class SavedPreferences final {
    bool m_view = Preferences::instance()->isGeneratedMovieViewEnabled();
    bool m_check =
        Preferences::instance()->getBoolValue(checkSceneBeforeRendering);

  public:
    SavedPreferences() {
      Preferences::instance()->setValue(generatedMovieViewEnabled, false,
                                        false);
      Preferences::instance()->setValue(checkSceneBeforeRendering, false,
                                        false);
    }
    ~SavedPreferences() {
      Preferences::instance()->setValue(generatedMovieViewEnabled, m_view,
                                        false);
      Preferences::instance()->setValue(checkSceneBeforeRendering, m_check,
                                        false);
    }
  };

  static void addColumn(ToonzScene* scene, int column, const TPixel32& color) {
    auto* level = scene
                      ->createNewLevel(OVL_XSHLEVEL, L"RenderProgress",
                                       TDimension(16, 16), 72.0)
                      ->getSimpleLevel();
    require(level, "render progress raster level was not created");
    TRaster32P raster(16, 16);
    raster->fill(color);
    TRasterImageP image(raster);
    image->setDpi(72.0, 72.0);
    level->setFrame(TFrameId(1), image);
    require(
        scene->getXsheet()->setCell(0, column, TXshCell(level, TFrameId(1))),
        "render progress exposure was not created");
  }

  static TFilePath outputPath(const QString& directory, const QString& name) {
    return TFilePath(QDir(directory).filePath(name + ".png"));
  }

  static void checkOutput(const QString& directory, const QString& prefix,
                          const QList<QRgb>& expected) {
    const QStringList files =
        QDir(directory).entryList({prefix + "*.png"}, QDir::Files, QDir::Name);
    require(files.size() == expected.size(),
            "render progress fixture produced the wrong number of outputs");
    QList<QRgb> actual;
    for (const QString& file : files) {
      const QImage image(QDir(directory).filePath(file));
      require(!image.isNull() && image.size() == QSize(16, 16),
              "render progress fixture output dimensions changed");
      actual.push_back(image.pixel(8, 8));
    }
    QList<QRgb> sorted = expected;
    std::sort(actual.begin(), actual.end());
    std::sort(sorted.begin(), sorted.end());
    require(actual == sorted, "render progress fixture output pixels changed");
  }

  static void checkNormalRenders(ToonzScene* scene, const QString& directory) {
    auto* output = scene->getProperties()->getOutputProperties();
    for (int cycle = 0; cycle < 8; ++cycle) {
      EventDeadline nestedEvents;
      const QString name = QString("normal-%1").arg(cycle);
      output->setPath(outputPath(directory, name));
      require(renderCommand.init(false),
              "normal render fixture did not initialize");
      renderCommand.rasterRender(false);
      const auto active = listeners<RenderListener>();
      require(active.size() == 1,
              "normal render retained an earlier progress dialog");
      awaitRetirement(QPointer<RenderListener>(active.front()));
      checkOutput(directory, name, {qRgba(0, 0, 255, 255)});
    }
  }

  static void checkRealCancellation(ToonzScene* scene,
                                    const QString& directory) {
    EventDeadline nestedEvents;
    scene->getProperties()->getOutputProperties()->setPath(
        outputPath(directory, "canceled"));
    require(renderCommand.init(false),
            "canceled render fixture did not initialize");
    renderCommand.rasterRender(false);
    const auto active = listeners<RenderListener>();
    require(active.size() == 1,
            "canceled render did not own one progress dialog");
    QPointer<RenderListener> listener(active.front());
    listener->onCancel();
    awaitRetirement(listener);
    require(QDir(directory).entryList({"canceled*.png"}, QDir::Files).isEmpty(),
            "canceled render published an output frame");
  }

  static void checkNestedCancellation() {
    for (int cycle = 0; cycle < 8; ++cycle) {
      QPointer<RenderListener> listener;
      bool insideCancel = false, survived = false, callbackRetained = false;
      auto token = std::make_shared<int>(cycle);
      std::weak_ptr<int> tokenObserver(token);
      listener = new RenderListener(
          [&, token]() {
            insideCancel = true;
            listener->onSequenceFinished(
                TFilePath(), MovieRenderer::CompletionStatus::Canceled, L"");
            drainEvents();
            drainEvents();
            survived         = !listener.isNull();
            callbackRetained = token && !tokenObserver.expired();
            insideCancel     = false;
          },
          TFilePath(), 1, false);
      token.reset();
      bool destroyedInsideCancel = false;
      QObject callbacks;
      QObject::connect(listener.data(), &QObject::destroyed, &callbacks,
                       [&]() { destroyedInsideCancel = insideCancel; });
      listener->retireWhenFinished();
      listener->onCancel();
      require(
          survived && callbackRetained && !destroyedInsideCancel,
          "terminal notification destroyed an active cancellation callback");
      awaitRetirement(listener);
      require(tokenObserver.expired(),
              "cancel callback retained its renderer token");
    }
  }

  static void checkHandoffAndLateMessage() {
    auto owned = std::unique_ptr<RenderListener>(
        new RenderListener({}, TFilePath(), 1, false));
    QPointer<RenderListener> listener(owned.get());
    RenderListener::Message pending(owned.get(), -1);
    // A worker can finish before its already-created progress message arrives.
    std::thread worker([&]() { pending.send(); });
    worker.join();
    owned->onSequenceFinished(TFilePath(),
                              MovieRenderer::CompletionStatus::Canceled, L"");
    drainEvents();
    require(listener, "terminal notification competed with startup ownership");
    owned.release()->retireWhenFinished();
    awaitRetirement(listener);
    pending.onDeliver();
  }

  static void checkNestedProgressDelivery() {
    QPointer<RenderListener> listener(
        new RenderListener({}, TFilePath(), 2, false));
    listener->setModal(true);
    listener->retireWhenFinished();
    listener->onSequenceFinished(
        TFilePath(), MovieRenderer::CompletionStatus::Canceled, L"");
    bool processedNestedEvents = false;
    QObject callbacks;
    QTimer::singleShot(0, &callbacks, [&]() { processedNestedEvents = true; });
    RenderListener::Message progress(
        listener.data(), 1, [](int) { return QString("fixture progress"); });
    progress.onDeliver();
    require(processedNestedEvents && listener,
            "queued retirement destroyed an active progress delivery");
    awaitRetirement(listener);
  }

  static void checkStartupFailure(ToonzScene* scene, const QString& directory) {
    const TFilePath path = outputPath(directory, "render-progress-invalid");
    MovieRenderer renderer(scene, path, 1, false);
    auto owner = std::unique_ptr<RenderListener>(
        new RenderListener(renderer.getCancelCallback(), path, 1, false));
    QPointer<RenderListener> listener(owner.get());
    renderer.addListener(owner.get());
    QTimer closeWarning;
    int warnings            = 0;
    bool aliveDuringWarning = false;
    QObject::connect(&closeWarning, &QTimer::timeout, [&]() {
      for (QWidget* widget : QApplication::topLevelWidgets()) {
        auto* dialog = qobject_cast<QDialog*>(widget);
        if (!dialog || !dialog->isVisible() || dialog == listener.data())
          continue;
        for (QLabel* label : dialog->findChildren<QLabel*>()) {
          if (!label->text().contains("render-progress-invalid")) continue;
          ++warnings;
          aliveDuringWarning = !listener.isNull();
          dialog->accept();
          break;
        }
      }
    });
    closeWarning.start(10);
    renderer.start();  // No frames: real asynchronous startup-failure path.
    owner.release()->retireWhenFinished();
    awaitRetirement(listener);
    closeWarning.stop();
    require(warnings == 1 && aliveDuringWarning,
            "startup error retired its listener while the terminal callback "
            "was active");
  }

  static void checkMultimedia(ToonzScene* scene, const QString& directory) {
    addColumn(scene, 1, TPixel32::Red);
    auto* output = scene->getProperties()->getOutputProperties();
    output->setMultimediaRendering(MultimediaRenderer::COLUMNS);
    for (int cycle = 0; cycle < 4; ++cycle) {
      EventDeadline nestedEvents;
      const QString name = QString("multimedia-%1").arg(cycle);
      output->setPath(outputPath(directory, name));
      require(renderCommand.init(false),
              "multimedia fixture did not initialize");
      renderCommand.multimediaRender();
      require(listeners<MultimediaProgressBar>().isEmpty(),
              "completed multimedia render retained its progress dialog");
      checkOutput(directory, name,
                  {qRgba(0, 0, 255, 255), qRgba(255, 0, 0, 255)});
    }
    MultimediaRenderer renderer(scene, outputPath(directory, "late"),
                                MultimediaRenderer::COLUMNS, 1, false);
    auto listener = std::unique_ptr<MultimediaProgressBar>(
        new MultimediaProgressBar(&renderer));
    MultimediaProgressBar::Message pending(listener.get(), -1, -1, -1);
    listener.reset();
    pending.onDeliver();
  }

public:
  static void run() {
    SavedPreferences preferences;
    resetScene();
    QTemporaryDir directory;
    require(directory.isValid(),
            "render progress output directory is unavailable");
    auto* scene = TApp::instance()->getCurrentScene()->getScene();
    scene->getCurrentCamera()->setSize(TDimensionD(16.0 / 72.0, 16.0 / 72.0));
    scene->getCurrentCamera()->setRes(TDimension(16, 16));
    auto* output = scene->getProperties()->getOutputProperties();
    output->setRange(0, 0, 1);
    output->setThreadIndex(0);
    output->setRenderSettings(TRenderSettings());
    addColumn(scene, 0, TPixel32::Blue);
    checkNormalRenders(scene, directory.path());
    checkRealCancellation(scene, directory.path());
    checkNestedCancellation();
    checkHandoffAndLateMessage();
    checkNestedProgressDelivery();
    checkStartupFailure(scene, directory.path());
    checkMultimedia(scene, directory.path());
    resetScene();
    std::cout
        << "render-progress-native: PASS normal, cancel, startup failure, "
           "nested delivery, handoff, late messages, multimedia"
        << std::endl;
  }
};

void runRenderProgressNativeChecks() { RenderProgressNativeChecks::run(); }
