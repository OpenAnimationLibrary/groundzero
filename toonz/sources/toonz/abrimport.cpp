// Extract Sampled Tips from ABR: reads the sampled images of .abr files
// (see toonz/abrparser.h for what is supported) and writes each tip as a
// grayscale PNG into <library>/brushtips/<name of the .abr file>/. The Brush
// tool on raster levels (full color brush) lists these images in its "Tip:"
// option; any PNG put in <library>/brushtips by hand is listed too.
//
// The PNG of a tip is black where the tip paints and white where it does not
// (the coverage of the ABR file, inverted), like the brush tip images of
// other painting programs.

#include "toonzqt/uitranslations.h"
#include "tapp.h"
#include "menubarcommandids.h"

#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"

#include "toonz/rasterbrushimport.h"
#include "toonz/rasterbrushlibrary.h"
#include "../toonzlib/rasterbrushlibrarypublication.h"
#include "../toonzlib/rasterbrushtipcollection.h"
#include "toonz/toonzfolders.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QMainWindow>
#include <QCoreApplication>
#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QPointer>
#include <QThread>
#include <QVBoxLayout>
#include <atomic>
#include <cmath>
#include <exception>
#include <memory>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

namespace {

//-----------------------------------------------------------------------------

// The GUI owns only cancellation and presentation. Untrusted ABR decoding runs
// in the existing restricted child; all staging/PNG encoding is off the UI
// thread, with an atomic collection publication as its only visible write.
class SampledTipImportProgress final : public QDialog {
  std::atomic_bool &cancelled;
  QLabel *message;
  QPushButton *cancel;

public:
  explicit SampledTipImportProgress(std::atomic_bool &flag)
      : cancelled(flag) {
    setObjectName("abrSampledTipImportProgress");
    setWindowTitle(tr("Extract Sampled Tips from ABR"));
    setWindowModality(Qt::ApplicationModal);
    auto *layout = new QVBoxLayout(this);
    message = new QLabel(tr("Reading and preparing sampled brush tips..."), this);
    layout->addWidget(message);
    auto *progress = new QProgressBar(this);
    progress->setRange(0, 0);
    layout->addWidget(progress);
    cancel = new QPushButton(tr("Cancel"), this);
    layout->addWidget(cancel);
    connect(cancel, &QPushButton::clicked, this,
            &SampledTipImportProgress::reject);
  }
  void reject() override {
    cancelled.store(true, std::memory_order_relaxed);
    message->setText(tr("Cancelling the current sampled-tip collection..."));
    cancel->setEnabled(false);
    // Keep the command modal until its child has been killed/reaped and its
    // private staging files retired. Esc cannot leave a live import behind.
  }
};

// Extracts exactly one complete sampled collection. No source preset/dynamics
// conversion is substituted for raw sample identity, dimensions or pixel data.
int importFile(const QString &abrPath, const QString &root,
               const QString &workerExecutable, QStringList &messages,
               const std::atomic_bool &cancelled) {
  auto stopped = [&] { return cancelled.load(std::memory_order_relaxed); };
  if (stopped()) return 0;
  const QString fileName = QFileInfo(abrPath).fileName();
  QString error;
  auto result = RasterBrush::importAbrTipsProcess(
      workerExecutable, abrPath, error, &cancelled);
  if (stopped()) return 0;
  if (!error.isEmpty() || !result.complete || result.records.empty()) {
    messages << QObject::tr("%1: %2").arg(fileName).arg(
        error.isEmpty() ? QObject::tr("No complete sampled-tip collection was returned.")
                        : error);
    return 0;
  }
  for (const auto &note : result.diagnostics)
    messages << QString("%1: %2").arg(fileName).arg(note);
  const QString collection = RasterBrush::legacyTipLibraryName(QFileInfo(abrPath).completeBaseName());
  if (!QDir().mkpath(root)) {
    messages << QObject::tr("%1: the folder %2 cannot be created.")
                    .arg(fileName).arg(root);
    return 0;
  }
  QTemporaryDir staging(root + "/.abr-import-XXXXXX");
  if (!staging.isValid()) {
    messages << QObject::tr("%1: the extraction staging folder cannot be created.")
                    .arg(fileName);
    return 0;
  }

  int written = 0;
  QStringList filenames;
  QSet<QString> usedNames;
  for (size_t i = 0; i < result.records.size(); ++i) {
    if (stopped()) return 0;
    const auto &record = result.records[i];
    if (record.provenance.value("conversionKind") !=
            QJsonValue("sampled-tip-extraction") ||
        record.provenance.value("sampledTipOrdinal") != QJsonValue(int(i)) ||
        record.provenance.value("sampledTipCount") !=
            QJsonValue(int(result.records.size())) ||
        record.preset.tip.shape != RasterBrush::Shape::Bitmap ||
        record.preset.tip.images.size() != 1 ||
        !record.preset.tip.images.front() ||
        record.preset.tip.images.front()->color) {
      messages << QObject::tr("%1: the sampled-tip response is incomplete; no tips were changed.")
                      .arg(fileName);
      return 0;
    }
    const auto &tip = *record.preset.tip.images.front();
    if (tip.width <= 0 || tip.height <= 0 ||
        size_t(tip.width) * size_t(tip.height) != tip.pixels.size()) {
      messages << QObject::tr("%1: the sampled-tip image is incomplete; no tips were changed.")
                      .arg(fileName);
      return 0;
    }
    QString name = RasterBrush::legacyTipLibraryName(QString::fromStdString(record.preset.name));
    if (!name.startsWith("tip_"))
      name = QString("%1 %2").arg(int(i) + 1, 3, 10, QChar('0')).arg(name);
    QImage image(tip.width, tip.height, QImage::Format_Grayscale8);
    if (image.isNull()) {
      messages << QObject::tr("%1: not enough memory to extract the sampled tips.")
                      .arg(fileName);
      return 0;
    }
    for (int y = 0; y < tip.height; ++y) {
      if (stopped()) return 0;
      unsigned char *line = image.scanLine(y);
      for (int x = 0; x < tip.width; ++x) {
        const double coverage = tip.pixels[size_t(y) * tip.width + x].a;
        line[x] = 255 - int(std::floor(coverage * 255 + .5));
      }
    }
    QString unique;
    QFile output;
    bool opened = false;
    for (int suffix = 1; suffix <= 10000; ++suffix) {
      if (stopped()) return 0;
      unique = suffix == 1 ? name : QString("%1 (%2)").arg(name).arg(suffix);
      const auto key = RasterBrush::legacyTipLibraryCollisionKey(unique);
      if (usedNames.contains(key)) continue;
      const QString path = staging.path() + "/" + unique + ".png";
      output.setFileName(path);
      if (output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        usedNames.insert(key);
        opened = true;
        break;
      }
      // Native filesystem equality can have additional aliases. Never let
      // PNG saving truncate one of the earlier staged images in that case.
      const QFileInfo conflict(path);
      if (!conflict.exists() && !conflict.isSymLink()) break;
    }
    if (!opened) {
      messages << QObject::tr("%1: %2 cannot be created without replacing another tip.")
                      .arg(fileName).arg(unique);
      return 0;
    }
    const bool encoded = image.save(&output, "PNG");
    const bool flushed = encoded && output.flush();
    output.close();
    if (!encoded || !flushed || output.error() != QFileDevice::NoError) {
      messages << QObject::tr("%1: %2 cannot be saved.")
                      .arg(fileName).arg(unique);
      return 0;
    }
    filenames << unique + ".png";
    ++written;
  }
  // Persist the exact parsed sample identity alongside its portable filename.
  // This file commits in the same rename as every PNG; it never stores the
  // source path, descriptor preset behavior or an inferred rights declaration.
  if (!RasterBrush::Detail::writeAbrTipCollectionManifest(
          staging.path(), result.records, filenames, error, &cancelled)) {
    if (!stopped())
      messages << QObject::tr("%1: sampled-tip identity could not be saved; no tips were changed.")
                      .arg(fileName) << error;
    return 0;
  }

  for (int n = 1; n <= 10000; ++n) {
    if (stopped()) return 0;
    const QString folder = root + "/" + collection +
                           (n == 1 ? QString() : QString(" (%1)").arg(n));
    const auto outcome = RasterBrush::Detail::publishLibraryEntryNoReplace(
        staging.path(), folder, error, &cancelled);
    if (outcome == RasterBrush::Detail::LibraryPublication::Published) {
      staging.setAutoRemove(false);
      // Publication is the linearization point. A later Cancel cannot claim
      // this complete, already visible collection was never installed.
      return written;
    }
    if (outcome == RasterBrush::Detail::LibraryPublication::Cancelled) return 0;
    if (outcome != RasterBrush::Detail::LibraryPublication::Exists) break;
  }
  messages << QObject::tr("%1: the sampled-tip collection could not be installed; no tips were changed.")
                  .arg(fileName);
  if (!error.isEmpty()) messages << QString("%1: %2").arg(fileName).arg(error);
  return 0;
}

}  // namespace

//=============================================================================

class ImportAbrCommand final : public MenuItemHandler {
public:
  ImportAbrCommand() : MenuItemHandler(MI_ImportAbr) {}
  void execute() override;
} importAbrCommand;

void ImportAbrCommand::execute() {
  static QString lastFolder;  // of the session
  QPointer<QMainWindow> owner(TApp::instance()->getMainWindow());
  if (!owner) return;

  QStringList paths = QFileDialog::getOpenFileNames(
      owner.data(),
      QObject::tr("Extract Sampled Tips from ABR"), lastFolder,
      QObject::tr("Photoshop brushes") + " (*.abr);;" +
          QObject::tr("All Files") + " (*)");
  if (paths.isEmpty() || !owner) return;
  lastFolder = QFileInfo(paths[0]).absolutePath();

  const QString root = ToonzFolder::getLibraryFolder().getQString() + "/brushtips";
  const QString executable = QCoreApplication::applicationFilePath();
  std::atomic_bool cancelled(false);
  // This stack dialog must not be QObject-owned by a window which can be
  // destroyed while its nested event loop is active.
  SampledTipImportProgress progress(cancelled);
  bool shuttingDown = false;
  const auto stopForShutdown = [&] {
    shuttingDown = true;
    progress.reject();
  };
  QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                   &progress, stopForShutdown);
  QObject::connect(owner.data(), &QObject::destroyed,
                   &progress, stopForShutdown);
  int written = 0;
  QStringList messages;
  std::unique_ptr<QThread> worker(QThread::create([&] {
    try {
      for (const QString &path : paths) {
        if (cancelled.load(std::memory_order_relaxed)) break;
        written += importFile(path, root, executable, messages, cancelled);
      }
    } catch (const std::exception &e) {
      messages << QObject::tr("Sampled-tip extraction failed: %1")
                      .arg(QString::fromUtf8(e.what()));
    } catch (...) {
      messages << QObject::tr("Sampled-tip extraction failed.");
    }
  }));
  bool completed = false;
  QObject::connect(worker.get(), &QThread::finished, &progress, [&] {
    completed = true;
    progress.accept();
  });
  worker->start();
  progress.exec();
  // An application shutdown or external accept can end the modal loop too.
  // Any exit before its finished callback must cancel before waiting. Do not
  // use isRunning(): normal finished-signal bookkeeping can still be active.
  if (!completed) cancelled.store(true, std::memory_order_relaxed);
  worker->wait();
  if (shuttingDown || !owner) return;
  if (cancelled.load(std::memory_order_relaxed))
    messages << QObject::tr("Extraction cancelled. No partial collection was installed; completed collections remain available.");

  QString text;
  if (written > 0)
    text =
        UiTranslations::text(QObject::tr(
            "%1 sampled brush tips were extracted into %2.\nChoose them in the "
            "\"Tip:\" option of the Brush tool on a raster level (select "
            "the tool again if it is already active)."))
            .arg(written)
            .arg(root);
  else
    text = QObject::tr("No sampled brush tips were extracted.");
  text += "\n\n" + QObject::tr("Source brush settings and dynamics were not imported.");
  if (!messages.isEmpty()) text += "\n\n" + messages.join("\n");

  if (written > 0)
    DVGui::info(text);
  else
    DVGui::warning(text);
}
