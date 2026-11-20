#include "automation.h"
#include "automation_dispatch.h"
#include "automation_fx.h"
#include "automation_macro.h"
#include <QTemporaryDir>
#include <functional>
#include "tdoubleparam.h"
#include "tenv.h"
#include "tfx.h"
#include "tfxattributes.h"
#include "tmacrofx.h"
#include "toonz/fxdag.h"
#include "toonz/scenefx.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshsimplelevel.h"
#include "tparamcontainer.h"
#include "traster.h"
#include "trenderer.h"
#include "tstream.h"
#include "tthread.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QSettings>
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

DV_IMPORT_API void initStdFx();
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
QString nativeId(TFx* fx) {
  return fx ? QString::fromStdWString(fx->getFxId()) : QString();
}
QJsonObject ports(TFx* fx) {
  QJsonObject result;
  for (int index = 0; index < fx->getInputPortCount(); ++index)
    result[QString::fromStdString(fx->getInputPortName(index))] =
        nativeId(fx->getInputPort(index)->getFx());
  return result;
}
QJsonObject wiring(FxDag* dag) {
  QJsonObject result;
  for (int index = 0; index < dag->getInternalFxs()->getFxCount(); ++index) {
    TFx* fx = dag->getInternalFxs()->getFx(index);
    result[nativeId(fx)] =
        QJsonObject{{"ports", ports(fx)},
                    {"terminal", dag->getTerminalFxs()->containsFx(fx)}};
  }
  result["current_output"] = ports(dag->getCurrentOutputFx());
  return result;
}
struct Fixture {
  TFx* blur;
  TFx* glow;
  TFx* root;
  TFx* downstream;
};
Fixture setup(Context& context, Document& document) {
  auto* camera =
      document.scene->getXsheet()->getStageObjectTree()->getCurrentCamera();
  camera->setRes(TDimension(64, 48));
  camera->setSize(TDimensionD(1.2, 0.9));
  TFx* background = create(context, document, "colorCardFx");
  parameter(context, document, background, "color",
            QJsonArray{24, 40, 80, 255});
  TFx* light = create(context, document, "STD_radialGradientFx");
  parameter(context, document, light, "period", 8);
  parameter(context, document, light, "innerperiod", 2);
  parameter(context, document, light, "color1", QJsonArray{160, 100, 40, 255});
  Fixture result{create(context, document, "STD_blurFx"),
                 create(context, document, "STD_glowFx"),
                 create(context, document, "overFx"),
                 create(context, document, "STD_blurFx")};
  parameter(context, document, result.blur, "value", 1);
  for (int frame : {0, 12})
    call(context, document, "fx.parameter.set_key",
         {{"fx_id", document.idForFx(result.blur)},
          {"parameter", "value"},
          {"frame", frame},
          {"value", frame / 1.5}});
  auto* curve =
      dynamic_cast<TDoubleParam*>(result.blur->getParams()->getParam("value"));
  require(curve && curve->getKeyframeCount() == 2 && curve->getValue(0) == 0 &&
              curve->getValue(6) == 4 && curve->getValue(12) == 8,
          "Fixture keys do not produce the intended native interpolation");
  parameter(context, document, result.glow, "value", 1);
  parameter(context, document, result.glow, "brightness", 50);
  parameter(context, document, result.downstream, "value", 0);
  result.blur->getInputPort("Source")->setFx(light);
  result.glow->getInputPort("Light")->setFx(result.blur);
  result.root->getInputPort("Source1")->setFx(result.glow);
  result.root->getInputPort("Source2")->setFx(background);
  result.downstream->getInputPort("Source")->setFx(result.root);
  FxDag* dag = document.scene->getXsheet()->getFxDag();
  dag->getCurrentOutputFx()->getInputPort(0)->setFx(result.root);
  dag->addToXsheet(result.root);
  return result;
}

QJsonObject group(Context& context, Document& document,
                  const Fixture& fixture) {
  return call(context, document, "fx.macro.create",
              {{"fx_ids", QJsonArray{document.idForFx(fixture.blur),
                                     document.idForFx(fixture.glow),
                                     document.idForFx(fixture.root)}},
               {"name", "Animated macro"}});
}
void samePixels(ToonzScene& scene, const std::map<int, QImage>& expected) {
  for (const auto& [frame, image] : expected)
    require(render(scene, frame) == image,
            "Macro changed native rendered pixels");
}
std::map<int, QImage> frames(ToonzScene& scene) {
  std::map<int, QImage> result;
  for (int frame : {0, 6, 12}) result.emplace(frame, render(scene, frame));
  require(result.at(0) != result.at(6) && result.at(6) != result.at(12),
          "Native animation fixture has no visible temporal change");
  require(result.at(0).pixel(0, 0) == qRgba(24, 40, 80, 255),
          "Background oracle failed");
  return result;
}
TMacroFx* macroFrom(Document& document, const QJsonObject& result) {
  auto* macro = dynamic_cast<TMacroFx*>(
      document.fx(result.value("node").toObject().value("fx_id").toString()));
  require(macro, "Create did not return a native macro handle");
  return macro;
}
void stale(Document& document, const QString& id) {
  try {
    document.fx(id);
  } catch (const Error& error) {
    require(error.code == "invalid_handle",
            "Retired handle failed unexpectedly");
    return;
  }
  throw std::runtime_error("Retired handle still resolves");
}
void indexes(ToonzScene& scene, TMacroFx* macro) {
  auto* graph = scene.getXsheet()->getFxDag();
  for (const auto& member : macro->getFxs())
    require(graph->getFxById(
                nativeId(member.getPointer()).toLower().toStdWString()) ==
                member.getPointer(),
            "Macro member native ID does not resolve its exact owner");
}
void lifecycle(Context& context) {
  auto& document        = context.addDocument(std::make_unique<ToonzScene>());
  const Fixture fixture = setup(context, document);
  const auto expected   = frames(*document.scene);
  std::vector<TFxP> originals{fixture.blur, fixture.glow, fixture.root};
  const auto grouped = group(context, document, fixture);
  auto* macro        = macroFrom(document, grouped);
  for (const auto& id : grouped.value("retired_fx_ids").toArray())
    stale(document, id.toString());
  samePixels(*document.scene, expected);
  indexes(*document.scene, macro);
  require(macro->getFxs().size() == 3, "Macro member count changed");
  for (const auto& original : originals) {
    auto* copy = macro->getFxById(original->getFxId());
    require(copy && copy != original.getPointer(),
            "Macro reused an original member object");
    for (int index = 0; index < original->getParams()->getParamCount(); ++index)
      require(copy->getParams()->getParam(index) !=
                  original->getParams()->getParam(index),
              "Macro reused an original parameter object");
  }
  const QString id = document.idForFx(macro);
  const auto expanded =
      call(context, document, "fx.macro.expand", {{"fx_id", id}});
  stale(document, id);
  require(expanded.value("member_mapping").toArray().size() == 3,
          "Expansion lost member handles");
  samePixels(*document.scene, expected);
}
void roundtrip(Context& context) {
  QTemporaryDir directory(context.workspace() + "/macro-native-XXXXXX");
  require(directory.isValid(), "Cannot create native macro fixture directory");
  auto& document        = context.addDocument(std::make_unique<ToonzScene>());
  const Fixture fixture = setup(context, document);
  const auto expected   = frames(*document.scene);
  auto* macro = macroFrom(document, group(context, document, fixture));
  auto* graph = document.scene->getXsheet()->getFxDag();
  graph->removeFromXsheet(macro);
  const auto fingerprint =
      automationMacroFingerprint(*document.scene, directory.path());
  const TFilePath path((directory.path() + "/macro.tnz").toStdWString());
  document.scene->saveCopy(path);
  auto loaded = std::make_unique<ToonzScene>();
  loaded->setProject(document.scene->getProject());
  loaded->loadTnzFile(path);
  validateAutomationFxGraph(*loaded);
  indexAutomationMacroMembers(*loaded);
  auto& reopened     = context.addDocument(std::move(loaded));
  TMacroFx* restored = nullptr;
  auto* effects = reopened.scene->getXsheet()->getFxDag()->getInternalFxs();
  for (int index = 0; index < effects->getFxCount(); ++index)
    if (auto* candidate = dynamic_cast<TMacroFx*>(effects->getFx(index)))
      restored = candidate;
  require(restored, "Native scene lost its macro");
  indexes(*reopened.scene, restored);
  samePixels(*reopened.scene, expected);
  call(context, reopened, "fx.macro.expand",
       {{"fx_id", reopened.idForFx(restored)}});
  samePixels(*reopened.scene, expected);
  const TFilePath expanded((directory.path() + "/expanded.tnz").toStdWString());
  reopened.scene->saveCopy(expanded);
  ToonzScene finalScene;
  finalScene.setProject(reopened.scene->getProject());
  finalScene.loadTnzFile(expanded);
  validateAutomationFxGraph(finalScene);
  samePixels(finalScene, expected);
}
void expectRejected(Context& context, Document& document,
                    const QJsonObject& args) {
  const auto before  = wiring(document.scene->getXsheet()->getFxDag());
  const auto handles = document.effects;
  context.resetMutationStarted();
  bool rejected = false;
  try {
    call(context, document, "fx.macro.create", args);
  } catch (const Error&) {
    rejected = true;
  }
  require(rejected && !context.mutationStarted(),
          "Invalid grouping started a native mutation");
  require(wiring(document.scene->getXsheet()->getFxDag()) == before &&
              document.effects == handles,
          "Rejected grouping changed graph state or handles");
}
void rejectionCases(Context& context) {
  auto& document        = context.addDocument(std::make_unique<ToonzScene>());
  const Fixture fixture = setup(context, document);
  const QString blur    = document.idForFx(fixture.blur);
  const QString glow    = document.idForFx(fixture.glow);
  const QString root    = document.idForFx(fixture.root);
  for (const QJsonArray& ids :
       {QJsonArray{}, QJsonArray{blur}, QJsonArray{blur, blur},
        QJsonArray{blur, root},
        QJsonArray{blur, document.idForFx(fixture.downstream)}})
    expectRejected(context, document, {{"fx_ids", ids}, {"name", "Rejected"}});
  const auto name = fixture.blur->getFxId();
  fixture.blur->setFxId(name + L"_invalid");
  expectRejected(
      context, document,
      {{"fx_ids", QJsonArray{blur, glow, root}}, {"name", "Bad ID"}});
  fixture.blur->setFxId(name);
  fixture.downstream->linkParams(fixture.blur);
  expectRejected(
      context, document,
      {{"fx_ids", QJsonArray{blur, glow, root}}, {"name", "Linked"}});
  fixture.downstream->unlinkParams();
  document.scene->getXsheet()->getFxDag()->addToXsheet(fixture.blur);
  expectRejected(
      context, document,
      {{"fx_ids", QJsonArray{blur, glow, root}}, {"name", "Side exit"}});
}
void createFault(Context& context, int point) {
  auto& document        = context.addDocument(std::make_unique<ToonzScene>());
  const Fixture fixture = setup(context, document);
  for (auto* fx : {fixture.blur, fixture.glow, fixture.root})
    document.idForFx(fx);
  const auto before   = wiring(document.scene->getXsheet()->getFxDag());
  const auto handles  = document.effects;
  const auto expected = frames(*document.scene);
  context.resetMutationStarted();
  setAutomationMacroFailurePoint(point);
  bool failed = false;
  try {
    group(context, document, fixture);
  } catch (const Error& error) {
    failed = error.code == "NATIVE_FAILURE";
  }
  setAutomationMacroFailurePoint(0);
  require(failed && context.mutationStarted(),
          "Create fault did not retain mutation-started outcome");
  validateAutomationFxGraph(*document.scene);
  require(wiring(document.scene->getXsheet()->getFxDag()) == before &&
              document.effects == handles,
          "Create fault did not restore exact graph/handles");
  samePixels(*document.scene, expected);
  auto* graph = document.scene->getXsheet()->getFxDag();
  for (auto* fx : {fixture.blur, fixture.glow, fixture.root})
    require(graph->getFxById(nativeId(fx).toLower().toStdWString()) == fx,
            "Create fault left a copied member in the native ID table");
  auto* retried = macroFrom(document, group(context, document, fixture));
  require(retried->getAttributes()->getId() == 1,
          "Create fault consumed a native macro ID despite graph restoration");
}
void expandFault(Context& context, int point) {
  auto& document        = context.addDocument(std::make_unique<ToonzScene>());
  const Fixture fixture = setup(context, document);
  auto* macro         = macroFrom(document, group(context, document, fixture));
  const auto before   = wiring(document.scene->getXsheet()->getFxDag());
  const auto handles  = document.effects;
  const auto expected = frames(*document.scene);
  context.resetMutationStarted();
  setAutomationMacroFailurePoint(point);
  bool failed = false;
  try {
    call(context, document, "fx.macro.expand",
         {{"fx_id", document.idForFx(macro)}});
  } catch (const Error& error) {
    failed = error.code == "NATIVE_FAILURE";
  }
  setAutomationMacroFailurePoint(0);
  require(failed && context.mutationStarted(),
          "Expand fault did not retain mutation-started outcome");
  validateAutomationFxGraph(*document.scene);
  require(wiring(document.scene->getXsheet()->getFxDag()) == before &&
              document.effects == handles,
          "Expand fault did not restore exact graph/handles");
  indexes(*document.scene, macro);
  samePixels(*document.scene, expected);
}

void maximumRoundtrip(Context& context, Document& document) {
  QTemporaryDir directory(context.workspace() + "/macro-maximum-XXXXXX");
  require(directory.isValid(),
          "Cannot create maximum macro persistence fixture");
  const TFilePath path((directory.path() + "/scene.tnz").toStdWString());
  document.scene->saveCopy(path);
  auto loaded = std::make_unique<ToonzScene>();
  loaded->setProject(document.scene->getProject());
  loaded->loadTnzFile(path);
  validateAutomationFxGraph(*loaded);
  indexAutomationMacroMembers(*loaded);
  auto& reopened = context.addDocument(std::move(loaded));
  TFx* next      = create(context, reopened, "overFx");
  require(next->getAttributes()->getId() == 65,
          "Nonterminal macro reopen lost child native type counters");
  auto* graph = reopened.scene->getXsheet()->getFxDag();
  require(graph->getFxById(nativeId(next).toLower().toStdWString()) == next,
          "New native effect collided with restored macro child indexing");
}
void maximumAndDisabled(Context& context) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  std::vector<TFxP> members;
  QJsonArray ids;
  for (int index = 0; index < 64; ++index) {
    TFx* fx = create(context, document, "overFx");
    if (!members.empty())
      fx->getInputPort("Source1")->setFx(members.back().getPointer());
    members.emplace_back(fx);
    ids.append(document.idForFx(fx));
  }
  const auto result = call(context, document, "fx.macro.create",
                           {{"fx_ids", ids}, {"name", "Maximum"}});
  auto* macro       = macroFrom(document, result);
  require(macro->getFxs().size() == 64 && macro->getInputPortCount() == 65,
          "Maximum native macro fixture boundary changed");
  maximumRoundtrip(context, document);
  call(context, document, "fx.macro.expand",
       {{"fx_id", document.idForFx(macro)}});
  TFx* next = create(context, document, "overFx");
  require(next->getAttributes()->getId() == 65,
          "Expanded macro lost native type indexing");
  QJsonArray tooMany;
  const auto graph = call(context, document, "fx.graph", {{"limit", 512}});
  for (const auto& value : graph.value("effects").toArray())
    if (value.toObject().value("type") == "overFx")
      tooMany.append(value.toObject().value("fx_id"));
  expectRejected(context, document,
                 {{"fx_ids", tooMany}, {"name", "Too many"}});
  auto& disabled        = context.addDocument(std::make_unique<ToonzScene>());
  const Fixture fixture = setup(context, disabled);
  fixture.blur->getAttributes()->enable(false);
  const auto blurId   = fixture.blur->getFxId();
  const QImage before = render(*disabled.scene, 6);
  auto* grouped       = macroFrom(disabled, group(context, disabled, fixture));
  require(!grouped->getFxById(blurId)->getAttributes()->isEnabled(),
          "Grouping enabled a disabled native member");
  require(render(*disabled.scene, 6) == before,
          "Grouping changed disabled-member pixels");
}
void invalidGraph(ToonzScene& scene) {
  try {
    validateAutomationFxGraph(scene);
  } catch (const Error& error) {
    require(error.code == "INVALID_MACRO",
            "Malformed macro failed for an unexpected reason");
    return;
  }
  throw std::runtime_error("Malformed macro was accepted for native rendering");
}
void malformedGraphs(Context& context) {
  auto& document        = context.addDocument(std::make_unique<ToonzScene>());
  const Fixture fixture = setup(context, document);
  auto* macro        = macroFrom(document, group(context, document, fixture));
  const auto members = macro->getFxs();
  TFx* first         = members[0].getPointer();
  TFx* second        = members[1].getPointer();
  const auto originalId = first->getFxId();
  first->setFxId(originalId + L"_bad");
  invalidGraph(*document.scene);
  first->setFxId(originalId);
  const auto secondId = second->getFxId();
  second->setFxId(QString::fromStdWString(originalId).toUpper().toStdWString());
  invalidGraph(*document.scene);
  second->setFxId(secondId);
  auto* port = macro->getInputPort(0);
  port->setOwnerFx(first);
  invalidGraph(*document.scene);
  port->setOwnerFx(macro);
  TFxP root(macro->getRoot());
  TFxP foreign = makeAutomationEffect("STD_blurFx");
  macro->setRoot(foreign.getPointer());
  invalidGraph(*document.scene);
  macro->setRoot(root.getPointer());
  validateAutomationFxGraph(*document.scene);
  expectRejected(context, document,
                 {{"fx_ids", QJsonArray{document.idForFx(macro),
                                        document.idForFx(fixture.downstream)}},
                  {"name", "Nested macro"}});
}
void admissionBudget(Context& context) {
  auto& document = context.addDocument(std::make_unique<ToonzScene>());
  TFx* first     = create(context, document, "overFx");
  TFx* root      = create(context, document, "overFx");
  root->getInputPort("Source1")->setFx(first);
  call(context, document, "fx.macro.create",
       {{"fx_ids", QJsonArray{document.idForFx(first), document.idForFx(root)}},
        {"name", "Budget"}});
  auto* graph = document.scene->getXsheet()->getFxDag();
  for (int total = 5; total < 4095; ++total) {
    TFxP fx = makeAutomationEffect("overFx");
    graph->assignUniqueId(fx.getPointer());
    graph->getInternalFxs()->addFx(fx.getPointer());
  }
  TFx* last = create(context, document, "overFx");
  validateAutomationFxGraph(*document.scene);
  for (const QString& method :
       {QString("fx.create"), QString("fx.duplicate")}) {
    context.resetMutationStarted();
    bool rejected = false;
    try {
      call(context, document, method,
           {{"type", "overFx"}, {"fx_id", document.idForFx(last)}});
    } catch (const Error& error) {
      rejected = error.code == "RESOURCE_LIMIT";
    }
    require(
        rejected && !context.mutationStarted(),
        "Effect admission ignored macro descendants in the 4096-node budget");
  }
}
void fingerprintConnections(Context& context) {
  auto& document        = context.addDocument(std::make_unique<ToonzScene>());
  const Fixture fixture = setup(context, document);
  auto* macro = macroFrom(document, group(context, document, fixture));
  auto* sheet = document.scene->getXsheet();
  auto* graph = sheet->getFxDag();
  graph->removeFromXsheet(macro);
  const int firstIndex = sheet->getColumnCount();
  sheet->insertColumn(firstIndex, new TXshLevelColumn());
  sheet->insertColumn(firstIndex + 1, new TXshLevelColumn());
  auto* level =
      document.scene->createNewLevel(PLI_XSHLEVEL, L"MacroColumnIdentity");
  sheet->setCell(0, firstIndex, TXshCell(level, TFrameId(1)));
  sheet->setCell(0, firstIndex + 1, TXshCell(level, TFrameId(1)));

  TFx* first  = sheet->getColumn(firstIndex)->getFx();
  TFx* second = sheet->getColumn(firstIndex + 1)->getFx();
  require(
      first != second && first->getFxId().empty() && second->getFxId().empty(),
      "Normal drawing-column identity regression fixture changed");
  TFxPort* port = macro->getInputPort(0);
  port->setFx(first);
  const QByteArray firstHash =
      automationMacroFingerprint(*document.scene, context.workspace());
  port->setFx(second);
  const QByteArray secondHash =
      automationMacroFingerprint(*document.scene, context.workspace());
  require(firstHash != secondHash,
          "Macro fingerprint conflated two normal drawing columns");
  port->setFx(nullptr);
  const QByteArray nullHash =
      automationMacroFingerprint(*document.scene, context.workspace());
  port->setFx(graph->getXsheetFx());
  const QByteArray xsheetHash =
      automationMacroFingerprint(*document.scene, context.workspace());
  require(nullHash != xsheetHash,
          "Macro fingerprint conflated null and Xsheet inputs");
  require(xsheetHash != firstHash && xsheetHash != secondHash,
          "Macro fingerprint conflated structural and column identities");
  QTemporaryDir directory(context.workspace() + "/macro-columns-XXXXXX");
  require(directory.isValid(),
          "Cannot create drawing-column roundtrip fixture");
  port->setFx(second);
  const TFilePath path((directory.path() + "/columns.tnz").toStdWString());
  document.scene->saveCopy(path);
  ToonzScene loaded;
  loaded.setProject(document.scene->getProject());
  loaded.loadTnzFile(path);
  validateAutomationFxGraph(loaded);
  indexAutomationMacroMembers(loaded);
  require(automationMacroFingerprint(loaded, context.workspace()) == secondHash,
          "Drawing-column macro connection identity changed across native "
          "persistence");
}
void dispatcherFault(Context& context, int point) {
  auto& document        = context.addDocument(std::make_unique<ToonzScene>());
  const Fixture fixture = setup(context, document);
  const bool expanding  = point == 4 || point == 5 || point == 6 || point == 9;
  QJsonObject params;
  if (expanding) {
    auto* macro = macroFrom(document, group(context, document, fixture));
    params.insert("fx_id", document.idForFx(macro));
  } else {
    params.insert("fx_ids", QJsonArray{document.idForFx(fixture.blur),
                                       document.idForFx(fixture.glow),
                                       document.idForFx(fixture.root)});
    params.insert("name", "Receipt failure");
  }
  params.insert("document_id", document.id);
  params.insert("expected_revision", document.revisionText());
  params.insert("operation_id", "macro-failure-" + QString::number(point));
  const QString method = expanding ? "fx.macro.expand" : "fx.macro.create";
  const QJsonObject request{
      {"id", "macro-fault"}, {"method", method}, {"params", params}};
  Dispatcher dispatcher(context);
  const quint64 before = document.revision;
  setAutomationMacroFailurePoint(point);
  const QJsonObject response = dispatcher.dispatch(request);
  setAutomationMacroFailurePoint(0);
  const auto error = response.value("error").toObject();
  require(error.value("code") == "partially_applied",
          "Macro fault hid partial-mutation semantics");
  require(
      error.value("details").toObject().value("cause_code") == "NATIVE_FAILURE",
      "Macro fault lost its native cause");
  require(document.revision == before + 1,
          "Macro partial failure did not advance revision once");
  require(error.value("details").toObject().value("revision") ==
              document.revisionText(),
          "Macro partial failure reported a different revision");
  const auto handles = document.effects;
  require(dispatcher.dispatch(request) == response,
          "Macro failure replay changed its recorded receipt");
  require(document.revision == before + 1 && document.effects == handles,
          "Macro failure retry reapplied its mutation");
  validateAutomationFxGraph(*document.scene);
}
}  // namespace
namespace GroundzeroAutomation {
void runAutomationMacroTests(Context& context) {
  lifecycle(context);
  roundtrip(context);
  rejectionCases(context);
  maximumAndDisabled(context);
  malformedGraphs(context);
  admissionBudget(context);
  fingerprintConnections(context);
  for (int point : {1, 2, 3, 7, 8}) createFault(context, point);
  for (int point : {4, 5, 6, 9}) expandFault(context, point);
  for (int point = 1; point <= 9; ++point) dispatcherFault(context, point);
}
}  // namespace GroundzeroAutomation
