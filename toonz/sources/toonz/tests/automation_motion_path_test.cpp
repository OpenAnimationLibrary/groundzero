#include "automation.h"
#include "automation_dispatch.h"
#include "automation_path.h"

#include "tdoubleparam.h"
#include "tstroke.h"
#include "toonz/stageunits.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectspline.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"

#include <QJsonDocument>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace GroundzeroAutomation {
namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance = 1e-5) {
  require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
          "Motion path native placement differs from the expected value");
}
QJsonObject request(Dispatcher& dispatcher, const QString& method,
                    QJsonObject params) {
  static int sequence = 0;
  if (!params.contains("operation_id"))
    params.insert("operation_id", "path-test-" + QString::number(++sequence));
  if (!method.startsWith("motion_path.") && method != "object.path.attach" &&
      method != "object.path.detach")
    return dispatcher.dispatch(
        {{"id", "fixture"}, {"method", method}, {"params", params}});
  if (method == "motion_path.list" || method == "motion_path.inspect")
    params.remove("operation_id");
  return dispatcher.dispatch(
      {{"id", "fixture"}, {"method", method}, {"params", params}});
}
QJsonObject result(Dispatcher& dispatcher, const QString& method,
                   const QJsonObject& params) {
  const auto response = request(dispatcher, method, params);
  if (!response.contains("result"))
    throw std::runtime_error(
        QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString());
  return response.value("result").toObject();
}
QJsonObject addressed(Document& document, QJsonObject params = {}) {
  params.insert("document_id", document.id);
  params.insert("expected_revision", document.revisionText());
  return params;
}
Document& createDocument(Context& context, Dispatcher& dispatcher) {
  const auto created = result(dispatcher, "scene.create",
                              {{"name", "Path tests"},
                               {"width_px", 32},
                               {"height_px", 32},
                               {"frame_rate", 24}});
  return context.document(created.value("document_id").toString());
}
QJsonArray points(double endX = 2, double endY = 0) {
  return {QJsonObject{{"x", 0}, {"y", 0}},
          QJsonObject{{"x", endX / 2}, {"y", endY / 2}},
          QJsonObject{{"x", endX}, {"y", endY}}};
}
QJsonObject geometry(const QJsonArray& controls) {
  return {{"unit", "inch"},
          {"control_points", controls},
          {"coordinate_policy", "native_roundtrip"},
          {"max_coordinate_error_inch", 1}};
}
QJsonObject createPath(Dispatcher& dispatcher, Document& document,
                       const QJsonArray& controls) {
  auto params = geometry(controls);
  params.insert("name", "Native path");
  return result(dispatcher, "motion_path.create", addressed(document, params));
}
QJsonObject updatePathParams(Document& document, const QString& path,
                             const QJsonArray& controls) {
  auto params = geometry(controls);
  params.insert("path_id", path);
  params.insert("sharing", "all_attached");
  return addressed(document, params);
}
QString peg(Dispatcher& dispatcher, Document& document) {
  return result(dispatcher, "object.create",
                addressed(document, {{"name", "Consumer"}, {"kind", "pegbar"}}))
      .value("object_id")
      .toString();
}
void attach(Dispatcher& dispatcher, Document& document, const QString& object,
            const QString& path, bool aim) {
  result(dispatcher, "object.path.attach",
         addressed(document, {{"object_id", object},
                              {"path_id", path},
                              {"aim", aim},
                              {"position_policy", "use_path_percentage"},
                              {"update_position_keys", false}}));
}
void rejected(Dispatcher& dispatcher, Document& document, const QString& method,
              const QJsonObject& params, const QString& code) {
  const auto revision = document.revision;
  const auto response = request(dispatcher, method, params);
  require(response.value("error").toObject().value("code") == code,
          "Motion path rejection has an unexpected error code");
  require(document.revision == revision,
          "Motion path rejection changed the document revision");
}

void sharedConsumersAndIndependentCopy(Context& context) {
  Dispatcher dispatcher(context);
  auto& document = createDocument(context, dispatcher);
  const QString path =
      createPath(dispatcher, document, points()).value("path_id").toString();
  const QString first  = peg(dispatcher, document),
                second = peg(dispatcher, document);
  auto* a              = document.object(first);
  auto* b              = document.object(second);
  a->getParam(TStageObject::T_Path)->setDefaultValue(25);
  b->getParam(TStageObject::T_Path)->setDefaultValue(75);
  attach(dispatcher, document, first, path, false);
  attach(dispatcher, document, second, path, false);
  near(a->getPlacement(0).a13 / Stage::inch, 0.5);
  near(b->getPlacement(0).a13 / Stage::inch, 1.5);
  const auto before =
      document.paths.value(path)->getStroke()->getControlPoint(2);
  b->lock(true);
  rejected(dispatcher, document, "motion_path.update",
           updatePathParams(document, path, points(0, 4)), "locked");
  require(document.paths.value(path)->getStroke()->getControlPoint(2) == before,
          "Locked shared geometry update changed the path");
  b->lock(false);
  result(dispatcher, "motion_path.update",
         updatePathParams(document, path, points(0, 4)));
  near(a->getPlacement(0).a23 / Stage::inch, 1);
  near(b->getPlacement(0).a23 / Stage::inch, 3);
  attach(dispatcher, document, first, path, true);
  near(a->getPlacement(0).a11, 0);
  near(a->getPlacement(0).a21, 1);
  const auto copied =
      result(dispatcher, "motion_path.duplicate",
             addressed(document, {{"path_id", path}, {"name", "Independent"}}));
  const QString copy = copied.value("path_id").toString();
  require(copied.value("consumers").toArray().isEmpty(),
          "Duplicate unexpectedly retained consumers");
  result(dispatcher, "motion_path.update",
         updatePathParams(document, copy, points(6, 0)));
  attach(dispatcher, document, second, copy, false);
  near(b->getPlacement(0).a13 / Stage::inch, 4.5);
  near(b->getPlacement(0).a23, 0);
  result(dispatcher, "motion_path.update",
         updatePathParams(document, path, points(6, 0)));
  near(b->getPlacement(0).a13 / Stage::inch, 4.5);
  rejected(dispatcher, document, "motion_path.delete",
           addressed(document, {{"path_id", path}}), "invalid_params");
  result(dispatcher, "object.path.detach",
         addressed(document, {{"object_id", first},
                              {"position_policy", "restore_existing_xy"}}));
  near(a->getPlacement(0).a13, 0);
  near(a->getParam(TStageObject::T_Path)->getDefaultValue(), 25);
  result(dispatcher, "motion_path.delete",
         addressed(document, {{"path_id", path}}));
  rejected(dispatcher, document, "motion_path.inspect",
           addressed(document, {{"path_id", path}}), "invalid_handle");
  context.removeDocument(document.id);
}

void canonicalGeometryAndDegeneracy(Context& context) {
  Dispatcher dispatcher(context);
  auto& document = createDocument(context, dispatcher);
  auto precise   = geometry(points(1.234567891, 0.123456789));
  precise.insert("name", "Precision");
  precise.insert("max_coordinate_error_inch", 0);
  rejected(dispatcher, document, "motion_path.create",
           addressed(document, precise), "invalid_params");
  require(document.paths.isEmpty(), "Rejected precision created a path handle");
  const QJsonArray bad[] = {
      points(0, 0),
      {QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 0}, {"y", 0}},
       QJsonObject{{"x", 2}, {"y", 0}}},
      {QJsonObject{{"x", 0}, {"y", 0}}, QJsonObject{{"x", 2}, {"y", 0}},
       QJsonObject{{"x", 1}, {"y", 0}}},
      {QJsonObject{{"x", 999999.9}, {"y", 0}},
       QJsonObject{{"x", 999999.95}, {"y", 0}},
       QJsonObject{{"x", 1000000}, {"y", 0}}}};
  for (const auto& controls : bad) {
    auto params = geometry(controls);
    params.insert("name", "Rejected");
    rejected(dispatcher, document, "motion_path.create",
             addressed(document, params), "invalid_params");
    require(document.paths.isEmpty(), "Degenerate geometry published a path");
  }
  const auto created =
      createPath(dispatcher, document, points(1.234567891, 0.123456789));
  const auto revision = document.revision;
  const auto repeated =
      result(dispatcher, "motion_path.update",
             updatePathParams(document, created.value("path_id").toString(),
                              created.value("control_points").toArray()));
  require(document.revision == revision && repeated.value("control_points") ==
                                               created.value("control_points"),
          "Canonical inspected geometry was not a stable no-op");
  context.removeDocument(document.id);
}

void nativeLockAndPercentageBoundaries(Context& context) {
  Dispatcher dispatcher(context);
  auto& document = createDocument(context, dispatcher);
  const QString path =
      createPath(dispatcher, document, points()).value("path_id").toString();
  const auto column    = result(dispatcher, "column.create",
                                addressed(document, {{"name", "Locked"}}));
  const QString object = column.value("object_id").toString();
  attach(dispatcher, document, object, path, false);
  auto* sheet  = document.scene->getXsheet();
  auto* native = document.object(object);
  sheet->getColumn(0)->lock(true);
  rejected(dispatcher, document, "motion_path.update",
           updatePathParams(document, path, points(0, 4)), "locked");
  sheet->getColumn(0)->lock(false);
  native->getParam(TStageObject::T_Path)->setEditorLocked(true);
  rejected(dispatcher, document, "motion_path.update",
           updatePathParams(document, path, points(0, 4)), "locked");
  native->getParam(TStageObject::T_Path)->setEditorLocked(false);
  auto* camera = sheet->getStageObjectTree()->getStageObject(
      sheet->getStageObjectTree()->getCurrentCameraId());
  const QString cameraId = document.idForObject(camera);
  attach(dispatcher, document, cameraId, path, false);
  sheet->setCameraColumnLocked(true);
  rejected(dispatcher, document, "motion_path.update",
           updatePathParams(document, path, points(0, 4)), "locked");
  sheet->setCameraColumnLocked(false);
  native->getParam(TStageObject::T_Path)->setDefaultValue(101);
  rejected(dispatcher, document, "motion_path.inspect",
           addressed(document, {{"path_id", path}}), "invalid_params");
  native->getParam(TStageObject::T_Path)->setDefaultValue(0);
  native->enableUppk(true);
  rejected(dispatcher, document, "motion_path.inspect",
           addressed(document, {{"path_id", path}}), "invalid_params");
  native->enableUppk(false);
  context.removeDocument(document.id);
}

void hierarchyAndPathCapacity(Context& context) {
  Dispatcher dispatcher(context);
  auto& document = createDocument(context, dispatcher);
  auto* stage    = document.scene->getXsheet()->getStageObjectTree();
  auto parent    = TStageObjectId::TableId;
  for (int index = 0; index < 64; ++index) {
    auto* object = stage->getStageObject(TStageObjectId::PegbarId(index));
    object->setParent(parent);
    parent = object->getId();
  }
  const auto path = createPath(dispatcher, document, points());
  auto* extra     = stage->getStageObject(TStageObjectId::PegbarId(64));
  bool refused    = false;
  try {
    validateAutomationPathParent(*document.scene, extra,
                                 stage->getStageObject(parent));
  } catch (const Error&) {
    refused = true;
  }
  require(refused && extra->getParent() == TStageObjectId::TableId,
          "Depth65 prospective parent was accepted or mutated");
  for (int index = 1; index < 128; ++index)
    createPath(dispatcher, document, points());
  auto params = geometry(points());
  params.insert("name", "Too many");
  rejected(dispatcher, document, "motion_path.create",
           addressed(document, params), "resource_limit");
  require(document.paths.size() == 128,
          "Path capacity rejection changed owned handles");
  context.removeDocument(document.id);
}
QJsonArray manyPoints(int count) {
  QJsonArray result;
  for (int index = 0; index < count; ++index)
    result.append(QJsonObject{{"x", index / 120.0}, {"y", 0}});
  return result;
}

void aggregateGeometryCapacity(Context& context) {
  Dispatcher dispatcher(context);
  auto& document = createDocument(context, dispatcher);
  for (int index = 0; index < 15; ++index)
    createPath(dispatcher, document, manyPoints(4097));
  createPath(dispatcher, document, manyPoints(4081));
  require(document.paths.size() == 16,
          "Exact 65536-point aggregate was refused");
  auto params = geometry(points());
  params.insert("name", "Excess points");
  rejected(dispatcher, document, "motion_path.create",
           addressed(document, params), "resource_limit");
  require(document.paths.size() == 16,
          "Aggregate point rejection published a path handle");
  params = geometry(manyPoints(4099));
  params.insert("name", "Excess points per path");
  rejected(dispatcher, document, "motion_path.create",
           addressed(document, params), "invalid_params");
  context.removeDocument(document.id);
}

}  // namespace

void runAutomationPathTests(Context& context) {
  canonicalGeometryAndDegeneracy(context);
  sharedConsumersAndIndependentCopy(context);
  nativeLockAndPercentageBoundaries(context);
  hierarchyAndPathCapacity(context);
  aggregateGeometryCapacity(context);
  std::cout << "automation-motion-path-test: geometry, ownership, locks and "
               "bounds PASS\n";
}
}  // namespace GroundzeroAutomation
