#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tenv.h"
#include "texception.h"
#include "texpression.h"
#include "tfx.h"
#include "tnotanimatableparam.h"
#include "toutputproperties.h"
#include "tparamcontainer.h"
#include "trasterfx.h"
#include "tseexprfx.h"
#include "toonz/fxdag.h"
#include "toonz/preferences.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshzeraryfxcolumn.h"

#include <QCoreApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

DV_IMPORT_API void initStdFx();

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

TFilePath path(const QString &value) { return TFilePath(value.toStdWString()); }

QJsonObject document(const QString &rgb, bool generator, int version = 1) {
  QJsonObject result{{"version", version}, {"backend", "interpreter"},
      {"rgb", rgb}, {"alpha", generator ? "1" : "$a"},
      {"controls", QJsonArray()}};
  if (generator) {
    result["controls"] = QJsonArray{QJsonObject{
        {"id", "gain"}, {"name", "gain"}, {"label", "Gain"},
        {"type", "scalar"}, {"value", 0.75}, {"min", 0.0}, {"max", 1.0}}};
  }
  return result;
}

void setDocument(TFx &fx, const QJsonObject &value) {
  auto *source = dynamic_cast<TFxSeExprSource *>(&fx);
  require(source != nullptr, "registered effect lacks native authoring interface");
  source->seExprDocumentParam()->setValue(
      QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)).toStdWString());
}

TDoubleParam *number(TFx &fx, const char *name) {
  auto *value = dynamic_cast<TDoubleParam *>(fx.getParams()->getParam(name));
  require(value != nullptr, "native numeric effect control is missing");
  return value;
}

void makeScene(const QDir &root, const QString &name, const QString &rgb,
               bool brokenAnimation = false, int version = 1,
               int frameCount = 2) {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  const QString sceneName = root.filePath("portablestuff/projects/acceptance/scenes/" + name + ".tnz");
  scene.setScenePath(path(sceneName));
  auto *camera = scene.getCurrentCamera();
  require(camera != nullptr, "native scene has no camera");
  camera->setRes(TDimension(32, 32));
  camera->setSize(TDimensionD(1, 1));
  scene.getProperties()->cloneCamerasFrom(scene.getXsheet()->getStageObjectTree());
  auto *output = scene.getProperties()->getOutputProperties();
  output->setRange(0, frameCount - 1, 1);
  output->setFrameRate(24.0);
  output->setPath(path(root.filePath("output/" + name + ".png")));

  TFxP generator(TFx::create("STD_seexprGeneratorFx"));
  TFxP color(TFx::create("STD_seexprColorFx"));
  require(generator && color, "native SeExpr effect registration is missing");
  setDocument(*generator, document(rgb, true));
  number(*generator, "width")->setDefaultValue(128);
  number(*generator, "height")->setDefaultValue(128);
  auto *gain = number(*generator, "seexprControl_gain__value");
  gain->setValue(0, 0.75);
  gain->setValue(frameCount - 1, 0.5);
  require(std::abs(gain->getValue(0) - 0.75) < 1e-12 &&
          std::abs(gain->getValue(frameCount - 1) - 0.5) < 1e-12,
          "named native control keyframes were not retained");
  if (version != 1) setDocument(*generator, document(rgb, true, version));
  setDocument(*color, document("$src * 0.5", false));

  auto *column = new TXshZeraryFxColumn(frameCount);
  column->getZeraryColumnFx()->setZeraryFx(generator.getPointer());
  TXsheet *xsheet = scene.getXsheet();
  xsheet->insertColumn(0, column);
  TFx *columnFx = column->getFx();
  color->getInputPort(0)->setFx(columnFx);
  auto *dag = xsheet->getFxDag();
  dag->assignUniqueId(color.getPointer());
  dag->getInternalFxs()->addFx(color.getPointer());
  dag->removeFromXsheet(columnFx);
  dag->addToXsheet(color.getPointer());
  if (brokenAnimation) {
    auto *horizontal = xsheet->getStageObject(TStageObjectId::ColumnId(0))
                           ->getParam(TStageObject::T_X);
    horizontal->setMeasureName("");
    TDoubleKeyframe first(0, 0), last(1, 0);
    first.m_type = TDoubleKeyframe::Expression;
    first.m_expressionText = TExpression::makeSeExprText("1/($frame-$frame)");
    last.m_type = TDoubleKeyframe::Linear;
    horizontal->setKeyframe(first);
    horizontal->setKeyframe(last);
  }
  xsheet->updateFrameCount();
  require(scene.getFrameCount() == frameCount, "native scene exposure range is incorrect");
  scene.saveCopy(path(sceneName));
  require(QFile::exists(sceneName), "native scene was not saved");
  ToonzScene reopened;
  reopened.loadTnzFile(path(sceneName));
  require(reopened.getFrameCount() == frameCount, "saved scene exposure range changed");
  auto *reopenedColumn = reopened.getXsheet()->getColumn(0)->getZeraryFxColumn();
  require(reopenedColumn != nullptr, "saved generator column was lost");
  TFx *reopenedGenerator = reopenedColumn->getZeraryColumnFx()->getZeraryFx();
  auto *reopenedSource = dynamic_cast<TFxSeExprSource *>(reopenedGenerator);
  require(reopenedSource != nullptr, "saved native expression source was lost");
  const auto stored = QJsonDocument::fromJson(QString::fromStdWString(
      reopenedSource->seExprDocumentParam()->getValue()).toUtf8()).object();
  require(stored.value("rgb").toString() == rgb &&
          stored.value("version").toInt() == version,
          "saved expression source or version changed");
  // Unsupported documents intentionally retain source without constructing
  // controls, so the normal controls check applies to supported schemas only.
  if (version == 1) {
    auto *reopenedGain = number(*reopenedGenerator, "seexprControl_gain__value");
    require(std::abs(reopenedGain->getValue(0) - 0.75) < 1e-12 &&
            std::abs(reopenedGain->getValue(frameCount - 1) - 0.5) < 1e-12,
            "saved named control animation changed");
  }
}

void verifyOutput(const QDir &root) {
  const QDir output(root.filePath("output"));
  const auto images = output.entryList({"valid*.png"}, QDir::Files);
  require(images.size() == 2, "valid batch did not write both requested frames");
  for (const auto &file : images) {
    const QImage image(output.filePath(file));
    require(!image.isNull() && image.width() == 32 && image.height() == 32,
            "batch image size or decoding is incorrect");
    const QColor center = image.pixelColor(16, 16);
    require(center.alpha() > 240 && center.red() >= center.green() &&
            center.green() > center.blue() && center.blue() > 0,
            "native generator and color chain did not render expected channels");
  }
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    require(argc == 2 || (argc == 3 && QString::fromLocal8Bit(argv[2]) == "--verify-output"),
            "fixture expects its temporary directory and optional output check");
    const QDir root(QString::fromLocal8Bit(argv[1]));
    require(root.isAbsolute() && QFile::exists(root.filePath(".seexpr-acceptance")),
            "fixture requires an explicitly marked disposable directory");
    if (argc == 3) { verifyOutput(root); return 0; }
    TEnv::setRootVarName("TOONZROOT");
    TEnv::setSystemVarPrefix("TOONZ");
    TEnv::setModuleName("seexpr_batch_fixture");
    TEnv::setStuffDir(path(root.filePath("portablestuff")));
    Preferences::instance()->setValue(projectRoot, 8, false);
    Preferences::instance()->setValue(compressSceneFiles, false, false);
    initStdFx();
    const TFilePath projects = path(root.filePath("portablestuff/projects"));
    auto *manager = TProjectManager::instance();
    manager->clearProjectsRoot();
    manager->addProjectsRoot(projects);
    const auto project = std::make_shared<TProject>();
    for (const auto &name : {TProject::Inputs, TProject::Drawings, TProject::Scenes,
                            TProject::Extras, TProject::Outputs, TProject::Scripts,
                            TProject::Palettes}) project->setFolder(name);
    const TFilePath projectPath = path(root.filePath(
        "portablestuff/projects/acceptance/acceptance_otprj.xml"));
    require(project->save(projectPath), "disposable project could not be saved");
    manager->setCurrentProjectPath(projectPath);
    QDir().mkpath(root.filePath("output"));
    makeScene(root, "valid", "[$gain,0.25,0.125]");
    makeScene(root, "syntax-error", "noise(");
    makeScene(root, "runtime-error", "[1/($frame-$frame),0,0]");
    makeScene(root, "runtime-first", "[$gain/abs($frame-1),0.25,0.125]", false, 1, 3);
    makeScene(root, "runtime-middle", "[$gain/abs($frame-2),0.25,0.125]", false, 1, 3);
    makeScene(root, "runtime-last", "[$gain/abs($frame-3),0.25,0.125]", false, 1, 3);
    makeScene(root, "animation-error", "[$gain,0.25,0.125]", true);
    // Preserve unsupported data for repair while requiring batch failure.
    makeScene(root, "unsupported-version", "[$gain,0.25,0.125]", false, 999);
    std::cout << "SeExpr native batch fixtures: PASS" << std::endl;
    return 0;
  } catch (const TException &) {
    std::cerr << "SeExpr native fixture failed with a native exception." << std::endl;
  } catch (const std::exception &error) {
    std::cerr << "SeExpr native fixture failed: " << error.what() << std::endl;
  }
  return 1;
}
