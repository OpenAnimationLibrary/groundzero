#include "automation_path.h"
#include "automation.h"
#include "automation_animation.h"

#include "tdoubleparam.h"
#include "tstroke.h"
#include "tstream.h"
#include "toonz/toonzscene.h"
#include "toonz/stageunits.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectspline.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace GroundzeroAutomation {
namespace {
constexpr int MaxPaths           = 128;
constexpr int MaxPoints          = 4097;
constexpr int MaxTotalPoints     = 65536;
constexpr int MaxObjects         = 4096;
constexpr int MaxDepth           = 64;
constexpr int MaxInspectionBytes = 1024 * 1024;
constexpr double MaxCoordinate   = 1000000;
using Points                     = std::vector<TThickPoint>;
using Consumers                  = std::vector<TStageObject*>;

[[noreturn]] void invalid(const QString& message) {
  throw Error("invalid_params", message);
}

TStageObjectTree* tree(ToonzScene& scene) {
  return scene.getXsheet()->getStageObjectTree();
}

void requireCounts(TStageObjectTree& stage) {
  if (stage.getSplineCount() > MaxPaths ||
      stage.getStageObjectCount() > MaxObjects)
    throw Error(
        "resource_limit",
        "Motion paths require at most 128 paths and 4096 stage objects.");
}

void validateParents(TStageObjectTree& stage) {
  requireCounts(stage);
  for (int index = 0; index < stage.getStageObjectCount(); ++index) {
    auto* object = stage.getStageObject(index);
    std::set<TStageObjectId> seen;
    int depth = 0;
    while (object) {
      if (!seen.insert(object->getId()).second)
        invalid("Motion path scene contains a parent cycle.");
      const auto parent = object->getParent();
      if (parent == TStageObjectId::NoneId) break;
      if (++depth > MaxDepth)
        invalid("Motion path scenes allow at most 64 parent edges.");
      object = stage.getStageObject(parent, false);
      if (!object) invalid("Motion path scene contains an unknown parent.");
    }
  }
}

void validateName(const QString& name) {
  if (name.isEmpty() || name.size() > 512 || name.toUcs4().size() > 256)
    invalid("Motion path name requires between 1 and 256 Unicode characters.");
  if (name.contains(QChar::Null))
    invalid("Motion path names cannot contain a null character.");
}

bool regularSegment(const TPointD& start, const TPointD& middle,
                    const TPointD& end) {
  const TPointD first      = middle - start;
  const TPointD last       = end - middle;
  const TPointD delta      = last - first;
  const double denominator = delta.x * delta.x + delta.y * delta.y;
  const double parameter =
      denominator == 0
          ? 0
          : std::clamp(-(first.x * delta.x + first.y * delta.y) / denominator,
                       0.0, 1.0);
  const TPointD minimum = first + parameter * delta;
  const double maximum =
      2 * std::max(std::hypot(first.x, first.y), std::hypot(last.x, last.y));
  return 2 * std::hypot(minimum.x, minimum.y) >=
         std::max(Stage::inch * 1e-6, maximum * 1e-6);
}

void validatePoint(const TThickPoint& point) {
  const double limit = MaxCoordinate * Stage::inch;
  if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
      std::abs(point.x) > limit || std::abs(point.y) > limit ||
      point.thick != 0)
    invalid(
        "Motion path coordinates must be finite bounded inches with zero "
        "native thickness.");
}

bool validPointCount(std::size_t count) {
  return count >= 3 && count <= MaxPoints && count % 2 == 1;
}

void validatePoints(const Points& points) {
  if (!validPointCount(points.size()))
    invalid(
        "Motion paths require an odd count of 3 through 4097 quadratic control "
        "points.");
  for (const auto& point : points) validatePoint(point);
  if (points.front().x == points.back().x &&
      points.front().y == points.back().y)
    invalid("Motion paths must have distinct open endpoints.");
  for (std::size_t index = 0; index + 2 < points.size(); index += 2)
    if (!regularSegment(points[index], points[index + 1], points[index + 2]))
      invalid(
          "A path span is degenerate or has a zero or ill-conditioned "
          "tangent.");
}

Points nativePoints(const TStageObjectSpline& path) {
  const TStroke* stroke = path.getStroke();
  if (!stroke || stroke->isSelfLoop())
    invalid("Motion paths require a native open stroke.");
  const int count = stroke->getControlPointCount();
  if (count < 3 || count > MaxPoints)
    throw Error(
        "resource_limit",
        "Native motion path control point count is outside 3 through 4097.");
  Points result;
  result.reserve(count);
  for (int index = 0; index < count; ++index)
    result.push_back(stroke->getControlPoint(index));
  validatePoints(result);
  return result;
}

int totalPoints(TStageObjectTree& stage) {
  requireCounts(stage);
  int total = 0;
  for (int index = 0; index < stage.getSplineCount(); ++index) {
    total += static_cast<int>(nativePoints(*stage.getSpline(index)).size());
    if (total > MaxTotalPoints)
      throw Error("resource_limit",
                  "Motion paths exceed 65536 total control points.");
  }
  return total;
}

void requireGrowth(TStageObjectTree& stage, int addedPaths, int addedPoints) {
  const int count = totalPoints(stage);
  if (stage.getSplineCount() + addedPaths > MaxPaths ||
      count + addedPoints > MaxTotalPoints)
    throw Error(
        "resource_limit",
        "Motion path growth exceeds 128 paths or 65536 control points.");
}

Consumers consumers(TStageObjectTree& stage, const TStageObjectSpline* path) {
  requireCounts(stage);
  Consumers result;
  for (int index = 0; index < stage.getStageObjectCount(); ++index) {
    auto* object = stage.getStageObject(index);
    if (object->getSpline() == path) result.push_back(object);
  }
  std::sort(result.begin(), result.end(),
            [](const auto* left, const auto* right) {
              return left->getId() < right->getId();
            });
  return result;
}

void validateConsumer(TStageObjectTree& stage, TStageObject& object) {
  const auto status = object.getStatus();
  if (status != TStageObject::PATH && status != TStageObject::PATH_AIM)
    invalid("A path consumer must use PATH or PATH_AIM without UPPK.");
  if (object.getId().isTable() || object.getOffsetLayerCount() != 0)
    invalid("Table objects and offset layers cannot consume motion paths.");
  if (!stage.containsSpline(object.getSpline()))
    invalid("A motion path consumer references an unowned spline.");
  validateAutomationPathPosition(*object.getParam(TStageObject::T_Path));
}

void validateStageModes(TStageObjectTree& stage) {
  for (int index = 0; index < stage.getStageObjectCount(); ++index) {
    auto* object = stage.getStageObject(index);
    if (object->getSpline())
      validateConsumer(stage, *object);
    else if (object->getStatus() != TStageObject::XY)
      invalid(
          "Motion path scenes require ordinary XY mode for unattached "
          "objects.");
  }
}

void requireEditable(ToonzScene& scene, TStageObject& object) {
  bool locked    = object.isLocked() ||
                   object.getParam(TStageObject::T_Path)->isEditorLocked();
  TXsheet* sheet = scene.getXsheet();
  if (object.getId().isColumn()) {
    auto* column = sheet->getColumn(object.getId().getIndex());
    locked       = locked || (column && column->isLocked());
  }
  if (object.getId().isCamera())
    locked = locked || sheet->isCameraColumnLocked();
  if (locked)
    throw Error("locked",
                "Unlock every affected object, column and path channel first.");
}

void requireTarget(ToonzScene& scene, TStageObject& object,
                   bool writable = true) {
  if (writable) requireEditable(scene, object);
  if (object.getId().isTable() || object.getOffsetLayerCount() != 0)
    invalid("This stage object cannot use the detached motion path profile.");
  const auto status = object.getStatus();
  if (status != TStageObject::XY && status != TStageObject::PATH &&
      status != TStageObject::PATH_AIM)
    invalid("Motion paths cannot replace IK, UPPK or unsupported stage modes.");
  validateAutomationPathPosition(*object.getParam(TStageObject::T_Path));
}

void invalidateConsumers(TStageObjectTree& stage, const Consumers& affected) {
  std::set<TStageObjectId> ids;
  for (auto* object : affected) ids.insert(object->getId());
  for (auto* object : affected) {
    auto parent  = object->getParent();
    bool covered = false;
    for (int depth = 0; parent != TStageObjectId::NoneId && depth < MaxDepth;
         ++depth) {
      if (ids.count(parent)) {
        covered = true;
        break;
      }
      parent = stage.getStageObject(parent, false)->getParent();
    }
    if (!covered) object->invalidate();
  }
}

QString registerPath(Document& document, TStageObjectSpline* path) {
  auto found = document.pathIds.constFind(path);
  if (found != document.pathIds.constEnd()) return found.value();
  const QString id =
      "path_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  document.paths.insert(id, path);
  document.pathIds.insert(path, id);
  return id;
}

TStageObjectSpline* getPath(Document& document, const QJsonObject& params) {
  const auto found =
      document.paths.constFind(requiredString(params, "path_id"));
  auto& stage = *tree(*document.scene);
  if (found != document.paths.constEnd()) {
    for (int index = 0; index < stage.getSplineCount(); ++index)
      if (stage.getSpline(index) == found.value()) return found.value();
  }
  throw Error(
      "invalid_handle",
      "The path handle is unknown, removed or belongs to another document.");
}

Document& currentDocument(Context& context, const QJsonObject& params) {
  auto& document = context.document(requiredString(params, "document_id"));
  if (document.revisionText() != requiredString(params, "expected_revision"))
    throw Error("revision_conflict",
                "The document changed; inspect its current revision first.");
  validateAutomationPaths(*document.scene);
  return document;
}

QJsonArray pointsJson(const Points& points) {
  QJsonArray result;
  for (const auto& point : points)
    result.append(QJsonObject{{"x", point.x / Stage::inch},
                              {"y", point.y / Stage::inch}});
  return result;
}

QJsonArray consumersJson(Document& document, const Consumers& attached) {
  QHash<TStageObject*, QString> handles;
  for (auto entry = document.objects.constBegin();
       entry != document.objects.constEnd(); ++entry)
    handles.insert(entry.value(), entry.key());
  QJsonArray result;
  for (auto* object : attached) {
    const QString id = handles.value(object);
    if (id.isEmpty())
      throw Error("invalid_handle",
                  "A native path consumer has no published handle.");
    result.append(QJsonObject{{"object_id", id},
                              {"aim", object->isAimEnabled()},
                              {"update_position_keys", false}});
  }
  return result;
}

void requireResponseBytes(const QJsonObject& response, int maximum) {
  if (QJsonDocument(response).toJson(QJsonDocument::Compact).size() + 1024 >
      maximum)
    throw Error(
        "resource_limit",
        "Motion path inspection exceeds its bounded serialized response.");
}

QJsonObject inspectPath(Document& document, TStageObjectSpline& path) {
  const auto attached = consumers(*tree(*document.scene), &path);
  QJsonObject result{{"document_id", document.id},
                     {"revision", document.revisionText()},
                     {"path_id", document.pathIds.value(&path)},
                     {"name", QString::fromStdString(path.getName())},
                     {"unit", "inch"},
                     {"control_points", pointsJson(nativePoints(path))},
                     {"consumers", consumersJson(document, attached)},
                     {"closed", false},
                     {"coordinate_policy", "native_roundtrip"}};
  requireResponseBytes(result, MaxInspectionBytes);
  return result;
}

Points inputPoints(const QJsonObject& params) {
  const auto records = params.value("control_points").toArray();
  if (records.size() < 3 || records.size() > MaxPoints ||
      records.size() % 2 != 1)
    invalid("control_points requires an odd count from 3 through 4097.");
  Points result;
  result.reserve(records.size());
  for (const auto& record : records) {
    const auto point = record.toObject();
    if (!point.value("x").isDouble() || !point.value("y").isDouble())
      invalid("Every path control point requires finite numeric x and y.");
    result.emplace_back(point.value("x").toDouble() * Stage::inch,
                        point.value("y").toDouble() * Stage::inch, 0);
  }
  validatePoints(result);
  return result;
}

TStageObjectSplineP roundTrip(TStageObjectSpline& source,
                              const QString& filename) {
  const TFilePath path(filename);
  {
    TOStream output(path);
    source.saveData(output);
  }
  if (QFileInfo(filename).size() > MaxInspectionBytes)
    throw Error("resource_limit",
                "Private native path serialization exceeds 1 MiB.");
  TStageObjectSplineP result(new TStageObjectSpline());
  {
    TIStream input(path);
    input.setVersion(VersionNumber(71, 1));
    result->loadData(input);
  }
  return result;
}

struct Geometry final {
  Points points;
  double error = 0;
};

Geometry canonicalGeometry(Context& context, const Points& original,
                           double maximumError) {
  if (!std::isfinite(maximumError) || maximumError < 0 || maximumError > 1)
    invalid("max_coordinate_error_inch must be between 0 and 1.");
  QTemporaryDir directory(context.scratchDirectory() +
                          "/path-canonical-XXXXXX");
  if (!directory.isValid())
    throw Error("io_error", "Cannot create private path preparation storage.");
  TStageObjectSplineP source(new TStageObjectSpline());
  source->setStroke(new TStroke(original));
  const auto first  = roundTrip(*source, directory.filePath("first.xml"));
  const auto second = roundTrip(*first, directory.filePath("second.xml"));
  Geometry result{nativePoints(*first), 0};
  if (result.points != nativePoints(*second))
    invalid("Native motion path coordinates are not stable.");
  for (std::size_t index = 0; index < original.size(); ++index) {
    const auto delta = result.points[index] - original[index];
    result.error =
        std::max(result.error, std::hypot(delta.x, delta.y) / Stage::inch);
  }
  if (result.error > maximumError)
    throw Error(
        "invalid_params",
        "Native path precision exceeds the caller's coordinate error limit.",
        {{"max_input_error_inch", result.error}, {"maximum", maximumError}});
  return result;
}

QJsonValue createPath(Context& context, const QJsonObject& params) {
  auto& document = currentDocument(context, params);
  auto& stage    = *tree(*document.scene);
  validateParents(stage);
  validateStageModes(stage);
  const QString name = requiredString(params, "name");
  validateName(name);
  const auto input = inputPoints(params);
  requireGrowth(stage, 1, static_cast<int>(input.size()));
  const auto geometry = canonicalGeometry(
      context, input, params.value("max_coordinate_error_inch").toDouble());
  auto stroke = std::make_unique<TStroke>(geometry.points);
  context.markMutationStarted();
  auto* path = stage.createSpline();
  path->setName(name.toStdString());
  path->setStroke(stroke.release());
  registerPath(document, path);
  auto result = inspectPath(document, *path);
  result.insert("max_input_error_inch", geometry.error);
  return result;
}

QJsonValue listPaths(Context& context, const QJsonObject& params) {
  auto& document = currentDocument(context, params);
  auto& stage    = *tree(*document.scene);
  QJsonArray paths;
  for (int index = 0; index < stage.getSplineCount(); ++index) {
    auto* path = stage.getSpline(index);
    paths.append(QJsonObject{
        {"path_id", document.pathIds.value(path)},
        {"name", QString::fromStdString(path->getName())},
        {"control_point_count", path->getStroke()->getControlPointCount()},
        {"consumer_count", static_cast<int>(consumers(stage, path).size())}});
  }
  QJsonObject result{{"document_id", document.id},
                     {"revision", document.revisionText()},
                     {"paths", paths}};
  requireResponseBytes(result, 128 * 1024);
  return result;
}

QJsonValue readPath(Context& context, const QJsonObject& params) {
  auto& document = currentDocument(context, params);
  return inspectPath(document, *getPath(document, params));
}

QJsonValue updatePath(Context& context, const QJsonObject& params) {
  auto& document      = currentDocument(context, params);
  auto& stage         = *tree(*document.scene);
  auto* path          = getPath(document, params);
  const auto attached = consumers(stage, path);
  for (auto* object : attached) requireEditable(*document.scene, *object);
  const auto input = inputPoints(params);
  requireGrowth(stage, 0,
                static_cast<int>(input.size()) -
                    path->getStroke()->getControlPointCount());
  const auto geometry = canonicalGeometry(
      context, input, params.value("max_coordinate_error_inch").toDouble());
  if (geometry.points != nativePoints(*path)) {
    auto stroke = std::make_unique<TStroke>(geometry.points);
    context.markMutationStarted();
    path->setStroke(stroke.release());
    invalidateConsumers(stage, attached);
  }
  auto result = inspectPath(document, *path);
  result.insert("max_input_error_inch", geometry.error);
  return result;
}

QJsonValue renamePath(Context& context, const QJsonObject& params) {
  auto& document     = currentDocument(context, params);
  auto* path         = getPath(document, params);
  const QString name = requiredString(params, "name");
  validateName(name);
  if (name.toStdString() != path->getName()) {
    context.markMutationStarted();
    path->setName(name.toStdString());
  }
  return inspectPath(document, *path);
}

QJsonValue duplicatePath(Context& context, const QJsonObject& params) {
  auto& document     = currentDocument(context, params);
  auto& stage        = *tree(*document.scene);
  auto* source       = getPath(document, params);
  const QString name = requiredString(params, "name");
  validateName(name);
  auto points = nativePoints(*source);
  requireGrowth(stage, 1, static_cast<int>(points.size()));
  auto stroke = std::make_unique<TStroke>(points);
  context.markMutationStarted();
  auto* path = stage.createSpline();
  path->setName(name.toStdString());
  path->setStroke(stroke.release());
  registerPath(document, path);
  return inspectPath(document, *path);
}

QJsonValue deletePath(Context& context, const QJsonObject& params) {
  auto& document = currentDocument(context, params);
  auto& stage    = *tree(*document.scene);
  auto* path     = getPath(document, params);
  if (!consumers(stage, path).empty())
    invalid("Detach every consumer before deleting a path.");
  const QString id = document.pathIds.value(path);
  context.markMutationStarted();
  document.pathIds.remove(path);
  document.paths.remove(id);
  stage.removeSpline(path);
  return QJsonObject{{"path_id", id}, {"deleted", true}};
}

QJsonValue attachPath(Context& context, const QJsonObject& params) {
  auto& document = currentDocument(context, params);
  auto* path     = getPath(document, params);
  auto* object   = document.object(requiredString(params, "object_id"));
  requireTarget(*document.scene, *object);
  const bool aim = params.value("aim").toBool();
  if (object->getSpline() != path || object->isAimEnabled() != aim) {
    context.markMutationStarted();
    object->doSetSpline(path);
    object->enableAim(aim);
    object->invalidate();
  }
  return QJsonObject{{"object_id", params.value("object_id")},
                     {"path_id", params.value("path_id")},
                     {"aim", aim},
                     {"position_policy", "use_path_percentage"},
                     {"update_position_keys", false}};
}

QJsonValue detachPath(Context& context, const QJsonObject& params) {
  auto& document = currentDocument(context, params);
  auto* object   = document.object(requiredString(params, "object_id"));
  requireTarget(*document.scene, *object);
  if (object->getSpline()) {
    context.markMutationStarted();
    object->doSetSpline(nullptr);
    object->invalidate();
  }
  return QJsonObject{{"object_id", params.value("object_id")},
                     {"path_id", QJsonValue()},
                     {"position_policy", "restore_existing_xy"}};
}

QJsonObject handleSchema() {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
}
QJsonObject nameSchema() { return boundedNameSchema(256); }
QJsonObject objectSchema(const QJsonObject& fields) {
  QJsonArray required;
  for (auto field = fields.begin(); field != fields.end(); ++field)
    required.append(field.key());
  return {{"type", "object"},
          {"properties", fields},
          {"required", required},
          {"additionalProperties", false}};
}
QJsonObject geometrySchema() {
  const QJsonObject coordinate{{"type", "number"},
                               {"minimum", -MaxCoordinate},
                               {"maximum", MaxCoordinate}};
  return {{"unit", QJsonObject{{"const", "inch"}}},
          {"control_points",
           QJsonObject{{"type", "array"},
                       {"minItems", 3},
                       {"maxItems", MaxPoints},
                       {"items",
                        objectSchema({{"x", coordinate}, {"y", coordinate}})}}},
          {"coordinate_policy", QJsonObject{{"const", "native_roundtrip"}}},
          {"max_coordinate_error_inch",
           QJsonObject{{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}};
}
QString toolDescription(const QString& name) {
  static const std::map<QString, QString> descriptions{
      {"motion_path.create",
       "Create an open quadratic path with explicit native precision "
       "tolerance."},
      {"motion_path.list",
       "List owned path summaries at the expected document revision."},
      {"motion_path.inspect",
       "Inspect canonical inch coordinates and all native path consumers."},
      {"motion_path.update",
       "Replace shared geometry after checking every consumer lock; preserve "
       "percentage keys."},
      {"motion_path.rename",
       "Rename an owned motion path without changing its geometry or "
       "consumers."},
      {"motion_path.duplicate",
       "Create an independent path copy with no attached consumers."},
      {"motion_path.delete",
       "Delete an unused path and retire its opaque handle."},
      {"object.path.attach",
       "Use stored path percentage and optional tangent aim; preserve existing "
       "XY keys."},
      {"object.path.detach",
       "Restore existing XY curves without baking world placement or changing "
       "percentage keys."}};
  return descriptions.at(name);
}

void addTool(QJsonArray& catalog, const QString& name, QJsonObject fields,
             bool writes) {
  fields.insert("document_id", handleSchema());
  fields.insert("expected_revision", handleSchema());
  if (writes) fields.insert("operation_id", handleSchema());
  auto schema = objectSchema(fields);
  auto tool   = operation(name, toolDescription(name), {}, {}, writes);
  tool.insert("inputSchema", schema);
  catalog.append(tool);
}
}  // namespace

void validatePositionKey(const TDoubleKeyframe& key) {
  if (key.m_type != TDoubleKeyframe::Constant &&
      key.m_type != TDoubleKeyframe::Linear)
    invalid(
        "Path percentage keys support only constant or linear interpolation.");
  if (key.m_step != 1 || key.m_value < 0 || key.m_value > 100)
    invalid(
        "Path percentage keys require continuous step 1 and values from 0 "
        "through 100.");
}

void validateAutomationPathPosition(const TDoubleParam& curve) {
  requireSupportedAnimationCurve(curve);
  const double value = curve.getDefaultValue();
  if (!std::isfinite(value) || value < 0 || value > 100)
    invalid(
        "Path position default must be a finite percentage from 0 through "
        "100.");
  for (int index = 0; index < curve.getKeyframeCount(); ++index)
    validatePositionKey(curve.getKeyframe(index));
}

void requireAutomationPathTarget(ToonzScene& scene, TStageObject& object,
                                 bool writable) {
  requireTarget(scene, object, writable);
  if (writable && tree(scene)->getSplineCount() != 0)
    validateParents(*tree(scene));
}

void requireAutomationPathObjectGrowth(ToonzScene& scene) {
  auto& stage = *tree(scene);
  if (stage.getSplineCount() != 0 && stage.getStageObjectCount() >= MaxObjects)
    throw Error("resource_limit",
                "Motion path scenes allow at most 4096 stage objects.");
}

void validateAutomationPaths(ToonzScene& scene) {
  auto& stage = *tree(scene);
  if (stage.getSplineCount() == 0) return;
  requireCounts(stage);
  validateParents(stage);
  totalPoints(stage);
  for (int index = 0; index < stage.getSplineCount(); ++index)
    validateName(QString::fromStdString(stage.getSpline(index)->getName()));
  validateStageModes(stage);
}

void initializeAutomationPaths(Document& document) {
  validateAutomationPaths(*document.scene);
  auto& stage = *tree(*document.scene);
  for (int index = 0; index < stage.getSplineCount(); ++index) {
    auto* path = stage.getSpline(index);
    registerPath(document, path);
    for (auto* object : consumers(stage, path)) document.idForObject(object);
  }
}

QJsonArray pathOperations() {
  QJsonArray catalog;
  auto geometry = geometrySchema();
  geometry.insert("name", nameSchema());
  addTool(catalog, "motion_path.create", geometry, true);
  addTool(catalog, "motion_path.list", {}, false);
  addTool(catalog, "motion_path.inspect", {{"path_id", handleSchema()}}, false);
  geometry = geometrySchema();
  geometry.insert("path_id", handleSchema());
  geometry.insert("sharing", QJsonObject{{"const", "all_attached"}});
  addTool(catalog, "motion_path.update", geometry, true);
  const QJsonObject named{{"path_id", handleSchema()}, {"name", nameSchema()}};
  addTool(catalog, "motion_path.rename", named, true);
  addTool(catalog, "motion_path.duplicate", named, true);
  addTool(catalog, "motion_path.delete", {{"path_id", handleSchema()}}, true);
  addTool(catalog, "object.path.attach",
          {{"object_id", handleSchema()},
           {"path_id", handleSchema()},
           {"aim", QJsonObject{{"type", "boolean"}}},
           {"position_policy", QJsonObject{{"const", "use_path_percentage"}}},
           {"update_position_keys", QJsonObject{{"const", false}}}},
          true);
  addTool(catalog, "object.path.detach",
          {{"object_id", handleSchema()},
           {"position_policy", QJsonObject{{"const", "restore_existing_xy"}}}},
          true);
  return catalog;
}

QJsonValue executePath(Context& context, const QString& method,
                       const QJsonObject& params) {
  static const std::map<QString, QJsonValue (*)(Context&, const QJsonObject&)>
      handlers{{"motion_path.create", createPath},
               {"motion_path.list", listPaths},
               {"motion_path.inspect", readPath},
               {"motion_path.update", updatePath},
               {"motion_path.rename", renamePath},
               {"motion_path.duplicate", duplicatePath},
               {"motion_path.delete", deletePath},
               {"object.path.attach", attachPath},
               {"object.path.detach", detachPath}};
  auto found = handlers.find(method);
  if (found == handlers.end())
    throw Error("unknown_method", "Unknown detached motion path operation.");
  return found->second(context, params);
}
}  // namespace GroundzeroAutomation
