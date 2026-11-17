#include "automation.h"
#include "automation_dispatch.h"
#include "automation_rig.h"

#include "toonz/hook.h"
#include "toonz/levelset.h"
#include "toonz/stageunits.h"
#include "toonz/toonzscene.h"
#include "toonz/tpinnedrangeset.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshsimplelevel.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

void near(double actual, double expected, const char* message,
          double tolerance = 1e-5) {
  require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
          message);
}

const QStringList Reads{"rig.inspect", "rig.evaluate", "hook.inspect",
                        "scene.summary", "animation.sample"};

QJsonObject request(Dispatcher& dispatcher, const QString& method,
                    QJsonObject params) {
  static int sequence = 0;
  if (!Reads.contains(method) && !params.contains("operation_id"))
    params.insert("operation_id", "rig-test-" + QString::number(++sequence));
  return dispatcher.dispatch(
      {{"id", "rig"}, {"method", method}, {"params", params}});
}

QJsonObject result(Dispatcher& dispatcher, const QString& method,
                   const QJsonObject& params) {
  const auto response = request(dispatcher, method, params);
  if (!response.contains("result"))
    throw std::runtime_error(
        QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString());
  return response.value("result").toObject();
}

QJsonObject at(Document& document, QJsonObject params = {}) {
  params.insert("document_id", document.id);
  params.insert("expected_revision", document.revisionText());
  return params;
}

void rejected(Dispatcher& dispatcher, Document& document, const QString& method,
              const QJsonObject& params, const QString& code) {
  const auto revision = document.revision;
  const auto response = request(dispatcher, method, params);
  const QString actual =
      response.value("error").toObject().value("code").toString();
  if (actual != code)
    throw std::runtime_error(("Rig rejection " + method + " returned " +
                              actual + " instead of " + code)
                                 .toStdString());
  require(document.revision == revision,
          "Rig rejection changed the document revision");
}

Document& createDocument(Context& context, Dispatcher& dispatcher) {
  const auto created =
      result(dispatcher, "scene.create",
             {{"name", "Rig tests"}, {"width_px", 32}, {"height_px", 32}});
  return context.document(created.value("document_id").toString());
}

QJsonObject drawingId(int number) {
  return {{"number", number}, {"suffix", ""}};
}

QString createLevel(Dispatcher& dispatcher, Document& document,
                    const QString& name, int drawings,
                    const QString& format = "vector") {
  const QString level =
      result(dispatcher, "level.create",
             at(document, {{"name", name}, {"format", format}}))
          .value("level_id")
          .toString();
  for (int number = 1; number <= drawings; ++number) {
    QJsonObject params{{"level_id", level},
                       {"drawing_id", drawingId(number)},
                       {"format", format}};
    if (format != "vector") {
      params.insert("width", 64);
      params.insert("height", 48);
      params.insert("dpi", 100);
    }
    result(dispatcher, "drawing.create", at(document, params));
  }
  return level;
}

QJsonObject createColumn(Dispatcher& dispatcher, Document& document,
                         const QString& level, const QList<int>& rows) {
  const auto column =
      result(dispatcher, "column.create", at(document, {{"name", "Part"}}));
  for (int row = 0; row < rows.size(); ++row)
    result(dispatcher, "column.expose",
           at(document, {{"column_id", column.value("column_id")},
                         {"level_id", level},
                         {"drawing_id", drawingId(rows[row])},
                         {"start_frame", row},
                         {"length", 1},
                         {"overwrite", true}}));
  return column;
}

QJsonObject point(double x, double y) { return {{"x", x}, {"y", y}}; }

QJsonObject hookParams(const QString& level, int number,
                       const QJsonArray& positions, double error = 1e-4) {
  return {{"level_id", level},
          {"hook_number", number},
          {"unit", "inch"},
          {"positions", positions},
          {"coordinate_policy", "native_roundtrip"},
          {"max_coordinate_error_inch", error}};
}

QJsonObject position(int drawing, const QJsonObject& a,
                     const QJsonObject& b = {}) {
  QJsonObject result{{"drawing_id", drawingId(drawing)}, {"a", a}};
  if (!b.isEmpty()) result.insert("b", b);
  return result;
}

QJsonObject parentParams(const QString& child, const QString& parent,
                         int hook = 0) {
  QJsonObject params{{"object_id", child},
                     {"parent_id", parent},
                     {"placement_policy", "preserve_local"}};
  if (hook)
    params.insert("parent_handle",
                  QJsonObject{{"kind", "hook"}, {"hook_number", hook}});
  return params;
}

TXshSimpleLevel* nativeLevel(Document& document, const QString& level) {
  return document.level(level)->getSimpleLevel();
}

TStageObjectTree& stageOf(Document& document) {
  return *document.scene->getXsheet()->getStageObjectTree();
}

QString savePackage(Dispatcher& dispatcher, Document& document,
                    const QString& name) {
  return result(dispatcher, "scene.save_copy",
                at(document, {{"directory", name}}))
      .value("package_directory")
      .toString();
}

void sidecarRoundTripAndNumbering(Context& context) {
  Dispatcher dispatcher(context);
  auto& document     = createDocument(context, dispatcher);
  const QString body = createLevel(dispatcher, document, "Body", 3);
  createColumn(dispatcher, document, body, {1, 2, 3});
  result(dispatcher, "hook.set",
         at(document,
            hookParams(body, 1,
                       {position(1, point(0.1, 0.2)),
                        position(2, point(0.3, 0.4), point(0.5, 0.6))})));
  result(dispatcher, "hook.set",
         at(document, hookParams(body, 3, {position(3, point(-1, 2))})));
  const auto source =
      result(dispatcher, "hook.inspect", at(document, {{"level_id", body}}));
  const QString package = savePackage(dispatcher, document, "rig-sidecar");
  const QString sidecar = package + "/assets/level-0_hooks.pli.xml";
  require(QFile::exists(sidecar), "The package has no native hooks sidecar");
  const TFilePath level((package + "/assets/level-0.pli").toStdWString());
  require(TXshSimpleLevel::getExistingHookFile(level).getQString() == sidecar,
          "Native level loading selects another hooks file");
  const auto opened = result(dispatcher, "scene.open", {{"path", package}});
  auto& reopened    = context.document(opened.value("document_id").toString());
  const auto summary =
      result(dispatcher, "scene.summary", {{"document_id", reopened.id}});
  const QString copy = summary.value("levels")
                           .toArray()[0]
                           .toObject()
                           .value("level_id")
                           .toString();
  const auto inspected =
      result(dispatcher, "hook.inspect", at(reopened, {{"level_id", copy}}));
  const auto hooks = inspected.value("hooks").toArray();
  require(hooks.size() == 2 && hooks[0].toObject().value("hook_number") == 1 &&
              hooks[1].toObject().value("hook_number") == 3,
          "Reopened hook numbers differ");
  for (int index = 0; index < 2; ++index)
    require(
        inspected.value("hooks").toArray()[index].toObject().value("keys") ==
            source.value("hooks").toArray()[index].toObject().value("keys"),
        "Reopened canonical hook keys differ from their source");
  HookSet* numbering = nativeLevel(reopened, copy)->getHookSet();
  require(numbering->getHookCount() == 3 && numbering->getHook(1)->isEmpty(),
          "Hook numbering was not kept through the native sidecar");
  context.removeDocument(reopened.id);
  QFile tampered(sidecar);
  require(tampered.open(QIODevice::Append) && tampered.write(" ") == 1,
          "Cannot alter the package sidecar");
  tampered.close();
  const auto refused = request(dispatcher, "scene.open", {{"path", package}});
  require(refused.value("error").toObject().value("code") == "invalid_package",
          "An altered hooks sidecar was admitted");
  context.removeDocument(document.id);
}

double hookA(Document& document, const QString& level, int drawing) {
  return nativeLevel(document, level)
      ->getHookSet()
      ->getHook(0)
      ->getAPos(TFrameId(drawing))
      .x;
}

void canonicalHookAndPivotPrecision(Context& context) {
  Dispatcher dispatcher(context);
  auto& document     = createDocument(context, dispatcher);
  const QString body = createLevel(dispatcher, document, "Body", 1);
  const QJsonArray precise{position(1, point(1.234567891, -999.987654321))};
  rejected(dispatcher, document, "hook.set",
           at(document, hookParams(body, 1, precise, 0)), "invalid_params");
  require(nativeLevel(document, body)->getHookSet()->getHookCount() == 0,
          "A rejected precision request allocated a hook");
  const auto set = result(dispatcher, "hook.set",
                          at(document, hookParams(body, 1, precise, 1e-2)));
  near(set.value("max_input_error_inch").toDouble(), 0, "Error not bounded",
       1e-2);
  const auto key = set.value("keys").toArray()[0].toObject();
  near(key.value("a").toObject().value("x").toDouble() * Stage::inch,
       hookA(document, body, 1), "Hook keys are not native canonical values",
       1e-9);
  const auto revision = document.revision;
  result(dispatcher, "hook.set",
         at(document, hookParams(body, 1,
                                 {QJsonObject{{"drawing_id", drawingId(1)},
                                              {"a", key.value("a")}}},
                                 0)));
  require(document.revision == revision,
          "Canonical hook keys were not a stable no-op");
  const QString peg =
      result(dispatcher, "object.create",
             at(document, {{"name", "Pivot"}, {"kind", "pegbar"}}))
          .value("object_id")
          .toString();
  QJsonObject pivot{{"object_id", peg},
                    {"unit", "inch"},
                    {"center", point(1.234567891, 0)},
                    {"policy", QJsonObject{{"kind", "keep_offset"}}},
                    {"coordinate_policy", "native_roundtrip"},
                    {"max_coordinate_error_inch", 0},
                    {"report_frames", QJsonArray{0}}};
  rejected(dispatcher, document, "object.pivot.set", at(document, pivot),
           "invalid_params");
  pivot.insert("max_coordinate_error_inch", 1e-4);
  const auto moved =
      result(dispatcher, "object.pivot.set", at(document, pivot));
  near(moved.value("center").toObject().value("x").toDouble(), 1.23457,
       "Pivot center is not the native canonical value", 1e-12);
  pivot.insert("center", moved.value("center"));
  pivot.insert("max_coordinate_error_inch", 0);
  const auto again = document.revision;
  result(dispatcher, "object.pivot.set", at(document, pivot));
  require(document.revision == again, "Canonical pivot was not a no-op");
  context.removeDocument(document.id);
}

QString columnObject(const QJsonObject& column) {
  return column.value("object_id").toString();
}

void rasterHookSpace(Context& context) {
  Dispatcher dispatcher(context);
  auto& document      = createDocument(context, dispatcher);
  const QString level = createLevel(dispatcher, document, "Raster", 1, "rgba8");
  const auto column   = createColumn(dispatcher, document, level, {1});
  const auto child    = createColumn(dispatcher, document, level, {1});
  result(dispatcher, "hook.set",
         at(document, hookParams(level, 1, {position(1, point(0.1, -0.06))})));
  const TPointD native = nativeLevel(document, level)
                             ->getHookSet()
                             ->getHook(0)
                             ->getAPos(TFrameId(1));
  near(native.x, 10, "Raster hooks are not pixels at the level DPI", 1e-9);
  near(native.y, -6, "Raster hooks are not pixels at the level DPI", 1e-9);
  const auto inspected =
      result(dispatcher, "hook.inspect", at(document, {{"level_id", level}}));
  const auto space = inspected.value("native_space").toObject();
  require(space.value("kind") == "raster_pixel" &&
              space.value("dpi_x").toDouble() == 100,
          "Raster hook space is misreported");
  rejected(
      dispatcher, document, "hook.set",
      at(document,
         hookParams(level, 1, {position(1, point(0, 0), point(0.009, 0))}, 1)),
      "invalid_params");
  result(
      dispatcher, "object.parent",
      at(document, parentParams(columnObject(child), columnObject(column), 1)));
  const auto sample =
      result(dispatcher, "rig.evaluate",
             at(document, {{"object_ids", QJsonArray{columnObject(child)}},
                           {"frames", QJsonArray{0}}}))
          .value("samples")
          .toArray()[0]
          .toObject()
          .value("parent_attachment_local")
          .toObject();
  near(sample.value("x").toDouble(), 0.1, "Raster attachment x differs");
  near(sample.value("y").toDouble(), -0.06, "Raster attachment y differs");
  context.removeDocument(document.id);
}

void passHookAccumulation(Context& context) {
  Dispatcher dispatcher(context);
  auto& document      = createDocument(context, dispatcher);
  const QString body  = createLevel(dispatcher, document, "Body", 4);
  const QString other = createLevel(dispatcher, document, "Other", 1);
  const auto column =
      createColumn(dispatcher, document, body, {1, 2, 3, 4, 2, 1, 3, 1, 3});
  result(dispatcher, "column.expose",
         at(document, {{"column_id", column.value("column_id")},
                       {"level_id", other},
                       {"drawing_id", drawingId(1)},
                       {"start_frame", 7},
                       {"length", 1},
                       {"overwrite", true}}));
  result(dispatcher, "hook.set",
         at(document, hookParams(body, 1,
                                 {position(1, point(0, 0)),
                                  position(2, point(1, 0), point(3, 0)),
                                  position(3, point(0, 1), point(0, 4))})));
  result(dispatcher, "hook.set",
         at(document, hookParams(other, 1, {position(1, point(0, 0))})));
  const auto child = createColumn(dispatcher, document, other, {});
  result(
      dispatcher, "object.parent",
      at(document, parentParams(columnObject(child), columnObject(column), 1)));
  QJsonArray frames;
  for (int row = 0; row < 9; ++row) frames.append(row);
  const auto samples =
      result(dispatcher, "rig.evaluate",
             at(document, {{"object_ids", QJsonArray{columnObject(child)}},
                           {"frames", frames}}))
          .value("samples")
          .toArray();
  const double expected[9][2] = {{0, 0}, {1, 0},  {-2, 1}, {-2, 1}, {1, 0},
                                 {0, 0}, {-2, 1}, {0, 0},  {0, 1}};
  auto& stage                 = stageOf(document);
  for (int row = 0; row < 9; ++row) {
    const auto attachment =
        samples[row].toObject().value("parent_attachment_local").toObject();
    const TPointD native =
        stage.getHandlePos(TStageObjectId::ColumnId(0), "H1", row);
    near(attachment.value("x").toDouble(), native.x, "Pass x is not native");
    near(attachment.value("y").toDouble(), native.y, "Pass y is not native");
    near(native.x, expected[row][0], "Native pass x rule changed", 1e-5);
    near(native.y, expected[row][1], "Native pass y rule changed", 1e-5);
  }
  context.removeDocument(document.id);
}

QJsonObject handleParams(const QString& object, const QString& key,
                         const QJsonObject& handle) {
  return {{"object_id", object},
          {key, handle},
          {"placement_policy", "native_no_compensation"},
          {"report_frames", QJsonArray{0}}};
}

QJsonObject hookHandle(int number) {
  return {{"kind", "hook"}, {"hook_number", number}};
}

void handleTargetsAndPivotReset(Context& context) {
  Dispatcher dispatcher(context);
  auto& document       = createDocument(context, dispatcher);
  const QString level  = createLevel(dispatcher, document, "Part", 1);
  const auto column    = createColumn(dispatcher, document, level, {1});
  const QString object = columnObject(column);
  result(dispatcher, "hook.set",
         at(document, hookParams(level, 1, {position(1, point(0.5, 0))})));
  result(dispatcher, "object.pivot.set",
         at(document, {{"object_id", object},
                       {"unit", "inch"},
                       {"center", point(0.1, 0.2)},
                       {"policy", QJsonObject{{"kind", "keep_offset"}}},
                       {"coordinate_policy", "native_roundtrip"},
                       {"max_coordinate_error_inch", 1e-4},
                       {"report_frames", QJsonArray{0}}}));
  const auto reset =
      result(dispatcher, "object.handles.set",
             at(document, handleParams(object, "handle", hookHandle(1))))
          .value("pivot_reset")
          .toObject();
  require(reset.value("occurred").toBool(), "Pivot reset was not reported");
  near(reset.value("previous_center").toObject().value("y").toDouble(), 0.2,
       "The previous center was not reported");
  TPointD center, offset;
  document.object(object)->getCenterAndOffset(center, offset);
  require(center == TPointD() && offset == TPointD(),
          "Native hook handles did not zero the pivot");
  const auto revision = document.revision;
  result(dispatcher, "object.handles.set",
         at(document, handleParams(object, "handle", hookHandle(1))));
  require(document.revision == revision, "Same handles were not a no-op");
  const QString peg =
      result(dispatcher, "object.create",
             at(document, {{"name", "Peg"}, {"kind", "pegbar"}}))
          .value("object_id")
          .toString();
  rejected(dispatcher, document, "object.handles.set",
           at(document, handleParams(peg, "handle", hookHandle(1))),
           "invalid_params");
  rejected(dispatcher, document, "object.handles.set",
           at(document, handleParams(peg, "parent_handle", hookHandle(1))),
           "invalid_params");
  const QString camera = document.idForObject(stageOf(document).getStageObject(
      stageOf(document).getCurrentCameraId(), false));
  rejected(dispatcher, document, "object.handles.set",
           at(document,
              handleParams(camera, "handle",
                           QJsonObject{{"kind", "peg_hole"}, {"letter", "C"}})),
           "unsupported_target");
  document.scene->getXsheet()->getColumn(0)->lock(true);
  rejected(dispatcher, document, "object.handles.set",
           at(document,
              handleParams(object, "handle",
                           QJsonObject{{"kind", "peg_hole"}, {"letter", "B"}})),
           "locked");
  document.scene->getXsheet()->getColumn(0)->lock(false);
  context.removeDocument(document.id);
}

QString createLayer(Dispatcher& dispatcher, Document& document,
                    const QString& object, const QString& name) {
  return result(dispatcher, "offset_layer.create",
                at(document, {{"object_id", object}, {"name", name}}))
      .value("layer")
      .toObject()
      .value("layer_id")
      .toString();
}

void layerHandlesAndSceneBudget(Context& context) {
  Dispatcher dispatcher(context);
  auto& first         = createDocument(context, dispatcher);
  auto& second        = createDocument(context, dispatcher);
  const QString owner = result(dispatcher, "object.create",
                               at(first, {{"name", "Peg"}, {"kind", "pegbar"}}))
                            .value("object_id")
                            .toString();
  const QString layer = createLayer(dispatcher, first, owner, "Walk");
  rejected(dispatcher, second, "offset_layer.update",
           at(second, {{"layer_id", layer}, {"enabled", false}}),
           "invalid_handle");
  rejected(dispatcher, first, "offset_layer.create",
           {{"document_id", first.id},
            {"expected_revision", "0"},
            {"object_id", owner},
            {"name", "Stale"}},
           "revision_conflict");
  const QString package = savePackage(dispatcher, first, "rig-layers");
  auto& reopened =
      context.document(result(dispatcher, "scene.open", {{"path", package}})
                           .value("document_id")
                           .toString());
  const auto objects = result(dispatcher, "rig.inspect", at(reopened))
                           .value("objects")
                           .toArray();
  QString reopenedLayer;
  for (const auto& object : objects)
    for (const auto& summary :
         object.toObject().value("offset_layers").toArray())
      reopenedLayer = summary.toObject().value("layer_id").toString();
  require(!reopenedLayer.isEmpty() && reopenedLayer != layer,
          "Reopen admission did not publish a new layer handle");
  result(dispatcher, "offset_layer.update",
         at(reopened, {{"layer_id", reopenedLayer}, {"enabled", false}}));
  context.removeDocument(reopened.id);
  result(dispatcher, "offset_layer.delete", at(first, {{"layer_id", layer}}));
  rejected(dispatcher, first, "offset_layer.delete",
           at(first, {{"layer_id", layer}}), "invalid_handle");
  auto& stage = stageOf(second);
  for (int pegbar = 0; pegbar < 64; ++pegbar) {
    auto* object = stage.getStageObject(TStageObjectId::PegbarId(pegbar));
    for (int index = 0; index < (pegbar == 63 ? 15 : 16); ++index)
      object->insertOffsetLayer(
          -1, object->createOffsetLayer(std::to_wstring(index)));
  }
  const QString last =
      second.idForObject(stage.getStageObject(TStageObjectId::PegbarId(63)));
  createLayer(dispatcher, second, last, "Last");
  const QString extra =
      second.idForObject(stage.getStageObject(TStageObjectId::PegbarId(64)));
  rejected(dispatcher, second, "offset_layer.create",
           at(second, {{"object_id", extra}, {"name", "Over"}}),
           "resource_limit");
  context.removeDocument(second.id);
  context.removeDocument(first.id);
}

using Breaker = std::function<std::function<void()>(Document&)>;

void requireRefusedPackage(Context& context, const char* name,
                           const Breaker& breaker) {
  Dispatcher dispatcher(context);
  auto& document     = createDocument(context, dispatcher);
  const QString body = createLevel(dispatcher, document, "Body", 1);
  createColumn(dispatcher, document, body, {1});
  result(dispatcher, "object.create",
         at(document, {{"name", "Peg"}, {"kind", "pegbar"}}));
  result(dispatcher, "hook.set",
         at(document, hookParams(body, 1, {position(1, point(0, 0))})));
  const auto restore  = breaker(document);
  const auto response = request(dispatcher, "scene.save_copy",
                                at(document, {{"directory", name}}));
  if (!response.contains("error"))
    throw std::runtime_error(std::string("Admission accepted ") + name);
  require(!QDir(context.workspace() + "/" + name).exists(),
          "A refused package was published");
  restore();
  savePackage(dispatcher, document, QString(name) + "-restored");
  context.removeDocument(document.id);
}

TStageObject* peg(Document& document) {
  return stageOf(document).getStageObject(TStageObjectId::PegbarId(0), false);
}

Hook* bodyHook(Document& document) {
  auto* level = document.scene->getLevelSet()->getLevel(0)->getSimpleLevel();
  return level->getHookSet()->getHook(0);
}

void stageAdmissionRefusals(Context& context) {
  requireRefusedPackage(context, "rig-ik", [](Document& document) {
    peg(document)->setStatus(TStageObject::IK);
    return [&document] { peg(document)->setStatus(TStageObject::XY); };
  });
  requireRefusedPackage(context, "rig-pin", [](Document& document) {
    peg(document)->getPinnedRangeSet()->setRange(0, 4);
    return
        [&document] { peg(document)->getPinnedRangeSet()->removeAllRanges(); };
  });
  requireRefusedPackage(context, "rig-port", [](Document& document) {
    TStageObject::PublishedChannel port;
    port.m_name = L"Port";
    peg(document)->insertPublishedChannel(-1, port);
    return [&document] { peg(document)->removePublishedChannel(0); };
  });
  requireRefusedPackage(context, "rig-peg-hook", [](Document& document) {
    peg(document)->setHandle("H1");
    return [&document] { peg(document)->setHandle("B"); };
  });
  requireRefusedPackage(context, "rig-table-layer", [](Document& document) {
    auto* table = stageOf(document).getStageObject(TStageObjectId::TableId);
    table->insertOffsetLayer(-1, table->createOffsetLayer(L"Table"));
    return [table] { table->removeOffsetLayer(0); };
  });
  requireRefusedPackage(context, "rig-shear", [](Document& document) {
    auto layer = peg(document)->createOffsetLayer(L"Shear");
    layer.m_channels[TStageObject::T_ShearX]->setDefaultValue(1);
    peg(document)->insertOffsetLayer(-1, layer);
    return [&document] { peg(document)->removeOffsetLayer(0); };
  });
  requireRefusedPackage(context, "rig-weight", [](Document& document) {
    auto layer = peg(document)->createOffsetLayer(L"Heavy");
    layer.m_weight->setDefaultValue(11);
    peg(document)->insertOffsetLayer(-1, layer);
    return [&document] { peg(document)->removeOffsetLayer(0); };
  });
}

void hookAdmissionRefusals(Context& context) {
  requireRefusedPackage(context, "rig-tracker", [](Document& document) {
    bodyHook(document)->setTrackerObjectId(0);
    return [&document] { bodyHook(document)->setTrackerObjectId(-1); };
  });
  requireRefusedPackage(context, "rig-orphan-key", [](Document& document) {
    bodyHook(document)->setAPos(TFrameId(9), TPointD());
    return [&document] { bodyHook(document)->eraseFrame(TFrameId(9)); };
  });
  requireRefusedPackage(context, "rig-noncanonical", [](Document& document) {
    bodyHook(document)->setAPos(TFrameId(1), TPointD(1.234567891, 0));
    return [&document] { bodyHook(document)->setAPos(TFrameId(1), TPointD()); };
  });
  requireRefusedPackage(context, "rig-far-key", [](Document& document) {
    bodyHook(document)->setAPos(TFrameId(1), TPointD(1001 * Stage::inch, 0));
    return [&document] { bodyHook(document)->setAPos(TFrameId(1), TPointD()); };
  });
  requireRefusedPackage(context, "rig-depth", [](Document& document) {
    auto& stage   = stageOf(document);
    auto previous = TStageObjectId::PegbarId(0);
    for (int index = 1; index <= 64; ++index) {
      stage.getStageObject(TStageObjectId::PegbarId(index))
          ->setParent(previous);
      previous = TStageObjectId::PegbarId(index);
    }
    return [&stage] {
      stage.getStageObject(TStageObjectId::PegbarId(1))
          ->setParent(TStageObjectId::TableId);
    };
  });
}

void evaluationBoundsAndPaging(Context& context) {
  Dispatcher dispatcher(context);
  auto& document = createDocument(context, dispatcher);
  QJsonArray objects, frames;
  for (int index = 0; index < 9; ++index)
    objects.append(result(dispatcher, "object.create",
                          at(document, {{"name", "Peg"}, {"kind", "pegbar"}}))
                       .value("object_id"));
  for (int frame = 0; frame < 64; ++frame) frames.append(frame);
  rejected(dispatcher, document, "rig.evaluate",
           at(document, {{"object_ids", objects}, {"frames", frames}}),
           "resource_limit");
  objects.removeLast();
  const auto samples =
      result(dispatcher, "rig.evaluate",
             at(document, {{"object_ids", objects}, {"frames", frames}}))
          .value("samples")
          .toArray();
  require(samples.size() == 512, "512 object-frame pairs were not evaluated");
  rejected(dispatcher, document, "rig.evaluate",
           at(document, {{"object_ids", QJsonArray{objects[0], objects[0]}},
                         {"frames", QJsonArray{0}}}),
           "invalid_params");
  const QString level = createLevel(dispatcher, document, "Body", 1);
  const auto column   = createColumn(dispatcher, document, level, {1});
  document.object(objects[0].toString())
      ->setParent(TStageObjectId::ColumnId(0));
  document.object(objects[0].toString())->setParentHandle("H2");
  const QJsonObject subtree{{"root_object_id", columnObject(column)},
                            {"limit", 1}};
  const auto page = result(dispatcher, "rig.inspect", at(document, subtree));
  require(page.value("total_objects").toInt() == 2 &&
              page.value("next_index").toInt() == 1,
          "Subtree paging is wrong");
  QJsonObject rest = subtree;
  rest.insert("start_index", 1);
  const auto last = result(dispatcher, "rig.inspect", at(document, rest));
  require(last.value("next_index").isNull(), "The last page has a next index");
  QJsonArray records = page.value("objects").toArray();
  records.append(last.value("objects").toArray()[0]);
  QJsonObject binding;
  for (const auto& record : records)
    if (record.toObject().value("object_id") == objects[0])
      binding =
          record.toObject().value("hook_bindings").toArray()[0].toObject();
  require(binding.value("hook_number").toInt() == 2 &&
              !binding.value("hook_defined").toBool(),
          "An undefined hook binding is not reported");
  context.removeDocument(document.id);
}

void renumberVerifiesHookMove(Context& context) {
  Dispatcher dispatcher(context);
  auto& document     = createDocument(context, dispatcher);
  const QString body = createLevel(dispatcher, document, "Body", 2);
  result(dispatcher, "hook.set",
         at(document, hookParams(body, 1,
                                 {position(1, point(0.1, 0)),
                                  position(2, point(0.2, 0))})));
  const QJsonArray swap{QJsonObject{{"drawing_id", drawingId(1)},
                                    {"destination_drawing_id", drawingId(2)}},
                        QJsonObject{{"drawing_id", drawingId(2)},
                                    {"destination_drawing_id", drawingId(1)}}};
  const auto renumbered = result(dispatcher, "drawing.renumber",
                                 at(document, {{"level_id", body},
                                               {"mapping", swap},
                                               {"sharing", "all_exposures"}}));
  require(renumbered.value("hook_keys_renumbered").toInt() == 2,
          "Renumbered hook keys are not reported");
  near(hookA(document, body, 1), 0.2 * Stage::inch,
       "Renumbering did not move hook keys", 1e-3);
  context.removeDocument(document.id);
}

}  // namespace

void runAutomationRigTests(Context& context) {
  sidecarRoundTripAndNumbering(context);
  canonicalHookAndPivotPrecision(context);
  rasterHookSpace(context);
  passHookAccumulation(context);
  handleTargetsAndPivotReset(context);
  layerHandlesAndSceneBudget(context);
  stageAdmissionRefusals(context);
  hookAdmissionRefusals(context);
  evaluationBoundsAndPaging(context);
  renumberVerifiesHookMove(context);
  std::cout << "automation-rig-test: hooks, handles, pivots, layers and "
               "admission PASS\n";
}

}  // namespace GroundzeroAutomation
