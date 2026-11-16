#include "automation.h"
#include "automation_fx.h"

#include "tdoubleparam.h"
#include "tdoublekeyframe.h"
#include "tfx.h"
#include "tnotanimatableparam.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "tzeraryfx.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshzeraryfxcolumn.h"

#include <functional>
#include <memory>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
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
  require(effects.size() == 6,
          "Builtin profile must expose only its six qualified effects");
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
  TFxP output = new TOutputFx();
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

}  // namespace

void runAutomationFxTests(Context& context) {
  Document& document = context.addDocument(std::make_unique<ToonzScene>());
  testCatalog(context, document);
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
  context.removeDocument(document.id);
}

}  // namespace GroundzeroAutomation
