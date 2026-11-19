#include "automation_path.h"
#include "automation_rig.h"
#include "automation_scene_host.h"
#include "automation_scene_shared.h"

#include "tdoubleparam.h"
#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"

#include <QRegularExpression>

#include <algorithm>
#include <memory>
#include <vector>

namespace GroundzeroAutomation {
namespace SceneShared {

QJsonObject describeCamera(Document& document) {
  TStageObjectTree* objects = tree(document);
  TCamera* camera           = objects->getCurrentCamera();
  TStageObject* object =
      objects->getStageObject(objects->getCurrentCameraId(), false);
  return {{"object_id", document.idForObject(object)},
          {"width_px", camera->getRes().lx},
          {"height_px", camera->getRes().ly},
          {"width_in", camera->getSize().lx},
          {"height_in", camera->getSize().ly}};
}

namespace {

constexpr int MaxCameras = 64;
constexpr int MaxListed  = 64;

void requireTopSheet(const QJsonObject& params) {
  if (!params.value("sheet_path").toArray().isEmpty())
    throw Error("unsupported_target",
                "Camera tools address the top sheet; nested sheets are not "
                "editable yet.",
                {{"reason", "nested_sheet_editing"}});
}

std::vector<TStageObject*> cameras(Document& document) {
  std::vector<TStageObject*> result;
  for (int index = 0; index < tree(document)->getStageObjectCount(); ++index)
    if (tree(document)->getStageObject(index)->getId().isCamera())
      result.push_back(tree(document)->getStageObject(index));
  std::sort(result.begin(), result.end(),
            [](TStageObject* left, TStageObject* right) {
              return left->getId().getIndex() < right->getId().getIndex();
            });
  return result;
}

// Reads never create a stage object; native getStageObject(id) would.
TStageObject* cameraById(Document& document, const TStageObjectId& id) {
  return tree(document)->getStageObject(id, false);
}

TStageObject* readCamera(Document& document, const QString& handle) {
  TStageObject* object = document.object(handle);
  if (!object->getId().isCamera())
    throw Error("invalid_handle", "The handle does not name a camera.",
                {{"camera_id", handle}});
  return object;
}

QJsonValue interestRecord(TCamera& camera) {
  const TRect rect = camera.getInterestRect();
  if (rect.isEmpty()) return QJsonValue();
  return QJsonObject{{"x", rect.x0},
                     {"y", rect.y0},
                     {"width", rect.getLx()},
                     {"height", rect.getLy()}};
}

int animatedChannels(TStageObject& object) {
  int animated = 0;
  for (int channel = 0; channel < TStageObject::T_ChannelCount; ++channel)
    if (object.getParam(TStageObject::Channel(channel))->getKeyframeCount())
      ++animated;
  return animated;
}

QJsonArray roles(Document& document, TStageObject* object) {
  QJsonArray result;
  if (tree(document)->getCurrentCameraId() == object->getId())
    result.append("output");
  if (tree(document)->getCurrentPreviewCameraId() == object->getId())
    result.append("preview");
  return result;
}

QJsonValue handleOrNull(Document& document, TStageObject* object) {
  return object ? QJsonValue(document.idForObject(object)) : QJsonValue();
}

TStageObject* parentOf(Document& document, TStageObject* object) {
  if (object->getParent() == TStageObjectId::NoneId) return nullptr;
  return tree(document)->getStageObject(object->getParent(), false);
}

QJsonObject cameraRecord(Document& document, TStageObject* object) {
  TCamera& camera   = *object->getCamera();
  const TPointD dpi = camera.getDpi();
  return {{"camera_id", document.idForObject(object)},
          {"name", QString::fromStdString(object->getName())},
          {"width_px", camera.getRes().lx},
          {"height_px", camera.getRes().ly},
          {"width_in", camera.getSize().lx},
          {"height_in", camera.getSize().ly},
          {"dpi_x", dpi.x},
          {"dpi_y", dpi.y},
          {"square_pixels", camera.isPixelSquared()},
          {"x_prevalence", camera.isXPrevalence()},
          {"interest_rect", interestRecord(camera)},
          {"parent_id", handleOrNull(document, parentOf(document, object))},
          {"roles", roles(document, object)},
          {"animated_channels", animatedChannels(*object)},
          {"follows_path", object->isPathEnabled()},
          {"offset_layers", object->getOffsetLayerCount()}};
}

TStageObject* targetCamera(Document& document, const QJsonObject& params) {
  if (params.contains("camera_id"))
    return readCamera(document, params.value("camera_id").toString());
  return cameraById(document, tree(document)->getCurrentCameraId());
}

QJsonValue inspectCamera(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  TStageObject* camera = targetCamera(document, params);
  if (!camera) return describeCamera(document);
  QJsonObject record = cameraRecord(document, camera);
  record.insert("object_id", record.value("camera_id"));
  return record;
}

// ------------------------------------------------------------------ list

void requireRevision(Document& document, const QJsonObject& params) {
  if (requiredString(params, "expected_revision") != document.revisionText())
    throw Error(
        "stale_revision", "Refresh scene.summary before continuing.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
}

QJsonValue listCameras(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireRevision(document, params);
  requireTopSheet(params);
  const std::vector<TStageObject*> all = cameras(document);
  const int offset = integerParam(params, "offset", 0, 4096, 0);
  const int limit  = integerParam(params, "limit", 1, MaxListed, MaxListed);
  QJsonArray page;
  for (int index = offset; index < int(all.size()) && page.size() < limit;
       ++index)
    page.append(cameraRecord(document, all[index]));
  const int next = offset + page.size();
  TStageObject* output =
      cameraById(document, tree(document)->getCurrentCameraId());
  TStageObject* preview =
      cameraById(document, tree(document)->getCurrentPreviewCameraId());
  return QJsonObject{{"cameras", page},
                     {"output_camera_id", handleOrNull(document, output)},
                     {"preview_camera_id", handleOrNull(document, preview)},
                     {"preview_camera_status", preview ? "ok" : "dangling"},
                     {"total", int(all.size())},
                     {"offset", offset},
                     {"limit", limit},
                     {"next_offset", next < int(all.size()) ? QJsonValue(next)
                                                            : QJsonValue()}};
}

// ---------------------------------------------------------------- update

QString readCameraName(Document& document, const QJsonObject& params,
                       TStageObject* self) {
  const QString name = nameParam(params);
  static const QRegularExpression reserved("^Camera[0-9]+$");
  if (reserved.match(name).hasMatch())
    throw Error("invalid_params",
                "Camera<digits> names follow camera numbers; choose another.",
                {{"reason", "reserved_name"}});
  for (TStageObject* camera : cameras(document))
    if (camera != self && QString::fromStdString(camera->getName()) == name)
      throw Error("name_conflict", "Another camera has that name.",
                  {{"name", name}});
  return name;
}

struct CameraValues {
  TDimension res;
  TDimensionD size;
};

CameraValues readValues(const QJsonObject& params, const TCamera& camera) {
  CameraValues values{camera.getRes(), camera.getSize()};
  values.res.lx = integerParam(params, "width_px", 1, 8192, values.res.lx);
  values.res.ly = integerParam(params, "height_px", 1, 8192, values.res.ly);
  values.size.lx =
      numberParam(params, "width_in", 0.001, 10000, values.size.lx);
  values.size.ly =
      numberParam(params, "height_in", 0.001, 10000, values.size.ly);
  if (qint64(values.res.lx) * values.res.ly > MaxPixels)
    throw Error("resource_limit",
                "Camera resolution exceeds the 16 megapixel limit.");
  return values;
}

// The interest rectangle after the edit: native setRes clears it when the
// resolution changes, so a set rectangle must then be restated or cleared.
TRect readInterest(const QJsonObject& params, TCamera& camera,
                   const TDimension& res) {
  if (!params.contains("interest_rect")) {
    const TRect current = camera.getInterestRect();
    if (!current.isEmpty() && res != camera.getRes())
      throw Error("invalid_params",
                  "The resolution change clears the interest rectangle; give "
                  "interest_rect, or null to clear it.",
                  {{"reason", "interest_rect_reset"}});
    return current;
  }
  if (params.value("interest_rect").isNull()) return TRect();
  const QJsonObject value = params.value("interest_rect").toObject();
  const int x             = value.value("x").toInt();
  const int y             = value.value("y").toInt();
  const TRect rect(x, y, x + value.value("width").toInt() - 1,
                   y + value.value("height").toInt() - 1);
  if (rect.x1 >= res.lx || rect.y1 >= res.ly)
    throw Error("invalid_params",
                "The interest rectangle must lie inside the resolution.",
                {{"reason", "interest_rect_bounds"}});
  return rect;
}

QJsonValue updateCamera(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  TStageObject* object      = targetCamera(document, params);
  TCamera& camera           = *object->getCamera();
  const CameraValues values = readValues(params, camera);
  const TRect interest      = readInterest(params, camera, values.res);
  const QString name        = params.contains("name")
                                  ? readCameraName(document, params, object)
                                  : QString::fromStdString(object->getName());
  const bool same           = values.res == camera.getRes() &&
                              values.size.lx == camera.getSize().lx &&
                              values.size.ly == camera.getSize().ly &&
                              interest == camera.getInterestRect() &&
                              name.toStdString() == object->getName();
  if (!same) {
    requireEditable(document, object);
    context.markMutationStarted();
    camera.setSize(values.size);
    camera.setRes(values.res);
    camera.setInterestRect(interest);
    object->setName(name.toStdString());
  }
  return inspectCamera(context, params);
}

// --------------------------------------------------------- create and copy

void requireCameraRoom(Document& document) {
  if (int(cameras(document).size()) >= MaxCameras)
    throw Error("resource_limit", "A sheet holds at most 64 cameras.");
  requireAutomationPathObjectGrowth(*document.scene);
}

TStageObject* newCamera(Document& document) {
  int index = 0;
  while (cameraById(document, TStageObjectId::CameraId(index))) ++index;
  return tree(document)->getStageObject(TStageObjectId::CameraId(index), true);
}

QJsonValue createCamera(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  const QString name = readCameraName(document, params, nullptr);
  requireCameraRoom(document);
  const CameraValues values = readValues(params, TCamera());
  context.markMutationStarted();
  TStageObject* object = newCamera(document);
  object->getCamera()->setSize(values.size);
  object->getCamera()->setRes(values.res);
  object->setName(name.toStdString());
  return QJsonObject{{"camera", cameraRecord(document, object)}};
}

QJsonArray layerMap(Document& document, TStageObject* source,
                    TStageObject* copy) {
  const QJsonArray from = automationLayerSummaries(document, *source);
  const QJsonArray to   = automationLayerSummaries(document, *copy);
  QJsonArray map;
  for (int index = 0; index < std::min(from.size(), to.size()); ++index)
    map.append(QJsonObject{
        {"source_layer_id", from.at(index).toObject().value("layer_id")},
        {"layer_id", to.at(index).toObject().value("layer_id")}});
  return map;
}

QJsonValue duplicateCamera(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  TStageObject* source =
      readCamera(document, requiredString(params, "camera_id"));
  if (source->isPathEnabled())
    throw Error("unsupported_target",
                "Cameras that follow a motion path are not duplicated here.",
                {{"reason", "follows_path"}});
  const QString name = readCameraName(document, params, nullptr);
  requireCameraRoom(document);
  const bool keepParent =
      params.value("parent_policy").toString() == "same_parent";
  context.markMutationStarted();
  TStageObject* copy = newCamera(document);
  std::unique_ptr<TStageObjectParams> state(source->getParams());
  // assignParams deep-copies curves, cycle flags included, and offset layers.
  copy->assignParams(state.get());
  *copy->getCamera() = *source->getCamera();
  copy->setName(name.toStdString());
  copy->setParent(keepParent ? source->getParent() : TStageObjectId::NoneId);
  copy->setParentHandle(keepParent ? source->getParentHandle() : "B");
  return QJsonObject{{"camera", cameraRecord(document, copy)},
                     {"source_camera_id", document.idForObject(source)},
                     {"layer_map", layerMap(document, source, copy)},
                     {"independent_curves", true}};
}

// ---------------------------------------------------------------- select

QJsonObject roleResult(Document& document) {
  TStageObjectTree* objects = tree(document);
  return {{"output_camera_id",
           handleOrNull(document,
                        cameraById(document, objects->getCurrentCameraId()))},
          {"preview_camera_id",
           handleOrNull(
               document,
               cameraById(document, objects->getCurrentPreviewCameraId()))}};
}

QJsonValue selectCamera(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  TStageObject* camera =
      readCamera(document, requiredString(params, "camera_id"));
  const QJsonArray wanted = params.value("roles").toArray();
  const bool output       = wanted.contains("output");
  const bool preview      = wanted.contains("preview");
  if (wanted.size() != int(output) + int(preview))
    throw Error("invalid_params", "Name each role once.",
                {{"reason", "roles"}});
  const TStageObjectId id   = camera->getId();
  TStageObjectTree* objects = tree(document);
  const bool same = (!output || objects->getCurrentCameraId() == id) &&
                    (!preview || objects->getCurrentPreviewCameraId() == id);
  if (!same) {
    context.markMutationStarted();
    if (output) objects->setCurrentCameraId(id);
    if (preview) objects->setCurrentPreviewCameraId(id);
  }
  return roleResult(document);
}

// ---------------------------------------------------------------- remove

template <typename Handles, typename Value>
void forgetHandles(Handles& handles, const Value& value) {
  for (auto entry = handles.begin(); entry != handles.end();) {
    if (entry.value() == value)
      entry = handles.erase(entry);
    else
      ++entry;
  }
}

void forgetCamera(Document& document, TStageObject* camera) {
  for (int index = 0; index < camera->getOffsetLayerCount(); ++index)
    forgetHandles(document.offsetLayers,
                  camera->getOffsetLayer(index).m_weight);
  forgetHandles(document.objects, camera);
}

std::vector<TStageObject*> childrenOf(Document& document,
                                      TStageObject* object) {
  std::vector<TStageObject*> children;
  for (int index = 0; index < tree(document)->getStageObjectCount(); ++index) {
    TStageObject* other = tree(document)->getStageObject(index);
    if (other != object && other->getParent() == object->getId())
      children.push_back(other);
  }
  return children;
}

struct Replacements {
  TStageObject* output  = nullptr;
  TStageObject* preview = nullptr;
};

TStageObject* readReplacement(Document& document, const QJsonObject& value,
                              const QString& role, bool held,
                              TStageObject* removed) {
  if (!value.contains(role)) {
    if (held)
      throw Error(
          "in_use",
          "The camera holds the " + role + " role; name its replacement.",
          {{"reason", "role"}, {"roles", QJsonArray{role}}});
    return nullptr;
  }
  if (!held)
    throw Error("invalid_params",
                "The camera does not hold the " + role + " role.",
                {{"reason", "replacement"}});
  TStageObject* camera = readCamera(document, value.value(role).toString());
  if (camera == removed)
    throw Error("invalid_params", "A camera cannot replace itself.",
                {{"reason", "replacement"}});
  return camera;
}

Replacements readReplacements(Document& document, const QJsonObject& params,
                              TStageObject* removed) {
  const QJsonObject value   = params.value("replacement").toObject();
  const TStageObjectId id   = removed->getId();
  TStageObjectTree* objects = tree(document);
  return {readReplacement(document, value, "output",
                          objects->getCurrentCameraId() == id, removed),
          readReplacement(document, value, "preview",
                          objects->getCurrentPreviewCameraId() == id, removed)};
}

void requireChildren(Document& document, TStageObject* camera,
                     const QString& policy) {
  const std::vector<TStageObject*> children = childrenOf(document, camera);
  QJsonArray listed;
  for (TStageObject* child : children)
    if (listed.size() < MaxListed) listed.append(document.idForObject(child));
  if (!children.empty() && policy != "reattach_to_parent")
    throw Error("has_dependents",
                "The camera has stage children; give children_policy "
                "reattach_to_parent or reparent them first.",
                {{"reason", "children"},
                 {"children", listed},
                 {"count", int(children.size())}});
  // A child hung on a hook keeps that handle only under a parent that has
  // it; the table, which children of a parentless camera join, has none.
  for (TStageObject* child : children)
    if (child->getParentHandle() != "B" &&
        camera->getParent() == TStageObjectId::NoneId)
      throw Error("has_dependents",
                  "A child uses a handle the table does not have.",
                  {{"reason", "hook_handle"},
                   {"object_id", document.idForObject(child)}});
}

QJsonArray reattachChildren(Document& document,
                            const std::vector<TStageObject*>& children,
                            const std::vector<std::vector<TAffine>>& before,
                            const std::vector<int>& frames) {
  QJsonArray result;
  for (size_t index = 0; index < children.size(); ++index) {
    TStageObject* child = children[index];
    // Children of a parentless camera go to the table, as the editor's
    // unlink does.
    if (child->getParent() == TStageObjectId::NoneId) {
      child->setParent(TStageObjectId::TableId);
      child->setParentHandle("B");
    }
    QJsonObject row{
        {"object_id", document.idForObject(child)},
        {"new_parent_id", handleOrNull(document, parentOf(document, child))}};
    insertAutomationWorldChanges(
        row, frames, before[index],
        automationPlacements(*document.scene, *child, frames));
    result.append(row);
  }
  return result;
}

int unnamedAbove(const std::vector<TStageObject*>& all, TStageObject* removed) {
  int count = 0;
  for (TStageObject* camera : all)
    if (camera->getId().getIndex() > removed->getId().getIndex() &&
        camera->getName() == camera->getId().toString())
      ++count;
  return count;
}

QJsonValue removeCamera(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireTopSheet(params);
  const QString id     = requiredString(params, "camera_id");
  TStageObject* camera = readCamera(document, id);
  requireEditable(document, camera);
  const std::vector<TStageObject*> all = cameras(document);
  if (all.size() <= 1)
    throw Error("in_use", "A sheet keeps at least one camera.",
                {{"reason", "last_camera"}});
  const Replacements replacement = readReplacements(document, params, camera);
  requireChildren(document, camera, params.value("children_policy").toString());
  const std::vector<int> frames             = automationReportFrames(params);
  const std::vector<TStageObject*> children = childrenOf(document, camera);
  std::vector<std::vector<TAffine>> before;
  for (TStageObject* child : children)
    before.push_back(automationPlacements(*document.scene, *child, frames));
  const int materialized = unnamedAbove(all, camera);
  context.markMutationStarted();
  forgetCamera(document, camera);
  TStageObjectTree* objects = tree(document);
  objects->removeStageObject(camera->getId());
  const int renumbered = objects->compactCameraIds();
  if (replacement.output)
    objects->setCurrentCameraId(replacement.output->getId());
  if (replacement.preview)
    objects->setCurrentPreviewCameraId(replacement.preview->getId());
  QJsonObject result = roleResult(document);
  result.insert("removed_camera_id", id);
  result.insert("children_reattached",
                reattachChildren(document, children, before, frames));
  result.insert("cameras_renumbered", renumbered);
  result.insert("names_materialized", materialized);
  return result;
}

QJsonObject interestSchema() {
  return {
      {"oneOf",
       QJsonArray{
           QJsonObject{
               {"type", "object"},
               {"additionalProperties", false},
               {"properties", QJsonObject{{"x", integerSchema(0, 8191)},
                                          {"y", integerSchema(0, 8191)},
                                          {"width", integerSchema(1, 8192)},
                                          {"height", integerSchema(1, 8192)}}},
               {"required", QJsonArray{"x", "y", "width", "height"}}},
           QJsonObject{{"type", "null"}}}}};
}

void addCameraLifecycle(QJsonArray& catalog, const QJsonObject& sheet,
                        const QJsonObject& camera) {
  catalog.append(operation(
      "camera.create",
      "Create a camera with an explicit name, resolution and size in inches; "
      "it has no parent and no role.",
      merge(merge(sheet, cameraSchema()), {{"name", textSchema()}}),
      {"document_id", "name", "width_px", "height_px", "width_in", "height_in"},
      true));
  catalog.append(operation(
      "camera.duplicate",
      "Copy a camera with independent curves and offset layers, keeping its "
      "parent or none.",
      merge(camera, {{"name", textSchema()},
                     {"parent_policy", enumSchema({"same_parent", "none"})}}),
      {"document_id", "camera_id", "name", "parent_policy"}, true));
  catalog.append(operation(
      "camera.select",
      "Make a camera the output (render) camera, the preview camera, or both.",
      merge(camera,
            {{"roles", QJsonObject{{"type", "array"},
                                   {"items", enumSchema({"output", "preview"})},
                                   {"minItems", 1},
                                   {"maxItems", 2}}}}),
      {"document_id", "camera_id", "roles"}, true));
  const QJsonObject replacement{
      {"type", "object"},
      {"additionalProperties", false},
      {"properties",
       QJsonObject{{"output", textSchema()}, {"preview", textSchema()}}}};
  catalog.append(operation(
      "camera.remove",
      "Remove a camera that is not the sheet's last; a held role needs a "
      "replacement and stage children need children_policy. Camera numbers "
      "stay packed and the other cameras keep their handles.",
      merge(camera,
            {{"replacement", replacement},
             {"children_policy", enumSchema({"refuse", "reattach_to_parent"})},
             {"report_frames", automationReportFramesSchema()}}),
      {"document_id", "camera_id", "children_policy"}, true));
}

}  // namespace

void addCameraOperations(QJsonArray& catalog) {
  const QJsonObject sheet =
      merge(documentSchema(), {{"sheet_path", sheetPathSchema()}});
  const QJsonObject camera = merge(sheet, {{"camera_id", textSchema()}});
  catalog.append(operation(
      "camera.inspect",
      "Inspect a camera, the output camera by default: size, resolution, "
      "DPI, interest rectangle, parent and roles.",
      camera, {"document_id"}));
  catalog.append(operation(
      "camera.update",
      "Set a camera's resolution, size in inches, name or interest "
      "rectangle, the output camera by default. A resolution change while an "
      "interest rectangle is set needs interest_rect (null clears it).",
      merge(merge(camera, cameraSchema()),
            {{"name", textSchema()}, {"interest_rect", interestSchema()}}),
      {"document_id"}, true));
  catalog.append(operation(
      "camera.list",
      "Page through the sheet's cameras in native order with their roles; "
      "next_offset is null on the last page.",
      merge(sheet, {{"expected_revision", textSchema()},
                    {"offset", integerSchema(0, 4096)},
                    {"limit", integerSchema(1, MaxListed)}}),
      {"document_id", "expected_revision"}));
  addCameraLifecycle(catalog, sheet, camera);
}

void addCameraHandlers(SceneHandlers& handlers) {
  handlers.insert({{"camera.inspect", inspectCamera},
                   {"camera.update", updateCamera},
                   {"camera.list", listCameras},
                   {"camera.create", createCamera},
                   {"camera.duplicate", duplicateCamera},
                   {"camera.select", selectCamera},
                   {"camera.remove", removeCamera}});
}

}  // namespace SceneShared
}  // namespace GroundzeroAutomation
