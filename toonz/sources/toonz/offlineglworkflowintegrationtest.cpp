#include "offlineglworkflowintegrationtest.h"
#include "tofflinegl.h"
#include "tpalette.h"
#include "tstroke.h"
#include "tsimplecolorstyles.h"
#include "tvectorimage.h"
#include "tlevel_io.h"
#include "tlevel.h"
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void checkpoint(const char *message) {
  std::cerr << "offline-gl-workflow: " << message << std::endl;
}

// A GUI QTimer cannot report a deadlock when that very thread is blocked in a
// constructor. This watchdog reports the stalled phase independently.
class Deadline final {
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_finished = false;
  std::thread m_thread;
public:
  Deadline() : m_thread([this] {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_changed.wait_for(lock, std::chrono::seconds(45),
                           [this] { return m_finished; }))
      qFatal("Concurrent GUI/background offscreen context creation deadlocked");
  }) {}
  ~Deadline() {
    { std::lock_guard<std::mutex> lock(m_mutex); m_finished = true; }
    m_changed.notify_all(); m_thread.join();
  }
};

class GuiContextOverlap final : public QObject {
  std::atomic<bool> &m_workerEntered;
public:
  bool intercepted = false;
  bool rendered = false;
  std::exception_ptr error;
  explicit GuiContextOverlap(std::atomic<bool> &workerEntered)
      : m_workerEntered(workerEntered) {
    qApp->installEventFilter(this);
  }
protected:
  bool eventFilter(QObject *receiver, QEvent *event) override {
    if (receiver != qApp || event->type() != QEvent::MetaCall || intercepted ||
        !m_workerEntered.load()) return false;
    // Enter the actual palette context pool before delivering the background
    // renderer's GUI-thread surface request. The previous global GLX mutex
    // deadlocked this order: worker held it while waiting for this thread.
    intercepted = true;
    checkpoint("GUI palette context overlaps pending background surface request");
    try {
      TOfflineGL *paletteContext = TOfflineGL::getStock(TDimension(119, 73));
      paletteContext->clear(TPixel32(17, 83, 149, 255));
      const TRaster32P pixels = paletteContext->getRaster();
      rendered = pixels && pixels->pixels(10)[10] == TPixel32(17, 83, 149, 255);
      paletteContext->doneCurrent();
    } catch (...) {
      // Let Qt deliver the worker's request before reporting a GUI render
      // failure; exceptions must never escape an event filter.
      error = std::current_exception();
    }
    return false;
  }
};

void concurrentContexts() {
  // Drain earlier application-targeted queued calls before installing the
  // interceptor; the new worker is the only producer in this fixture.
  QApplication::sendPostedEvents(qApp, QEvent::MetaCall);
  std::atomic<bool> entered{false}, finished{false};
  GuiContextOverlap overlap(entered);
  Deadline deadline;
  auto worker = std::async(std::launch::async, [&] {
    entered.store(true);
    try {
      TOfflineGL context(TDimension(97, 61));
      context.clear(TPixel32(29, 113, 197, 255));
      const TRaster32P pixels = context.getRaster();
      const bool valid = pixels && pixels->pixels(10)[10] == TPixel32(29, 113, 197, 255);
      context.doneCurrent();
      finished.store(true);
      return valid;
    } catch (...) { finished.store(true); throw; }
  });
  while (!finished.load()) {
    QApplication::processEvents();
    QThread::msleep(1);
  }
  require(worker.get(), "background offscreen context returned incorrect pixels");
  if (overlap.error) std::rethrow_exception(overlap.error);
  require(overlap.intercepted && overlap.rendered,
          "GUI palette/background creation overlap was not exercised");
  checkpoint("GUI and background contexts completed without lock inversion");
}

QString converterPath() {
#ifdef _WIN32
  const QString leaf = "tconverter.exe";
#else
  const QString leaf = "tconverter";
#endif
  const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(leaf);
  require(QFileInfo(path).isFile(), "native converter is missing beside the application");
  return path;
}

QImage convert(const QString &executable, const QString &source,
               const QString &destination, bool rasterOnly = false,
               int outputWidth = 0) {
  QProcess process;
  process.setProcessChannelMode(QProcess::MergedChannels);
  auto environment = QProcessEnvironment::systemEnvironment();
  if (rasterOnly) {
    // A raster-only command must not load any Qt GUI platform plugin.
    environment.insert("QT_QPA_PLATFORM", "groundzero-nonexistent-platform");
    environment.remove("DISPLAY"); environment.remove("WAYLAND_DISPLAY");
  }
  process.setProcessEnvironment(environment);
  QStringList arguments{source, destination};
  if (outputWidth > 0) arguments << "-w" << QString::number(outputWidth);
  process.start(executable, arguments);
  require(process.waitForStarted(10000), "could not start native converter");
  if (!process.waitForFinished(30000)) {
    process.kill(); process.waitForFinished(5000);
    throw std::runtime_error("native converter timed out");
  }
  const QByteArray output = process.readAll();
  constexpr int maximumLoggedBytes = 64 * 1024;
  std::cerr << output.left(maximumLoggedBytes).constData() << std::flush;
  if (output.size() > maximumLoggedBytes)
    std::cerr << "\noffline-gl-workflow: converter output truncated after "
              << maximumLoggedBytes << " bytes (total " << output.size()
              << ")" << std::endl;
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    const QString failure =
        QString("native converter failed: exit_status=%1 exit_code=%2 "
                "process_error=%3 detail=%4 requested_width=%5 output_bytes=%6")
            .arg(process.exitStatus() == QProcess::NormalExit ? "normal" : "crashed")
            .arg(process.exitCode()).arg(static_cast<int>(process.error()))
            .arg(process.errorString()).arg(outputWidth).arg(output.size());
    throw std::runtime_error(failure.toStdString());
  }
  require(!output.toLower().contains("conversion failed"),
          "native converter reported a frame conversion failure");
  const QFileInfo target(destination);
  const QStringList files = target.dir().entryList(
      {target.completeBaseName() + "*.png"}, QDir::Files);
  require(files.size() == 1, "native converter did not create exactly one PNG frame");
  QImage pixels(target.dir().filePath(files.front()));
  require(!pixels.isNull(), "independent PNG reader rejected converter output");
  pixels = pixels.convertToFormat(QImage::Format_ARGB32);
  int visible = 0, maximumAlpha = 0, centerAlpha = 0;
  for (int y = 0; y < pixels.height(); ++y)
    for (int x = 0; x < pixels.width(); ++x) {
      const int alpha = qAlpha(pixels.pixel(x,y));
      if (alpha) ++visible;
      maximumAlpha = std::max(maximumAlpha,alpha);
      if (x == pixels.width()/2) centerAlpha = std::max(centerAlpha,alpha);
    }
  std::cerr << "offline-gl-workflow: decoded " << target.fileName().toStdString()
            << " size=" << pixels.width() << 'x' << pixels.height()
            << " visible=" << visible << " max_alpha=" << maximumAlpha
            << " center_column_alpha=" << centerAlpha << std::endl;
  return pixels;
}

int columnAlpha(const QImage &image, int x) {
  int result = 0;
  for (int y = 0; y < image.height(); ++y)
    result = std::max(result, qAlpha(image.pixel(x, y)));
  return result;
}

void commandLineConversion() {
  const QString executable = converterPath();
  QTemporaryDir directory;
  require(directory.isValid(), "native converter fixture directory unavailable");
  TPaletteP palette = new TPalette;
  const int style = palette->addStyle(new TSolidColorStyle(TPixel32(80, 160, 220, 255)));
  // Native PLI writes displayed palette pages. A detached style slot alone
  // does not describe a persisted authored style, even if in-memory GL draws it.
  palette->getPage(0)->addStyle(style);
  TVectorImageP image = new TVectorImage;
  image->setPalette(palette.getPointer());
  auto *stroke = new TStroke(std::vector<TThickPoint>{{-48, 0, 8}, {0, 0, 8}, {48, 0, 8}});
  stroke->setStyle(style); image->addStroke(stroke, false);
  const auto verifySavedInput = [&](const QString &path) {
    TLevelReaderP reader{TFilePath(path)};
    const TLevelP level = reader->loadInfo();
    TPalette *savedPalette = level ? level->getPalette() : nullptr;
    require(savedPalette && savedPalette->getStyleCount() > style &&
                savedPalette->getStylePage(style) &&
                savedPalette->getStyle(style)->getMainColor() == TPixel32(80,160,220,255),
            "saved PLI fixture lost its authored opaque palette style");
    const TVectorImageP saved = reader->getFrameReader(TFrameId(1))->load();
    require(saved && saved->getStrokeCount() == 1 &&
                saved->getStroke(0)->getControlPointCount() == stroke->getControlPointCount() &&
                saved->getStroke(0)->getStyle() == style &&
                saved->getStroke(0)->getOpacity() == stroke->getOpacity(),
            "saved PLI fixture lost its authored stroke or opacity");
    for (int point = 0; point < stroke->getControlPointCount(); ++point)
      require(saved->getStroke(0)->getControlPoint(point) == stroke->getControlPoint(point),
              "saved PLI fixture changed its authored geometry");
    saved->setPalette(savedPalette);
    const TRectD bounds = saved->getBBox();
    require(bounds.getLx() > 96. && bounds.getLy() >= 16.,
            "saved PLI fixture lost its style-dependent drawing bounds");
    checkpoint("saved PLI palette, geometry and opacity verified before conversion");
  };
  const QString ordinaryPath = directory.filePath("ordinary.pli");
  {
    TLevelWriterP writer{TFilePath(ordinaryPath)};
    writer->getFrameWriter(TFrameId(1))->save(image);
  }
  verifySavedInput(ordinaryPath);
  checkpoint("ordinary PLI command-line conversion begins");
  const QImage ordinary = convert(executable, ordinaryPath, directory.filePath("opaque.png"));
  require(ordinary.width() > 40 && ordinary.height() > 8 &&
              columnAlpha(ordinary, ordinary.width() / 2) == 255,
          "ordinary PLI command-line conversion lost opaque artwork");
  checkpoint("explicit-width PLI command-line conversion begins");
  const QImage wide = convert(executable, ordinaryPath,
                             directory.filePath("opaque-wide.png"), false, 232);
  require(wide.width() == 232 && wide.height() > ordinary.height() &&
              columnAlpha(wide, wide.width() / 2) == 255,
          "explicit-width PLI conversion lost dimensions or opaque artwork");
  const QString rasterPath = directory.filePath("raster-source.png");
  QImage raster(16, 12, QImage::Format_ARGB32); raster.fill(qRgba(20, 60, 100, 255));
  require(raster.save(rasterPath), "headless raster input could not be written");
  checkpoint("display-independent raster command-line conversion begins");
  const QImage copied = convert(executable, rasterPath, directory.filePath("raster-copy.png"), true);
  require(copied.size() == raster.size() && copied.pixel(8, 6) == raster.pixel(8, 6),
          "headless raster conversion changed source pixels");
}
}  // namespace

void runOfflineGLWorkflowNativeChecks() {
  concurrentContexts();
  commandLineConversion();
  checkpoint("PASS (concurrent GUI/background contexts; PLI and headless raster CLI)");
}
