#include "exportfidelity.h"
// File > Export > Export Lottie...: writes the current xsheet as a Lottie
// animation (Bodymovin JSON), for lottie-web and the iOS and Android players.
// The export itself is LottieExport::exportXsheet() (toonzlib,
// toonz/lottieexporter.h); this file adds the command, its options (frame
// range, frame rate, embedded images), the PNG rendering of raster drawings
// and the files. See doc/animate_lottie_export.md.

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"
#include "filebrowserpopup.h"
#include "exportlevelcommand.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/gutil.h"
#include "toonzqt/uitranslations.h"

// TnzLib includes
#include "toonz/lottieexporter.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/sceneproperties.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txsheet.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelproperties.h"
#include "toutputproperties.h"

// TnzCore includes
#include "tsystem.h"
#include "trop.h"
#include "trasterimage.h"

// Qt includes
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QGridLayout>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QSpinBox>
#include <QUrl>

// STD includes
#include <algorithm>

namespace {

/*!
Renders a raster drawing (Toonz Raster or Raster level) as a PNG image at the
level resolution. The conversion to full color is the one of renderDrawing()
in spineexport.cpp (copied, not shared, so that the Spine export stays as it
is); the image is not resampled to the camera resolution, the Lottie layer
transform scales it.
*/
bool renderRaster(TXshSimpleLevel *sl, const TFrameId &fid,
                  LottieExport::RasterImage &image) {
  if (!sl) return false;
  TRasterImageP ri = IoCmd::exportedImage("png", *sl, fid);
  if (!ri || !ri->getRaster()) return false;

  // Work on a full color copy: the image may be the cached one
  TRasterP src = ri->getRaster();
  TRaster32P ras(src->getSize());
  try {
    TRaster32P srcFullColor = src;
    if (srcFullColor)
      ras->copy(srcFullColor);
    else
      TRop::convert(ras, src);
  } catch (...) {
    return false;
  }
  if (sl->getType() == OVL_XSHLEVEL) {
    LevelProperties *prop = sl->getProperties();
    if (prop->whiteTransp()) TRop::whiteTransp(ras);
    if (prop->doPremultiply()) TRop::premultiply(ras);
  }

  QImage img = rasterToQImage(ras);
  if (img.isNull()) return false;
  QBuffer buffer(&image.m_png);
  if (!buffer.open(QIODevice::WriteOnly) || !img.save(&buffer, "PNG"))
    return false;

  TPoint offset  = ri->getOffset();
  image.m_width  = ras->getLx();
  image.m_height = ras->getLy();
  image.m_offset = TPointD(offset.x, offset.y);
  return !image.m_png.isEmpty();
}

//=============================================================================

class ExportLottieCommand final : public MenuItemHandler {
public:
  ExportLottieCommand() : MenuItemHandler(MI_ExportLottie) {}
  void execute() override;
};

//-----------------------------------------------------------------------------

void ExportLottieCommand::execute() {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  TXsheet *xsh      = TApp::instance()->getCurrentXsheet()->getXsheet();
  TFilePath fp      = scene->getScenePath().withType("json");

  int frameCount = xsh->getFrameCount();
  if (frameCount <= 0) {
    DVGui::error(QObject::tr("No columns can be exported."));
    return;
  }

  // Default frame range and frame rate: the output settings of the scene
  TOutputProperties *oprop = scene->getProperties()->getOutputProperties();
  int r0 = 0, r1 = frameCount - 1, step = 1;
  if (scene->getTopXsheet() != xsh || !oprop->getRange(r0, r1, step) || r1 < r0)
    r0 = 0, r1 = frameCount - 1;
  r0         = std::max(0, std::min(r0, frameCount - 1));
  r1         = std::max(r0, std::min(r1, frameCount - 1));
  double fps = oprop->getFrameRate();
  if (!(fps > 0.0)) fps = 24.0;

  static GenericSaveFilePopup *savePopup = nullptr;
  static QSpinBox *fromField             = nullptr;
  static QSpinBox *toField               = nullptr;
  static QDoubleSpinBox *fpsField        = nullptr;
  static QCheckBox *embedImages          = nullptr;

  if (!savePopup) {
    fromField   = new QSpinBox();
    toField     = new QSpinBox();
    fpsField    = new QDoubleSpinBox();
    embedImages = new QCheckBox(QObject::tr("Embed Raster Levels as PNG Data"));
    embedImages->setObjectName("lottieEmbedImages");
    fpsField->setRange(1.0, 240.0);
    fpsField->setDecimals(3);
    embedImages->setChecked(true);
    embedImages->setToolTip(
        QObject::tr("Checked: the images of the raster levels are stored in "
                    "the JSON file\nUnchecked: they are saved as PNG files "
                    "in a folder next to it"));

    QWidget *customWidget = new QWidget();
    QGridLayout *layout   = new QGridLayout(customWidget);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(5);
    auto *fromLabel = new QLabel(QObject::tr("From Frame:"));
    auto *toLabel = new QLabel(QObject::tr("To Frame:"));
    auto *rateLabel = new QLabel(QObject::tr("Frame Rate:"));
    layout->addWidget(fromLabel, 0, 0);
    layout->addWidget(fromField, 0, 1);
    layout->addWidget(toLabel, 0, 2);
    layout->addWidget(toField, 0, 3);
    layout->addWidget(rateLabel, 0, 4);
    layout->addWidget(fpsField, 0, 5);
    layout->addWidget(embedImages, 1, 0, 1, 6);
    layout->setColumnStretch(6, 1);

    savePopup =
        new GenericSaveFilePopup(QObject::tr("Export Lottie"), customWidget);
    savePopup->setObjectName("lottieExportFilePopup");
    savePopup->addFilterType("json");
    // The save popup is retained between exports. Only captions are bound;
    // frame values, the filename and the selected folder remain user state.
    UiTranslations::bind(savePopup, "windowTitle", "QObject", "Export Lottie");
    UiTranslations::bind(fromLabel, "text", "QObject", "From Frame:");
    UiTranslations::bind(toLabel, "text", "QObject", "To Frame:");
    UiTranslations::bind(rateLabel, "text", "QObject", "Frame Rate:");
    UiTranslations::bind(embedImages, "text", "QObject",
                         "Embed Raster Levels as PNG Data");
    UiTranslations::bind(embedImages, "toolTip", "QObject",
                         "Checked: the images of the raster levels are stored in "
                         "the JSON file\nUnchecked: they are saved as PNG files "
                         "in a folder next to it");
  }
  // Frames are shown from 1, as in the xsheet
  fromField->setRange(1, frameCount);
  toField->setRange(1, frameCount);
  fromField->setValue(r0 + 1);
  toField->setValue(r1 + 1);
  fpsField->setValue(fps);

  if (!scene->isUntitled())
    savePopup->setFolder(fp.getParentDir());
  else
    savePopup->setFolder(
        TProjectManager::instance()->getCurrentProject()->getScenesPath());
  savePopup->setFilename(fp.withoutParentDir());
  fp = savePopup->getPath();
  if (fp.isEmpty()) return;
  fp = fp.withType("json");

  LottieExport::Options options;
  options.m_r0           = std::min(fromField->value(), toField->value()) - 1;
  options.m_r1           = std::max(fromField->value(), toField->value()) - 1;
  options.m_frameRate    = fpsField->value();
  options.m_name         = QString::fromStdString(fp.getName());
  options.m_embedImages  = embedImages->isChecked();
  options.m_imagesFolder = QString::fromStdString(fp.getName()) + "_images/";
  options.m_renderRaster = renderRaster;

  QApplication::setOverrideCursor(Qt::WaitCursor);
  LottieExport::Result result = LottieExport::exportXsheet(xsh, options);
  QApplication::restoreOverrideCursor();

  if (result.m_contentLayers == 0) {
    DVGui::error(QObject::tr("No columns can be exported."));
    return;
  }

  // The renderer has only prepared memory buffers so far. Confirm fidelity
  // losses before creating any output directory or replacing destination files.
  if (!ExportFidelity::confirmLottie(result.m_skipped)) return;

  // Image files
  if (!result.m_imageFiles.empty()) {
    TFilePath imagesDir =
        fp.getParentDir() +
        TFilePath(QString::fromStdString(fp.getName()) + "_images");
    if (!QDir(imagesDir.getQString()).mkpath(".")) {
      DVGui::error(QObject::tr("Unable to create folder for saving images."));
      return;
    }
    for (const LottieExport::ImageFile &image : result.m_imageFiles) {
      QFile file(QDir(imagesDir.getQString()).filePath(image.m_fileName));
      if (!file.open(QIODevice::WriteOnly) ||
          file.write(image.m_data) != image.m_data.size()) {
        DVGui::error(QObject::tr("Unable to save %1.").arg(image.m_fileName));
        return;
      }
    }
  }

  // The animation
  QFile jsonFile(fp.getQString());
  if (!jsonFile.open(QIODevice::WriteOnly) ||
      jsonFile.write(
          QJsonDocument(result.m_animation).toJson(QJsonDocument::Compact)) <
          0) {
    DVGui::error(QObject::tr("Unable to save %1.").arg(fp.getQString()));
    return;
  }
  jsonFile.close();

  QString str = QObject::tr("%1 has been exported successfully.")
                    .arg(QString::fromStdString(fp.getLevelName()));
  if (!result.m_skipped.isEmpty())
    str += "\n\n" + QObject::tr("Not exported:") + "\n- " +
           result.m_skipped.join("\n- ");
  std::vector<QString> buttons = {QObject::tr("OK"),
                                  QObject::tr("Open containing folder")};
  if (DVGui::MsgBox(
          result.m_skipped.isEmpty() ? DVGui::INFORMATION : DVGui::WARNING, str,
          buttons) == 2) {
    TFilePath folderPath = fp.getParentDir();
    if (TSystem::isUNC(folderPath))
      QDesktopServices::openUrl(QUrl(folderPath.getQString()));
    else
      QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath.getQString()));
  }
}

ExportLottieCommand exportLottieCommand;

}  // namespace
