#include "automation.h"
#include "automation_fx.h"
#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "toonz/fxdag.h"
#include "toonz/scenefx.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "tparamcontainer.h"
#include "traster.h"
#include "trenderer.h"
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QFileInfo>
#include <functional>
#include <limits>
#include <map>
#include <stdexcept>

using namespace GroundzeroAutomation;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
QJsonObject call(Context& context, Document& document, const QString& method,
                 QJsonObject params) {
  params["document_id"] = document.id;
  return executeFx(context, method, params).toObject();
}
TFx* create(Context& context, Document& document, const QString& type) {
  return document.fx(call(context, document, "fx.create",
                          {{"type", type}, {"frame_count", 13}})
                         .value("fx_id")
                         .toString());
}
void parameter(Context& context, Document& document, TFx* fx,
               const QString& name, const QJsonValue& value) {
  call(
      context, document, "fx.parameter.set",
      {{"fx_id", document.idForFx(fx)}, {"parameter", name}, {"value", value}});
}
QImage render(ToonzScene& scene, int frame) {
  TRenderer renderer(1);
  const auto id = TRenderer::buildRenderId();
  renderer.install(id);
  renderer.declareRenderStart(id);
  renderer.declareFrameStart(frame);
  TRasterFxP fx = buildSceneFx(&scene, double(frame));
  require(bool(fx), "Scene renderer produced no effect");
  TTile tile;
  TRenderSettings settings;
  settings.m_bpp = 32;
  fx->allocateAndCompute(tile, TPointD(-32, -24), TDimension(64, 48),
                         TRaster32P(1, 1), frame, settings);
  TRaster32P raster = tile.getRaster();
  QImage image(64, 48, QImage::Format_ARGB32);
  raster->lock();
  for (int y = 0; y < 48; ++y)
    for (int x = 0; x < 64; ++x) {
      const auto pixel = raster->pixels(47 - y)[x];
      image.setPixel(x, y, qRgba(pixel.r, pixel.g, pixel.b, pixel.m));
    }
  raster->unlock();
  renderer.declareFrameEnd(frame);
  renderer.declareRenderEnd(id);
  renderer.uninstall();
  return image;
}

const QStringList types = {"STD_brightContFx", "STD_hsvScaleFx", "multFx",
                           "screenFx",         "inFx",           "outFx",
                           "invertFx",         "blendFx"};
void configure(Context& context, Document& document, TFx* effect,
               const QString& type) {
  if (type == "STD_brightContFx")
    parameter(context, document, effect, "brightness", 40);
  if (type == "STD_hsvScaleFx") parameter(context, document, effect, "hue", 90);
  if (type == "multFx") parameter(context, document, effect, "value", 40);
  if (type == "blendFx") parameter(context, document, effect, "value", 35);
}
void attach(TFx* effect, TFx* fore, TFx* back) {
  require(effect->getInputPortCount() >= 1 && effect->getInputPortCount() <= 2,
          "Candidate port shape unexpectedly changed");
  effect->getInputPort(0)->setFx(fore);
  if (effect->getInputPortCount() == 2) effect->getInputPort(1)->setFx(back);
}
QJsonArray imageHashes(ToonzScene& scene, const QString& prefix) {
  QJsonArray result;
  for (int frame : {0, 6, 12}) {
    const QImage image = render(scene, frame);
    const QString path = prefix + QString::number(frame) + ".png";
    require(image.save(path), "Cannot retain actual candidate PNG");
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read candidate PNG");
    result.append(QString::fromLatin1(
        QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256)
            .toHex()));
  }
  return result;
}

std::map<QString, std::pair<double, double>> bounds(const QString& type) {
  if (type == "STD_brightContFx")
    return {{"globalIntensity", {0, 1}},
            {"brightness", {-127, 127}},
            {"contrast", {-127, 127}}};
  if (type == "STD_hsvScaleFx")
    return {{"globalIntensity", {0, 1}}, {"hue", {-180, 180}},
            {"saturation", {-1, 1}},     {"value", {-1, 1}},
            {"hue_scale", {0, 400}},     {"saturation_scale", {0, 400}},
            {"value_scale", {0, 400}}};
  if (type == "multFx") return {{"value", {-255, 255}}};
  if (type == "blendFx") return {{"value", {0, 100}}};
  return {};
}
QJsonArray boundaryChecks(Context& context, Document& document, TFx* effect,
                          const QString& type) {
  QJsonArray checks;
  for (const auto& [name, range] : bounds(type)) {
    auto* value = dynamic_cast<TDoubleParam*>(
        effect->getParams()->getParam(name.toStdString()));
    require(value, "Bounded candidate scalar is absent");
    const double original = value->getDefaultValue();
    for (double boundary : {range.first, range.second}) {
      parameter(context, document, effect, name, boundary);
      const QImage first = render(*document.scene, 6);
      require(first == render(*document.scene, 6),
              "Candidate boundary rendering is nondeterministic");
      checks.append(QJsonObject{
          {"parameter", name},
          {"value", boundary},
          {"center_rgba", QString::number(first.pixel(32, 24), 16)}});
    }
    parameter(context, document, effect, name, original);
  }
  if (type == "multFx") {
    parameter(context, document, effect, "matte", true);
    const auto image = render(*document.scene, 6);
    require(image == render(*document.scene, 6),
            "Multiply alpha option is nondeterministic");
    parameter(context, document, effect, "matte", false);
    checks.append(QJsonObject{{"parameter", "matte"}, {"value", true}});
  }
  return checks;
}
QString animate(Context& context, Document& document, TFx* effect,
                const QString& type) {
  const std::map<QString, QString> names{{"STD_brightContFx", "brightness"},
                                         {"STD_hsvScaleFx", "hue"},
                                         {"multFx", "value"},
                                         {"blendFx", "value"}};
  const auto found = names.find(type);
  if (found == names.end()) return {};
  const double low  = type == "blendFx" ? 25 : -60;
  const double high = type == "blendFx" ? 75 : 60;
  for (int frame : {0, 12})
    call(context, document, "fx.parameter.set_key",
         {{"fx_id", document.idForFx(effect)},
          {"parameter", found->second},
          {"frame", frame},
          {"value", frame ? high : low}});
  return found->second;
}
void endpointOracles(Context& context, Document& document, TFx* effect,
                     const QString& type) {
  if (type != "STD_brightContFx" && type != "blendFx") return;
  auto* output = document.scene->getXsheet()
                     ->getFxDag()
                     ->getCurrentOutputFx()
                     ->getInputPort(0);
  TFx* fore    = effect->getInputPort(0)->getFx();
  output->setFx(fore);
  const auto source = render(*document.scene, 6);
  output->setFx(effect);
  if (type == "STD_brightContFx") {
    parameter(context, document, effect, "brightness", -127);
    const auto black = render(*document.scene, 6).pixel(32, 24);
    const int alpha  = qAlpha(source.pixel(32, 24));
    require(black == qRgba(0, 0, 0, alpha),
            "Brightness black/alpha oracle failed");
    parameter(context, document, effect, "brightness", 127);
    const auto white = render(*document.scene, 6).pixel(32, 24);
    require(white == qRgba(alpha, alpha, alpha, alpha),
            "Brightness white endpoint oracle failed");
  } else {
    parameter(context, document, effect, "value", 100);
    require(render(*document.scene, 6) == source,
            "Cross dissolve100 did not retain foreground");
    output->setFx(effect->getInputPort(1)->getFx());
    const auto back = render(*document.scene, 6);
    output->setFx(effect);
    parameter(context, document, effect, "value", 0);
    require(render(*document.scene, 6) == back,
            "Cross dissolve0 did not retain background");
  }
  configure(context, document, effect, type);
}
void verifyFrames(ToonzScene& scene, const std::map<int, QImage>& expected) {
  for (const auto& [frame, image] : expected)
    require(render(scene, frame) == image,
            "Candidate native pixels differ from original");
}
QJsonObject candidate(Context& context, const QString& type,
                      const QString& directory) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  document.scene->getXsheet()->getStageObjectTree()->getCurrentCamera()->setRes(
      TDimension(64, 48));
  document.scene->getXsheet()
      ->getStageObjectTree()
      ->getCurrentCamera()
      ->setSize(TDimensionD(1.2, 0.9));
  TFx* fore = create(context, document, "colorCardFx");
  TFx* back = create(context, document, "colorCardFx");
  parameter(context, document, back, "color", QJsonArray{40, 100, 180, 128});
  for (int frame : {0, 12})
    call(context, document, "fx.parameter.set_key",
         {{"fx_id", document.idForFx(fore)},
          {"parameter", "color"},
          {"frame", frame},
          {"value", QJsonArray{60 + frame * 12, 80, 40, 192}}});
  TFx* effect = create(context, document, type);
  configure(context, document, effect, type);
  attach(effect, fore, back);
  auto* graph = document.scene->getXsheet()->getFxDag();
  graph->getCurrentOutputFx()->getInputPort(0)->setFx(effect);
  validateAutomationFxGraph(*document.scene);
  const auto boundaries = boundaryChecks(context, document, effect, type);
  endpointOracles(context, document, effect, type);
  const auto animated = animate(context, document, effect, type);
  std::map<int, QImage> frames;
  for (int frame : {0, 6, 12}) frames[frame] = render(*document.scene, frame);
  require(frames.at(0) != frames.at(6) && frames.at(6) != frames.at(12),
          "Candidate source animation did not change visible pixels");
  call(context, document, "fx.enabled.set",
       {{"fx_id", document.idForFx(effect)}, {"enabled", false}});
  require(render(*document.scene, 0) != frames.at(0),
          "Candidate disable/bypass has no observable effect");
  call(context, document, "fx.enabled.set",
       {{"fx_id", document.idForFx(effect)}, {"enabled", true}});
  verifyFrames(*document.scene, frames);
  const auto original =
      imageHashes(*document.scene, directory + "/" + type + "-before-");
  const auto preset = call(context, document, "fx.preset.save",
                           {{"fx_id", document.idForFx(effect)},
                            {"directory", directory + "/" + type + "-preset"}});
  const auto duplicate = call(context, document, "fx.duplicate",
                              {{"fx_id", document.idForFx(effect)}});
  TFx* copy            = document.fx(duplicate.value("fx_id").toString());
  for (int index = 0; index < effect->getParams()->getParamCount(); ++index)
    require(copy->getParams()->getParam(index) !=
                effect->getParams()->getParam(index),
            "Candidate duplicate reused a parameter object");
  attach(copy, fore, back);
  graph->getCurrentOutputFx()->getInputPort(0)->setFx(copy);
  for (const auto& [frame, expected] : frames)
    require(render(*document.scene, frame) == expected,
            "Candidate duplicate changed native pixels");
  TFx* applied = create(context, document, type);
  call(context, document, "fx.preset.apply",
       {{"fx_id", document.idForFx(applied)},
        {"directory", directory + "/" + type + "-preset"}});
  attach(applied, fore, back);
  graph->getCurrentOutputFx()->getInputPort(0)->setFx(applied);
  verifyFrames(*document.scene, frames);
  const QString path = directory + "/" + type + ".tnz";
  document.scene->saveCopy(TFilePath(path.toStdWString()));
  ToonzScene loaded;
  loaded.setProject(document.scene->getProject());
  loaded.loadTnzFile(TFilePath(path.toStdWString()));
  validateAutomationFxGraph(loaded);
  for (const auto& [frame, expected] : frames)
    require(render(loaded, frame) == expected,
            "Candidate native roundtrip changed pixels");
  const auto restored =
      imageHashes(loaded, directory + "/" + type + "-restored-");
  require(original == restored,
          "Candidate saved PNG bytes differ after native roundtrip");
  const QJsonObject summary{
      {"type", type},
      {"native_fx_version", effect->getFxVersion()},
      {"descriptor", call(context, document, "fx.parameter.get",
                          {{"fx_id", document.idForFx(effect)}})},
      {"preset", preset},
      {"safe_boundary_checks", boundaries},
      {"effect_animated_parameter", animated},
      {"preset_apply_pixels_exact", true},
      {"enable_bypass_observed", true},
      {"pixel_hashes", original},
      {"roundtrip_pixels_exact", true},
      {"independent_parameter_copy", true}};
  context.removeDocument(document.id);
  return summary;
}
void rejects(const std::function<void()>& action, const QString& code) {
  try {
    action();
  } catch (const Error& error) {
    require(error.code == code, "Color/composite rejection code changed");
    return;
  }
  throw std::runtime_error("Color/composite invalid value was accepted");
}
void rejectedEdit(Context& context, Document& document, TFx* fx,
                  const QString& name, double value) {
  const auto before =
      executeFx(context, "fx.graph", {{"document_id", document.id}});
  for (const QString& method :
       {QString("fx.parameter.set"), QString("fx.parameter.set_key")}) {
    context.resetMutationStarted();
    rejects(
        [&] {
          call(context, document, method,
               {{"fx_id", document.idForFx(fx)},
                {"parameter", name},
                {"value", value}});
        },
        "OUT_OF_RANGE");
    require(!context.mutationStarted(),
            "Rejected bounds marked mutation started");
    require(executeFx(context, "fx.graph", {{"document_id", document.id}}) ==
                before,
            "Rejected bounds changed the graph or parameter keys");
  }
}
void corruptedProfile(Context& context, Document& document, TFx* fx,
                      const QString& path) {
  context.resetMutationStarted();
  rejects([&] { validateAutomationFxGraph(*document.scene); }, "OUT_OF_RANGE");
  rejects(
      [&] {
        call(context, document, "fx.duplicate",
             {{"fx_id", document.idForFx(fx)}});
      },
      "OUT_OF_RANGE");
  rejects(
      [&] {
        call(context, document, "fx.preset.save",
             {{"fx_id", document.idForFx(fx)}, {"directory", path}});
      },
      "OUT_OF_RANGE");
  require(!context.mutationStarted() && !QFileInfo::exists(path),
          "Invalid profile published a preset or began mutation");
  document.scene->saveCopy(TFilePath((path + ".tnz").toStdWString()));
  ToonzScene reopened;
  reopened.setProject(document.scene->getProject());
  reopened.loadTnzFile(TFilePath((path + ".tnz").toStdWString()));
  rejects([&] { validateAutomationFxGraph(reopened); }, "OUT_OF_RANGE");
}
void invalidBounds(Context& context, const QString& directory) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  TFx* hsv       = create(context, document, "STD_hsvScaleFx");
  TFx* multiply  = create(context, document, "multFx");
  for (const QString& name : {QString("hue_scale"), QString("saturation_scale"),
                              QString("value_scale"), QString("value")}) {
    TFx* fx           = name == "value" ? multiply : hsv;
    const double low  = name == "value" ? -255 : 0;
    const double high = name == "value" ? 255 : 400;
    rejectedEdit(context, document, fx, name, low - 0.001);
    rejectedEdit(context, document, fx, name, high + 0.001);
    auto* number = dynamic_cast<TDoubleParam*>(
        fx->getParams()->getParamVar(name.toStdString())->getParam());
    const double original = number->getDefaultValue();
    number->setDefaultValue(high + 1);
    corruptedProfile(context, document, fx,
                     directory + "/" + name + "-default");
    number->setDefaultValue(original);
    TDoubleKeyframe key(12, high + 1);
    key.m_isKeyframe = true;
    number->setKeyframe(key);
    corruptedProfile(context, document, fx, directory + "/" + name + "-key");
    number->clearKeyframes();
    validateAutomationFxGraph(*document.scene);
  }
  context.removeDocument(document.id);
}
void scalarAdmissionEdges(Context& context) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  TFx* fx        = create(context, document, "multFx");
  auto* number   = dynamic_cast<TDoubleParam*>(
      fx->getParams()->getParamVar("value")->getParam());
  number->setDefaultValue(std::numeric_limits<double>::infinity());
  rejects([&] { validateAutomationFxGraph(*document.scene); },
          "INVALID_PARAMS");
  number->setDefaultValue(0);
  for (int frame = 0; frame < 4096; ++frame) {
    TDoubleKeyframe key(frame, frame % 2 == 0 ? -255 : 255);
    key.m_isKeyframe = true;
    number->setKeyframe(key);
  }
  validateAutomationFxGraph(*document.scene);
  context.resetMutationStarted();
  rejects(
      [&] {
        call(context, document, "fx.parameter.set_key",
             {{"fx_id", document.idForFx(fx)},
              {"parameter", "value"},
              {"frame", 4096},
              {"value", 0}});
      },
      "RESOURCE_LIMIT");
  require(!context.mutationStarted() && number->getKeyframeCount() == 4096,
          "Scalar key limit rejection changed the native parameter");
  call(context, document, "fx.parameter.set_key",
       {{"fx_id", document.idForFx(fx)},
        {"parameter", "value"},
        {"frame", 4095},
        {"value", 12}});
  require(number->getKeyframeCount() == 4096 && number->getValue(4095) == 12,
          "Scalar key limit blocked replacement of an existing frame");
  TDoubleKeyframe key(4096, 0);
  key.m_isKeyframe = true;
  number->setKeyframe(key);
  rejects([&] { validateAutomationFxGraph(*document.scene); },
          "RESOURCE_LIMIT");
  number->clearKeyframes();
  context.removeDocument(document.id);
}
void retainedCurvePolicy(Context& context) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  TFx* fx        = create(context, document, "STD_hsvScaleFx");
  auto* number   = dynamic_cast<TDoubleParam*>(
      fx->getParams()->getParamVar("hue_scale")->getParam());
  TDoubleKeyframe key(0, 100);
  key.m_isKeyframe     = true;
  key.m_type           = TDoubleKeyframe::Expression;
  key.m_expressionText = "10000000000000000000";
  number->setKeyframe(key);
  rejects([&] { validateAutomationFxGraph(*document.scene); },
          "UNSUPPORTED_PARAMETER");
  number->clearKeyframes();
  fx->setFxVersion(2);
  rejects([&] { validateAutomationFxGraph(*document.scene); },
          "UNSUPPORTED_FX");
  fx->setFxVersion(1);
  validateAutomationFxGraph(*document.scene);
  context.removeDocument(document.id);
}
}  // namespace
namespace GroundzeroAutomation {
void runAutomationColorCompositeTests(Context& context) {
  require(executeFx(context, "fx.catalog", {})
                  .toObject()
                  .value("effects")
                  .toArray()
                  .size() == 31,
          "Color/composite catalog must retain the earlier factories");
  QTemporaryDir directory(context.workspace() +
                          "/color-composite-tests-XXXXXX");
  require(directory.isValid(),
          "Cannot create color/composite fixture directory");
  for (const auto& type : types) candidate(context, type, directory.path());
  invalidBounds(context, directory.path());
  retainedCurvePolicy(context);
  scalarAdmissionEdges(context);
}
}  // namespace GroundzeroAutomation
