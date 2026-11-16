#include "automation.h"
#include "automation_fx.h"

#include "tdoubleparam.h"
#include "tfx.h"
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
  testGenerator(context, document);
  testConnections(context, document);
  testImplicitCycles(context, document);
  testParameters(context, document);
  testOutputSelection(context, document);
  testDeletion(context, document);
  testColumnRules(context, document);
  testGraphValidation(context, document);
  testAdmission(context, document);
  context.removeDocument(document.id);
}

}  // namespace GroundzeroAutomation
