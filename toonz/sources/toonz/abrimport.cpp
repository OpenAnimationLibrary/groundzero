// Import Photoshop Brushes (ABR): reads the sampled brush tips of .abr files
// (see toonz/abrparser.h for what is supported) and writes each tip as a
// grayscale PNG into <library>/brushtips/<name of the .abr file>/. The Brush
// tool on raster levels (full color brush) lists these images in its "Tip:"
// option; any PNG put in <library>/brushtips by hand is listed too.
//
// The PNG of a tip is black where the tip paints and white where it does not
// (the coverage of the ABR file, inverted), like the brush tip images of
// other painting programs.

#include "tapp.h"
#include "menubarcommandids.h"

#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"

#include "toonz/abrparser.h"
#include "toonz/toonzfolders.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QSet>
#include <QStringList>

namespace {

// Name usable as a file or folder name on every platform
QString sanitizedName(const QString &name) {
  QString result;
  for (const QChar &c : name) {
    if (c.unicode() < 32 || QString("\\/:*?\"<>|").contains(c))
      result += '_';
    else
      result += c;
  }
  result = result.trimmed();
  while (result.endsWith('.')) result.chop(1);
  return result.isEmpty() ? QString("brushes") : result;
}

//-----------------------------------------------------------------------------

// Writes the tips of one file. Returns the number of images written.
int importFile(const QString &abrPath, QStringList &messages) {
  QString fileName = QFileInfo(abrPath).fileName();

  QFile file(abrPath);
  if (!file.open(QIODevice::ReadOnly)) {
    messages << QObject::tr("%1: the file cannot be read.").arg(fileName);
    return 0;
  }
  QByteArray bytes = file.readAll();
  file.close();

  AbrParser::Result result;
  std::string error;
  if (!AbrParser::parseAbr((const unsigned char *)bytes.constData(),
                           (size_t)bytes.size(), result, error)) {
    messages << QObject::tr("%1: %2").arg(fileName).arg(
        QString::fromStdString(error));
    return 0;
  }
  if (result.tips.empty()) {
    messages << QObject::tr("%1: no sampled brushes found.").arg(fileName);
  }

  QString folder = ToonzFolder::getLibraryFolder().getQString() +
                   "/brushtips/" +
                   sanitizedName(QFileInfo(abrPath).completeBaseName());
  if (!result.tips.empty() && !QDir().mkpath(folder)) {
    messages << QObject::tr("%1: the folder %2 cannot be created.")
                    .arg(fileName)
                    .arg(folder);
    return 0;
  }

  int written = 0;
  QSet<QString> usedNames;
  for (size_t i = 0; i < result.tips.size(); ++i) {
    const AbrParser::Tip &tip = result.tips[i];

    // keep the order of the file; the names of the file may repeat
    QString name = sanitizedName(QString::fromStdString(tip.name));
    if (!name.startsWith("tip_"))
      name = QString("%1 %2").arg((int)i + 1, 3, 10, QChar('0')).arg(name);
    QString unique = name;
    for (int n = 2; usedNames.contains(unique.toLower()); ++n)
      unique = QString("%1 (%2)").arg(name).arg(n);
    usedNames.insert(unique.toLower());

    QImage image(tip.width, tip.height, QImage::Format_Grayscale8);
    for (int y = 0; y < tip.height; ++y) {
      unsigned char *line      = image.scanLine(y);
      const unsigned char *src = &tip.pixels[(size_t)y * tip.width];
      for (int x = 0; x < tip.width; ++x) line[x] = 255 - src[x];
    }
    if (image.save(folder + "/" + unique + ".png", "PNG"))
      ++written;
    else
      messages
          << QObject::tr("%1: %2 cannot be saved.").arg(fileName).arg(unique);
  }

  if (result.computedBrushes > 0)
    messages << QObject::tr(
                    "%1: %2 computed (round) brushes have no image "
                    "and were skipped.")
                    .arg(fileName)
                    .arg(result.computedBrushes);
  if (result.skippedSamples > 0)
    messages << QObject::tr(
                    "%1: %2 brushes could not be read and were "
                    "skipped.")
                    .arg(fileName)
                    .arg(result.skippedSamples);
  for (const std::string &w : result.warnings)
    messages << QString("%1: %2").arg(fileName).arg(QString::fromStdString(w));
  return written;
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

  QStringList paths = QFileDialog::getOpenFileNames(
      TApp::instance()->getMainWindow(),
      QObject::tr("Import Photoshop Brushes (ABR)"), lastFolder,
      QObject::tr("Photoshop brushes") + " (*.abr);;" +
          QObject::tr("All Files") + " (*)");
  if (paths.isEmpty()) return;
  lastFolder = QFileInfo(paths[0]).absolutePath();

  int written = 0;
  QStringList messages;
  for (const QString &path : paths) written += importFile(path, messages);

  QString text;
  if (written > 0)
    text =
        QObject::tr(
            "%1 brush tips were imported into %2.\nChoose them in the "
            "\"Tip:\" option of the Brush tool on a raster level (select "
            "the tool again if it is already active).")
            .arg(written)
            .arg(ToonzFolder::getLibraryFolder().getQString() + "/brushtips");
  else
    text = QObject::tr("No brush tips were imported.");
  if (!messages.isEmpty()) text += "\n\n" + messages.join("\n");

  if (written > 0)
    DVGui::info(text);
  else
    DVGui::warning(text);
}
