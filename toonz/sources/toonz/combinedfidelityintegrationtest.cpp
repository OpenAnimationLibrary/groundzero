#include "combinedfidelityintegrationtest.h"
#include "nativeuiintegrationtest.h"
#include "exportfidelity.h"
#include "filebrowserpopup.h"
#include "iocommand.h"
#include "menubarcommandids.h"
#include "tapp.h"
#include "tcolorstyles.h"
#include "tdoubleparam.h"
#include "tlevel_io.h"
#include "tpalette.h"
#include "tstroke.h"
#include "tundo.h"
#include "tvectorimage.h"
#include "toonz/levelset.h"
#include "toonz/lottieexporter.h"
#include "toonz/morphhints.h"
#include "toonz/scenechecker.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/menubarcommand.h"
#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMap>
#include <QRectF>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QPainter>
#include <QPushButton>
#include <QSvgRenderer>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QXmlStreamReader>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
QByteArray readFile(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Combined fidelity output file missing");
  return file.readAll();
}
struct ExportChoice {
  QString button;
  bool assetBlocked = false;
  bool embed = true;
  bool renderDirectly = false;
  QString expectedText;
};
void exportCommand(const char *command, const QString &destination,
                   const ExportChoice &choice) {
  bool selected = false, decided = false;
  QString error;
  QTimer timer, deadline;
  QObject::connect(&timer, &QTimer::timeout, [&]() {
    auto *modal = QApplication::activeModalWidget();
    if (!modal || dynamic_cast<DVGui::ProgressDialog *>(modal)) return;
    if (auto *file = dynamic_cast<GenericSaveFilePopup *>(modal)) {
      if (selected) return;
      selected = true;
      file->setFolder(TFilePath(QFileInfo(destination).absolutePath()));
      file->setFilename(TFilePath(QFileInfo(destination).fileName()));
      if (auto *check = file->findChild<QCheckBox *>("svgRenderScene")) check->setChecked(choice.renderDirectly);
      if (auto *check = file->findChild<QCheckBox *>("svgEmbedImages")) check->setChecked(choice.embed);
      if (auto *check = file->findChild<QCheckBox *>("svgOpenBrowser")) check->setChecked(false);
      if (auto *check = file->findChild<QCheckBox *>("svgRasterizeVectors")) check->setChecked(false);
      QTimer::singleShot(0, file, &FileBrowserPopup::onOkPressed);
      return;
    }
    auto *dialog = qobject_cast<QDialog *>(modal);
    if (!dialog) return;
    if (dialog->objectName() == "exportFidelityDialog") {
      auto *details = dialog->findChild<QTextEdit *>("exportFidelityDetails");
      auto *assets = dialog->findChild<QPushButton *>("exportSupportedContent");
      auto *button = dialog->findChild<QPushButton *>(choice.button);
      if (decided || !details || !details->toPlainText().contains(choice.expectedText) ||
          !assets || assets->isEnabled() == choice.assetBlocked || !button || !button->isEnabled()) {
        error = "Combined fidelity choices or explanation do not match the drawing capabilities";
        dialog->reject();
      } else { decided = true; button->click(); }
      return;
    }
    for (auto *button : dialog->findChildren<QPushButton *>()) {
      const QString text = button->text().remove('&');
      if (button->isVisible() && (text == QObject::tr("Overwrite") || text == QObject::tr("OK"))) {
        button->click(); return;
      }
    }
    error = "Unexpected native export dialog";
    dialog->reject();
  });
  QObject::connect(&deadline, &QTimer::timeout, [&]() {
    error = "Combined export dialog deadline exceeded";
    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
  });
  timer.start(10); deadline.setSingleShot(true); deadline.start(45000);
  CommandManager::instance()->execute(command);
  timer.stop(); deadline.stop();
  require(selected && (decided || choice.renderDirectly), "Combined native exporter was not exercised");
  if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
}
QList<QByteArray> embedded(const QByteArray &svg, const QString &mime) {
  QList<QByteArray> result;
  QXmlStreamReader xml(svg);
  const QString prefix = QStringLiteral("data:") + mime + QStringLiteral(";base64,");
  while (!xml.atEnd()) {
    xml.readNext();
    if (!xml.isStartElement()) continue;
    for (const auto &attribute : xml.attributes()) {
      const QString value = attribute.value().toString();
      if (attribute.name() == "href" && value.startsWith(prefix))
        result.append(QByteArray::fromBase64(value.mid(prefix.size()).toLatin1()));
    }
  }
  require(!xml.hasError(), "Exported animation XML is malformed");
  return result;
}
qint64 alphaSum(const QImage &image) {
  require(!image.isNull(), "Exported image could not be independently decoded");
  qint64 sum = 0;
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) sum += qAlpha(image.pixel(x, y));
  return sum;
}
QImage renderSvg(const QByteArray &bytes) {
  QSvgRenderer renderer(bytes);
  require(renderer.isValid(), "Independent SVG renderer refused the mixed asset");
  QImage image(512, 256, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  QPainter painter(&image); renderer.render(&painter); painter.end();
  require(alphaSum(image) > 1000, "Independent SVG renderer produced blank artwork");
  return image;
}
QImage renderFirstVectorAsset(const QByteArray &document) {
  struct Asset { QByteArray bytes; QRectF rect; };
  QMap<QString, Asset> assets;
  QSize size;
  QString used;
  double opacity = 1.;
  QXmlStreamReader xml(document);
  while (!xml.atEnd()) {
    xml.readNext();
    if (!xml.isStartElement()) continue;
    const auto attrs = xml.attributes();
    if (xml.name() == "svg") size = QSize(attrs.value("width").toInt(), attrs.value("height").toInt());
    if (xml.name() == "image") {
      const QString href = attrs.value("href").toString();
      const QString prefix = "data:image/svg+xml;base64,";
      if (href.startsWith(prefix)) assets.insert(attrs.value("id").toString(),
          {QByteArray::fromBase64(href.mid(prefix.size()).toLatin1()),
           QRectF(attrs.value("x").toDouble(), attrs.value("y").toDouble(),
                  attrs.value("width").toDouble(), attrs.value("height").toDouble())});
    }
    if (xml.name() == "use" && used.isEmpty()) {
      used = attrs.value("href").toString().mid(1);
      opacity = attrs.value("opacity").toDouble();
    }
  }
  require(!xml.hasError() && size == QSize(256, 128) && assets.contains(used),
          "Animated SVG does not identify its first vector asset and viewport");
  const auto &asset = assets[used];
  QSvgRenderer renderer(asset.bytes);
  require(renderer.isValid(), "Independent renderer rejected the camera-aligned asset");
  QImage result(size, QImage::Format_ARGB32_Premultiplied); result.fill(Qt::transparent);
  QPainter painter(&result); painter.setOpacity(opacity); renderer.render(&painter, asset.rect); painter.end();
  return result;
}
void samePlacement(const QImage &asset, const QImage &reference) {
  struct Extent { QRect bounds; double x = 0, y = 0; qint64 alpha = 0; };
  auto extent = [](const QImage &image) {
    Extent result;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
      const int alpha = qAlpha(image.pixel(x, y));
      if (alpha > 16) result.bounds |= QRect(x, y, 1, 1);
      result.alpha += alpha; result.x += x * alpha; result.y += y * alpha;
    }
    require(result.alpha > 1000, "Placement oracle produced blank artwork");
    result.x /= result.alpha; result.y /= result.alpha;
    return result;
  };
  require(asset.size() == reference.size(), "Vector asset changed the camera resolution");
  const auto a = extent(asset), b = extent(reference);
  require(std::abs(a.bounds.x() - b.bounds.x()) <= 2 && std::abs(a.bounds.y() - b.bounds.y()) <= 2 &&
          std::abs(a.bounds.width() - b.bounds.width()) <= 3 && std::abs(a.bounds.height() - b.bounds.height()) <= 3 &&
          std::abs(a.x - b.x) <= 1.5 && std::abs(a.y - b.y) <= 1.5 &&
          std::abs(double(a.alpha) / b.alpha - 1.) < 0.1,
          "Cropped vector asset was stretched or moved relative to the native camera render");
}

}

void runCombinedFidelityIntegrationTest(QMainWindow *) {
  auto *app = TApp::instance();
  QString resetError;
  if (!resetNativeUiIntegrationScene(&resetError))
    throw std::runtime_error(resetError.toStdString());
  QTemporaryDir output;
  require(output.isValid(), "Combined fidelity fixture directory unavailable");
  auto *scene = app->getCurrentScene()->getScene();
  auto *sheet = app->getCurrentXsheet()->getXsheet();
  scene->getCurrentCamera()->setRes(TDimension(256, 128));
  scene->getCurrentCamera()->setSize(TDimensionD(2, 1));
  scene->getProperties()->setBgColor(TPixel32::Transparent);
  auto *level = scene->createNewLevel(PLI_XSHLEVEL, L"CombinedFidelity")->getSimpleLevel();
  TVectorImageP drawing = new TVectorImage;
  drawing->setPalette(level->getPalette());
  const int color = level->getPalette()->addStyle(TPixel32(220, 40, 20, 255));
  auto *stroke = new TStroke(std::vector<TThickPoint>{{-8, -18, 3}, {16, -18, 3}, {40, -18, 3}});
  stroke->setStyle(color);
  require(stroke->setOpacityProfile({{0, 0.25}, {1, 0.25}}), "Cannot assign fidelity opacity profile");
  drawing->addStroke(stroke, false);
  auto *ordinary = new TStroke(std::vector<TThickPoint>{{16, 4, 1}, {22, 4, 1}, {28, 4, 1}});
  ordinary->setStyle(color); drawing->addStroke(ordinary, false);
  level->setFrame(TFrameId(1), drawing);
  level->setFrame(TFrameId(2), drawing->cloneImage());
  level->setCastTags({"portable", "profile"});
  level->setCastCover(TFrameId(2));
  QJsonObject hint{{"kind", "pair"}, {"firstFrame", MorphHints::frame(TFrameId(1))},
      {"lastFrame", MorphHints::frame(TFrameId(2))}, {"first", MorphHints::anchor(drawing, 0)},
      {"last", MorphHints::anchor(drawing, 0)}};
  level->setMorphHints(MorphHints::encode(QJsonArray{hint}));
  require(sheet->setCell(0, 0, TXshCell(level, TFrameId(1))), "Cannot expose fidelity fixture");
  auto *curve = sheet->getStageObject(TStageObjectId::ColumnId(0))->getParam(TStageObject::T_X);
  curve->setWorkspaceId("combined-fidelity-channel");
  const TFilePath levelPath(output.filePath("drawing.pli"));
  level->setPath(levelPath); level->save(levelPath);
  const auto profile = stroke->getOpacityProfile();
  const int history = TUndoManager::manager()->getHistoryCount();
  const bool dirty = app->getCurrentScene()->getDirtyFlag();
  std::cerr << "combined-fidelity: metadata and compatibility" << std::endl;
  const auto issues = SceneChecker::compatibilityReport(scene);
  int format = 0, metadata = 0, identityCount = 0;
  for (const auto &issue : issues) {
    if (issue.text.contains("PLI 150.1")) { ++format; require(issue.severity == SceneChecker::Warning, "PLI reader requirement has wrong severity"); }
    if (issue.levelName == level->getName() && issue.severity == SceneChecker::Info) ++metadata;
    if (issue.text.contains("channel identities")) ++identityCount;
  }
  require(format == 1 && metadata == 2 && identityCount == 1, "Combined compatibility report lost format or authoring metadata distinction");
  require(stroke->getOpacityProfile() == profile && history == TUndoManager::manager()->getHistoryCount() && dirty == app->getCurrentScene()->getDirtyFlag(),
          "Compatibility report mutated artwork, undo, or dirty state");
  const TFilePath scenePath(output.filePath("combined.tnz"));
  scene->saveCopy(scenePath);
  {
    ToonzScene loaded; loaded.loadTnzFile(scenePath);
    auto *restoredLevel = loaded.getLevelSet()->getLevel(L"CombinedFidelity");
    require(restoredLevel, "Combined saved level missing");
    auto *restored = restoredLevel->getSimpleLevel();
    require(restored && restored->getCastTags() == level->getCastTags() && restored->hasCastCover() &&
            restored->getCastCover() == TFrameId(2) && restored->getMorphHints() == level->getMorphHints(),
            "Combined save/reload lost Cast metadata or morph hints");
    TVectorImageP image = restored->getFrame(TFrameId(1), false);
    require(image && image->getStroke(0)->getOpacityProfile() == profile, "Combined reload lost native opacity profile");
    require(loaded.getXsheet()->getStageObject(TStageObjectId::ColumnId(0))->getParam(TStageObject::T_X)->getWorkspaceId() == "combined-fidelity-channel",
            "Combined reload lost stable Function Editor identity");
  }
  auto *child = scene->createNewLevel(CHILD_XSHLEVEL, L"FidelityNested")->getChildLevel();
  child->getXsheet()->setCell(0, 0, TXshCell(level, TFrameId(1)));
  sheet->setCell(0, 1, TXshCell(child, TFrameId(1)));
  sheet->setCell(1, 1, TXshCell(child, TFrameId(1)));
  const auto capabilities = ExportFidelity::inspectAnimatedSvg(sheet);
  require(capabilities.profiledStrokes == 1 && capabilities.profiledDrawings.size() == 1 && capabilities.assetBlockers.isEmpty(),
          "SVG preflight counted unused or repeated drawings incorrectly");
  sheet->removeColumn(1);
  std::cerr << "combined-fidelity: Lottie choices and overwrite" << std::endl;
  const auto lottie = LottieExport::exportXsheet(sheet, LottieExport::Options());
  require(lottie.m_skipped.join(' ').contains("opacity envelopes"), "Lottie silently dropped an opacity envelope");
  const QString json = output.filePath("cancel.json");
  exportCommand(MI_ExportLottie, json, {"cancelExportFidelity", false, true, false, "opacity envelopes"});
  require(!QFileInfo::exists(json), "Canceled Lottie export wrote a file");
  exportCommand(MI_ExportLottie, json, {"exportSupportedContent", false, true, false, "opacity envelopes"});
  const QByteArray originalJson = readFile(json);
  require(QJsonDocument::fromJson(originalJson).isObject(), "Accepted Lottie export is invalid JSON");
  exportCommand(MI_ExportLottie, json, {"cancelExportFidelity", false, true, false, "opacity envelopes"});
  require(readFile(json) == originalJson, "Canceled Lottie overwrite replaced the destination");

  std::cerr << "combined-fidelity: mixed SVG and independent render" << std::endl;
  const QString mixed = output.filePath("mixed.svg");
  exportCommand(MI_ExportAnimatedSVG, mixed, {"cancelExportFidelity", false, true, false, "PLI 150.1"});
  require(!QFileInfo::exists(mixed), "Canceled mixed SVG export wrote a file");
  exportCommand(MI_ExportAnimatedSVG, mixed, {"exportSupportedContent", false, true, false, "embedded PNG"});
  const QByteArray mixedBytes = readFile(mixed);
  const auto vectors = embedded(mixedBytes, "image/svg+xml");
  require(vectors.size() == 1 && vectors.front().contains("data-groundzero-opacity-raster=\"1\"") &&
          vectors.front().contains("<path") && embedded(vectors.front(), "image/png").size() == 1,
          "Mixed SVG did not preserve vector paths beside the profiled PNG stroke");
  renderSvg(vectors.front());
  exportCommand(MI_ExportAnimatedSVG, mixed, {"cancelExportFidelity", false, true, false, "PLI 150.1"});
  require(readFile(mixed) == mixedBytes, "Canceled SVG overwrite replaced its document");

  // Reconstruct the first displayed asset from the actual animated document's
  // image rectangle and href, then compare off-center, unequal-aspect artwork
  // with the independent full-camera native renderer. Identity envelopes must
  // not change placement merely because the native SVG crop gained padding.
  stroke->setOpacityProfile({{0, 1}, {1, 1}});
  level->getFrame(TFrameId(1), true); app->getCurrentXsheet()->notifyXsheetChanged();
  const QString placed = output.filePath("placed.svg");
  exportCommand(MI_ExportAnimatedSVG, placed, {"exportSupportedContent", false, true, false, "embedded PNG"});
  const QString camera = output.filePath("camera.svg");
  exportCommand(MI_ExportAnimatedSVG, camera, {"renderSceneForExport", false, true, false, "PLI 150.1"});
  const auto cameraFrames = embedded(readFile(camera), "image/png");
  require(cameraFrames.size() == 1, "Placement reference camera frame missing");
  QImage cameraReference; require(cameraReference.loadFromData(cameraFrames.front(), "PNG"), "Placement reference PNG is unreadable");
  samePlacement(renderFirstVectorAsset(readFile(placed)), cameraReference);

  // An unbounded procedural style must offer the full-camera route, which
  // renders the native envelope. Compare decoded PNG alpha against the same
  // native style with an identity envelope, not a second SVG implementation.
  std::cerr << "combined-fidelity: procedural full-camera fallback" << std::endl;
  auto *procedural = TColorStyle::create(104);
  require(procedural && procedural->isStrokeStyle(), "Native procedural fixture style missing");
  procedural->setMainColor(TPixel32(220, 40, 20, 255));
  const int proceduralId = level->getPalette()->addStyle(procedural);
  drawing->deleteStroke(1);
  stroke->setStyle(proceduralId);
  stroke->setOpacityProfile({{0, 0.5}, {1, 0.5}});
  level->getFrame(TFrameId(1), true);
  app->getCurrentXsheet()->notifyXsheetChanged();
  require(!ExportFidelity::inspectAnimatedSvg(sheet).assetBlockers.isEmpty(), "Unbounded procedural style did not block asset export");
  const QString fallback = output.filePath("fallback.svg");
  exportCommand(MI_ExportAnimatedSVG, fallback, {"cancelExportFidelity", true, false, false, "without verified SVG bounds"});
  require(!QFileInfo::exists(fallback) && QDir(output.path()).entryList({"fallback_files_*"}, QDir::Dirs).isEmpty(),
          "Rejected procedural export created a destination or image generation");
  exportCommand(MI_ExportAnimatedSVG, fallback, {"renderSceneForExport", true, true, false, "full camera canvas"});
  const auto halfFrames = embedded(readFile(fallback), "image/png");
  require(halfFrames.size() == 1, "Rendered SVG fallback did not contain the camera frame");
  QImage half; require(half.loadFromData(halfFrames.front(), "PNG"), "Rendered fallback PNG could not be decoded");
  stroke->setOpacityProfile({{0, 1}, {1, 1}});
  level->getFrame(TFrameId(1), true); app->getCurrentXsheet()->notifyXsheetChanged();
  const QString identityPath = output.filePath("identity.svg");
  exportCommand(MI_ExportAnimatedSVG, identityPath, {"renderSceneForExport", true, true, false, "full camera canvas"});
  const auto fullFrames = embedded(readFile(identityPath), "image/png");
  require(fullFrames.size() == 1, "Identity rendered SVG frame missing");
  QImage full; require(full.loadFromData(fullFrames.front(), "PNG"), "Identity PNG could not be decoded");
  const qint64 total = alphaSum(full), faded = alphaSum(half);
  require(full.size() == QSize(256, 128) && half.size() == full.size() && total > 1000 &&
          std::abs(double(faded) / total - 0.5) < 0.03,
          "Full-camera rendered SVG did not preserve the opacity envelope");
  // Repeated external exports publish distinct generations. Canceling an
  // overwrite preserves both the current SVG and the previously linked assets.
  std::cerr << "combined-fidelity: external generation publication" << std::endl;
  const QString external = output.filePath("external.svg");
  exportCommand(MI_ExportAnimatedSVG, external, {"renderSceneForExport", true, false, false, "full camera canvas"});
  const auto firstFolders = QDir(output.path()).entryList({"external_files_*"}, QDir::Dirs);
  require(firstFolders.size() == 1, "External rendered SVG did not publish one image generation");
  const QDir firstGeneration(output.filePath(firstFolders.front()));
  const auto firstImages = firstGeneration.entryList({"*.png"}, QDir::Files);
  require(!firstImages.isEmpty(), "External generation has no rendered images");
  const QString preservedImage = firstGeneration.filePath(firstImages.front());
  const QByteArray preservedPixels = readFile(preservedImage);
  exportCommand(MI_ExportAnimatedSVG, external, {"renderSceneForExport", true, false, false, "full camera canvas"});
  const QByteArray externalBytes = readFile(external);
  const auto folders = QDir(output.path()).entryList({"external_files_*"}, QDir::Dirs);
  require(folders.size() == 2 && readFile(preservedImage) == preservedPixels,
          "Repeated export overwrote a previously published image generation");
  exportCommand(MI_ExportAnimatedSVG, external, {"cancelExportFidelity", true, false, false, "full camera canvas"});
  require(readFile(external) == externalBytes && QDir(output.path()).entryList({"external_files_*"}, QDir::Dirs) == folders,
          "Canceled SVG overwrite changed its external image generation");
  // Hide the real rendering progress dialog and invoke the real New Scene
  // workflow while the render is queued or active. Cancellation must drain the
  // renderer before the old scene's deletion timer can be scheduled.
  std::cerr << "combined-fidelity: scene replacement cancels export" << std::endl;
  bool switchedDuringRender = false;
  QString interruptedResetError;
  QTimer replaceScene;
  QObject::connect(&replaceScene, &QTimer::timeout, [&]() {
    if (switchedDuringRender) return;
    for (QWidget *widget : QApplication::topLevelWidgets()) {
      if (widget->objectName() != "animatedSvgExportProgress" ||
          !widget->property("sceneRenderActive").toBool()) continue;
      switchedDuringRender = true;
      replaceScene.stop();
      widget->hide();
      if (!resetNativeUiIntegrationScene(&interruptedResetError))
        return;
      break;
    }
  });
  replaceScene.start(0);
  exportCommand(MI_ExportAnimatedSVG, external, {"", false, false, true, ""});
  replaceScene.stop();
  require(switchedDuringRender && interruptedResetError.isEmpty(), "Native scene replacement was not exercised during export");
  require(readFile(external) == externalBytes && QDir(output.path()).entryList({"external_files_*"}, QDir::Dirs) == folders,
          "Scene replacement published or replaced an interrupted export");
  if (!resetNativeUiIntegrationScene(&resetError))
    throw std::runtime_error(resetError.toStdString());
  std::cout << "combined-fidelity-native: PASS" << std::endl;
}
