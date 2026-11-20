#include "automation.h"
#include "automation_dispatch.h"
#include "automation_rig.h"

#include "tdoubleparam.h"
#include "tvectorimage.h"
#include "toonz/hook.h"
#include "toonz/levelset.h"
#include "toonz/poselibrary.h"
#include "toonz/stageunits.h"
#include "toonz/toonzscene.h"
#include "toonz/tpinnedrangeset.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcell.h"
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

const QStringList Reads{"rig.inspect",   "rig.evaluate",     "hook.inspect",
                        "scene.summary", "animation.sample", "rig.dependents",
                        "object.groups"};

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

QString filledLevel(Dispatcher& dispatcher, Document& document,
                    const QString& name) {
  const QString id =
      result(dispatcher, "level.create",
             at(document, {{"name", name}, {"format", "vector"}}))
          .value("level_id")
          .toString();
  auto* level = nativeLevel(document, id);
  for (int number = 1; number <= 128; ++number)
    level->setFrame(TFrameId(number), TImageP(new TVectorImage()));
  for (int hook = 0; hook < 64; ++hook)
    for (int number = 1; number <= 128; ++number)
      level->getHookSet()->touchHook(hook)->setAPos(TFrameId(number),
                                                    TPointD());
  return id;
}

void hookKeyBudgets(Context& context) {
  Dispatcher dispatcher(context);
  auto& document     = createDocument(context, dispatcher);
  const QString full = filledLevel(dispatcher, document, "Full");
  rejected(dispatcher, document, "hook.set",
           at(document, hookParams(full, 65, {position(1, point(0, 0))})),
           "resource_limit");
  result(dispatcher, "hook.set",
         at(document, hookParams(full, 1, {position(1, point(0, 0))})));
  for (int index = 1; index < 8; ++index)
    filledLevel(dispatcher, document, "Full " + QString::number(index));
  const QString spare = createLevel(dispatcher, document, "Spare", 1);
  rejected(dispatcher, document, "hook.set",
           at(document, hookParams(spare, 1, {position(1, point(0, 0))})),
           "resource_limit");
  nativeLevel(document, full)
      ->getHookSet()
      ->getHook(1)
      ->eraseFrame(TFrameId(1));
  result(dispatcher, "hook.set",
         at(document, hookParams(spare, 1, {position(1, point(0, 0))})));
  context.removeDocument(document.id);
}

QString createPeg(Dispatcher& dispatcher, Document& document,
                  const QString& name) {
  return result(dispatcher, "object.create",
                at(document, {{"name", name}, {"kind", "pegbar"}}))
      .value("object_id")
      .toString();
}

void setKeys(Dispatcher& dispatcher, Document& document, const QString& object,
             const QString& channel, const QString& unit,
             const QList<QPair<int, double>>& keys) {
  QJsonArray records;
  for (const auto& key : keys)
    records.append(QJsonObject{{"frame", key.first},
                               {"value", key.second},
                               {"interpolation", "linear"}});
  result(dispatcher, "animation.set_keys",
         at(document, {{"object_id", object},
                       {"channel", channel},
                       {"unit", unit},
                       {"keys", records}}));
}

QString errorCode(const QJsonObject& response) {
  return response.value("error").toObject().value("code").toString();
}

QJsonObject errorDetails(const QJsonObject& response) {
  return response.value("error").toObject().value("details").toObject();
}

// Probe P5: getParams + assignParams(clone) gives the copy its own curves,
// observers and layers, the source's handles, and no schematic groups.
void duplicateIsIndependent(Context& context) {
  Dispatcher dispatcher(context);
  auto& document     = createDocument(context, dispatcher);
  const QString root = createPeg(dispatcher, document, "Root");
  const QString peg  = createPeg(dispatcher, document, "Peg");
  auto parent        = parentParams(peg, root);
  parent.insert("parent_handle",
                QJsonObject{{"kind", "peg_hole"}, {"letter", "C"}});
  result(dispatcher, "object.parent", at(document, parent));
  setKeys(dispatcher, document, peg, "x", "inch", {{0, 0}, {10, 2}});
  const QString layer = createLayer(dispatcher, document, peg, "Walk");
  result(
      dispatcher, "object.group",
      at(document, {{"object_ids", QJsonArray{root, peg}}, {"name", "Rig"}}));
  QJsonObject params = at(document, {{"object_id", peg}, {"name", "Copy"}});
  params.insert("operation_id", "duplicate-once");
  const auto copied = result(dispatcher, "object.duplicate", params);
  const auto replay = request(dispatcher, "object.duplicate", params);
  require(replay.value("result") == copied,
          "A replayed duplicate did not return its recorded result");
  const auto record = copied.value("object").toObject();
  require(record.value("parent_id") == root &&
              record.value("parent_handle").toObject().value("native") == "C" &&
              record.value("name") == "Copy",
          "The copy lost its parent, parent handle or name");
  const auto map = copied.value("layer_map").toArray();
  require(map.size() == 1 &&
              map[0].toObject().value("source_layer_id") == layer &&
              map[0].toObject().value("layer_id") != layer &&
              copied.value("control_map").toArray().isEmpty(),
          "The layer map is wrong");
  TStageObject* source = document.object(peg);
  TStageObject* copy   = document.object(record.value("object_id").toString());
  require(copy->getParam(TStageObject::T_X) !=
                  source->getParam(TStageObject::T_X) &&
              copy->getOffsetLayer(0).m_weight.getPointer() !=
                  source->getOffsetLayer(0).m_weight.getPointer(),
          "The copy shares curves with its source");
  require(!copy->isGrouped() && source->isGrouped(),
          "Native copies are not grouped, unlike their sources");
  near(copy->getPlacement(5).a13, source->getPlacement(5).a13,
       "The copy is not placed like its source", 1e-9);
  source->getParam(TStageObject::T_X)->setValue(10, 5);
  near(copy->getParam(TStageObject::T_X)->getValue(10), 2,
       "Editing the source changed the copy");
  const double before = copy->getPlacement(10).a13;
  copy->getParam(TStageObject::T_X)->setValue(10, 3);
  near(copy->getPlacement(10).a13 - before, Stage::inch,
       "The copy's curve does not invalidate its placement");
  const QString column = columnObject(createColumn(
      dispatcher, document, createLevel(dispatcher, document, "Art", 1), {1}));
  rejected(dispatcher, document, "object.duplicate",
           at(document, {{"object_id", column}, {"name", "Column"}}),
           "unsupported_kind");
  rejected(
      dispatcher, document, "object.duplicate",
      at(document,
         {{"object_id", column}, {"name", "Column"}, {"scope", "subtree"}}),
      "invalid_params");
  context.removeDocument(document.id);
}

void subtreeDuplicateSharesLevels(Context& context) {
  Dispatcher dispatcher(context);
  auto& document       = createDocument(context, dispatcher);
  const QString level  = createLevel(dispatcher, document, "Body", 2);
  const QString peg    = createPeg(dispatcher, document, "Rig");
  const auto column    = createColumn(dispatcher, document, level, {1, 2, 1});
  const QString object = columnObject(column);
  result(dispatcher, "object.parent", at(document, parentParams(object, peg)));
  createLayer(dispatcher, document, object, "Bounce");
  const int columns = document.scene->getXsheet()->getColumnCount();
  const auto copied = result(dispatcher, "object.duplicate",
                             at(document, {{"object_id", peg},
                                           {"name", "Rig copy"},
                                           {"scope", "subtree"},
                                           {"level_sharing", "shared"}}));
  const auto copies = copied.value("objects").toArray();
  require(copies.size() == 2, "The subtree copy has the wrong size");
  const auto columnCopy = copies[1].toObject();
  require(columnCopy.value("source_object_id") == object &&
              columnCopy.value("output_connected").toBool(),
          "The column copy is not mapped or not connected to the output");
  TXsheet* sheet = document.scene->getXsheet();
  require(sheet->getColumnCount() == columns + 1,
          "The column copy was not appended");
  for (int row = 0; row < 3; ++row)
    require(sheet->getCell(row, columns).m_level ==
                    sheet->getCell(row, 0).m_level &&
                sheet->getCell(row, columns).m_frameId ==
                    sheet->getCell(row, 0).m_frameId,
            "The column copy does not show the same cells and level");
  TStageObject* copy =
      document.object(columnCopy.value("object_id").toString());
  require(copy->getParent() == document
                                   .object(copied.value("object")
                                               .toObject()
                                               .value("object_id")
                                               .toString())
                                   ->getId(),
          "The column copy is not parented to the pegbar copy");
  require(copy->getOffsetLayerCount() == 1,
          "The column copy lost its offset layer");
  const QString package = savePackage(dispatcher, document, "rig-subtree");
  const auto opened     = result(dispatcher, "scene.open", {{"path", package}});
  auto& reopened = context.document(opened.value("document_id").toString());
  require(reopened.scene->getXsheet()->getColumnCount() == columns + 1,
          "The reopened scene lost the column copy");
  context.removeDocument(reopened.id);
  context.removeDocument(document.id);
}

void renameAndDeleteObjects(Context& context) {
  Dispatcher dispatcher(context);
  auto& document      = createDocument(context, dispatcher);
  const QString root  = createPeg(dispatcher, document, "Root");
  const QString child = createPeg(dispatcher, document, "Child");
  result(dispatcher, "object.parent", at(document, parentParams(child, root)));
  const QString layer = createLayer(dispatcher, document, root, "Sway");
  const QString name  = QString::fromUtf8("Bras gauche \xC3\xA9 \xE2\x9C\x93");
  const auto renamed =
      result(dispatcher, "object.rename",
             at(document, {{"object_id", root}, {"name", name}}));
  require(renamed.value("name") == name, "The rename is not reported");
  rejected(dispatcher, document, "object.rename",
           at(document, {{"object_id", root}, {"name", "Bad\nname"}}),
           "invalid_params");
  const QString table = document.idForObject(
      stageOf(document).getStageObject(TStageObjectId::TableId));
  rejected(dispatcher, document, "object.rename",
           at(document, {{"object_id", table}, {"name", "Table"}}),
           "unsupported_target");
  const QJsonObject refuse{{"object_id", root},
                           {"children_policy", "refuse"},
                           {"report_frames", QJsonArray{0}}};
  const auto refused =
      request(dispatcher, "object.delete", at(document, refuse));
  require(errorCode(refused) == "has_dependents" &&
              errorDetails(refused)
                      .value("dependents")
                      .toArray()[0]
                      .toObject()
                      .value("object_id") == child,
          "Deleting a parent did not list its child");
  PoseLibrary* poses = document.scene->getPoseLibrary();
  poses->setBase({PoseLibrary::Entry(TStageObjectId::PegbarId(0), "x", 0)});
  QJsonObject reattach = refuse;
  reattach.insert("children_policy", "reattach_to_parent");
  rejected(dispatcher, document, "object.delete", at(document, reattach),
           "has_dependents");
  poses->setBase({});
  const auto deleted =
      result(dispatcher, "object.delete", at(document, reattach));
  const auto moved = deleted.value("reattached").toArray()[0].toObject();
  require(moved.value("object_id") == child &&
              moved.value("parent_id") == table &&
              moved.value("world_changes").toArray().size() == 1,
          "The re-attached child is not reported");
  rejected(dispatcher, document, "rig.inspect",
           at(document, {{"root_object_id", root}}), "invalid_handle");
  rejected(dispatcher, document, "offset_layer.update",
           at(document, {{"layer_id", layer}, {"enabled", false}}),
           "invalid_handle");
  const QString package = savePackage(dispatcher, document, "rig-rename");
  const auto opened     = result(dispatcher, "scene.open", {{"path", package}});
  auto& reopened = context.document(opened.value("document_id").toString());
  bool found     = false;
  for (int index = 0; index < stageOf(reopened).getStageObjectCount(); ++index)
    found = found ||
            QString::fromStdString(
                stageOf(reopened).getStageObject(index)->getName()) == "Child";
  require(found && !stageOf(reopened).getStageObject(
                       TStageObjectId::PegbarId(0), false),
          "The deletion did not survive save and reopen");
  context.removeDocument(reopened.id);
  context.removeDocument(document.id);
}

QString reasonOf(const QJsonObject& response) {
  if (errorCode(response) != "has_dependents") return errorCode(response);
  return errorDetails(response).value("reason").toString();
}

// column.delete and camera.remove refuse what object.delete refuses: pose
// entries that name the object and a child on a hook its new parent lacks.
void removalsShareThePreflight(Context& context) {
  Dispatcher dispatcher(context);
  auto& document      = createDocument(context, dispatcher);
  const QString level = createLevel(dispatcher, document, "Part", 1);
  const auto parent   = createColumn(dispatcher, document, level, {1});
  const auto child    = createColumn(dispatcher, document, level, {1});
  result(dispatcher, "hook.set",
         at(document, hookParams(level, 1, {position(1, point(0.5, 0))})));
  result(
      dispatcher, "object.parent",
      at(document, parentParams(columnObject(child), columnObject(parent), 1)));
  const QJsonObject remove{{"column_id", parent.value("column_id")},
                           {"children_policy", "reattach_to_parent"},
                           {"report_frames", QJsonArray{0}}};
  require(reasonOf(request(dispatcher, "column.delete",
                           at(document, remove))) == "hook_handle",
          "A child kept a hook of the deleted column under the table");
  result(dispatcher, "object.handles.set",
         at(document,
            handleParams(columnObject(child), "parent_handle",
                         QJsonObject{{"kind", "peg_hole"}, {"letter", "B"}})));
  PoseLibrary* poses = document.scene->getPoseLibrary();
  const int index =
      document.column(parent.value("column_id").toString())->getIndex();
  poses->setBase({PoseLibrary::Entry(TStageObjectId::ColumnId(index), "x", 0)});
  require(reasonOf(request(dispatcher, "column.delete",
                           at(document, remove))) == "pose_entries",
          "A column named by a pose entry was deleted");
  poses->setBase({});
  const auto deleted =
      result(dispatcher, "column.delete", at(document, remove));
  require(deleted.value("children_reattached").toArray().size() == 1,
          "The re-attached column child is not reported");
  const QString camera = result(dispatcher, "camera.create",
                                at(document, {{"name", "Side"},
                                              {"width_px", 64},
                                              {"height_px", 48},
                                              {"width_in", 4},
                                              {"height_in", 3}}))
                             .value("camera")
                             .toObject()
                             .value("camera_id")
                             .toString();
  const QJsonObject drop{{"camera_id", camera}, {"children_policy", "refuse"}};
  poses->setBase(
      {PoseLibrary::Entry(document.object(camera)->getId(), "x", 0)});
  require(reasonOf(request(dispatcher, "camera.remove", at(document, drop))) ==
              "pose_entries",
          "A camera named by a pose entry was removed");
  poses->setBase({});
  result(dispatcher, "camera.remove", at(document, drop));
  context.removeDocument(document.id);
}

void dependentsOfHooksAndLayers(Context& context) {
  Dispatcher dispatcher(context);
  auto& document     = createDocument(context, dispatcher);
  const QString body = createLevel(dispatcher, document, "Body", 1);
  const QString owner =
      columnObject(createColumn(dispatcher, document, body, {1}));
  result(dispatcher, "hook.set",
         at(document, hookParams(body, 1, {position(1, point(0.2, 0))})));
  const QString child = createPeg(dispatcher, document, "Child");
  const QString arm =
      columnObject(createColumn(dispatcher, document, body, {1}));
  result(dispatcher, "object.parent",
         at(document, parentParams(arm, owner, 1)));
  const auto hook =
      result(dispatcher, "rig.dependents",
             at(document, {{"target", QJsonObject{{"kind", "hook"},
                                                  {"level_id", body},
                                                  {"hook_number", 1}}}}));
  require(
      hook.value("complete").toBool() &&
          hook.value("dependents").toArray().size() == 1 &&
          hook.value("dependents").toArray()[0].toObject().value("object_id") ==
              arm,
      "The hook user is not listed");
  const auto object = result(
      dispatcher, "rig.dependents",
      at(document,
         {{"target", QJsonObject{{"kind", "object"}, {"object_id", owner}}}}));
  require(object.value("dependents").toArray().size() == 2,
          "A column parent lists its child and its hook handle");
  const QString layer = createLayer(dispatcher, document, child, "Lean");
  const auto layered  = result(
      dispatcher, "rig.dependents",
      at(document,
         {{"target", QJsonObject{{"kind", "layer"}, {"layer_id", layer}}}}));
  require(layered.value("dependents").toArray().isEmpty(),
          "A layer without controls has dependents");
  rejected(dispatcher, document, "rig.dependents",
           at(document, {{"target", QJsonObject{{"kind", "control"},
                                                {"control_id", "control_x"}}}}),
           "invalid_handle");
  context.removeDocument(document.id);
}

QJsonObject groupOf(Dispatcher& dispatcher, Document& document, int number) {
  for (const auto& group : result(dispatcher, "object.groups", at(document))
                               .value("groups")
                               .toArray())
    if (group.toObject().value("group_number").toInt() == number)
      return group.toObject();
  return {};
}

void schematicGroups(Context& context) {
  Dispatcher dispatcher(context);
  auto& document  = createDocument(context, dispatcher);
  const QString a = createPeg(dispatcher, document, "Left leg");
  const QString b = createPeg(dispatcher, document, "Right leg");
  const QString c = createPeg(dispatcher, document, "Torso");
  const int legs =
      result(dispatcher, "object.group",
             at(document, {{"object_ids", QJsonArray{a, b}}, {"name", "Legs"}}))
          .value("group")
          .toObject()
          .value("group_number")
          .toInt();
  rejected(dispatcher, document, "object.group",
           at(document, {{"object_ids", QJsonArray{a, c}}, {"name", "Split"}}),
           "invalid_params");
  const int body =
      result(
          dispatcher, "object.group",
          at(document, {{"object_ids", QJsonArray{a, b, c}}, {"name", "Body"}}))
          .value("group")
          .toObject()
          .value("group_number")
          .toInt();
  require(
      groupOf(dispatcher, document, legs).value("parent_group_number") == body,
      "The nested group does not name its enclosing group");
  rejected(dispatcher, document, "object.ungroup",
           at(document, {{"group_number", legs}}), "invalid_params");
  const QString name = QString::fromUtf8("Jambes \xC3\xA9");
  result(dispatcher, "object.group.rename",
         at(document, {{"group_number", legs}, {"name", name}}));
  rejected(dispatcher, document, "object.group.rename",
           at(document, {{"group_number", 999}, {"name", "None"}}),
           "not_found");
  const QString package = savePackage(dispatcher, document, "rig-groups");
  const auto opened     = result(dispatcher, "scene.open", {{"path", package}});
  auto& reopened   = context.document(opened.value("document_id").toString());
  const auto inner = groupOf(dispatcher, reopened, legs);
  require(inner.value("name") == name &&
              inner.value("object_ids").toArray().size() == 2 &&
              groupOf(dispatcher, reopened, body).value("name") == "Body",
          "Groups did not survive save and reopen");
  require(stageOf(reopened).getNewGroupId() > body,
          "Reopened group numbering can reuse a saved group");
  context.removeDocument(reopened.id);
  result(dispatcher, "object.ungroup", at(document, {{"group_number", body}}));
  require(groupOf(dispatcher, document, body).isEmpty() &&
              !groupOf(dispatcher, document, legs).isEmpty(),
          "Dissolving the outer group changed the inner one");
  context.removeDocument(document.id);
}

TAffine placementOf(Document& document, const QString& object, int frame) {
  auto* native = document.object(object);
  for (int index = 0; index < stageOf(document).getStageObjectCount(); ++index)
    stageOf(document).getStageObject(index)->invalidate();
  return native->getPlacement(frame);
}

void samePlacement(const TAffine& left, const TAffine& right,
                   const char* message) {
  near(left.a11, right.a11, message, 1e-9);
  near(left.a12, right.a12, message, 1e-9);
  near(left.a21, right.a21, message, 1e-9);
  near(left.a22, right.a22, message, 1e-9);
  near(left.a13 / Stage::inch, right.a13 / Stage::inch, message, 1e-6);
  near(left.a23 / Stage::inch, right.a23 / Stage::inch, message, 1e-6);
}

QJsonObject worldParams(const QString& child, const QString& parent) {
  auto params = parentParams(child, parent);
  params.insert("placement_policy", "preserve_world");
  return params;
}

void preserveWorldPlacement(Context& context) {
  Dispatcher dispatcher(context);
  auto& document     = createDocument(context, dispatcher);
  const QString from = createPeg(dispatcher, document, "From");
  const QString to   = createPeg(dispatcher, document, "To");
  const QString tilt = createPeg(dispatcher, document, "Tilted");
  const QString walk = createPeg(dispatcher, document, "Walking");
  const QString item = createPeg(dispatcher, document, "Item");
  setKeys(dispatcher, document, from, "x", "inch", {{0, 1}});
  setKeys(dispatcher, document, to, "y", "inch", {{0, -0.5}});
  setKeys(dispatcher, document, tilt, "rotation", "degree", {{0, 30}});
  setKeys(dispatcher, document, tilt, "scale", "scalar", {{0, 2}});
  setKeys(dispatcher, document, walk, "x", "inch", {{0, 0}, {8, 3}});
  result(dispatcher, "object.parent", at(document, parentParams(item, from)));
  setKeys(dispatcher, document, item, "x", "inch", {{0, 0}, {4, 1}});
  setKeys(dispatcher, document, item, "rotation", "degree", {{0, 0}, {4, 90}});
  QList<TAffine> before;
  for (int frame = 0; frame <= 8; ++frame)
    before.append(placementOf(document, item, frame));
  const auto shifted =
      result(dispatcher, "object.parent", at(document, worldParams(item, to)));
  require(shifted.value("world_preservation").toObject().value("method") ==
              "exact_rewrite",
          "A static new parent was not preserved exactly");
  for (int frame = 0; frame <= 8; ++frame)
    samePlacement(placementOf(document, item, frame), before[frame],
                  "Preserve-world moved the object");
  const auto refused = request(dispatcher, "object.parent",
                               at(document, worldParams(item, tilt)));
  require(errorCode(refused) == "unsupported_target" &&
              errorDetails(refused).value("reason") ==
                  "animated_position_in_rotated_frame" &&
              errorDetails(refused).contains("bake_proposal"),
          "A rotated frame with animated position was not refused with a "
          "bake proposal");
  const auto still =
      result(dispatcher, "object.create",
             at(document, {{"name", "Still"}, {"kind", "pegbar"}}))
          .value("object_id")
          .toString();
  setKeys(dispatcher, document, still, "x", "inch", {{0, 0.25}});
  const TAffine stillBefore = placementOf(document, still, 3);
  result(dispatcher, "object.parent", at(document, worldParams(still, tilt)));
  samePlacement(placementOf(document, still, 3), stillBefore,
                "A rotated and scaled static parent moved the object");
  const auto animated = request(dispatcher, "object.parent",
                                at(document, worldParams(item, walk)));
  const auto proposal =
      errorDetails(animated).value("bake_proposal").toObject();
  require(errorCode(animated) == "unsupported_target" &&
              proposal.value("end_frame").toInt() == 8,
          "An animated new parent was not refused with a bake proposal");
  QJsonObject bake = worldParams(item, walk);
  bake.insert("world_policy",
              QJsonObject{{"kind", "bake"},
                          {"start_frame", proposal.value("start_frame")},
                          {"end_frame", proposal.value("end_frame")},
                          {"new_key_interpolation", "linear"}});
  const auto baked = result(dispatcher, "object.parent", at(document, bake));
  require(
      baked.value("world_preservation").toObject().value("method") == "baked",
      "The explicit bake was not applied");
  for (int frame = 0; frame <= 8; ++frame)
    samePlacement(placementOf(document, item, frame), before[frame],
                  "The bake moved the object at an integer frame");
  QJsonObject local = parentParams(item, from);
  local.insert("world_policy", QJsonObject{{"kind", "exact_or_reject"}});
  rejected(dispatcher, document, "object.parent", at(document, local),
           "invalid_params");
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
  hookKeyBudgets(context);
  duplicateIsIndependent(context);
  subtreeDuplicateSharesLevels(context);
  renameAndDeleteObjects(context);
  removalsShareThePreflight(context);
  dependentsOfHooksAndLayers(context);
  schematicGroups(context);
  preserveWorldPlacement(context);
  std::cout << "automation-rig-test: hooks, handles, pivots, layers, object "
               "lifecycle, groups, preserve-world and admission PASS\n";
}

}  // namespace GroundzeroAutomation
