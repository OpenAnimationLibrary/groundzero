#include "exportfidelity.h"
#include "toonzqt/uitranslations.h"
#include "ocaio.h"

#include "toonz/tcamera.h"
#include "toonz/scenefx.h"
#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/tscenehandle.h"
#include "tapp.h"
#include "menubarcommandids.h"
#include "filebrowserpopup.h"
#include "timage_io.h"
#include "toutputproperties.h"
#include "trenderer.h"
#include "texception.h"

#include <atomic>
#include <exception>

#include <QCheckBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QUuid>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QTemporaryDir>
#include <QUrl>
#include <QEventLoop>
#include <QThread>
#include <memory>
#include <QXmlStreamWriter>
#include <stdexcept>

namespace {
#ifdef BUILD_UI_PORT_TESTS
ExportFidelity::SvgRenderTestHooks *svgRenderTestHooks = nullptr;
#endif

struct SvgExportData {
  QJsonArray layers;
  QMap<QString, OCAIo::OCAAsset> assets;
  double frameRate = 24.0;
  int frameCount   = 0;
  int width        = 0;
  int height       = 0;
};

QString assetHref(const OCAIo::OCAAsset &asset, const TFilePath &imageDir,
                  bool embedImages) {
  QString fileName = QFileInfo(asset.fileName).fileName();
  if (!embedImages)
    return imageDir.withoutParentDir().getQString() + "/" + fileName;

  QFile file(QDir(imageDir.getQString()).filePath(fileName));
  if (!file.open(QIODevice::ReadOnly)) return QString();
  QString mime = asset.fileExt == "svg" ? "image/svg+xml" : "image/png";
  return QString("data:%1;base64,%2")
      .arg(mime, QString::fromLatin1(file.readAll().toBase64()));
}

void writeLayer(QXmlStreamWriter &xml, const QJsonObject &layer,
                const SvgExportData &data,
                const QMap<QString, QString> &assetIds, int &layerId) {
  // Columns hidden with the camstand toggle are exported to OCA, as hidden
  // layers: they are not drawn
  if (!layer["visible"].toBool(true)) return;

  QJsonArray childLayers = layer["childLayers"].toArray();
  if (!childLayers.isEmpty()) {
    xml.writeStartElement("g");
    xml.writeAttribute("id", QString("group_%1").arg(++layerId));
    for (const QJsonValue &child : childLayers)
      writeLayer(xml, child.toObject(), data, assetIds, layerId);
    xml.writeEndElement();
    return;
  }

  QJsonArray frames = layer["frames"].toArray();
  if (frames.isEmpty()) return;

  QStringList values;
  QStringList keyTimes;
  QString firstHref;
  bool firstValue = true;
  int elapsed     = 0;
  for (const QJsonValue &value : frames) {
    QJsonObject frame = value.toObject();
    keyTimes.append(
        QString::number(double(elapsed) / data.frameCount, 'g', 12));
    QString href     = "#blank_frame";
    QString fileName = QFileInfo(frame["fileName"].toString()).fileName();
    if (assetIds.contains(fileName)) {
      href = "#" + assetIds.value(fileName);
    }
    values.append(href);
    if (firstValue) {
      firstHref  = href;
      firstValue = false;
    }
    elapsed += frame["duration"].toInt(1);
  }
  keyTimes.append("1");
  values.append(values.last());

  xml.writeStartElement("use");
  xml.writeAttribute("id", QString("layer_%1").arg(++layerId));
  xml.writeAttribute("opacity",
                     QString::number(layer["opacity"].toDouble(1.0)));
  xml.writeAttribute("href", firstHref);

  xml.writeStartElement("animate");
  xml.writeAttribute("attributeName", "href");
  xml.writeAttribute("calcMode", "discrete");
  xml.writeAttribute("values", values.join(';'));
  xml.writeAttribute("keyTimes", keyTimes.join(';'));
  xml.writeAttribute(
      "dur", QString::number(data.frameCount / data.frameRate, 'g', 12) + "s");
  xml.writeAttribute("repeatCount", "indefinite");
  xml.writeEndElement();
  xml.writeEndElement();
}

bool writeAnimatedSvg(const TFilePath &svgPath, const TFilePath &imageDir,
                      const SvgExportData &data, bool embedImages) {
  QSaveFile file(svgPath.getQString());
  file.setDirectWriteFallback(false);
  if (!file.open(QIODevice::WriteOnly)) return false;

  QXmlStreamWriter xml(&file);
  xml.setAutoFormatting(true);
  xml.writeStartDocument();
  xml.writeStartElement("svg");
  xml.writeDefaultNamespace("http://www.w3.org/2000/svg");
  xml.writeAttribute("width", QString::number(data.width));
  xml.writeAttribute("height", QString::number(data.height));
  xml.writeAttribute("viewBox",
                     QString("0 0 %1 %2").arg(data.width).arg(data.height));
  xml.writeStartElement("desc");
  xml.writeCharacters("Animated SVG exported by OpenToonz");
  xml.writeEndElement();

  QMap<QString, QString> assetIds;
  xml.writeStartElement("defs");
  xml.writeEmptyElement("g");
  xml.writeAttribute("id", "blank_frame");
  int assetNumber = 0;
  for (const OCAIo::OCAAsset &asset : data.assets) {
    QString fileName = QFileInfo(asset.fileName).fileName();
    QString id       = QString("asset_%1").arg(++assetNumber);
    assetIds.insert(fileName, id);
    xml.writeEmptyElement("image");
    xml.writeAttribute("id", id);
    xml.writeAttribute("x", QString::number((data.width - asset.width) / 2));
    xml.writeAttribute("y", QString::number((data.height - asset.height) / 2));
    xml.writeAttribute("width", QString::number(asset.width));
    xml.writeAttribute("height", QString::number(asset.height));
    xml.writeAttribute("preserveAspectRatio", "none");
    const QString href = assetHref(asset, imageDir, embedImages);
    if (href.isEmpty()) return false;
    xml.writeAttribute("href", href);
  }
  xml.writeEndElement();

  int layerId = 0;
  for (const QJsonValue &value : data.layers)
    writeLayer(xml, value.toObject(), data, assetIds, layerId);

  xml.writeEndElement();
  xml.writeEndDocument();
  return !xml.hasError() && file.commit();
}

SvgExportData fromOcaData(const OCAIo::OCAData &ocaData) {
  SvgExportData data;
  data.layers     = ocaData.layers();
  data.assets     = ocaData.assets();
  data.frameRate  = ocaData.frameRate();
  data.frameCount = ocaData.frameCount();
  data.width      = ocaData.width();
  data.height     = ocaData.height();
  return data;
}

class SceneRenderPort final : public TRenderPort {
  TFilePath m_imageDir;
  QMap<QByteArray, QString> m_hashes;
  int m_assetNumber = 0;
  QMutex m_failureMutex;
  QString m_failure;

  void recordFailure(const RenderData &renderData, const QString &reason) {
    QMutexLocker locker(&m_failureMutex);
    if (!m_failure.isEmpty()) return;
    const QString detail = reason.isEmpty()
                               ? QObject::tr("Scene rendering failed without "
                                             "an error message.")
                               : reason;
    m_failure = renderData.m_frames.empty()
                    ? detail
                    : QObject::tr("Frame %1: %2")
                          .arg(renderData.m_frames.front() + 1)
                          .arg(detail);
  }

public:
  std::atomic<bool> finished{false};
  QMap<int, QString> frameAssets;
  QMap<QString, OCAIo::OCAAsset> assets;

  QString failure() {
    QMutexLocker locker(&m_failureMutex);
    return m_failure;
  }

  SceneRenderPort(const TFilePath &imageDir, const TDimension &resolution)
      : m_imageDir(imageDir) {
    double rx = resolution.lx * 0.5;
    double ry = resolution.ly * 0.5;
    setRenderArea(TRectD(-rx, -ry, rx, ry));
  }

  void onRenderRasterCompleted(const RenderData &renderData) override {
    if (!renderData.m_rasA) {
      recordFailure(renderData, QObject::tr("The renderer returned no image."));
      return;
    }

    QString fileName =
        QString("rendered_%1.png").arg(++m_assetNumber, 4, 10, QChar('0'));
    TFilePath filePath = m_imageDir + TFilePath(fileName);
    try {
      TRasterImageP image(renderData.m_rasA->clone());
      TImageWriterP writer(filePath);
      writer->save(image);

      QFile file(filePath.getQString());
      if (!file.open(QIODevice::ReadOnly)) {
        recordFailure(renderData,
                      QObject::tr("Unable to read the rendered PNG: %1")
                          .arg(file.errorString()));
        return;
      }
      QByteArray hash =
          QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
      file.close();

      if (m_hashes.contains(hash)) {
        QFile::remove(filePath.getQString());
        fileName = m_hashes.value(hash);
      } else {
        m_hashes.insert(hash, fileName);
        OCAIo::OCAAsset asset;
        asset.width    = image->getRaster()->getLx();
        asset.height   = image->getRaster()->getLy();
        asset.fileName = fileName;
        asset.fileExt  = "png";
        assets.insert(fileName, asset);
      }
      for (double frame : renderData.m_frames)
        frameAssets.insert(qRound(frame), fileName);
    } catch (const TException &error) {
      recordFailure(renderData,
                    QObject::tr("Unable to save the rendered PNG: %1")
                        .arg(QString::fromStdWString(error.getMessage())));
    } catch (const std::exception &error) {
      recordFailure(renderData,
                    QObject::tr("Unable to save the rendered PNG: %1")
                        .arg(QString::fromUtf8(error.what())));
    } catch (...) {
      recordFailure(
          renderData,
          QObject::tr("Unable to save the rendered PNG: unknown error."));
    }
  }

  void onRenderFailure(const RenderData &renderData, TException &error) override {
    recordFailure(renderData, QString::fromStdWString(error.getMessage()));
  }

  void onRenderFinished(bool isCanceled = false) override {
    if (isCanceled)
      recordFailure(RenderData(), QObject::tr("Scene rendering was canceled."));
#ifdef BUILD_UI_PORT_TESTS
    if (svgRenderTestHooks && svgRenderTestHooks->completed)
      svgRenderTestHooks->completed();
#endif
    finished = true;
  }
};

bool renderScene(ToonzScene *scene, TXsheet *xsheet, const TFilePath &imageDir,
                 SvgExportData &data, DVGui::ProgressDialog *progress,
                 QString &failure) {
  TOutputProperties *properties = scene->getProperties()->getOutputProperties();
  TRenderSettings settings      = properties->getRenderSettings();
  TDimension cameraRes          = scene->getCurrentCamera()->getRes();
  int shrink                    = qMax(1, settings.m_shrinkX);
  const int frameCount = xsheet->getFrameCount();
  const double frameRate = properties->getFrameRate();
  if (frameCount <= 0) {
    failure = QObject::tr("The scene contains no frames to render.");
    return false;
  }

  SceneRenderPort port(imageDir, cameraRes);
  TRenderer renderer(1);
  renderer.enablePrecomputing(false);
  renderer.addPort(&port);

  // A scene replacement signal is delivered before TSceneHandle schedules
  // deletion of the old scene. Drain even a queued (not yet active) render in
  // that direct callback; stopRendering(true) alone only sees active tasks.
  struct RenderSession {
    TRenderer &renderer;
    SceneRenderPort &port;
    bool submitted = false;
    void drain() {
      while (submitted && !port.finished.load()) {
        renderer.stopRendering(false);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 10);
        QThread::msleep(1);
      }
      renderer.stopRendering(true);
    }
    ~RenderSession() { drain(); renderer.removePort(&port); }
  };
  QObject lifetime;
  bool contextChanged = false;
  RenderSession session{renderer, port};
  auto cancelRender = [&]() {
    if (contextChanged) return;
    contextChanged = true;
#ifdef BUILD_UI_PORT_TESTS
    if (svgRenderTestHooks && svgRenderTestHooks->cancelStarted)
      svgRenderTestHooks->cancelStarted(port.finished.load());
#endif
    session.drain();
#ifdef BUILD_UI_PORT_TESTS
    if (svgRenderTestHooks && svgRenderTestHooks->cancelDrained)
      svgRenderTestHooks->cancelDrained(port.finished.load());
#endif
  };
  QObject::connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched,
                   &lifetime, cancelRender, Qt::DirectConnection);
  QObject::connect(TApp::instance()->getCurrentXsheet(), &TXsheetHandle::xsheetSwitched,
                   &lifetime, cancelRender, Qt::DirectConnection);
  auto renderData = std::make_unique<std::vector<TRenderer::RenderData>>();
  for (int frame = 0; frame < frameCount && !contextChanged; ++frame) {
    TFxPair fxPair;
    fxPair.m_frameA = ::buildSceneFx(
        scene, xsheet, frame, TOutputProperties::AllLevels, shrink, false);
    renderData->push_back(TRenderer::RenderData(frame, settings, fxPair));
  }

  if (contextChanged) return false;
  renderer.startRendering(renderData.get());
  renderData.release();
  session.submitted = true;
  progress->setProperty("sceneRenderActive", true);
#ifdef BUILD_UI_PORT_TESTS
  if (svgRenderTestHooks && svgRenderTestHooks->queued)
    svgRenderTestHooks->queued();
#endif
  while (!port.finished.load()) {
    QCoreApplication::processEvents();
    QThread::msleep(1);
  }
  session.drain();
  if (contextChanged) return false;
  failure = port.failure();
  if (failure.isEmpty() && port.frameAssets.isEmpty())
    failure = QObject::tr("The renderer produced no scene frames.");
  if (!failure.isEmpty()) return false;
  for (int frame = 0; frame < frameCount; ++frame)
    if (!port.frameAssets.contains(frame)) {
      failure = QObject::tr("The renderer did not produce frame %1.").arg(frame + 1);
      return false;
    }

  QJsonArray frames;
  for (int frame = 0; frame < frameCount;) {
    QString fileName = port.frameAssets.value(frame);
    int duration     = 1;
    while (frame + duration < frameCount &&
           port.frameAssets.value(frame + duration) == fileName)
      ++duration;

    QJsonObject frameObject;
    frameObject["fileName"] = fileName;
    frameObject["duration"] = duration;
    frames.append(frameObject);
    frame += duration;
  }
  QJsonObject layer;
  layer["frames"]      = frames;
  layer["childLayers"] = QJsonArray();
  layer["opacity"]     = 1.0;
  data.layers.append(layer);
  data.assets     = port.assets;
  data.frameRate  = frameRate;
  data.frameCount = frameCount;
  data.width      = cameraRes.lx / shrink;
  data.height     = cameraRes.ly / qMax(1, settings.m_shrinkY);
  return true;
}

class ExportAnimatedSVGCommand final : public MenuItemHandler {
public:
  ExportAnimatedSVGCommand() : MenuItemHandler(MI_ExportAnimatedSVG) {}
  void execute() override;
};

void ExportAnimatedSVGCommand::execute() {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  TXsheet *xsheet   = TApp::instance()->getCurrentXsheet()->getXsheet();
  TFilePath fp      = scene->getScenePath().withType("svg");
  QObject contextLifetime;
  bool contextChanged = false;
  QObject::connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched,
                   &contextLifetime, [&]() { contextChanged = true; }, Qt::DirectConnection);
  QObject::connect(TApp::instance()->getCurrentXsheet(), &TXsheetHandle::xsheetSwitched,
                   &contextLifetime, [&]() { contextChanged = true; }, Qt::DirectConnection);
  auto currentContext = [&]() {
    return !contextChanged && TApp::instance()->getCurrentScene()->getScene() == scene &&
           TApp::instance()->getCurrentXsheet()->getXsheet() == xsheet;
  };

  static GenericSaveFilePopup *savePopup       = nullptr;
  static QCheckBox *rasterizeVectors           = nullptr;
  static QCheckBox *renderSceneResult          = nullptr;
  static QCheckBox *embedImages                = nullptr;
  static QCheckBox *openInBrowser              = nullptr;
  static DVGui::ProgressDialog *progressDialog = nullptr;

  if (!savePopup) {
    QWidget *customWidget = new QWidget();
    rasterizeVectors      = new QCheckBox(QObject::tr("Rasterize Vectors"));
    renderSceneResult =
        new QCheckBox(QObject::tr("Render Scene Before Export"));
    embedImages   = new QCheckBox(QObject::tr("Embed Images in SVG"));
    openInBrowser = new QCheckBox(QObject::tr("Open in Browser When Complete"));
    rasterizeVectors->setObjectName("svgRasterizeVectors");
    renderSceneResult->setObjectName("svgRenderScene");
    embedImages->setObjectName("svgEmbedImages");
    openInBrowser->setObjectName("svgOpenBrowser");
    rasterizeVectors->setChecked(true);
    openInBrowser->setChecked(true);
    rasterizeVectors->setToolTip(QObject::tr(
        "Checked: vector drawings are exported as PNG images\nUnchecked: "
        "vector drawings are exported as SVG images"));
    renderSceneResult->setToolTip(QObject::tr(
        "Render the complete scene to PNG frames so animation, camera moves, "
        "compositing, and effects are included"));
    embedImages->setToolTip(QObject::tr(
        "Store image data as base64 inside the animated SVG instead of "
        "creating an external image folder"));

    QGridLayout *layout = new QGridLayout(customWidget);
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(5);
    layout->addWidget(rasterizeVectors, 0, 0);
    layout->addWidget(renderSceneResult, 1, 0);
    layout->addWidget(embedImages, 2, 0);
    layout->addWidget(openInBrowser, 3, 0);
    QObject::connect(renderSceneResult, &QCheckBox::toggled, rasterizeVectors,
                     &QCheckBox::setDisabled);

    progressDialog = new DVGui::ProgressDialog("", QObject::tr("Hide"), 0, 0);
    progressDialog->setObjectName("animatedSvgExportProgress");
    progressDialog->setWindowTitle(QObject::tr("Exporting..."));
    progressDialog->setWindowFlags(Qt::Dialog | Qt::WindowTitleHint);
    progressDialog->setWindowModality(Qt::WindowModal);

    savePopup = new GenericSaveFilePopup(QObject::tr("Export Animated SVG"),
                                         customWidget);
    savePopup->setObjectName("animatedSvgExportFilePopup");
    savePopup->addFilterType("svg");
  }

  if (!scene->isUntitled())
    savePopup->setFolder(fp.getParentDir());
  else
    savePopup->setFolder(
        TProjectManager::instance()->getCurrentProject()->getScenesPath());
  savePopup->setFilename(fp.withoutParentDir());
  fp = savePopup->getPath();
  if (fp.isEmpty() || !currentContext()) return;

  // Resolve fidelity and unsupported profile styles before creating output.
  // Profiled strokes can be rasterized inside SVG; the native PLI remains
  // their editable source.
  if (!renderSceneResult->isChecked()) {
    const auto capabilities = ExportFidelity::inspectAnimatedSvg(xsheet);
    const auto choice = ExportFidelity::chooseAnimatedSvg(capabilities);
    if (choice == ExportFidelity::Cancel || !currentContext()) return;
    if (choice == ExportFidelity::RenderedScene) renderSceneResult->setChecked(true);
  }

  TDimension resolution = scene->getCurrentCamera()->getRes();
  double estimatedMb    = double(resolution.lx) * resolution.ly *
                       xsheet->getFrameCount() * 4.0 * 4.0 / 3.0 /
                       (1024.0 * 1024.0);
  if (embedImages->isChecked() && estimatedMb >= 64.0) {
    QString warning = UiTranslations::text(QObject::tr(
                          "Embedding this animation may create an SVG as large "
                          "as approximately %1 MB. Continue?"))
                          .arg(qRound64(estimatedMb));
    std::vector<QString> buttons = {QObject::tr("Cancel"),
                                    QObject::tr("Export")};
    if (DVGui::MsgBox(DVGui::WARNING, warning, buttons) != 2 || !currentContext()) return;
  }

  // Build assets in private staging. A failure must not overwrite assets used
  // by a previously exported document. Each external publication gets its own
  // generation, followed by one atomic document replacement.
  QTemporaryDir temporaryImageDir;
  if (!temporaryImageDir.isValid()) {
    DVGui::error(QObject::tr("Unable to prepare the image folder."));
    return;
  }
  const TFilePath stagingDir(temporaryImageDir.path());
  TFilePath publishedDir = stagingDir;
  struct RemoveUnpublished {
    QString folder;
    bool committed = false;
    ~RemoveUnpublished() {
      if (!committed && !folder.isEmpty()) QDir(folder).removeRecursively();
    }
  } publication;
  progressDialog->setLabelText(QObject::tr("Exporting image assets..."));
  progressDialog->setValue(0);
  progressDialog->show();

  try {
    SvgExportData data;
    if (renderSceneResult->isChecked()) {
      progressDialog->setLabelText(QObject::tr("Rendering scene frames..."));
      QString failure;
      const bool rendered = renderScene(scene, xsheet, stagingDir, data, progressDialog, failure);
      progressDialog->setProperty("sceneRenderActive", false);
      if (!currentContext()) { progressDialog->hide(); return; }
      if (!rendered)
        throw std::runtime_error((QObject::tr("Unable to render the scene for SVG export.") +
                                  "\n" + failure).toStdString());
    } else {
      OCAIo::OCAData ocaData;
      // Asset conversion is synchronous. Do not let OCA's per-column progress
      // callback dispatch scene edits while it owns raw scene/level pointers.
      ocaData.setProgressDialog(nullptr);
      ocaData.build(scene, xsheet, QString::fromStdString(fp.getName()),
                    stagingDir.getQString(), false, !rasterizeVectors->isChecked(),
                    false);
      if (ocaData.isEmpty())
        throw std::runtime_error("No columns can be exported.");
      data = fromOcaData(ocaData);
    }
    // Verify every asset before opening the final SVG. Empty hrefs would create
    // a syntactically valid document with silently missing artwork.
    for (const OCAIo::OCAAsset &asset : data.assets) {
      QFile input(QDir(stagingDir.getQString()).filePath(QFileInfo(asset.fileName).fileName()));
      if (!input.open(QIODevice::ReadOnly) || input.size() <= 0)
        throw std::runtime_error("An exported image asset is missing or unreadable.");
    }
    if (!embedImages->isChecked()) {
      const QString generation = QString::fromStdString(fp.getName()) +
          QStringLiteral("_files_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
      publishedDir = fp.getParentDir() + TFilePath(generation);
      if (!QDir().mkdir(publishedDir.getQString()))
        throw std::runtime_error("Unable to create the image folder.");
      publication.folder = publishedDir.getQString();
      for (const OCAIo::OCAAsset &asset : data.assets) {
        const QString name = QFileInfo(asset.fileName).fileName();
        if (!QFile::copy(QDir(stagingDir.getQString()).filePath(name),
                        QDir(publishedDir.getQString()).filePath(name)))
          throw std::runtime_error("Unable to publish an exported image asset.");
      }
    }
    if (!writeAnimatedSvg(fp, publishedDir, data, embedImages->isChecked()))
      throw std::runtime_error("Unable to save the animated SVG file.");
    publication.committed = true;
  } catch (const std::exception &problem) {
    progressDialog->setProperty("sceneRenderActive", false);
    progressDialog->hide();
    DVGui::error(QObject::tr("Animated SVG export failed: %1")
                    .arg(QString::fromUtf8(problem.what())));
    return;
  } catch (...) {
    progressDialog->setProperty("sceneRenderActive", false);
    progressDialog->hide();
    DVGui::error(QObject::tr("Animated SVG export failed while preparing image assets."));
    return;
  }
  progressDialog->hide();

  if (openInBrowser->isChecked())
    QDesktopServices::openUrl(QUrl::fromLocalFile(fp.getQString()));
  else
    DVGui::info(QObject::tr("Animated SVG exported successfully."));
}

ExportAnimatedSVGCommand exportAnimatedSVGCommand;

}  // namespace

#ifdef BUILD_UI_PORT_TESTS
void ExportFidelity::setSvgRenderTestHooks(SvgRenderTestHooks *hooks) {
  svgRenderTestHooks = hooks;
}
#endif
