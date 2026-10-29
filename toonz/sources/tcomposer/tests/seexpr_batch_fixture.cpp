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
#include "../../glb/tests/glbfixture.h"

#include <QCoreApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

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

void string(TFx &fx, const char *name, const QString &value) {
  auto *parameter = dynamic_cast<TStringParam *>(fx.getParams()->getParam(name));
  require(parameter != nullptr, "native string effect control is missing");
  parameter->setValue(value.toStdWString());
}

void choice(TFx &fx, const char *name, int value) {
  auto *parameter = dynamic_cast<TIntEnumParam *>(fx.getParams()->getParam(name));
  require(parameter != nullptr, "native choice effect control is missing");
  parameter->setValue(value);
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

void makeSurfaceScene(const QDir &root, const QString &name,
                      bool animatedDisplacement, bool brokenDisplacement,
                      bool imagePlane = false) {
  constexpr int frames = 3;
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  const QString sceneName = root.filePath("portablestuff/projects/acceptance/scenes/" + name + ".tnz");
  scene.setScenePath(path(sceneName));
  auto *camera = scene.getCurrentCamera();
  require(camera != nullptr, "native surface scene has no camera");
  camera->setRes(TDimension(32, 32));
  camera->setSize(TDimensionD(1, 1));
  scene.getProperties()->cloneCamerasFrom(scene.getXsheet()->getStageObjectTree());
  scene.getProperties()->setBgColor(TPixel32(0, 0, 0, 0));
  auto *output = scene.getProperties()->getOutputProperties();
  output->setRange(0, frames - 1, 1);
  output->setFrameRate(24);
  output->setPath(path(root.filePath("output/" + name + ".png")));
  auto settings = output->getRenderSettings();
  settings.m_bpp = 32; settings.m_gamma = 1;
  settings.m_linearColorSpace = false;
  output->setRenderSettings(settings);

  TFxP source(TFx::create(imagePlane ? "STD_seexprGeneratorFx" : "STD_glbModelFx"));
  TFxP transform(TFx::create("STD_transform3DFx"));
  TFxP surface(TFx::create("STD_seexprSurfaceFx"));
  TFxP attributes(TFx::create("STD_seexprAttributeFx"));
  TFxP plane;
  require(source && transform && surface && attributes,
          "production surface, attribute or 3D effect registration is missing");
  if (imagePlane) {
    auto generator = document("[0.25,0.5,0.75]", true);
    generator["alpha"] = "0.5";
    setDocument(*source, generator);
    number(*source, "width")->setDefaultValue(64);
    number(*source, "height")->setDefaultValue(64);
    plane = TFx::create("STD_imagePlaneFx");
    require(bool(plane), "production Image Plane effect registration is missing");
  } else {
    require(QDir().mkpath(root.filePath("assets")), "surface asset directory was not created");
    const QString filename = root.filePath("assets/surface.glb");
    if (!QFile::exists(filename)) {
      QFile file(filename);
      const auto bytes = triangleGlb(0, true);
      require(file.open(QIODevice::WriteOnly) &&
                  file.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) == qint64(bytes.size()),
              "generated surface geometry could not be written");
    }
    string(*source, "modelFile", filename);
    number(*source, "scale")->setDefaultValue(20);
    choice(*source, "projection", 1);
    choice(*source, "colorMode", 1);
  }
  number(*transform, "rotationZ")->setDefaultValue(10);
  const QString rgb = imagePlane ? "[$r,$shot::tint[0],$b]"
                                : "[($Po[0]+1)/2,$shot::tint[0],0.125]";
  const QString displacement = brokenDisplacement ? "1/($frame-2)"
      : animatedDisplacement ? "2*($frame-1)" : "0";
  auto declaration = document(rgb, false);
  declaration["alpha"] = imagePlane ? "$a" : "0.5";
  declaration["displacement"] = displacement;
  setDocument(*surface, declaration);
  string(*attributes, "namespace", "shot");
  string(*attributes, "name", "tint");
  for (int frame = 0; frame < frames; ++frame)
    number(*attributes, "value")->setValue(frame, .25 * (frame + 1));

  auto *column = new TXshZeraryFxColumn(frames);
  column->getZeraryColumnFx()->setZeraryFx(source.getPointer());
  TXsheet *xsheet = scene.getXsheet();
  xsheet->insertColumn(0, column);
  TFx *columnFx = column->getFx();
  if (imagePlane) plane->getInputPort("Source")->setFx(columnFx);
  transform->getInputPort("Source")->setFx(imagePlane ? plane.getPointer() : columnFx);
  surface->getInputPort("Source")->setFx(transform.getPointer());
  // Render-local providers wrap the expression they affect. This exercises
  // actual scheduler settings propagation rather than passing a test snapshot.
  attributes->getInputPort("Source")->setFx(surface.getPointer());
  auto *dag = xsheet->getFxDag();
  for (const auto &fx : {plane, transform, surface, attributes}) {
    if (!fx) continue;
    dag->assignUniqueId(fx.getPointer());
    dag->getInternalFxs()->addFx(fx.getPointer());
  }
  dag->removeFromXsheet(columnFx);
  dag->addToXsheet(attributes.getPointer());
  xsheet->updateFrameCount();
  require(scene.getFrameCount() == frames, "surface exposure range was not retained");
  scene.saveCopy(path(sceneName));
  require(QFile::exists(sceneName), "surface scene was not saved");

  ToonzScene reopened;
  reopened.loadTnzFile(path(sceneName));
  require(reopened.getFrameCount() == frames, "saved surface exposure range changed");
  TFx *restoredSurface = nullptr, *restoredAttributes = nullptr, *restoredTransform = nullptr;
  const auto *stored = reopened.getXsheet()->getFxDag()->getInternalFxs();
  for (int i = 0; i < stored->getFxCount(); ++i) {
    TFx *fx = stored->getFx(i);
    if (fx->getFxType() == "STD_seexprSurfaceFx") restoredSurface = fx;
    if (fx->getFxType() == "STD_seexprAttributeFx") restoredAttributes = fx;
    if (fx->getFxType() == "STD_transform3DFx") restoredTransform = fx;
  }
  require(restoredSurface && restoredAttributes && restoredTransform &&
              restoredAttributes->getInputPort("Source")->getFx() == restoredSurface &&
              restoredSurface->getInputPort("Source")->getFx() == restoredTransform,
          "saved surface graph lost its production effect connections");
  auto *native = dynamic_cast<TFxSeExprSource *>(restoredSurface);
  require(native != nullptr, "saved surface lost its native expression authoring interface");
  const auto json = QJsonDocument::fromJson(QString::fromStdWString(
      native->seExprDocumentParam()->getValue()).toUtf8()).object();
  require(json.value("rgb").toString() == rgb &&
              json.value("displacement").toString() == displacement &&
              number(*restoredTransform, "rotationZ")->getValue(0) == 10,
          "saved surface expressions or 3D transform changed");
  for (int frame = 0; frame < frames; ++frame)
    require(number(*restoredAttributes, "value")->isKeyframe(frame) &&
                number(*restoredAttributes, "value")->getValue(frame) == .25 * (frame + 1),
            "saved native attribute animation changed");
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

QMap<int, QImage> surfaceImages(const QDir &root, const QString &prefix) {
  const QDir output(root.filePath("output"));
  const auto files = output.entryList({prefix + ".*.png"}, QDir::Files);
  require(files.size() == 3, "surface batch did not write its three requested frames");
  const QRegularExpression pattern("^" + QRegularExpression::escape(prefix) + "\\.([0-9]+)\\.png$");
  QMap<int, QImage> result;
  for (const auto &filename : files) {
    const auto match = pattern.match(filename);
    require(match.hasMatch(), "surface output filename is malformed");
    const int frame = match.captured(1).toInt();
    require(frame >= 1 && frame <= 3 && !result.contains(frame), "surface output frame number changed");
    QImage image(output.filePath(filename));
    require(!image.isNull() && image.width() == 32 && image.height() == 32,
            "surface batch output did not decode to the requested dimensions");
    result.insert(frame, std::move(image));
  }
  return result;
}

int coveredSurfacePixels(const QImage &image) {
  int result = 0;
  for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
    if (image.pixelColor(x, y).alpha() >= 64) ++result;
  return result;
}

void verifySurfaceOutput(const QDir &root) {
  const auto single = surfaceImages(root, "surface-valid");
  const auto workers = surfaceImages(root, "surface-workers");
  const auto baseline = surfaceImages(root, "surface-static");
  const auto imagePlane = surfaceImages(root, "surface-image-plane");
  for (int frame = 1; frame <= 3; ++frame) {
    const QImage &current = single.constFind(frame).value();
    const QImage &parallel = workers.constFind(frame).value();
    const QImage &fixed = baseline.constFind(frame).value();
    const QImage &planeImage = imagePlane.constFind(frame).value();
    require(current == parallel, "actual batch worker counts changed surface pixels");
    for (const auto *image : {&current, &fixed, &planeImage}) {
      const auto center = image->pixelColor(16, 16);
      require(std::abs(center.alpha() - 128) <= 2 &&
                  std::abs(center.green() - int(std::round(255 * .25 * frame))) <= 3,
              "surface batch lost known coverage, animated attributes or straight opacity");
      require(coveredSurfacePixels(*image) > 20 && coveredSurfacePixels(*image) < 1000,
              "surface batch returned an empty or full-frame placeholder");
    }
    const auto color = current.pixelColor(16, 16);
    require(color.red() >= 80 && color.red() <= 180 && std::abs(color.blue() - 32) <= 3,
            "surface batch did not preserve the expected object-position and blue channels");
    const auto plane = planeImage.pixelColor(16, 16);
    require(std::abs(plane.red() - 64) <= 3 && std::abs(plane.blue() - 191) <= 3,
            "real Image Plane scheduling lost the source raster or applied alpha twice");
    int minimum = 255, maximum = 0;
    for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
      const auto pixel = current.pixelColor(x, y);
      require(fixed.pixelColor(x, y).alpha() == baseline.constFind(1).value().pixelColor(x, y).alpha(),
              "attribute animation changed static surface geometry");
      if (pixel.alpha() < 120) continue;
      minimum = std::min(minimum, pixel.red()); maximum = std::max(maximum, pixel.red());
    }
    require(maximum - minimum > 60, "saved surface was shaded at a centroid instead of at coverage samples");
  }
  require(coveredSurfacePixels(single[3]) > coveredSurfacePixels(single[1]) + 4 &&
              coveredSurfacePixels(single[3]) > coveredSurfacePixels(baseline[3]) + 4,
          "animated displacement failed to enlarge the actual projected batch geometry");
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    require(argc == 2 || (argc == 3 &&
                (QString::fromLocal8Bit(argv[2]) == "--verify-output" ||
                 QString::fromLocal8Bit(argv[2]) == "--verify-surfaces")),
            "fixture expects its temporary directory and optional output check");
    const QDir root(QString::fromLocal8Bit(argv[1]));
    require(root.isAbsolute() && QFile::exists(root.filePath(".seexpr-acceptance")),
            "fixture requires an explicitly marked disposable directory");
    if (argc == 3) {
      if (QString::fromLocal8Bit(argv[2]) == "--verify-surfaces") verifySurfaceOutput(root);
      else verifyOutput(root);
      return 0;
    }
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
    makeSurfaceScene(root, "surface-valid", true, false);
    makeSurfaceScene(root, "surface-static", false, false);
    makeSurfaceScene(root, "surface-runtime-middle", false, true);
    makeSurfaceScene(root, "surface-image-plane", false, false, true);
    std::cout << "SeExpr native batch fixtures: PASS" << std::endl;
    return 0;
  } catch (const TException &) {
    std::cerr << "SeExpr native fixture failed with a native exception." << std::endl;
  } catch (const std::exception &error) {
    std::cerr << "SeExpr native fixture failed: " << error.what() << std::endl;
  }
  return 1;
}
