#include "automation.h"
#include "automation_dispatch.h"
#include "automation_drawing.h"
#include "automation_fx.h"
#include "automation_fx_journal.h"
#include "automation_live_protocol.h"

#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tfx.h"
#include "timage.h"
#include "tnotanimatableparam.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "traster.h"
#include "tpalette.h"
#include "tstroke.h"
#include "tvectorimage.h"
#include "tzeraryfx.h"
#include "toonz/fxdag.h"
#include "toonz/levelset.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshzeraryfxcolumn.h"

#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace GroundzeroAutomation {
namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void testKeyframeInitialization() {
  alignas(TDoubleKeyframe) std::array<unsigned char, sizeof(TDoubleKeyframe)>
      storage;
  storage.fill(0xa5);
  const auto destroy = [](TDoubleKeyframe* key) { key->~TDoubleKeyframe(); };
  std::unique_ptr<TDoubleKeyframe, decltype(destroy)> key(
      new (storage.data()) TDoubleKeyframe(3, 7), destroy);
  std::underlying_type_t<TDoubleKeyframe::Type> previous;
  // Read bytes so the regression never evaluates the former uninitialized enum.
  std::memcpy(&previous, &key->m_prevType, sizeof(previous));
  require(previous == TDoubleKeyframe::None,
          "A newly constructed key must have no predecessor interpolation");
  TDoubleParamP curve(new TDoubleParam());
  curve->setKeyframe(*key);
  require(curve->getKeyframe(0).m_prevType == TDoubleKeyframe::None,
          "Native insertion must preserve the first key's absent predecessor");
}

void testNativeKeyframePredecessors() {
  TDoubleParamP curve(new TDoubleParam());
  TDoubleKeyframe first(3, 7), last(8, 10), middle(5, 9);
  first.m_type = TDoubleKeyframe::Constant;
  curve->setKeyframe(first);
  curve->setKeyframe(last);
  require(curve->getKeyframe(1).m_prevType == TDoubleKeyframe::Constant,
          "Native insertion must derive predecessor interpolation from the "
          "adjacent key");
  curve->setKeyframe(middle);
  require(curve->getKeyframe(1).m_prevType == TDoubleKeyframe::Constant &&
              curve->getKeyframe(2).m_prevType == TDoubleKeyframe::Linear,
          "Insertion between keys must update both predecessor relationships");
  first.m_type = TDoubleKeyframe::Linear;
  curve->setKeyframe(0, first);
  require(
      curve->getKeyframe(0).m_prevType == TDoubleKeyframe::None &&
          curve->getKeyframe(1).m_prevType == TDoubleKeyframe::Linear,
      "Indexed key replacement must retain correct predecessor relationships");
  require(
      curve->getValue(4) == 8,
      "Initialized predecessor metadata must preserve native interpolation");
}

QJsonObject call(Context& context, Document& document, const QString& method,
                 QJsonObject params = {}) {
  params["document_id"] = document.id;
  return executeFx(context, method, params).toObject();
}

QString create(Context& context, Document& document, const QString& type) {
  const QJsonObject result =
      call(context, document, "fx.create", {{"type", type}});
  const QString id = result.value("fx_id").toString();
  require(!id.isEmpty(), "FX creation must return an opaque handle");
  return id;
}

void expectError(Context& context, const QString& code,
                 const std::function<void()>& action) {
  context.resetMutationStarted();
  try {
    action();
  } catch (const Error& error) {
    require(error.code == code,
            "FX rejection must return the expected error code");
    require(!context.mutationStarted(),
            "Invalid FX requests must fail before native mutation");
    return;
  }
  throw std::runtime_error("Invalid FX request unexpectedly succeeded");
}

void expectNoMutation(Context& context, const std::function<void()>& action) {
  context.resetMutationStarted();
  action();
  require(!context.mutationStarted(),
          "Unchanged FX operations must preserve the document revision");
}

void testCatalog(Context& context, Document& document) {
  const QJsonObject catalog = executeFx(context, "fx.catalog", {}).toObject();
  const QJsonArray effects  = catalog.value("effects").toArray();
  require(effects.size() == 31,
          "Builtin profile must expose only its 31 qualified effects");
  require(catalog.value("external_dependencies").toArray().isEmpty(),
          "Builtin profile must not declare external dependencies");
  for (const QJsonValue& entry : effects) {
    const QJsonObject descriptor = entry.toObject();
    require(descriptor.value("available").toBool(),
            "Every qualified builtin must be registered");
    const QString type = descriptor.value("type").toString();
    const QString id   = create(context, document, type);
    TFx* fx            = document.fx(id);
    if (auto* generator = dynamic_cast<TZeraryColumnFx*>(fx))
      fx = generator->getZeraryFx();
    require(
        QString::fromStdString(fx->getFxType()) == type,
        "Qualified factories must return the advertised native effect type");
    for (const QJsonValue& parameter : descriptor.value("parameters").toArray())
      require(parameter.toObject().value("supported").toBool(),
              "Qualified builtin constructors must expose only supported "
              "public parameter types");
  }
}

void testCatalogWithFailingFactory() {
  const QString missing = "STD_blurFx";
  const QJsonObject healthy =
      automationFxCatalog(makeAutomationEffect).toObject();
  const EffectFactory failing = [&missing](const QString& type) {
    if (type == missing)
      throw Error("UNAVAILABLE_FX", "Builtin effect is not registered.",
                  {{"type", type}});
    return makeAutomationEffect(type);
  };
  const QJsonObject degraded = automationFxCatalog(failing).toObject();
  const QJsonArray expected  = healthy.value("effects").toArray();
  const QJsonArray actual    = degraded.value("effects").toArray();
  require(actual.size() == expected.size() && actual.size() == 31,
          "A failing factory changed the catalog size");
  int absent = 0;
  for (int index = 0; index < actual.size(); ++index) {
    const QJsonObject entry = actual[index].toObject();
    if (entry.value("type") == missing) {
      ++absent;
      require(entry.value("available") == false &&
                  entry.value("reason").isString() &&
                  !entry.contains("parameters"),
              "An unavailable type was not reported as available:false");
    } else {
      require(entry == expected[index].toObject(),
              "A failing factory changed another type's entry");
    }
  }
  require(absent == 1, "The failing type was not reported exactly once");
  const EffectFactory broken = [](const QString&) -> TFxP {
    throw Error("NATIVE_FAILURE", "Unrelated failure.");
  };
  bool propagated = false;
  try {
    automationFxCatalog(broken);
  } catch (const Error& error) {
    propagated = error.code == "NATIVE_FAILURE";
  }
  require(propagated, "An unrelated factory failure was reported as absence");
}

QStringList parameterNames(const QJsonArray& parameters) {
  QStringList names;
  for (const QJsonValue& parameter : parameters)
    names.append(parameter.toObject().value("name").toString());
  return names;
}

void testHiddenParametersAreNotPublic(Context& context, Document& document) {
  const QJsonObject catalog = executeFx(context, "fx.catalog", {}).toObject();
  int hidden = 0, obsolete = 0;
  for (const QJsonValue& entry : catalog.value("effects").toArray()) {
    const QString type = entry.toObject().value("type").toString();
    const QStringList advertised =
        parameterNames(entry.toObject().value("parameters").toArray());
    const QString id = create(context, document, type);
    TFx* fx          = document.fx(id);
    if (auto* generator = dynamic_cast<TZeraryColumnFx*>(fx))
      fx = generator->getZeraryFx();
    const QStringList described = parameterNames(
        call(context, document, "fx.parameter.get", {{"fx_id", id}})
            .value("parameters")
            .toArray());
    TParamContainer* params = fx->getParams();
    for (int index = 0; index < params->getParamCount(); ++index) {
      const TParamVar* var = params->getParamVar(index);
      const QString name   = QString::fromStdString(var->getName());
      if (!var->isHidden() && !var->isObsolete()) continue;
      ++(var->isObsolete() ? obsolete : hidden);
      require(!advertised.contains(name) && !described.contains(name),
              "A hidden or obsolete native parameter was described as public");
      expectError(context, "UNKNOWN_PARAMETER", [&] {
        call(context, document, "fx.parameter.set",
             {{"fx_id", id}, {"parameter", name}, {"value", 0}});
      });
    }
  }
  require(hidden > 0, "No admitted effect carries a hidden native parameter");
  require(obsolete > 0,
          "No admitted effect carries an obsolete native parameter");
}

void testGenerator(Context& context, Document& document) {
  const QJsonObject created =
      call(context, document, "fx.create",
           {{"type", "colorCardFx"}, {"frame_count", 5}});
  const QString id = created.value("fx_id").toString();
  auto* wrapper    = dynamic_cast<TZeraryColumnFx*>(document.fx(id));
  require(wrapper != nullptr, "Generator must have a native column wrapper");
  require(wrapper->getZeraryFx() != nullptr,
          "Generator wrapper must own its native effect");
  require(wrapper->getColumn()->getRowCount() == 5,
          "Generator span must reach native cells");
  FxDag* graph = document.scene->getXsheet()->getFxDag();
  require(!graph->getTerminalFxs()->containsFx(wrapper),
          "New generator must start disconnected");
  require(created.value("column_id").isString(),
          "Generator must expose its column handle");
  const QJsonArray color{16, 80, 192, 128};
  call(context, document, "fx.parameter.set",
       {{"fx_id", id}, {"parameter", "color"}, {"value", color}});
  auto* pixel = dynamic_cast<TPixelParam*>(
      wrapper->getZeraryFx()->getParams()->getParam("color"));
  require(pixel && pixel->getValue(0) == TPixel32(16, 80, 192, 128),
          "RGBA updates must use native normalized color channels");
  call(context, document, "fx.parameter.set_key",
       {{"fx_id", id}, {"parameter", "color"}, {"frame", 0}, {"value", color}});
  const QJsonArray nextColor{200, 40, 20, 255};
  call(context, document, "fx.parameter.set_key",
       {{"fx_id", id},
        {"parameter", "color"},
        {"frame", 4},
        {"value", nextColor}});
  require(
      pixel->isKeyframe(4) && pixel->getValue(4) == TPixel32(200, 40, 20, 255),
      "Color keys must be native keyframes with the requested value");
  expectError(context, "ANIMATED_PARAMETER", [&] {
    call(context, document, "fx.parameter.set",
         {{"fx_id", id}, {"parameter", "color"}, {"value", color}});
  });
}

void testConnections(Context& context, Document& document) {
  const QString card = create(context, document, "colorCardFx");
  const QString blur = create(context, document, "STD_blurFx");
  const QString glow = create(context, document, "STD_glowFx");
  call(context, document, "fx.connect",
       {{"fx_id", blur}, {"port", "Source"}, {"source_fx_id", card}});
  const QJsonObject connected =
      call(context, document, "fx.connect",
           {{"fx_id", glow}, {"port", "Source"}, {"source_fx_id", blur}});
  require(
      document.fx(glow)->getInputPort("Source")->getFx() == document.fx(blur),
      "Connection must change the native input port");
  require(
      connected.value("diagnostics").toArray().size() == 1,
      "Legal Glow without Light must report its ineffective glow contribution");
  expectError(context, "UNKNOWN_PORT", [&] {
    call(context, document, "fx.connect",
         {{"fx_id", glow}, {"port", "source"}, {"source_fx_id", card}});
  });
  expectError(context, "GRAPH_CYCLE", [&] {
    call(context, document, "fx.connect",
         {{"fx_id", blur}, {"port", "Source"}, {"source_fx_id", glow}});
  });
  require(
      document.fx(blur)->getInputPort("Source")->getFx() == document.fx(card),
      "Rejected cycle must preserve the previous connection");
  call(context, document, "fx.connect",
       {{"fx_id", glow}, {"port", "Light"}, {"source_fx_id", card}});
  call(context, document, "fx.disconnect",
       {{"fx_id", glow}, {"port", "Light"}});
  require(!document.fx(glow)->getInputPort("Light")->isConnected(),
          "Disconnect must clear the native port");
}

void testImplicitCycles(Context& context, Document& document) {
  const QString blur   = create(context, document, "STD_blurFx");
  FxDag* graph         = document.scene->getXsheet()->getFxDag();
  const QString xsheet = document.idForFx(graph->getXsheetFx());
  call(context, document, "fx.connect",
       {{"fx_id", blur}, {"port", "Source"}, {"source_fx_id", xsheet}});
  expectError(context, "GRAPH_CYCLE", [&] {
    call(context, document, "fx.terminal.set",
         {{"fx_id", blur}, {"enabled", true}});
  });
  call(context, document, "fx.disconnect",
       {{"fx_id", blur}, {"port", "Source"}});
  call(context, document, "fx.terminal.set",
       {{"fx_id", blur}, {"enabled", true}});
  expectError(context, "GRAPH_CYCLE", [&] {
    call(context, document, "fx.connect",
         {{"fx_id", blur}, {"port", "Source"}, {"source_fx_id", xsheet}});
  });
  require(graph->getTerminalFxs()->containsFx(document.fx(blur)),
          "Rejected implicit cycle must retain the terminal membership");
}

void testParameters(Context& context, Document& document) {
  const QString gradient =
      create(context, document, "STD_iwa_LinearGradientFx");
  call(context, document, "fx.parameter.set",
       {{"fx_id", gradient},
        {"parameter", "startPoint"},
        {"value", QJsonArray{-40, 12}}});
  auto* wrapper = dynamic_cast<TZeraryColumnFx*>(document.fx(gradient));
  auto* point   = dynamic_cast<TPointParam*>(
      wrapper->getZeraryFx()->getParams()->getParam("startPoint"));
  require(point && point->getValue(0) == TPointD(-40, 12),
          "Point values must reach native channels");
  const QString blur = create(context, document, "STD_blurFx");
  expectError(context, "OUT_OF_RANGE", [&] {
    call(context, document, "fx.parameter.set",
         {{"fx_id", blur}, {"parameter", "value"}, {"value", -1}});
  });
  expectError(context, "INVALID_PARAMS", [&] {
    call(context, document, "fx.parameter.set",
         {{"fx_id", blur}, {"parameter", "value"}, {"value", "12"}});
  });
  call(context, document, "fx.parameter.set_key",
       {{"fx_id", blur}, {"parameter", "value"}, {"frame", 0}, {"value", 2}});
  call(context, document, "fx.parameter.set_key",
       {{"fx_id", blur}, {"parameter", "value"}, {"frame", 4}, {"value", 10}});
  auto* number = dynamic_cast<TDoubleParam*>(
      document.fx(blur)->getParams()->getParam("value"));
  require(number && number->getValue(2) == 6,
          "Native keys must interpolate linearly");
}

void testNoOpParameters(Context& context, Document& document) {
  const QString blur = create(context, document, "STD_blurFx");
  const QJsonObject change{
      {"fx_id", blur}, {"parameter", "value"}, {"value", 20}};
  expectNoMutation(
      context, [&] { call(context, document, "fx.parameter.set", change); });
  QJsonObject key = change;
  key["frame"]    = 0;
  context.resetMutationStarted();
  call(context, document, "fx.parameter.set_key", key);
  require(context.mutationStarted(),
          "Setting a first key must mutate even when its value is unchanged");
  expectNoMutation(
      context, [&] { call(context, document, "fx.parameter.set_key", key); });
  auto* number = dynamic_cast<TDoubleParam*>(
      document.fx(blur)->getParams()->getParam("value"));
  TDoubleKeyframe eased = number->getKeyframeAt(0);
  eased.m_type          = TDoubleKeyframe::SpeedInOut;
  number->setKeyframe(eased);
  context.resetMutationStarted();
  call(context, document, "fx.parameter.set_key", key);
  require(context.mutationStarted() &&
              number->getKeyframeAt(0).m_type == TDoubleKeyframe::Linear,
          "An equal value with different interpolation must still replace the "
          "native key");
  const QString gradient =
      create(context, document, "STD_iwa_LinearGradientFx");
  const QJsonObject point{{"fx_id", gradient},
                          {"parameter", "startPoint"},
                          {"value", QJsonArray{-40, 12}}};
  call(context, document, "fx.parameter.set", point);
  expectNoMutation(context,
                   [&] { call(context, document, "fx.parameter.set", point); });
  auto* wrapper     = dynamic_cast<TZeraryColumnFx*>(document.fx(gradient));
  auto* enumeration = dynamic_cast<TEnumParam*>(
      wrapper->getZeraryFx()->getParams()->getParam("curveType"));
  expectNoMutation(context, [&] {
    call(context, document, "fx.parameter.set",
         {{"fx_id", gradient},
          {"parameter", "curveType"},
          {"value", enumeration->getValue()}});
  });
  expectError(context, "OUT_OF_RANGE", [&] {
    call(context, document, "fx.parameter.set",
         {{"fx_id", gradient}, {"parameter", "curveType"}, {"value", 9999}});
  });
  expectError(context, "NOT_ANIMATABLE", [&] {
    call(context, document, "fx.parameter.set_key",
         {{"fx_id", gradient},
          {"parameter", "curveType"},
          {"value", enumeration->getValue()},
          {"frame", 0}});
  });
  const QString card = create(context, document, "colorCardFx");
  const QJsonObject color{{"fx_id", card},
                          {"parameter", "color"},
                          {"value", QJsonArray{16, 80, 192, 128}}};
  call(context, document, "fx.parameter.set", color);
  expectNoMutation(context,
                   [&] { call(context, document, "fx.parameter.set", color); });
}

void testOutputSelection(Context& context, Document& document) {
  FxDag* graph           = document.scene->getXsheet()->getFxDag();
  const QString original = document.idForFx(graph->getCurrentOutputFx());
  expectError(context, "LAST_OUTPUT", [&] {
    call(context, document, "fx.delete", {{"fx_id", original}});
  });
  const QString output = create(context, document, "output");
  call(context, document, "fx.output.select", {{"fx_id", output}});
  require(graph->getCurrentOutputFx() == document.fx(output),
          "Selected output must be the native render output");
  expectError(context, "INVALID_CONNECTION", [&] {
    call(context, document, "fx.connect",
         {{"fx_id", original}, {"port", "source"}, {"source_fx_id", output}});
  });
  call(context, document, "fx.delete", {{"fx_id", output}});
  require(graph->getCurrentOutputFx() == document.fx(original),
          "Deleting selected output must retain a valid current output");
}

void testDeletion(Context& context, Document& document) {
  const QString card = create(context, document, "colorCardFx");
  const QString blur = create(context, document, "STD_blurFx");
  call(context, document, "fx.connect",
       {{"fx_id", blur}, {"port", "Source"}, {"source_fx_id", card}});
  TFxP wrapper    = document.fx(card);
  auto* generator = dynamic_cast<TZeraryColumnFx*>(wrapper.getPointer());
  const std::wstring nativeId =
      QString::fromStdWString(generator->getZeraryFx()->getFxId())
          .toLower()
          .toStdWString();
  TXsheet* sheet  = document.scene->getXsheet();
  const int count = sheet->getColumnCount();
  call(context, document, "fx.delete", {{"fx_id", card}});
  require(sheet->getColumnCount() == count - 1,
          "Deleting generator must remove its column");
  require(!document.fx(blur)->getInputPort("Source")->isConnected(),
          "Deleting a source must disconnect downstream ports");
  require(sheet->getFxDag()->getFxById(nativeId) == nullptr,
          "Permanent deletion must remove the native expression lookup entry");
  expectError(context, "invalid_handle",
              [&] { call(context, document, "fx.delete", {{"fx_id", card}}); });
  const QString next = create(context, document, "colorCardFx");
  require(next != card, "Recreated effect must not revive a stale handle");
}

void testRepeatedReconnect(Context& context, Document& document) {
  const QString card   = create(context, document, "colorCardFx");
  const QString glow   = create(context, document, "STD_glowFx");
  FxDag* graph         = document.scene->getXsheet()->getFxDag();
  const QString output = document.idForFx(graph->getCurrentOutputFx());
  const QJsonObject outputLink{
      {"fx_id", output}, {"port", "source"}, {"source_fx_id", glow}};
  call(context, document, "fx.connect", outputLink);
  expectNoMutation(context,
                   [&] { call(context, document, "fx.connect", outputLink); });
  expectNoMutation(context, [&] {
    call(context, document, "fx.output.select", {{"fx_id", output}});
  });
  expectNoMutation(context, [&] {
    call(context, document, "fx.terminal.set",
         {{"fx_id", card}, {"enabled", false}});
  });
  for (int iteration = 0; iteration < 8; ++iteration) {
    const QString blur = create(context, document, "STD_blurFx");
    TFx* native        = document.fx(blur);
    const std::wstring nativeId =
        QString::fromStdWString(native->getFxId()).toLower().toStdWString();
    call(context, document, "fx.connect",
         {{"fx_id", blur}, {"port", "Source"}, {"source_fx_id", card}});
    call(context, document, "fx.connect",
         {{"fx_id", glow}, {"port", "Source"}, {"source_fx_id", blur}});
    call(context, document, "fx.terminal.set",
         {{"fx_id", blur}, {"enabled", true}});
    call(context, document, "fx.delete", {{"fx_id", blur}});
    require(!document.fx(glow)->getInputPort("Source")->isConnected(),
            "Repeated intermediate deletion must clear the downstream source");
    require(
        document.fx(card)->getOutputConnectionCount() == 0,
        "Repeated intermediate deletion must release the upstream connection");
    require(graph->getFxById(nativeId) == nullptr &&
                !document.effects.contains(blur),
            "Repeated deletion must expire native lookup and opaque handles");
    require(graph->getCurrentOutputFx()->getInputPort(0)->getFx() ==
                document.fx(glow),
            "Deleting intermediate nodes must retain the chosen output graph");
    validateAutomationFxGraph(*document.scene);
  }
}

void testRepeatedGeneratorDeletion(Context& context, Document& document) {
  const QString composite = create(context, document, "overFx");
  QString generator       = create(context, document, "colorCardFx");
  const QString neighbor  = create(context, document, "colorCardFx");
  auto* neighborFx = dynamic_cast<TZeraryColumnFx*>(document.fx(neighbor));
  TXsheet* sheet   = document.scene->getXsheet();
  for (int iteration = 0; iteration < 8; ++iteration) {
    const int count       = sheet->getColumnCount();
    const QString expired = generator;
    auto* wrapper = dynamic_cast<TZeraryColumnFx*>(document.fx(generator));
    const std::wstring nativeId =
        QString::fromStdWString(wrapper->getZeraryFx()->getFxId())
            .toLower()
            .toStdWString();
    for (const QString& port : {QString("Source1"), QString("Source2")})
      call(context, document, "fx.connect",
           {{"fx_id", composite}, {"port", port}, {"source_fx_id", generator}});
    call(context, document, "fx.delete", {{"fx_id", generator}});
    require(!document.fx(composite)->getInputPort("Source1")->isConnected() &&
                !document.fx(composite)->getInputPort("Source2")->isConnected(),
            "Deleting a shared generator must clear every downstream port");
    require(
        sheet->getColumnCount() == count - 1 &&
            sheet->getFxDag()->getFxById(nativeId) == nullptr,
        "Repeated generator deletion must remove the column and native lookup");
    require(sheet->getColumn(neighborFx->getColumnIndex()) ==
                neighborFx->getXshColumn(),
            "Generator deletion must retain neighboring column ownership after "
            "index shifts");
    generator = create(context, document, "colorCardFx");
    require(generator != expired && !document.effects.contains(expired),
            "Reallocated generators must never revive an expired handle");
    validateAutomationFxGraph(*document.scene);
  }
}

void testColumnRules(Context& context, Document& document) {
  const QString card = create(context, document, "colorCardFx");
  auto* generator    = dynamic_cast<TZeraryColumnFx*>(document.fx(card));
  TXshColumn* column = generator->getXshColumn();
  column->lock(true);
  expectError(context, "locked",
              [&] { call(context, document, "fx.delete", {{"fx_id", card}}); });
  column->lock(false);
  TStageObjectTree* tree = document.scene->getXsheet()->getStageObjectTree();
  TStageObject* object   = tree->getStageObject(
      TStageObjectId::ColumnId(generator->getColumnIndex()));
  const QString objectId = document.idForObject(object);
  object->lock(true);
  expectError(context, "locked", [&] {
    call(context, document, "fx.parameter.set",
         {{"fx_id", card},
          {"parameter", "color"},
          {"value", QJsonArray{0, 0, 0, 255}}});
  });
  object->lock(false);
  TStageObject* child = tree->getStageObject(TStageObjectId::PegbarId(99));
  child->setParent(object->getId());
  expectError(context, "has_dependents",
              [&] { call(context, document, "fx.delete", {{"fx_id", card}}); });
  child->setParent(TStageObjectId::TableId);
  call(context, document, "fx.delete", {{"fx_id", card}});
  require(!document.objects.contains(objectId),
          "Generator deletion must expire its stage handle");
}

void testGraphValidation(Context& context, Document& document) {
  const QString first  = create(context, document, "STD_blurFx");
  const QString second = create(context, document, "STD_blurFx");
  TFx* a               = document.fx(first);
  TFx* b               = document.fx(second);
  a->getInputPort("Source")->setFx(b);
  b->getInputPort("Source")->setFx(a);
  expectError(context, "GRAPH_CYCLE",
              [&] { validateAutomationFxGraph(*document.scene); });
  b->getInputPort("Source")->setFx(nullptr);
  a->getInputPort("Source")->setFx(nullptr);
  validateAutomationFxGraph(*document.scene);
}

void testGraphOwnership(Context& context, Document& document) {
  FxDag* graph       = document.scene->getXsheet()->getFxDag();
  const QString card = create(context, document, "colorCardFx");
  auto* wrapper      = dynamic_cast<TZeraryColumnFx*>(document.fx(card));
  graph->getInternalFxs()->addFx(wrapper->getZeraryFx());
  expectError(context, "INVALID_GRAPH",
              [&] { validateAutomationFxGraph(*document.scene); });
  graph->getInternalFxs()->removeFx(wrapper->getZeraryFx());
  std::unique_ptr<TPersist> persisted(TPersist::create("Toonz_outputFx"));
  auto* nativeOutput = dynamic_cast<TOutputFx*>(persisted.get());
  require(nativeOutput != nullptr,
          "The registered native output factory must be available");
  TFxP output = nativeOutput;
  persisted.release();
  graph->getInternalFxs()->addFx(output.getPointer());
  expectError(context, "INVALID_GRAPH",
              [&] { validateAutomationFxGraph(*document.scene); });
  graph->getInternalFxs()->removeFx(output.getPointer());
  TFxP detached = new TZeraryColumnFx();
  graph->getInternalFxs()->addFx(detached.getPointer());
  expectError(context, "INVALID_GRAPH",
              [&] { validateAutomationFxGraph(*document.scene); });
  graph->getInternalFxs()->removeFx(detached.getPointer());
  validateAutomationFxGraph(*document.scene);
}

void testAdmission(Context& context, Document& document) {
  expectError(context, "UNSUPPORTED_FX",
              [&] { create(context, document, "externalCommandFx"); });
  Document& other       = context.addDocument(std::make_unique<ToonzScene>());
  const QString foreign = create(context, other, "colorCardFx");
  expectError(context, "invalid_handle", [&] {
    call(context, document, "fx.parameter.get", {{"fx_id", foreign}});
  });
  context.removeDocument(other.id);
  validateAutomationFxGraph(*document.scene);
}

QString createBudgetAssets(Context& context, Document& document) {
  QString vectorId;
  const QJsonObject frame{{"number", 1}, {"suffix", ""}};
  for (const QString& format : {QString("rgba8"), QString("vector")}) {
    const QJsonObject level = executeScene(context, "level.create",
                                           {{"document_id", document.id},
                                            {"name", format},
                                            {"format", format}})
                                  .toObject();
    QJsonObject drawing{{"document_id", document.id},
                        {"level_id", level.value("level_id")},
                        {"drawing_id", frame},
                        {"format", format}};
    if (format == "rgba8") {
      drawing.insert("width", 2);
      drawing.insert("height", 3);
      drawing.insert("dpi", 128);
    } else {
      vectorId = level.value("level_id").toString();
    }
    executeDrawing(context, "drawing.create", drawing);
  }
  const QJsonArray points{QJsonObject{{"x", -1}, {"y", 0}, {"thickness", 1}},
                          QJsonObject{{"x", 0}, {"y", 1}, {"thickness", 1}},
                          QJsonObject{{"x", 1}, {"y", 0}, {"thickness", 1}}};
  executeDrawing(context, "vector.edit",
                 {{"document_id", document.id},
                  {"level_id", vectorId},
                  {"drawing_id", frame},
                  {"coordinate_space", "drawing_native"},
                  {"sharing", "all_exposures"},
                  {"edits", QJsonArray{QJsonObject{{"action", "add"},
                                                   {"style_id", 1},
                                                   {"closed", false},
                                                   {"points", points}}}}});
  return vectorId;
}

void checkBudgetBoundary(Context& context, Document& document,
                         const QString& vectorId, qint64 usedBytes) {
  const QJsonObject target{{"document_id", document.id}};
  const QJsonObject paletteTarget{{"document_id", document.id},
                                  {"level_id", vectorId}};
  const QJsonValue before = executeScene(context, "scene.summary", target);
  const QJsonValue paletteBefore =
      executeDrawing(context, "palette.inspect", paletteTarget);
  const quint64 revision = document.revision;
  const qint64 remaining = AutomationAssetLimitBytes - usedBytes;
  expectNoMutation(context, [&] {
    requireAssetGrowth(*document.scene, remaining, 4094, 254);
  });
  expectError(context, "resource_limit",
              [&] { requireAssetGrowth(*document.scene, remaining + 1); });
  expectError(context, "resource_limit",
              [&] { requireAssetGrowth(*document.scene, 0, 4095); });
  expectError(context, "resource_limit",
              [&] { requireAssetGrowth(*document.scene, 0, 0, 255); });
  require(document.revision == revision,
          "Asset admission must not change the document revision");
  require(executeScene(context, "scene.summary", target) == before,
          "Asset admission must preserve native levels and drawing IDs");
  require(executeDrawing(context, "palette.inspect", paletteTarget) ==
              paletteBefore,
          "Asset admission must preserve palette styles and key snapshots");
}

void checkBudgetFixture(Document& document, const QString& vectorId) {
  auto* levels = document.scene->getLevelSet();
  require(levels->getLevelCount() == 2, "Budget fixture must have two levels");
  auto* raster = levels->getLevel(L"rgba8")->getSimpleLevel();
  auto* vector = document.level(vectorId)->getSimpleLevel();
  require(raster != nullptr, "Budget raster level must exist");
  require(vector != nullptr, "Budget vector level must exist");
  require(raster->getFrameCount() == 1, "Budget raster must have one drawing");
  require(vector->getFrameCount() == 1, "Budget vector must have one drawing");
  const TImageP image = raster->getFrame(TFrameId(1), false);
  require(bool(image), "Budget raster image must exist");
  require(bool(image->raster()), "Budget image must have a native raster");
  require(image->raster()->getLx() == 2, "Budget raster width must be two");
  require(image->raster()->getLy() == 3, "Budget raster height must be three");
  const TVectorImageP geometry = vector->getFrame(TFrameId(1), false);
  require(bool(geometry), "Budget vector image must exist");
  require(geometry->getStrokeCount() == 1,
          "Budget vector must have one stroke");
  require(geometry->getStroke(0)->getControlPointCount() == 3,
          "Budget stroke must have three control points");
  require(raster->getPalette() == nullptr, "Budget RGBA must have no palette");
  require(vector->getPalette() != nullptr, "Budget vector must have a palette");
  require(vector->getPalette()->getStyleCount() == 2,
          "Budget vector must start with two native styles");
}

void testAssetBudget(Context& context) {
  require(AutomationAssetLimitBytes == 256LL * 1024 * 1024,
          "Asset boundary regression must retain the production 256 MiB cap");
  Document& document     = context.addDocument(std::make_unique<ToonzScene>());
  const QString vectorId = createBudgetAssets(context, document);
  checkBudgetFixture(document, vectorId);
  // Two drawing headers, six RGBA pixels, one stroke, two styles, one page.
  qint64 used = 2 * 65536 + 6 * 4 + 3 * 64 + 1024 + 2 * 1024 + 1024;
  checkBudgetBoundary(context, document, vectorId, used);
  auto* level             = document.level(vectorId)->getSimpleLevel();
  const TImageP heldImage = level->getFrame(TFrameId(1), false);
  const TPaletteP previousPalette(level->getPalette());
  executeDrawing(context, "palette.page.create",
                 {{"document_id", document.id},
                  {"level_id", vectorId},
                  {"name", "Copied styles"}});
  used += 1024;
  checkBudgetBoundary(context, document, vectorId, used);
  require(previousPalette->getPageCount() == 1,
          "Page edits must preserve detached palette readers");
  require(level->getPalette()->getPageCount() == 2,
          "Page creation must publish the native page");
  require(heldImage->getPalette() == level->getPalette(),
          "Loaded drawings must bind the newly published level palette");
  const QJsonObject color{{"r", 23}, {"g", 41}, {"b", 73}, {"a", 255}};
  QJsonObject style{{"document_id", document.id},
                    {"level_id", vectorId},
                    {"name", "Budget style"},
                    {"color", color}};
  const QJsonObject created =
      executeDrawing(context, "palette.create_style", style).toObject();
  used += 1024;
  checkBudgetBoundary(context, document, vectorId, used);
  style.remove("name");
  style.insert("style_id", created.value("style_id"));
  for (int frame : {0, 12}) {
    style.insert("keyframe", frame);
    executeDrawing(context, "palette.update_style", style);
    used += 1024;
    checkBudgetBoundary(context, document, vectorId, used);
  }
  style.insert("color",
               QJsonObject{{"r", 180}, {"g", 80}, {"b", 25}, {"a", 255}});
  executeDrawing(context, "palette.update_style", style);
  checkBudgetBoundary(context, document, vectorId, used);
  executeDrawing(context, "palette.clone_style",
                 {{"document_id", document.id},
                  {"level_id", vectorId},
                  {"style_id", created.value("style_id")},
                  {"page_index", 1},
                  {"name", "Copied animated style"}});
  used += 3 * 1024;
  checkBudgetBoundary(context, document, vectorId, used);
  require(heldImage->getPalette() == level->getPalette(),
          "Animated style copies must rebind previously loaded drawings");
  context.removeDocument(document.id);
}

// One write as the dispatcher runs it: journal prepared, revision advanced
// when the handler began a change, step recorded.
QJsonObject journaledCall(Context& context, Document& document,
                          const QString& method, QJsonObject params = {}) {
  prepareFxGraphJournal(context, document.id);
  context.resetMutationStarted();
  params["document_id"] = document.id;
  QJsonObject result    = executeFx(context, method, params).toObject();
  const bool advanced   = context.mutationStarted();
  if (advanced) ++document.revision;
  recordFxGraphStep(context, document.id, method, params, advanced, &result);
  return result;
}

QJsonObject deltaFrom(Document& document, quint64 revision) {
  return automationFxGraphDelta(document,
                                {{"document_id", document.id},
                                 {"from_revision", QString::number(revision)}})
      .toObject();
}

void requireDeltaError(Document& document, quint64 revision,
                       const QString& reason) {
  try {
    deltaFrom(document, revision);
  } catch (const Error& error) {
    require(error.details.value("reason") == reason,
            "fx.graph.delta must name why a delta is unavailable");
    return;
  }
  throw std::runtime_error("fx.graph.delta accepted an unavailable range");
}

void testGraphJournalSteps(Context& context) {
  Document& document  = context.addDocument(std::make_unique<ToonzScene>());
  const quint64 start = document.revision;
  const QJsonObject created =
      journaledCall(context, document, "fx.create", {{"type", "STD_blurFx"}});
  const QString blur       = created.value("fx_id").toString();
  const QJsonObject step   = created.value("graph_delta").toObject();
  const QJsonArray entries = step.value("entries").toArray();
  require(entries.size() == 1 &&
              entries[0].toObject().value("change") == "created" &&
              entries[0].toObject().value("cause") == "requested",
          "fx.create must report its node as a requested creation");
  const QJsonObject same = journaledCall(
      context, document, "fx.rename",
      {{"fx_id", blur},
       {"name", QString::fromStdWString(document.fx(blur)->getName())}});
  require(same.value("graph_delta").isNull(),
          "A no-op write must report a null graph delta");
  journaledCall(context, document, "fx.parameter.set",
                {{"fx_id", blur}, {"parameter", "value"}, {"value", 3}});
  journaledCall(context, document, "fx.parameter.set",
                {{"fx_id", blur}, {"parameter", "value"}, {"value", 0}});
  const QJsonArray net = deltaFrom(document, start).value("changes").toArray();
  require(net.size() == 1 && net[0].toObject().value("change") == "created" &&
              net[0].toObject().value("parameters_changed").toArray().isEmpty(),
          "A net delta must fold intermediate parameter changes away");
  journaledCall(context, document, "fx.delete", {{"fx_id", blur}});
  const QJsonArray gone = deltaFrom(document, start).value("changes").toArray();
  require(gone.size() == 1 && gone[0].toObject().value("change") == "transient",
          "An effect created and deleted inside a window is transient");
  requireDeltaError(document, document.revision + 1, "future_revision");
  context.removeDocument(document.id);
}

void testGraphJournalWindow(Context& context) {
  Document& document  = context.addDocument(std::make_unique<ToonzScene>());
  const quint64 start = document.revision;
  const QString blur =
      journaledCall(context, document, "fx.create", {{"type", "STD_blurFx"}})
          .value("fx_id")
          .toString();
  for (int index = 0; index < 1030; ++index)
    journaledCall(context, document, "fx.parameter.set",
                  {{"fx_id", blur},
                   {"parameter", "value"},
                   {"value", index % 2 ? 1 : 2}});
  requireDeltaError(document, start, "window_exceeded");
  const QJsonObject window = automationFxDeltaWindow(document);
  require(window.value("retained_steps").toInt() == 1024 &&
              window.value("oldest_from_revision").toString() ==
                  QString::number(document.revision - 1024),
          "The journal must keep exactly its last 1024 steps");
  require(deltaFrom(document, document.revision - 1024)
                  .value("total_changes")
                  .toInt() == 0,
          "A delta at the oldest retained revision must stay available");
  require(deltaFrom(document, document.revision - 1)
                  .value("total_changes")
                  .toInt() == 1,
          "A one-step delta must report the parameter change");
  context.removeDocument(document.id);
}

qint64 medianMicroseconds(std::vector<qint64> times) {
  std::sort(times.begin(), times.end());
  return times[times.size() / 2];
}

// A write whose effect declaration says it cannot change the FX graph, as
// the dispatcher records it.
qint64 unchangedStepMicroseconds(Context& context, Document& document) {
  QElapsedTimer timer;
  timer.start();
  ++document.revision;
  QJsonObject result;
  recordFxGraphStep(context, document.id, "level.create", {}, true, &result);
  return timer.nsecsElapsed() / 1000;
}

// P-2: the cost of one recorded step at the document's FX node limit, for an
// FX write and for a write that cannot change the graph (the shipping path,
// without the test-build check).
void testGraphJournalCost(Context& context) {
  Document& document = context.addDocument(std::make_unique<ToonzScene>());
  QString blur;
  for (int index = 0; index < 4090; ++index)
    blur = call(context, document, "fx.create", {{"type", "STD_blurFx"}})
               .value("fx_id")
               .toString();
  QElapsedTimer baseline;
  baseline.start();
  prepareFxGraphJournal(context, document.id);
  const qint64 baselineMicroseconds = baseline.nsecsElapsed() / 1000;
  std::vector<qint64> fxTimes, otherTimes;
  setFxJournalVerification(false);
  for (int index = 0; index < 21; ++index) {
    QElapsedTimer timer;
    timer.start();
    journaledCall(context, document, "fx.parameter.set",
                  {{"fx_id", blur},
                   {"parameter", "value"},
                   {"value", index % 2 ? 1 : 2}});
    fxTimes.push_back(timer.nsecsElapsed() / 1000);
    otherTimes.push_back(unchangedStepMicroseconds(context, document));
  }
  setFxJournalVerification(true);
  std::fprintf(
      stderr,
      "fx journal at %d effects: baseline %lld us; FX write median "
      "%lld us, worst %lld us; other write median %lld us\n",
      4090, static_cast<long long>(baselineMicroseconds),
      static_cast<long long>(medianMicroseconds(fxTimes)),
      static_cast<long long>(*std::max_element(fxTimes.begin(), fxTimes.end())),
      static_cast<long long>(medianMicroseconds(otherTimes)));
  require(medianMicroseconds(fxTimes) < 50000,
          "A journal step at the FX node limit must stay near 20 ms");
  require(medianMicroseconds(otherTimes) < 1000,
          "A write that cannot change the graph must not snapshot it");
  context.removeDocument(document.id);
}

// K2: a curve the editor locked refuses every parameter write before any
// change; unlocking admits the same call.
void testCurveLockRefusesEdits(Context& context, Document& document) {
  const QString blur = create(context, document, "STD_blurFx");
  auto* value        = dynamic_cast<TDoubleParam*>(
      document.fx(blur)->getParams()->getParam("value"));
  const double before = value->getDefaultValue();
  value->setEditorLocked(true);
  const QJsonObject set{{"fx_id", blur}, {"parameter", "value"}, {"value", 4}};
  QJsonObject key = set;
  key["frame"]    = 2;
  expectError(context, "locked",
              [&] { call(context, document, "fx.parameter.set", set); });
  expectError(context, "locked",
              [&] { call(context, document, "fx.parameter.set_key", key); });
  require(value->getDefaultValue() == before && !value->hasKeyframes(),
          "A locked curve must not change");
  value->setEditorLocked(false);
  call(context, document, "fx.parameter.set", set);
  require(value->getDefaultValue() == 4,
          "An unlocked curve must accept the refused write");
  const QString card = create(context, document, "colorCardFx");
  auto* wrapper      = dynamic_cast<TZeraryColumnFx*>(document.fx(card));
  auto* color        = dynamic_cast<TPixelParam*>(
      wrapper->getZeraryFx()->getParams()->getParam("color"));
  color->getGreen()->setEditorLocked(true);
  try {
    call(context, document, "fx.parameter.set",
         {{"fx_id", card},
          {"parameter", "color"},
          {"value", QJsonArray{1, 2, 3, 255}}});
    throw std::runtime_error("A locked color channel accepted a write");
  } catch (const Error& error) {
    require(error.code == "locked" &&
                error.details.value("reason") == "curve_locked" &&
                error.details.value("channel").toString().startsWith("color."),
            "A locked channel must be named in the refusal");
  }
  color->getGreen()->setEditorLocked(false);
}

}  // namespace

void runAutomationFxTests(Context& context) {
  testKeyframeInitialization();
  testNativeKeyframePredecessors();
  Document& document = context.addDocument(std::make_unique<ToonzScene>());
  testCatalog(context, document);
  testHiddenParametersAreNotPublic(context, document);
  testCatalogWithFailingFactory();
  testGenerator(context, document);
  testConnections(context, document);
  testImplicitCycles(context, document);
  testParameters(context, document);
  testNoOpParameters(context, document);
  testOutputSelection(context, document);
  testDeletion(context, document);
  testRepeatedReconnect(context, document);
  testRepeatedGeneratorDeletion(context, document);
  testColumnRules(context, document);
  testGraphValidation(context, document);
  testGraphOwnership(context, document);
  testAdmission(context, document);
  testCurveLockRefusesEdits(context, document);
  context.removeDocument(document.id);
  testAssetBudget(context);
  testGraphJournalSteps(context);
  testGraphJournalWindow(context);
  testGraphJournalCost(context);
  runAutomationDispatcherTests(context);
  Live::runAutomationLiveProtocolTests();
  Live::runAutomationLiveRaceTests();
}

}  // namespace GroundzeroAutomation
