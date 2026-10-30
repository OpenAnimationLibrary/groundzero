#include "toonz/rasterbrushimport.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QTemporaryDir>
#include <set>
#ifndef _WIN32
#include <sys/resource.h>
#endif

namespace RasterBrush {
namespace {
constexpr qint64 packageBudget  = 256 * 1024 * 1024;
constexpr qint64 manifestBudget = 1024 * 1024;
bool writeFile(const QString &path, const QByteArray &bytes) {
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() &&
         file.commit();
}
}  // namespace

int runImportWorker(const QString &input, const QString &directory) {
  // A private process contains crashes and can be killed on cancellation.
  // It is not a filesystem/network privilege sandbox.
#ifndef _WIN32
  rlimit noCore{0, 0};
  if (setrlimit(RLIMIT_CORE, &noCore) != 0) return 2;
  rlimit cpu{60, 60};
  if (setrlimit(RLIMIT_CPU, &cpu) != 0) return 2;
#endif
  try {
    QDir output(directory);
    if (!output.exists() || QFileInfo(directory).isSymLink() ||
        !output
             .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
             .empty())
      return 2;
    QString error;
    auto imported = importBrushFile(input, error);
    if (!error.isEmpty() || !imported.complete || imported.records.empty() ||
        imported.records.size() > 256) {
      writeFile(output.filePath("error.txt"), error.left(4096).toUtf8());
      return 1;
    }
    qint64 total = 0;
    for (size_t i = 0; i < imported.records.size(); ++i) {
      auto bytes = encode(imported.records[i], error);
      total += bytes.size();
      if (!error.isEmpty() || total > packageBudget ||
          !writeFile(output.filePath(QString::number(i) + ".gzbrush"), bytes))
        return 2;
    }
    QJsonArray diagnostics;
    for (const auto &message : imported.diagnostics)
      diagnostics.append(message);
    QByteArray manifest =
        QJsonDocument(QJsonObject{{"protocol", 1},
                                  {"count", int(imported.records.size())},
                                  {"diagnostics", diagnostics}})
            .toJson(QJsonDocument::Compact);
    if (manifest.size() > manifestBudget ||
        !writeFile(output.filePath("result.json"), manifest))
      return 2;
    return 0;
  } catch (...) {
    return 2;
  }
}

ImportResult importBrushProcess(const QString &executable, const QString &path,
                                QString &error,
                                const std::atomic_bool *cancelled) {
  error.clear();
  auto stopped = [&] {
    return cancelled && cancelled->load(std::memory_order_relaxed);
  };
  if (stopped()) return {};
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = "Cannot create isolated brush import directory";
    return {};
  }
  QProcess process;
  process.setProgram(executable);
  process.setArguments({"--raster-brush-import-worker",
                        QFileInfo(path).absoluteFilePath(), directory.path()});
  process.setWorkingDirectory(directory.path());
  process.start();
  if (!process.waitForStarted(5000)) {
    error = "Cannot start brush import worker: " + process.errorString();
    return {};
  }
  QElapsedTimer elapsed;
  elapsed.start();
  while (process.state() != QProcess::NotRunning) {
    process.waitForFinished(50);
    // Drain diagnostics without retaining unbounded decoder output.
    process.readAllStandardOutput();
    process.readAllStandardError();
    if (stopped() || elapsed.elapsed() > 120000) {
      process.kill();
      process.waitForFinished(5000);
      if (!stopped()) error = "Brush import exceeded the 120-second limit";
      return {};
    }
  }
  if (stopped()) return {};
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    QFile diagnostic(directory.filePath("error.txt"));
    if (diagnostic.open(QIODevice::ReadOnly))
      error = QString::fromUtf8(diagnostic.read(16384));
    if (error.isEmpty())
      error = "Brush import worker failed or exceeded its resource limits";
    return {};
  }
  QFile manifest(directory.filePath("result.json"));
  if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > manifestBudget) {
    error = "Invalid brush import result";
    return {};
  }
  auto json   = QJsonDocument::fromJson(manifest.read(manifestBudget + 1));
  auto object = json.object();
  int count   = object.value("count").toInt(-1);
  if (!json.isObject() || object.value("protocol").toInt() != 1 || count < 1 ||
      count > 256 || !object.value("diagnostics").isArray()) {
    error = "Invalid brush import result protocol";
    return {};
  }
  ImportResult result;
  qint64 total  = 0;
  size_t pixels = 0;
  for (int i = 0; i < count; ++i) {
    if (stopped()) return {};
    const QString package = directory.filePath(QString::number(i) + ".gzbrush");
    QFileInfo info(package);
    total += info.size();
    if (info.isSymLink() || !info.isFile() || total > packageBudget) {
      error = "Brush import result exceeded its package budget";
      return {};
    }
    auto record = load(package, error);
    if (!error.isEmpty()) return {};
    std::set<const Image *> resources;
    auto account = [&](const std::shared_ptr<const Image> &image) {
      if (image && resources.insert(image.get()).second)
        pixels += image->pixels.size();
    };
    for (const auto &image : record.preset.tip.images) account(image);
    for (const auto &image : record.preset.secondary.tip.images) account(image);
    account(record.preset.texture.image);
    if (pixels > size_t(packageBudget) / sizeof(Pixel)) {
      error = "Brush import result exceeded its decoded image budget";
      return {};
    }
    result.records.push_back(std::move(record));
  }
  for (const auto &message : object.value("diagnostics").toArray()) {
    if (!message.isString()) {
      error = "Invalid brush import diagnostic";
      return {};
    }
    result.diagnostics << message.toString();
  }
  result.complete = true;
  return result;
}
}  // namespace RasterBrush
