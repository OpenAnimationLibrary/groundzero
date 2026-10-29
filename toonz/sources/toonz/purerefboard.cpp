#include "purerefboard.h"
#include "processprogress.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>

namespace {

QString boardDirectory(const QString &source, const QString &root) {
  const QByteArray key = QCryptographicHash::hash(
      PureRefBoard::sourcePath(source).toUtf8(), QCryptographicHash::Sha256);
  return QDir(root).filePath(QString::fromLatin1(key.toHex()));
}

bool validGeneration(const QString &name) {
  return QRegularExpression("^import-[A-Za-z0-9]+$").match(name).hasMatch();
}

bool validImage(const QString &path, bool decode) {
  QImageReader reader(path, "png");
  const QSize size = reader.size();
  // Bound decoded storage, including corrupt or unreasonable export headers.
  if (!reader.canRead() || size.width() <= 0 || size.height() <= 0 ||
      qint64(size.width()) * size.height() > 64 * 1024 * 1024)
    return false;
  return !decode || !reader.read().isNull();
}

bool safeItemPath(const QString &path) {
  if (path == "overview.png") return true;
  if (!path.startsWith("images/")) return false;
  const QString name = path.mid(7);
  return !name.isEmpty() && name != "." && name != ".." &&
         !name.contains('/') && !name.contains('\\') &&
         name.endsWith(".png", Qt::CaseInsensitive);
}

}  // namespace

namespace PureRefBoard {

QString sourcePath(const QString &path) {
  // Do not require an existing file: the key must stay the same when a board
  // is temporarily unavailable (for example on a disconnected drive).
  return QDir::fromNativeSeparators(QFileInfo(path).absoluteFilePath());
}

QString executablePath(const QString &path) {
#ifdef Q_OS_MAC
  if (path.endsWith(".app", Qt::CaseInsensitive))
    return QDir(path).filePath("Contents/MacOS/PureRef");
#endif
  return path;
}

bool load(const QString &source, const QString &cacheRoot, Snapshot &snapshot,
          QString &error) {
  error.clear();
  const QString directory = boardDirectory(source, cacheRoot);
  QFile file(QDir(directory).filePath("manifest.json"));
  if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) {
    error = QObject::tr(
        "No saved PureRef import is available. Import the board again.");
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument document =
      QJsonDocument::fromJson(file.readAll(), &parseError);
  const QJsonObject object = document.object();
  const QString generation = object.value("generation").toString();
  if (parseError.error != QJsonParseError::NoError ||
      object.value("version").toInt() != 1 ||
      object.value("source").toString() != sourcePath(source) ||
      !validGeneration(generation)) {
    error = QObject::tr(
        "The saved PureRef import is invalid. Import the board again.");
    return false;
  }
  Snapshot result;
  result.source        = sourcePath(source);
  result.includeImages = object.value("includeImages").toBool();
  const QDir exports(QDir(directory).filePath(generation));
  for (const QJsonValue &value : object.value("items").toArray()) {
    const QJsonObject entry = value.toObject();
    const QString relative  = entry.value("file").toString();
    if (!safeItemPath(relative)) {
      error = QObject::tr(
          "The saved PureRef import contains an invalid image path.");
      return false;
    }
    const QString path = exports.filePath(relative);
    if (!validImage(path, false)) {
      error = QObject::tr(
          "A saved PureRef reference image is missing or unreadable.");
      return false;
    }
    result.items.append({relative, entry.value("label").toString(), path});
  }
  if (result.items.isEmpty() || result.items.first().id != "overview.png") {
    error = QObject::tr("The saved PureRef import has no board overview.");
    return false;
  }
  snapshot = result;
  return true;
}

bool importBoard(const QString &executable, const QString &source,
                 const QString &cacheRoot, bool includeImages,
                 Snapshot &snapshot, QString &error, QWidget *parent,
                 int timeoutMs) {
  error.clear();
  const QString input = sourcePath(source);
  const QFileInfo before(input);
  if (!before.isFile()) {
    error = QObject::tr("The PureRef board could not be found.");
    return false;
  }
  const qint64 sourceSize        = before.size();
  const QDateTime sourceModified = before.lastModified();
  // Semicolons delimit PureRef commands; its documented CLI specifies no
  // escape for them. Commas in load paths are escaped below as documented.
  const QString directory = boardDirectory(input, cacheRoot);
  if (input.contains(';') || directory.contains(';')) {
    error = QObject::tr(
        "PureRef command-line import cannot use paths containing a semicolon. "
        "Move or rename the board, or export images from PureRef manually.");
    return false;
  }
  if (!QDir().mkpath(directory)) {
    error = QObject::tr(
        "Unable to create the PureRef reference storage directory.");
    return false;
  }
  QLockFile lock(QDir(directory).filePath("import.lock"));
  if (!lock.tryLock()) {
    error = QObject::tr("This PureRef board is already being imported.");
    return false;
  }
  QTemporaryDir staging(QDir(directory).filePath("import-XXXXXX"));
  if (!staging.isValid() || !QDir(staging.path()).mkdir("images")) {
    error = QObject::tr("Unable to create the PureRef export directory.");
    return false;
  }
  // A private settings file isolates the export from the user's open boards,
  // last-opened scene and single-window preferences. Never send exit to an
  // interactive PureRef instance.
  const QString settings = QDir(staging.path()).filePath("PureRef.ini");
  QFile settingsFile(settings);
  if (!settingsFile.open(QIODevice::WriteOnly)) {
    error = QObject::tr("Unable to create the PureRef import settings.");
    return false;
  }
  settingsFile.close();
  const QString overview = QDir(staging.path()).filePath("overview.png");
  QString escapedInput   = input;
  escapedInput.replace(",", ",,");
  QStringList arguments;
  arguments << "-s" << settings << "-c"
            << "load;" + escapedInput << "-c"
            << "exportScene;" + overview + ";4096;4096;true;false;true";
  if (includeImages)
    arguments << "-c"
              << "exportImages;" + QDir(staging.path()).filePath("images") +
                     ";true;%2 - %0";
  arguments << "-c"
            << "exit";

  QProcess process;
  process.setWorkingDirectory(before.absolutePath());
  // Do not accumulate unbounded external-program output in memory.
  process.setStandardOutputFile(QProcess::nullDevice());
  process.setStandardErrorFile(QProcess::nullDevice());
  process.start(executablePath(executable), arguments);
  const ProcessProgress::Result wait = ProcessProgress::wait(
      process, QObject::tr("Importing PureRef board..."), parent, timeoutMs);
  if (wait == ProcessProgress::Canceled) return false;
  if (wait == ProcessProgress::TimedOut) {
    error = QObject::tr(
        "PureRef import timed out. The previous reference images were kept.");
    return false;
  }
  if (process.error() == QProcess::FailedToStart ||
      process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    error = QObject::tr(
        "PureRef could not export the board. Check the application path and "
        "try opening the board in PureRef.");
    return false;
  }
  const QFileInfo after(input);
  if (!after.isFile() || sourceSize != after.size() ||
      sourceModified != after.lastModified()) {
    error = QObject::tr(
        "The PureRef board changed during import. Save it and refresh again.");
    return false;
  }

  QStringList files;
  files << "overview.png";
  const QDir images(QDir(staging.path()).filePath("images"));
  if (includeImages) {
    const QStringList names =
        images.entryList({"*.png"}, QDir::Files, QDir::Name);
    for (const QString &name : names) files << "images/" + name;
  }
  QJsonArray items;
  Snapshot result;
  result.source        = input;
  result.includeImages = includeImages;
  for (const QString &relative : files) {
    const QString path = QDir(staging.path()).filePath(relative);
    if (!safeItemPath(relative) || !validImage(path, true)) {
      error = QObject::tr(
          "PureRef did not export readable reference images. The previous "
          "import was kept.");
      return false;
    }
    const QString label = relative == "overview.png"
                              ? QObject::tr("Board overview")
                              : QFileInfo(relative).completeBaseName();
    items.append(QJsonObject{{"file", relative}, {"label", label}});
    result.items.append({relative, label, path});
  }
  QJsonObject manifest{{"version", 1},
                       {"source", input},
                       {"generation", QFileInfo(staging.path()).fileName()},
                       {"includeImages", includeImages},
                       {"items", items}};
  // Read the old generation only for cleanup after a successful publication.
  QFile oldFile(QDir(directory).filePath("manifest.json"));
  QString oldGeneration;
  if (oldFile.open(QIODevice::ReadOnly))
    oldGeneration = QJsonDocument::fromJson(oldFile.read(16 * 1024 * 1024))
                        .object()
                        .value("generation")
                        .toString();
  oldFile.close();
  const QByteArray bytes = QJsonDocument(manifest).toJson();
  QSaveFile output(QDir(directory).filePath("manifest.json"));
  if (!output.open(QIODevice::WriteOnly) ||
      output.write(bytes) != bytes.size() || !output.commit()) {
    error = QObject::tr(
        "Unable to save the PureRef import. The previous import was kept.");
    return false;
  }
  staging.setAutoRemove(false);
  if (validGeneration(oldGeneration) &&
      oldGeneration != QFileInfo(staging.path()).fileName())
    QDir(QDir(directory).filePath(oldGeneration)).removeRecursively();
  snapshot = result;
  return true;
}

}  // namespace PureRefBoard
