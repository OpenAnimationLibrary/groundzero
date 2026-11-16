#include "mcphandoffintegrationtest.h"

#include "iocommand.h"
#include "tapp.h"
#include "tdoubleparam.h"
#include "toutputproperties.h"
#include "tpalette.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "tproperty.h"
#include "trasterimage.h"
#include "tstream.h"
#include "tundo.h"
#include "tzeraryfx.h"
#include "toonz/boardsettings.h"
#include "toonz/doubleparamcmd.h"
#include "toonz/fullcolorpalette.h"
#include "toonz/fxdag.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/movierenderer.h"
#include "toonz/preferences.h"
#include "toonz/scenefx.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshzeraryfxcolumn.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

constexpr int kFrame = 60;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void near(double actual, double expected, const char* message) {
  require(std::abs(actual - expected) < 1e-8, message);
}

QString fixturePath(const QString& relative) {
  return QDir::current().filePath("mcp-handoff/" + relative);
}

TFilePath nativePath(const QString& path) {
  return TFilePath(path.toStdWString());
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Handoff evidence file is missing");
  require(file.size() <= 32 * 1024 * 1024,
          "Handoff evidence file exceeds its bound");
  return file.readAll();
}

QJsonObject readProof() {
  QJsonParseError error;
  QJsonDocument proof =
      QJsonDocument::fromJson(readFile(fixturePath("proof.json")), &error);
  require(error.error == QJsonParseError::NoError,
          "Shot proof is not valid JSON");
  require(proof.object()["status"].toString() == "passed",
          "Shot proof did not pass");
  return proof.object()["drawings"].toObject();
}

ToonzScene* currentScene() {
  return TApp::instance()->getCurrentScene()->getScene();
}

void loadNormally(const QString& path) {
  const TFilePath file = nativePath(path);
  auto* manager        = TProjectManager::instance();
  auto project         = manager->loadSceneProject(file);
  require(bool(project), "Relocated scene has no native project");
  manager->setCurrentProjectPath(project->getProjectPath());
  require(IoCmd::loadScene(file, false, false),
          "Normal app scene load rejected the package");
  require(currentScene()->getScenePath() == file,
          "Normal load imported an unexpected scene copy");
}

QByteArray rasterHash(const TRaster32P& raster) {
  QByteArray bytes;
  bytes.reserve(raster->getLx() * raster->getLy() * 4);
  raster->lock();
  for (int y = raster->getLy() - 1; y >= 0; --y) {
    const TPixel32* pixels = raster->pixels(y);
    for (int x = 0; x < raster->getLx(); ++x) {
      const TPixel32 pixel = pixels[x];
      bytes.append(char(pixel.r));
      bytes.append(char(pixel.g));
      bytes.append(char(pixel.b));
      bytes.append(char(pixel.m));
    }
  }
  raster->unlock();
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

void checkLevel(ToonzScene* scene, const QString& name,
                const QJsonObject& expected) {
  TXshLevel* base = scene->getLevelSet()->getLevel(name.toStdWString());
  require(base != nullptr, "Normal loader lost a named level");
  auto* level = base->getSimpleLevel();
  require(level && level->getType() == OVL_XSHLEVEL,
          "Loaded level is not editable RGBA");
  require(level->getFids() == std::vector<TFrameId>{TFrameId(1)},
          "Drawing IDs changed");
  const QString decoded = QString::fromStdWString(
      scene->decodeFilePath(level->getPath()).getWideString());
  const QString parent = QFileInfo(decoded).absolutePath();
  require(parent == fixturePath("package/assets"),
          "A level still references the worker workspace");
  TRasterImageP image = level->getFrame(TFrameId(1), false);
  require(bool(image), "Normal level.load did not decode the drawing");
  TRaster32P raster = image->getRaster();
  require(bool(raster), "Loaded drawing has no RGBA raster");
  require(raster->getLx() == expected["width"].toInt(),
          "Drawing width changed");
  require(raster->getLy() == expected["height"].toInt(),
          "Drawing height changed");
  require(rasterHash(raster) == expected["pixels_sha256"].toString().toLatin1(),
          "Normal loading changed authored drawing pixels");
  require(
      level->getProperties()->getDpiPolicy() == LevelProperties::DP_CustomDpi,
      "Native level lost its explicit physical scale");
  const TPointD dpi = level->getDpi(TFrameId(1));
  near(dpi.x, 128, "Drawing horizontal DPI changed");
  near(dpi.y, 128, "Drawing vertical DPI changed");
  require(
      level->getPalette() == FullColorPalette::instance()->getPalette(scene),
      "RGBA level did not bind the native full-color palette");
}

QByteArray paletteSnapshot(const QString& label) {
  const QString path = fixturePath(label + ".tpl");
  {
    TOStream stream(nativePath(path));
    require(bool(stream), "Cannot retain native palette evidence");
    FullColorPalette::instance()->getPalette(currentScene())->saveData(stream);
  }
  return readFile(path);
}

TStageObject* flightPeg(ToonzScene* scene) {
  auto* tree = scene->getXsheet()->getStageObjectTree();
  for (int index = 0; index < tree->getStageObjectCount(); ++index) {
    TStageObject* object = tree->getStageObject(index);
    if (object->getName() == "Courier flight") return object;
  }
  throw std::runtime_error("Normal loading lost the flight peg");
}

void checkKeys(TDoubleParam* parameter,
               const std::vector<std::pair<int, double>>& keys) {
  require(parameter->getKeyframeCount() == int(keys.size()),
          "Animation key count changed");
  require(!parameter->isCycleEnabled(), "Animation unexpectedly became cyclic");
  for (const auto& key : keys) {
    require(parameter->isKeyframe(key.first), "Animation key frame changed");
    near(parameter->getValue(key.first), key.second,
         "Animation key value changed");
    require(
        parameter->getKeyframeAt(key.first).m_type == TDoubleKeyframe::Linear,
        "Animation interpolation changed");
    require(parameter->getKeyframeAt(key.first).m_step == 1,
            "Animation unexpectedly became stepped");
  }
  for (size_t index = 1; index < keys.size(); ++index) {
    const double frame = (keys[index - 1].first + keys[index].first) / 2.0;
    const double value = (keys[index - 1].second + keys[index].second) / 2.0;
    near(parameter->getValue(frame), value,
         "Native interpolation sample changed");
  }
}

void checkAnimation(ToonzScene* scene, bool edited) {
  TStageObject* peg = flightPeg(scene);
  require(peg->getId().isPegbar(), "Flight object is not a pegbar");
  const double shift = edited ? 0.125 : 0;
  checkKeys(peg->getParam(TStageObject::T_X),
            {{0, -90.0 / 128}, {119, 70.0 / 128 + shift}});
  checkKeys(peg->getParam(TStageObject::T_Y), {{0, -10.0 / 128},
                                               {30, 8.0 / 128},
                                               {60, -8.0 / 128},
                                               {90, 12.0 / 128},
                                               {119, -4.0 / 128}});
  checkKeys(peg->getParam(TStageObject::T_Angle),
            {{0, -5}, {60, 3}, {119, -3}});
  auto* camera =
      scene->getXsheet()->getStageObject(TStageObjectId::CameraId(0));
  checkKeys(camera->getParam(TStageObject::T_X), {{0, 0}, {119, 12.0 / 128}});
  require(scene->getXsheet()
                  ->getStageObject(TStageObjectId::ColumnId(1))
                  ->getParent() == peg->getId(),
          "Courier column lost its peg parent");
}

void checkExposures(ToonzScene* scene) {
  auto* sheet = scene->getXsheet();
  require(sheet->getColumnCount() == 3, "Shot column count changed");
  require(sheet->getFrameCount() == 120, "Shot duration changed");
  const std::wstring names[] = {L"Harbor", L"Courier"};
  for (int column = 0; column < 2; ++column) {
    auto* level = scene->getLevelSet()->getLevel(names[column]);
    for (int row = 0; row < 120; ++row) {
      const TXshCell& cell = sheet->getCell(row, column);
      require(cell.m_level.getPointer() == level,
              "Exposure level identity changed");
      require(cell.m_frameId == TFrameId(1),
              "Exposure drawing identity changed");
    }
    require(sheet->getCell(120, column).isEmpty(),
            "Unexpected exposure after shot end");
  }
}

TFx* namedFx(ToonzScene* scene, const std::wstring& name) {
  std::vector<TFx*> effects;
  scene->getXsheet()->getFxDag()->getFxs(effects);
  for (TFx* effect : effects)
    if (effect->getName() == name) return effect;
  throw std::runtime_error("Normal loading lost a named effect");
}

void checkPort(TFx* effect, const char* port, TFx* source) {
  require(effect != nullptr, "Effect node is missing");
  TFxPort* input = effect->getInputPort(port);
  require(input != nullptr, "Effect input port is missing");
  require(input->getFx() == source, "Effect input connection changed");
}

void checkNumber(TFx* effect, const char* name, double expected) {
  auto* value =
      dynamic_cast<TDoubleParam*>(effect->getParams()->getParam(name));
  require(value != nullptr, "Numeric FX parameter is missing");
  near(value->getValue(kFrame), expected, "Numeric FX parameter changed");
}

void checkColor(TFx* effect, const char* name, const TPixel32& expected) {
  auto* value = dynamic_cast<TPixelParam*>(effect->getParams()->getParam(name));
  require(value != nullptr, "Color FX parameter is missing");
  require(value->getValue(kFrame) == expected, "Color FX parameter changed");
}

void checkSky(TFx* effect) {
  auto* wrapper = dynamic_cast<TZeraryColumnFx*>(effect);
  require(wrapper != nullptr, "Sky effect lost its native generator column");
  TFx* sky = wrapper->getZeraryFx();
  require(sky != nullptr, "Sky wrapper has no generator");
  require(sky->getFxType() == "STD_iwa_LinearGradientFx",
          "Sky factory type changed");
  auto* start =
      dynamic_cast<TPointParam*>(sky->getParams()->getParam("startPoint"));
  auto* end =
      dynamic_cast<TPointParam*>(sky->getParams()->getParam("endPoint"));
  require(start && end, "Sky gradient endpoints are missing");
  require(start->getValue(kFrame) == TPointD(0, 170), "Gradient start changed");
  require(end->getValue(kFrame) == TPointD(0, -170), "Gradient end changed");
  checkColor(sky, "startColor", TPixel32(11, 20, 43, 255));
  checkColor(sky, "endColor", TPixel32(49, 91, 112, 255));
}

void checkGraphOwnership(ToonzScene* scene) {
  auto* sheet = scene->getXsheet();
  auto* dag   = sheet->getFxDag();
  require(dag->getInternalFxs()->getFxCount() == 3,
          "Internal effect count changed");
  require(dag->getTerminalFxs()->getFxCount() == 0, "Terminal effects changed");
  require(dag->getOutputFxCount() == 1, "Output effect count changed");
  require(dag->getCurrentOutputFx() == dag->getOutputFx(0),
          "Current output changed");
  for (int column = 0; column < 3; ++column)
    require(sheet->getColumn(column) != nullptr, "Native column is missing");
  require(
      dynamic_cast<TLevelColumnFx*>(sheet->getColumn(0)->getFx()) != nullptr,
      "Harbor column lost its native level effect");
  require(
      dynamic_cast<TLevelColumnFx*>(sheet->getColumn(1)->getFx()) != nullptr,
      "Courier column lost its native level effect");
  auto* column = dynamic_cast<TXshZeraryFxColumn*>(sheet->getColumn(2));
  require(column != nullptr, "Sky column lost its generator type");
  auto* wrapper = column->getZeraryColumnFx();
  require(wrapper != nullptr, "Sky column lost its wrapper");
  require(wrapper->getXshColumn() == column,
          "Sky wrapper lost its column owner");
  require(wrapper->getColumnIndex() == 2, "Sky wrapper column index changed");
  require(wrapper->getZeraryFx() != nullptr, "Sky wrapper lost its generator");
  require(wrapper->getZeraryFx()->getColumnFx() == wrapper,
          "Generator owner changed");
  for (int row = 0; row < 120; ++row)
    require(!sheet->getCell(row, 2).isEmpty(),
            "Sky generator exposure is missing");
  require(sheet->getCell(120, 2).isEmpty(),
          "Sky generator extends beyond the shot");
}

void checkGraph(ToonzScene* scene) {
  checkGraphOwnership(scene);
  auto* sheet      = scene->getXsheet();
  auto* dag        = sheet->getFxDag();
  TFx* sky         = sheet->getColumn(2)->getFx();
  TFx* harbor      = sheet->getColumn(0)->getFx();
  TFx* courier     = sheet->getColumn(1)->getFx();
  TFx* environment = namedFx(scene, L"Harbor over sky");
  TFx* glow        = namedFx(scene, L"Courier halo");
  TFx* final       = namedFx(scene, L"Courier over harbor");
  require(environment->getFxType() == "overFx",
          "Harbor composite type changed");
  require(glow->getFxType() == "STD_glowFx", "Glow factory type changed");
  require(final->getFxType() == "overFx", "Final composite type changed");
  checkSky(sky);
  checkPort(environment, "Source1", harbor);
  checkPort(environment, "Source2", sky);
  checkPort(glow, "Source", courier);
  checkPort(glow, "Light", courier);
  checkPort(final, "Source1", glow);
  checkPort(final, "Source2", environment);
  checkPort(dag->getCurrentOutputFx(), "source", final);
  require(!dag->getTerminalFxs()->containsFx(harbor),
          "Harbor terminal flag changed");
  require(!dag->getTerminalFxs()->containsFx(courier),
          "Courier terminal flag changed");
  checkNumber(glow, "value", 7);
  checkNumber(glow, "brightness", 55);
  checkNumber(glow, "fade", 100);
  checkColor(glow, "color", TPixel32(82, 214, 255, 255));
}

void checkScene(const QJsonObject& drawings, bool edited) {
  ToonzScene* scene = currentScene();
  require(scene != nullptr, "Normal app has no loaded scene");
  require(scene->getCurrentCamera() != nullptr, "Normal app has no camera");
  require(scene->getLevelSet()->getLevelCount() == 2, "Native cast changed");
  require(scene->getCurrentCamera()->getRes() == TDimension(512, 288),
          "Camera resolution changed");
  const TDimensionD size = scene->getCurrentCamera()->getSize();
  near(size.lx, 4, "Camera width changed");
  near(size.ly, 2.25, "Camera height changed");
  near(scene->getProperties()->getOutputProperties()->getFrameRate(), 24,
       "Frame rate changed");
  checkLevel(scene, "Harbor", drawings["Harbor"].toObject());
  checkLevel(scene, "Courier", drawings["Courier"].toObject());
  checkExposures(scene);
  checkAnimation(scene, edited);
  checkGraph(scene);
}

TRenderSettings renderSettings(ToonzScene* scene, const TFilePath& path) {
  TRenderSettings settings;
  settings.m_bpp              = 32;
  settings.m_linearColorSpace = false;
  settings.m_colorSpaceGamma  = 2.2;
  settings.m_gamma            = 1;
  settings.m_timeStretchFrom = settings.m_timeStretchTo = 1;
  settings.m_shrinkX = settings.m_shrinkY = 1;
  settings.m_fieldPrevalence              = TRenderSettings::NoField;
  settings.m_stereoscopic                 = false;
  settings.m_maxTileSize                  = TOutputProperties::SmallVal;
  auto* output = scene->getProperties()->getOutputProperties();
  output->setRenderSettings(settings);
  output->setRange(kFrame, kFrame, 1);
  output->setOffset(0);
  output->setAspectMode(TOutputProperties::NoAspectExport);
  output->setMultimediaRendering(0);
  output->getBoardSettings()->setActive(false);
  output->setPath(path);
  output->formatTemplateFId() = TFrameId(1);
  auto* properties            = output->getFileFormatProperties("png");
  require(properties != nullptr, "Native PNG writer is missing");
  auto* alpha =
      dynamic_cast<TBoolProperty*>(properties->getProperty("Alpha Channel"));
  require(alpha != nullptr, "Native PNG alpha option is missing");
  alpha->setValue(true);
  return settings;
}

class RenderListener final : public MovieRenderer::Listener {
public:
  QEventLoop loop;
  std::atomic<int> frames{0};
  std::atomic<bool> failed{false};
  bool completed = false;

  bool onFrameCompleted(int frame) override {
    if (frame != kFrame) failed = true;
    ++frames;
    return true;
  }
  bool onFrameFailed(int, TException&) override {
    failed = true;
    return false;
  }
  void onSequenceCompleted(const TFilePath&) override {}
  void onSequenceFinished(const TFilePath&,
                          MovieRenderer::CompletionStatus status,
                          const std::wstring&) override {
    QTimer::singleShot(0, &loop, [this, status]() {
      completed = status == MovieRenderer::CompletionStatus::Completed;
      loop.quit();
    });
  }
};

QImage renderFrame(const QString& label) {
  const QString directory = fixturePath(label);
  require(QDir().mkpath(directory), "Cannot create render evidence directory");
  const TFilePath path = nativePath(QDir(directory).filePath("frame..png"));
  ToonzScene* scene    = currentScene();
  const auto settings  = renderSettings(scene, path);
  RenderListener listener;
  MovieRenderer renderer(scene, path, 1, false);
  renderer.setRenderSettings(settings);
  const TPointD dpi = scene->getCurrentCamera()->getDpi();
  renderer.setDpi(dpi.x, dpi.y);
  renderer.enablePrecomputing(false);
  renderer.addListener(&listener);
  TFxPair pair;
  pair.m_frameA =
      buildSceneFx(scene, kFrame, 1, false, &settings.m_activeChecks);
  require(bool(pair.m_frameA), "Native app scene has no render graph");
  renderer.addFrame(kFrame, pair);
  QTimer cancel;
  bool expired = false;
  QObject::connect(&cancel, &QTimer::timeout, &listener.loop, [&]() {
    expired = true;
    renderer.onCanceled();
    cancel.start(10);
  });
  cancel.start(60000);
  renderer.start();
  listener.loop.exec();
  cancel.stop();
  require(!expired, "Native app render exceeded its deadline and was canceled");
  require(listener.completed && !listener.failed && listener.frames == 1,
          "Native app render did not close one successful frame");
  QImage image(QDir(directory).filePath("frame.0061.png"));
  require(image.size() == QSize(512, 288),
          "Native app rendered wrong frame dimensions");
  return image.convertToFormat(QImage::Format_RGBA8888);
}

void editWithNativeUndo() {
  TStageObject* peg       = flightPeg(currentScene());
  TDoubleParam* parameter = peg->getParam(TStageObject::T_X);
  const double original   = parameter->getValue(119);
  auto* undo              = TUndoManager::manager();
  const int before        = undo->getHistoryCount();
  {
    KeyframeSetter edit(parameter, parameter->getClosestKeyframe(119));
    edit.setValue(original + 0.125);
  }
  require(undo->getHistoryCount() == before + 1,
          "Native edit did not create undo history");
  near(parameter->getValue(119), original + 0.125,
       "Native edit did not change the peg");
  undo->undo();
  near(parameter->getValue(119), original,
       "Native undo did not restore the peg");
  undo->redo();
  near(parameter->getValue(119), original + 0.125,
       "Native redo did not restore the edit");
  TApp::instance()->getCurrentScene()->notifySceneChanged();
}

}  // namespace

void runMcpHandoffIntegrationTest() {
  Preferences::instance()->setValue(pixelsOnly, false);
  const QJsonObject drawings = readProof();
  const QString path         = fixturePath("package/scene.tnz");
  loadNormally(path);
  checkScene(drawings, false);
  const QByteArray palette = paletteSnapshot("palette-before");
  std::cout << "mcp-handoff-native: normal-load PASS" << std::endl;
  QImage reference(fixturePath("reference.png"));
  require(!reference.isNull(), "Worker preview reference is missing");
  const QImage before = renderFrame("before-edit");
  require(before == reference.convertToFormat(QImage::Format_RGBA8888),
          "Normal app render differs from the worker preview");
  std::cout << "mcp-handoff-native: original-render PASS" << std::endl;
  editWithNativeUndo();
  checkScene(drawings, true);
  require(IoCmd::saveAll(IoCmd::SILENTLY_OVERWRITE), "Native Save All failed");
  require(!TApp::instance()->getCurrentScene()->getDirtyFlag(),
          "Native save left the scene dirty");
  IoCmd::newScene();
  require(currentScene()->getLevelSet()->getLevelCount() == 0,
          "Native New Scene did not unload assets");
  loadNormally(path);
  checkScene(drawings, true);
  require(paletteSnapshot("palette-after") == palette,
          "Native edit/save/reload changed the palette");
  std::cout << "mcp-handoff-native: edit-undo-save-reload PASS" << std::endl;
  require(renderFrame("after-edit") != before,
          "Saved native peg edit did not change rendered pixels");
  std::cout << "mcp-handoff-native: edited-render PASS" << std::endl;
}
