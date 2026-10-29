// Uses this executable as an external exporter, exercising process isolation,
// cancellation and transactional publication without requiring PureRef in CI.
// Real PureRef compatibility is a separate manual acceptance check.
#include "purerefboard.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProgressDialog>
#include <QPushButton>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <iostream>

namespace {
int failures = 0;

void check(bool condition, const char *message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << std::endl;
  ++failures;
}

void write(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  check(file.open(QIODevice::WriteOnly), "open fixture");
  check(file.write(bytes) == bytes.size(), "write fixture");
}

QByteArray read(const QString &path) {
  QFile file(path);
  check(file.open(QIODevice::ReadOnly), "read fixture");
  return file.readAll();
}

int exporter(const QStringList &arguments) {
  if (arguments.size() < 5 || arguments[1] != "-s" ||
      !QFileInfo::exists(arguments[2]))
    return 10;
  QString mode;
  for (int i = 3; i + 1 < arguments.size(); i += 2) {
    if (arguments[i] != "-c") return 11;
    const QStringList command = arguments[i + 1].split(';');
    if (command[0] == "load") {
      QString source;
      const QString escaped = command.value(1);
      for (int j = 0; j < escaped.size(); ++j) {
        source += escaped[j];
        if (escaped[j] == ',') {
          if (j + 1 >= escaped.size() || escaped[j + 1] != ',') return 12;
          ++j;
        }
      }
      QFile input(source);
      if (!input.open(QIODevice::ReadOnly)) return 13;
      mode = QString::fromUtf8(input.readAll());
      if (mode == "fail") return 3;
      if (mode == "hang") QThread::sleep(60);
      if (mode == "changed") {
        input.close();
        write(source, "changed-during-export");
      }
    } else if (command[0] == "exportScene") {
      if (mode == "missing") continue;
      QImage image(32, 16, QImage::Format_ARGB32);
      image.fill(Qt::red);
      if (!image.save(command.value(1), "PNG")) return 14;
      if (mode == "corrupt") write(command.value(1), "not an image");
    } else if (command[0] == "exportImages") {
      if (!QDir(command.value(1)).exists() || command.value(2) != "true")
        return 15;
      for (int j = 0; j < 80; ++j) {
        QImage image(16, 8, QImage::Format_ARGB32);
        image.fill(j % 2 ? Qt::blue : Qt::green);
        const QString name =
            QString("%1 - reference.png").arg(j, 3, 10, QChar('0'));
        if (!image.save(QDir(command[1]).filePath(name), "PNG")) return 16;
      }
    } else if (command[0] != "exit")
      return 17;
  }
  return 0;
}
}  // namespace

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);
  if (app.arguments().contains("-s")) return exporter(app.arguments());

  QTemporaryDir temporary;
  check(temporary.isValid(), "temporary directory");
  const QString source =
      QDir(temporary.path())
          .filePath(QString::fromUtf8("board with spaces, caf\xc3\xa9.pur"));
  const QString root       = QDir(temporary.path()).filePath("references");
  const QString executable = QCoreApplication::applicationFilePath();
  PureRefBoard::Snapshot snapshot, restored;
  QString error;
  write(source, "ok");
  check(PureRefBoard::importBoard(executable, source, root, true, snapshot,
                                  error),
        "import board with escaped comma, spaces and Unicode");
  if (!error.isEmpty()) std::cerr << error.toStdString() << std::endl;
  check(snapshot.items.size() == 81,
        "all 80 images plus overview survive import");
  check(read(source) == "ok", "source board is unchanged");
  check(PureRefBoard::load(source, root, restored, error),
        "restore saved import");
  check(restored.items.size() == 81 && restored.includeImages,
        "all members survive restart");
  if (snapshot.items.isEmpty()) return 1;
  const QString firstOverview = snapshot.items.first().path;
  check(QFile::remove(source), "remove unavailable source");
  check(PureRefBoard::load(source, root, restored, error),
        "cached references work without original board or exporter");

  for (const QByteArray &mode :
       {QByteArray("fail"), QByteArray("missing"), QByteArray("corrupt"),
        QByteArray("changed")}) {
    write(source, mode);
    check(!PureRefBoard::importBoard(executable, source, root, true, snapshot,
                                     error),
          "failed/incomplete/changed exports are rejected");
    check(!error.isEmpty(), "failed export has an explanation");
    check(PureRefBoard::load(source, root, restored, error) &&
              restored.items.first().path == firstOverview,
          "failure preserves previous complete generation");
  }
  write(source, "hang");
  check(!PureRefBoard::importBoard(executable, source, root, true, snapshot,
                                   error, nullptr, 300),
        "export times out");
  check(!error.isEmpty(), "timeout explained");
  QTimer::singleShot(300, []() {
    for (QWidget *widget : QApplication::topLevelWidgets())
      if (auto *dialog = qobject_cast<QProgressDialog *>(widget))
        if (auto *button = dialog->findChild<QPushButton *>()) button->click();
  });
  check(!PureRefBoard::importBoard(executable, source, root, true, snapshot,
                                   error),
        "user can cancel export");
  check(error.isEmpty(), "cancel is quiet");
  check(PureRefBoard::load(source, root, restored, error) &&
            restored.items.first().path == firstOverview,
        "cancel and timeout preserve previous generation");
  write(source, "ok");
  check(!PureRefBoard::importBoard(QDir(temporary.path()).filePath("absent"),
                                   source, root, false, snapshot, error),
        "missing executable is rejected");
  check(PureRefBoard::importBoard(executable, source, root, false, snapshot,
                                  error),
        "overview-only refresh succeeds after failed jobs");
  check(snapshot.items.size() == 1 && !snapshot.includeImages,
        "overview-only import has one member");
  check(!QFileInfo::exists(firstOverview), "replaced generation is cleaned up");

  // Untrusted/incomplete manifests cannot point outside the export directory.
  const QDir boardDir(QFileInfo(snapshot.items.first().path).absolutePath());
  const QString manifest =
      QDir(QFileInfo(boardDir.path()).absolutePath()).filePath("manifest.json");
  QJsonObject data   = QJsonDocument::fromJson(read(manifest)).object();
  data["generation"] = "../outside";
  write(manifest, QJsonDocument(data).toJson());
  check(!PureRefBoard::load(source, root, restored, error),
        "unsafe generation rejected");
  data["generation"] = boardDir.dirName();
  data["items"] = QJsonArray{QJsonObject{{"file", "images/../outside.png"}}};
  write(manifest, QJsonDocument(data).toJson());
  check(!PureRefBoard::load(source, root, restored, error),
        "unsafe item rejected");

  if (failures) return 1;
  std::cout << "PureRef board import tests passed" << std::endl;
  return 0;
}
