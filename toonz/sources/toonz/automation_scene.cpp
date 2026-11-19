#include "automation.h"
#include "automation_animation.h"
#include "automation_animation_targets.h"
#include "automation_audio.h"
#include "automation_path.h"
#include "automation_projects.h"
#include "automation_rig.h"
#include "automation_scene_host.h"
#include "automation_scene_shared.h"

#include "tdoubleparam.h"
#include "toutputproperties.h"
#include "toonz/levelset.h"
#include "toonz/sceneproperties.h"
#include "toonz/tcamera.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"

#include <QDir>
#include <QJsonDocument>
#include <QSet>

#include <cmath>
#include <functional>
#include <map>
#include <vector>

namespace GroundzeroAutomation {
using namespace SceneShared;
namespace {

QJsonObject summaryOffsetSchema() {
  return {{"type", "integer"}, {"minimum", 0}, {"maximum", 16384}};
}

QJsonObject arraySchema(const QJsonObject& items) {
  return {{"type", "array"},
          {"items", items},
          {"minItems", 1},
          {"maxItems", MaxSpan}};
}

QJsonObject describeObject(Document& document, TStageObject* object) {
  const TStageObjectId id = object->getId();
  QString kind            = "table";
  if (id.isColumn()) kind = "column";
  if (id.isCamera()) kind = "camera";
  if (id.isPegbar()) kind = "pegbar";
  TStageObject* parent =
      tree(document)->getStageObject(object->getParent(), false);
  return {{"object_id", document.idForObject(object)},
          {"kind", kind},
          {"name", QString::fromStdString(object->getName())},
          {"parent_id",
           parent ? QJsonValue(document.idForObject(parent)) : QJsonValue()},
          {"locked", object->isLocked()}};
}

constexpr int DefaultSummaryLimit = 256;
constexpr qint64 MaxSummaryBytes  = 1024 * 1024;
constexpr int MaxSummaryLimit     = 1024;
constexpr int MaxSummaryOffset    = 16384;
// Records stop being added to a page once it holds this many bytes; a page
// always carries the first record of each list it continues.
constexpr qint64 SummaryPageBytes = 256 * 1024;

// Which part of each of the summary's three lists a call returns.
struct SummaryPage final {
  int limit   = DefaultSummaryLimit;
  int columns = 0;
  int levels  = 0;
  int objects = 0;
};

SummaryPage summaryPage(const QJsonObject& params) {
  return {
      integerParam(params, "limit", 1, MaxSummaryLimit, DefaultSummaryLimit),
      integerParam(params, "columns_offset", 0, MaxSummaryOffset, 0),
      integerParam(params, "levels_offset", 0, MaxSummaryOffset, 0),
      integerParam(params, "objects_offset", 0, MaxSummaryOffset, 0)};
}

// One list page: records [offset, next) of total, within the limit and the
// shared byte budget. A null record (an empty column slot) is skipped.
QJsonObject listPage(const QString& name, int total, int offset, int limit,
                     qint64& bytes,
                     const std::function<QJsonValue(int)>& record,
                     QJsonArray& items) {
  if (offset > total)
    throw Error("invalid_params", name + "_offset is beyond the list.",
                {{"total", total}});
  int index = offset;
  for (; index < total && items.size() < limit; ++index) {
    const QJsonValue value = record(index);
    if (value.isNull()) continue;
    const qint64 size =
        QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact).size();
    if (!items.isEmpty() && bytes + size > SummaryPageBytes) break;
    bytes += size;
    items.append(value);
  }
  return {{"offset", offset},
          {"total", total},
          {"next_offset", index < total ? QJsonValue(index) : QJsonValue()}};
}

QJsonObject describeScene(Document& document, bool includeDrawings = true,
                          const SummaryPage& page = SummaryPage()) {
  QJsonArray columns, levels, objects;
  TXsheet* sheet          = document.scene->getXsheet();
  TLevelSet* cast         = document.scene->getLevelSet();
  TStageObjectTree* stage = tree(document);
  qint64 bytes            = 0;
  QJsonObject pages{
      {"columns",
       listPage(
           "columns", sheet->getColumnCount(), page.columns, page.limit, bytes,
           [&](int index) -> QJsonValue {
             TXshColumn* column = sheet->getColumn(index);
             return column ? QJsonValue(describeColumn(document, column))
                           : QJsonValue();
           },
           columns)},
      {"levels",
       listPage(
           "levels", cast->getLevelCount(), page.levels, page.limit, bytes,
           [&](int index) -> QJsonValue {
             return describeLevel(document, cast->getLevel(index),
                                  includeDrawings);
           },
           levels)},
      {"objects", listPage(
                      "objects", stage->getStageObjectCount(), page.objects,
                      page.limit, bytes,
                      [&](int index) -> QJsonValue {
                        return describeObject(document,
                                              stage->getStageObject(index));
                      },
                      objects)}};
  if (bytes > MaxSummaryBytes)
    throw Error("resource_limit",
                "One summary record exceeds 1 MiB; pass include_drawings "
                "false or use level.inspect.",
                {{"limit", "page_bytes"}});
  return {
      {"document_id", document.id},
      {"revision", document.revisionText()},
      {"name", QString::fromStdWString(document.scene->getSceneName())},
      {"frame_rate",
       document.scene->getProperties()->getOutputProperties()->getFrameRate()},
      {"frame_count", document.scene->getFrameCount()},
      {"frame_origin", 0},
      {"camera", describeCamera(document)},
      {"columns", columns},
      {"levels", levels},
      {"objects", objects},
      {"mode", "detached"},
      {"access", document.readOnly ? "read_only" : "editable"},
      {"source", document.readOnly
                     ? QJsonValue(document.readOnly->summarySource())
                     : QJsonValue()},
      {"limit", page.limit},
      {"pages", pages},
      {"project", document.project.isEmpty() ? QJsonValue()
                                             : QJsonValue(document.project)}};
}

struct Settings {
  int width, height;
  double widthIn, heightIn, fps;
};

Settings readSettings(const QJsonObject& params, ToonzScene& scene) {
  TCamera* camera = scene.getCurrentCamera();
  Settings settings;
  settings.width =
      integerParam(params, "width_px", 1, 8192, camera->getRes().lx);
  settings.height =
      integerParam(params, "height_px", 1, 8192, camera->getRes().ly);
  if (qint64(settings.width) * settings.height > MaxPixels)
    throw Error("resource_limit",
                "Camera resolution exceeds the 16 megapixel limit.");
  settings.widthIn =
      numberParam(params, "width_in", 0.001, 10000, camera->getSize().lx);
  settings.heightIn =
      numberParam(params, "height_in", 0.001, 10000, camera->getSize().ly);
  const double fps =
      scene.getProperties()->getOutputProperties()->getFrameRate();
  settings.fps = numberParam(params, "frame_rate", 1, 240, fps);
  return settings;
}

void applySettings(ToonzScene& scene, const Settings& settings) {
  scene.getCurrentCamera()->setSize(
      TDimensionD(settings.widthIn, settings.heightIn));
  scene.getCurrentCamera()->setRes(TDimension(settings.width, settings.height));
  scene.getProperties()->getOutputProperties()->setFrameRate(settings.fps);
  scene.getProperties()->getPreviewProperties()->setFrameRate(settings.fps);
  setAutomationSoundFrameRate(scene, settings.fps);
}

// A project's scene defaults as explicit settings; a project without a
// camera keeps the scene's.
Settings projectSettings(const QJsonValue& defaults, ToonzScene& scene) {
  const QJsonObject value = defaults.toObject();
  QJsonObject params;
  const std::pair<const char*, const char*> fields[]{
      {"camera_width_px", "width_px"},
      {"camera_height_px", "height_px"},
      {"camera_width_in", "width_in"},
      {"camera_height_in", "height_in"},
      {"frame_rate", "frame_rate"}};
  for (const auto& field : fields)
    if (value.contains(field.first))
      params.insert(field.second, value.value(field.first));
  return readSettings(params, scene);
}

QJsonValue createScene(Context& context, const QJsonObject& params) {
  context.requireDocumentCapacity();
  const QString name = nameParam(params);
  if (name.contains('/') || name.contains('\\') || name == "." || name == "..")
    throw Error("invalid_argument",
                "Scene name must be a name without path separators.");
  auto scene = std::make_unique<ToonzScene>();
  scene->setProject(context.project());
  scene->setScenePath(TFilePath(
      QDir(context.workspace()).filePath(name + ".tnz").toStdWString()));
  applySettings(*scene, {1920, 1080, 16.0, 9.0, 24.0});
  DraftProject project;
  if (params.contains("project_id")) {
    project = draftProject(context, requiredString(params, "project_id"));
    applySettings(*scene, projectSettings(project.sceneDefaults, *scene));
  }
  applySettings(*scene, readSettings(params, *scene));
  context.markMutationStarted();
  Document& document = context.addDocument(std::move(scene));
  document.project   = project.binding;
  return describeScene(document);
}

QJsonValue sceneSummary(Context& context, const QJsonObject& params) {
  return describeScene(getDocument(context, params),
                       boolParam(params, "include_drawings", true),
                       summaryPage(params));
}

}  // namespace

QJsonValue SceneShared::updateSettings(Context& context,
                                       const QJsonObject& params) {
  Document& document      = getDocument(context, params);
  const Settings settings = readSettings(params, *document.scene);
  const Settings current  = readSettings({}, *document.scene);
  if (settings.width == current.width && settings.height == current.height &&
      settings.widthIn == current.widthIn &&
      settings.heightIn == current.heightIn && settings.fps == current.fps)
    return describeScene(document);
  TStageObject* camera = tree(document)->getStageObject(
      tree(document)->getCurrentCameraId(), false);
  requireEditable(document, camera);
  // Native clip positions are rounded when the frame rate changes.
  if (settings.fps != current.fps &&
      automationSceneHasSoundClips(*document.scene))
    throw Error("timing_conflict",
                "Remove every sound clip before changing the frame rate.",
                {{"frame_rate", current.fps}});
  context.markMutationStarted();
  applySettings(*document.scene, settings);
  return describeScene(document);
}

namespace {

QJsonValue discardScene(Context& context, const QJsonObject& params) {
  const QString id = getDocument(context, params).id;
  context.markMutationStarted();
  context.removeDocument(id);
  return QJsonObject{{"document_id", id}, {"discarded", true}};
}

QJsonValue inspectProject(Context& context, const QJsonObject& params) {
  if (params.contains("project_id"))
    return inspectGrantedProject(context, requiredString(params, "project_id"));
  return QJsonObject{{"mode", "private_workspace"},
                     {"documents", documentRows(context)},
                     {"arbitrary_scene_open", true},
                     {"read_only_documents",
                      QJsonObject{{"open", readOnlyDocumentCount(context)},
                                  {"max", ReadOnlyDocumentLimit}}},
                     {"live_gui_mutation", false}};
}

QJsonValue createObject(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  requireAutomationPathObjectGrowth(*document.scene);
  const QString name = nameParam(params);
  if (requiredString(params, "kind") != "pegbar")
    throw Error("unsupported_kind",
                "Only pegbar creation is currently supported.");
  TStageObjectTree* objects = tree(document);
  if (objects->getStageObjectCount() >= MaxEntities)
    throw Error("resource_limit",
                "The document already contains 4096 stage objects.");
  int index = 0;
  while (objects->getStageObject(TStageObjectId::PegbarId(index), false))
    ++index;
  context.markMutationStarted();
  TStageObject* object =
      objects->getStageObject(TStageObjectId::PegbarId(index));
  object->setName(name.toStdString());
  return describeObject(document, object);
}

void checkParentCycle(Document& document, TStageObject* object,
                      TStageObject* parent) {
  TStageObject* ancestor = parent;
  for (int depth = 0; ancestor && depth <= MaxEntities; ++depth) {
    if (ancestor == object)
      throw Error("cycle", "The requested parent would create a cycle.");
    ancestor = tree(document)->getStageObject(ancestor->getParent(), false);
  }
  if (ancestor) throw Error("cycle", "The existing parent chain is cyclic.");
}

void checkParent(Document& document, TStageObject* object,
                 TStageObject* parent) {
  if (object->getId().isTable())
    throw Error("invalid_parent", "The table cannot be reparented.");
  if (object->getId().isPegbar() && parent->getId().isColumn())
    throw Error("invalid_parent", "A pegbar cannot be parented to a column.");
  if (parent->getId().isColumn()) {
    TXshColumn* column =
        document.scene->getXsheet()->getColumn(parent->getId().getIndex());
    if (!column || !column->canBeParent())
      throw Error("invalid_parent", "This column cannot parent stage objects.");
  }
  checkParentCycle(document, object, parent);
}

QJsonValue parentObject(Context& context, const QJsonObject& params) {
  Document& document   = getDocument(context, params);
  TStageObject* object = document.object(requiredString(params, "object_id"));
  TStageObject* parent = document.object(requiredString(params, "parent_id"));
  requireEditable(document, object);
  checkParent(document, object, parent);
  validateAutomationStageParent(*document.scene, *object, *parent);
  const std::string handle = automationParentHandle(
      document, *object, *parent, params.value("parent_handle"));
  const auto frames = automationReportFrames(params);
  const auto before = automationPlacements(*document.scene, *object, frames);
  if (object->getParent() != parent->getId() ||
      object->getParentHandle() != handle) {
    context.markMutationStarted();
    object->setParent(parent->getId());
    object->setParentHandle(handle);
  }
  QJsonObject result = describeObject(document, object);
  result.insert("parent_handle", automationHandleRecord(handle));
  insertAutomationWorldChanges(
      result, frames, before,
      automationPlacements(*document.scene, *object, frames));
  return result;
}

struct Channel {
  TStageObject::Channel native;
  QString unit;
};

const std::map<QString, Channel>& animationChannels() {
  static const std::map<QString, Channel> channels = {
      {"x", {TStageObject::T_X, "inch"}},
      {"y", {TStageObject::T_Y, "inch"}},
      {"z", {TStageObject::T_Z, "native_z"}},
      {"rotation", {TStageObject::T_Angle, "degree"}},
      {"scale_x", {TStageObject::T_ScaleX, "scalar"}},
      {"scale_y", {TStageObject::T_ScaleY, "scalar"}},
      {"scale", {TStageObject::T_Scale, "scalar"}},
      {"path_position", {TStageObject::T_Path, "percent"}}};
  return channels;
}

const Channel WeightChannel{TStageObject::T_ChannelCount, "scalar"};

Channel readChannel(const QJsonObject& params) {
  const auto& channels = animationChannels();
  const QString name   = requiredString(params, "channel");
  const auto channel   = channels.find(name);
  if (channel == channels.end() && name != "weight")
    throw Error("unsupported_channel", "Unsupported animation channel.");
  const Channel result = name == "weight" ? WeightChannel : channel->second;
  if (requiredString(params, "unit") != result.unit)
    throw Error("invalid_unit",
                "This channel requires unit " + result.unit + ".");
  return result;
}

// The base channel of an object, or one curve or the weight of its layer.
struct CurveTarget {
  TStageObject* object;
  TDoubleParam* curve;
  Channel channel;
  QString valueSpace;
  int layer = -1;
};

bool isWeight(const CurveTarget& target) {
  return target.channel.native == TStageObject::T_ChannelCount;
}

// Cyclic curves are qualified for the base channels of stage objects only;
// weights, path positions and offset-layer curves keep refusing them.
CyclePolicy cyclePolicy(const CurveTarget& target) {
  return target.layer < 0 && !isWeight(target) &&
                 target.channel.native != TStageObject::T_Path
             ? CyclePolicy::Allow
             : CyclePolicy::Refuse;
}

CurveTarget readTarget(Document& document, const QJsonObject& params) {
  TStageObject* object  = document.object(requiredString(params, "object_id"));
  const Channel channel = readChannel(params);
  const bool weight     = channel.native == TStageObject::T_ChannelCount;
  if (!params.contains("layer_id")) {
    if (weight)
      throw Error("invalid_params", "The weight channel requires layer_id.");
    return {object, object->getParam(channel.native), channel, "base_channel"};
  }
  const auto layer =
      automationLayer(document, requiredString(params, "layer_id"));
  if (layer.object != object)
    throw Error("invalid_params", "layer_id belongs to another object.");
  if (channel.native == TStageObject::T_Path)
    throw Error("unsupported_channel",
                "Offset layers have no path_position curve.");
  if (weight)
    return {object, object->getOffsetLayer(layer.index).m_weight.getPointer(),
            channel, "offset_layer_weight", layer.index};
  return {object, object->getOffsetParam(layer.index, channel.native), channel,
          "offset_layer_curve", layer.index};
}

QString curveAdmission(Document& document, const CurveTarget& target) {
  try {
    if (target.channel.native == TStageObject::T_Path)
      requireAutomationPathTarget(*document.scene, *target.object, true);
    else
      requireEditable(document, target.object);
    if (target.curve->isEditorLocked()) return "The channel is editor-locked.";
    return animationUnsupportedReason(*target.curve, cyclePolicy(target));
  } catch (const Error& error) {
    return error.message;
  }
}

QJsonObject channelDescriptor(Document& document, const QString& name,
                              const CurveTarget& target,
                              const QJsonObject& identity) {
  const QString reason = curveAdmission(document, target);
  const bool path      = target.channel.native == TStageObject::T_Path;
  const int bound      = isWeight(target) ? 10 : 1000000;
  QJsonObject address  = identity;
  address.insert("channel", name);
  address.insert("unit", target.channel.unit);
  return {
      {"channel", name},
      {"unit", target.channel.unit},
      {"kind", target.layer < 0   ? "stage_channel"
               : isWeight(target) ? "offset_layer_weight"
                                  : "offset_layer_channel"},
      {"target", address},
      {"edit_via", "animation.set_keys"},
      {"editor_locked", target.curve->isEditorLocked()},
      {"cycle", target.curve->isCycleEnabled()},
      {"cycle_supported", cyclePolicy(target) == CyclePolicy::Allow},
      {"key_count", target.curve->getKeyframeCount()},
      {"writable", reason.isEmpty()},
      {"unsupported_reason",
       reason.isEmpty() ? QJsonValue() : QJsonValue(reason)},
      {"interpolations",
       path ? QJsonArray{"constant", "linear"}
            : QJsonArray{"constant", "linear", "ease_in_out", "speed_in_out"}},
      {"frame_bounds", QJsonArray{0, MaxFrame}},
      {"value_bounds", path ? QJsonArray{0, 100} : QJsonArray{-bound, bound}},
      {"step_bounds", path ? QJsonArray{1, 1} : QJsonArray{1, 1000}},
      {"key_time", "integer"},
      {"sample_time", "fractional"}};
}

QJsonValue inspectAnimationChannels(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  if (isAnimationExternalRequest(params))
    return describeAnimationExternalChannels(document, params);
  TStageObject* object = document.object(requiredString(params, "object_id"));
  const bool layered   = params.contains("layer_id");
  QJsonObject identity{{"object_id", document.idForObject(object)}};
  if (layered) identity.insert("layer_id", params.value("layer_id"));
  QJsonArray result;
  for (const auto& item : animationChannels()) {
    if (layered && item.second.native == TStageObject::T_Path) continue;
    QJsonObject request = params;
    request.insert("channel", item.first);
    request.insert("unit", item.second.unit);
    result.append(channelDescriptor(document, item.first,
                                    readTarget(document, request), identity));
  }
  if (layered) {
    QJsonObject request = params;
    request.insert("channel", "weight");
    request.insert("unit", "scalar");
    result.append(channelDescriptor(document, "weight",
                                    readTarget(document, request), identity));
  }
  return QJsonObject{
      {"object_id", document.idForObject(object)},
      {"channels", result},
      {"value_space", layered ? "offset_layer" : "base_channel"},
      {"offset_layers", automationLayerSummaries(document, *object)}};
}

QJsonArray readArray(const QJsonObject& params, const QString& name) {
  if (!params.value(name).isArray())
    throw Error("invalid_argument", name + " must be an array.");
  const QJsonArray values = params.value(name).toArray();
  if (values.isEmpty() || values.size() > MaxSpan)
    throw Error("resource_limit",
                name + " must contain between 1 and 10000 items.");
  return values;
}

std::vector<int> readFrames(const QJsonObject& params) {
  std::vector<int> frames;
  QSet<int> seen;
  for (const QJsonValue& value : readArray(params, "frames")) {
    const int frame =
        integerParam({{"frame", value}}, "frame", 0, MaxFrame, -1);
    if (seen.contains(frame))
      throw Error("invalid_argument", "frames must not contain duplicates.");
    seen.insert(frame);
    frames.push_back(frame);
  }
  return frames;
}

QJsonValue inspectKeys(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  if (isAnimationExternalRequest(params))
    return inspectAnimationExternalKeys(document, params);
  const CurveTarget target = readTarget(document, params);
  if (target.channel.native == TStageObject::T_Path)
    requireAutomationPathTarget(*document.scene, *target.object, false);
  return QJsonObject{{"object_id", document.idForObject(target.object)},
                     {"channel", params.value("channel")},
                     {"unit", target.channel.unit},
                     {"value_space", target.valueSpace},
                     {"editor_locked", target.curve->isEditorLocked()},
                     {"cycle", target.curve->isCycleEnabled()},
                     {"keys", inspectAnimationKeys(*target.curve)}};
}

void requireWeightKeys(const CurveTarget& target,
                       const std::vector<TDoubleKeyframe>& keys) {
  if (!isWeight(target)) return;
  for (const auto& key : keys)
    if (std::abs(key.m_value) > 10)
      throw Error("invalid_argument",
                  "Layer weights must stay within -10 and 10.",
                  {{"frame", key.m_frame}, {"field", "value"}});
}

QJsonValue setKeys(Context& context, const QJsonObject& params) {
  Document& document       = getDocument(context, params);
  const CurveTarget target = readTarget(document, params);
  requireEditable(document, target.object);
  TDoubleParam* curve = target.curve;
  if (target.channel.native == TStageObject::T_Path)
    requireAutomationPathTarget(*document.scene, *target.object, true);
  const auto edit = prepareAnimationKeyEdit(*curve, readArray(params, "keys"),
                                            cyclePolicy(target));
  requireWeightKeys(target, edit.keys);
  if (target.channel.native == TStageObject::T_Path) {
    TDoubleParamP prospective(new TDoubleParam(*curve));
    for (const auto& key : edit.keys) prospective->setKeyframe(key);
    validateAutomationPathPosition(*prospective);
  }
  if (!edit.changed) return inspectKeys(context, params);
  context.markMutationStarted();
  for (const auto& key : edit.keys) curve->setKeyframe(key);
  return inspectKeys(context, params);
}

QJsonValue deleteKeys(Context& context, const QJsonObject& params) {
  Document& document       = getDocument(context, params);
  const CurveTarget target = readTarget(document, params);
  requireEditable(document, target.object);
  const std::vector<int> frames = readFrames(params);
  if (target.channel.native == TStageObject::T_Path)
    requireAutomationPathTarget(*document.scene, *target.object, true);
  validateAnimationKeyDeletion(*target.curve, frames, cyclePolicy(target));
  context.markMutationStarted();
  for (const int frame : frames) target.curve->deleteKeyframe(frame);
  return inspectKeys(context, params);
}

void requireStageBaseChannel(const CurveTarget& target, const QString& tool) {
  if (target.layer >= 0 || isWeight(target) ||
      target.channel.native == TStageObject::T_Path)
    throw Error("unsupported_channel",
                tool +
                    " edits stage base channels only; use "
                    "animation.set_keys for path_position, weights and "
                    "offset layers.");
}

QJsonValue insertBreakdown(Context& context, const QJsonObject& params) {
  Document& document       = getDocument(context, params);
  const CurveTarget target = readTarget(document, params);
  requireEditable(document, target.object);
  requireStageBaseChannel(target, "animation.insert_breakdown");
  const auto edit =
      prepareAnimationBreakdown(*target.curve, params, cyclePolicy(target));
  context.markMutationStarted();
  applyAnimationKeyEdit(*target.curve, edit);
  const int frame    = params.value("frame").toInt();
  QJsonObject result = inspectKeys(context, params).toObject();
  result.insert("inserted_frame", frame);
  result.insert("inserted_value", target.curve->getValue(frame));
  return result;
}

QJsonArray editedFrames(const AnimationKeyEdit& edit) {
  QJsonArray frames;
  for (const auto& key : edit.keys) frames.append(key.m_frame);
  return frames;
}

QJsonValue changeKeys(Context& context, const QJsonObject& params,
                      bool duplicate) {
  Document& document       = getDocument(context, params);
  const CurveTarget target = readTarget(document, params);
  requireEditable(document, target.object);
  requireStageBaseChannel(
      target, duplicate ? "animation.duplicate_keys" : "animation.move_keys");
  const std::vector<int> frames = readFrames(params);
  const int frameOffset =
      integerParam(params, "frame_offset", -MaxFrame, MaxFrame, 0);
  const double valueOffset =
      numberParam(params, "value_offset", -2000000, 2000000, 0);
  const CyclePolicy policy = cyclePolicy(target);
  const auto edit =
      duplicate ? prepareAnimationKeyDuplicate(*target.curve, frames,
                                               frameOffset, valueOffset, policy)
                : prepareAnimationKeyMove(*target.curve, frames, frameOffset,
                                          valueOffset, policy);
  if (edit.changed) {
    context.markMutationStarted();
    applyAnimationKeyEdit(*target.curve, edit);
  }
  QJsonObject result = inspectKeys(context, params).toObject();
  if (duplicate)
    result.insert("created_frames", editedFrames(edit));
  else
    result.insert("moved_frames", editedFrames(edit));
  return result;
}

QJsonValue setCycle(Context& context, const QJsonObject& params) {
  Document& document       = getDocument(context, params);
  const CurveTarget target = readTarget(document, params);
  requireEditable(document, target.object);
  requireStageBaseChannel(target, "animation.set_cycle");
  if (!params.value("enabled").isBool())
    throw Error("invalid_argument", "enabled must be a boolean.");
  const bool enabled = params.value("enabled").toBool();
  if (prepareAnimationCycle(*target.curve, enabled)) {
    context.markMutationStarted();
    target.curve->enableCycle(enabled);
  }
  return inspectKeys(context, params);
}

QJsonValue moveKeys(Context& context, const QJsonObject& params) {
  return changeKeys(context, params, false);
}

QJsonValue duplicateKeys(Context& context, const QJsonObject& params) {
  return changeKeys(context, params, true);
}

TDoubleParam* wholeCurveTarget(Document& document, const QJsonObject& target,
                               bool writable) {
  auto* object = document.object(requiredString(target, "object_id"));
  if (readChannel(target).native == TStageObject::T_Path)
    throw Error("unsupported_channel", "Whole-curve copy and retime do not admit path_position.");
  if (writable) requireEditable(document, object);
  if (object->getOffsetLayerCount() != 0)
    throw Error("unsupported_interpolation",
                "Whole-curve edits do not admit offset layers.");
  return object->getParam(readChannel(target).native);
}

QJsonValue copyAnimationCurve(Context& context, const QJsonObject& params) {
  Document& document     = getDocument(context, params);
  const auto source      = params.value("source").toObject();
  const auto destination = params.value("destination").toObject();
  if (source.value("unit") != destination.value("unit"))
    throw Error("invalid_params",
                "Curve copy requires matching source and destination units.");
  auto* sourceCurve      = wholeCurveTarget(document, source, false);
  auto* destinationCurve = wholeCurveTarget(document, destination, true);
  const auto edit = prepareAnimationCurveCopy(*sourceCurve, *destinationCurve);
  if (edit.changed) {
    context.markMutationStarted();
    applyAnimationCurveEdit(*destinationCurve, edit);
  }
  return QJsonObject{{"source", source},
                     {"destination", destination},
                     {"default_value", destinationCurve->getDefaultValue()},
                     {"cycle", destinationCurve->isCycleEnabled()},
                     {"keys", inspectAnimationKeys(*destinationCurve)},
                     {"independent", true},
                     {"affected_domains", QJsonArray{"destination_channel"}}};
}

QJsonValue retimeAnimationCurve(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  auto* curve        = wholeCurveTarget(document, params, true);
  const QJsonObject mapping{{"origin_frame", params.value("origin_frame")},
                            {"shift_frames", params.value("shift_frames")},
                            {"scale", params.value("scale")}};
  const auto edit = prepareAnimationCurveRetime(*curve, mapping);
  if (edit.changed) {
    context.markMutationStarted();
    applyAnimationCurveEdit(*curve, edit);
  }
  return QJsonObject{{"object_id", params.value("object_id")},
                     {"channel", params.value("channel")},
                     {"unit", params.value("unit")},
                     {"default_value", curve->getDefaultValue()},
                     {"cycle", curve->isCycleEnabled()},
                     {"keys", inspectAnimationKeys(*curve)},
                     {"time_mapping", mapping},
                     {"affected_domains", QJsonArray{"target_channel"}}};
}

void requireSampledLayers(const CurveTarget& target) {
  requireSupportedAnimationCurve(*target.curve, cyclePolicy(target));
  if (target.layer >= 0) {
    requireSupportedAnimationCurve(
        *target.object->getOffsetLayer(target.layer).m_weight);
    return;
  }
  if (!TStageObject::isOffsetChannel(target.channel.native)) return;
  for (int index = 0; index < target.object->getOffsetLayerCount(); ++index) {
    requireSupportedAnimationCurve(
        *target.object->getOffsetLayer(index).m_weight);
    requireSupportedAnimationCurve(
        *target.object->getOffsetParam(index, target.channel.native));
  }
}

QJsonObject sampleValue(const CurveTarget& target, double frame) {
  double value = 0;
  std::string error;
  if (!target.curve->getValueChecked(frame, value, error) ||
      !std::isfinite(value))
    throw Error(
        "invalid_argument",
        "Native animation evaluation failed: " + QString::fromStdString(error),
        {{"frame", frame}});
  if (target.layer < 0) {
    const double offset =
        target.object->getOffsetValue(target.channel.native, frame);
    return {{"frame", frame},
            {"value", value + offset},
            {"base_value", value},
            {"offset_value", offset}};
  }
  const auto& layer   = target.object->getOffsetLayer(target.layer);
  const double weight = isWeight(target) ? 1 : layer.m_weight->getValue(frame);
  return {{"frame", frame},
          {"value", value},
          {"weighted_value", layer.m_enabled ? weight * value : 0.0}};
}

QJsonValue sampleAnimation(Context& context, const QJsonObject& params) {
  Document& document = getDocument(context, params);
  if (isAnimationExternalRequest(params))
    return sampleAnimationExternal(document, params);
  const CurveTarget target = readTarget(document, params);
  const QJsonArray frames  = readArray(params, "frames");
  requireSampledLayers(target);
  if (target.channel.native == TStageObject::T_Path)
    requireAutomationPathTarget(*document.scene, *target.object, false);
  QSet<double> seen;
  std::vector<double> requested;
  for (const auto& value : frames) {
    const double frame = numberParam({{"frame", value}}, "frame", 0, MaxFrame, 0);
    if (seen.contains(frame))
      throw Error("invalid_argument", "frames must not contain duplicates.");
    seen.insert(frame);
    requested.push_back(frame);
  }
  requireAnimationSampleBudget(*target.curve, requested);
  QJsonArray values;
  for (const double frame : requested)
    values.append(sampleValue(target, frame));
  return QJsonObject{
      {"object_id", document.idForObject(target.object)},
      {"channel", params.value("channel")},
      {"unit", target.channel.unit},
      {"value_space", target.layer < 0 ? QString("effective_local_channel")
                                       : target.valueSpace},
      {"values", values}};
}

void addSceneOperations(QJsonArray& catalog) {
  QJsonObject settings = cameraSchema();
  settings.insert("frame_rate", numberSchema(1, 240));
  catalog.append(operation(
      "project.inspect",
      "Inspect the worker's private project and documents; with a project_id "
      "from project.list, parse that project file and report its folders.",
      {{"project_id",
        QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}}},
      {}));
  catalog.append(operation(
      "scene.create",
      "Create an isolated native scene. Timeline frames start at 0. "
      "project_id (from project.list or project.create) binds the "
      "draft to that project: its scene defaults become the "
      "settings unless given explicitly, and summaries report the "
      "binding; files are still written only as worker packages.",
      merge(settings, {{"name", textSchema()},
                       {"project_id", QJsonObject{{"type", "string"},
                                                  {"minLength", 1},
                                                  {"maxLength", 64}}}}),
      {"name"}, true));
  catalog.append(
      operation("scene.summary",
                "Inspect native scene, levels, columns and stage objects; "
                "include_drawings false omits per-level drawing lists. Each "
                "list is paged: at most limit records (default 256) and "
                "256 KiB per call; pages names each list's next_offset, "
                "null on its last page, to pass back as columns_offset, "
                "levels_offset or objects_offset.",
                merge(documentSchema(),
                      {{"include_drawings", QJsonObject{{"type", "boolean"}}},
                       {"limit", QJsonObject{{"type", "integer"},
                                             {"minimum", 1},
                                             {"maximum", MaxSummaryLimit}}},
                       {"columns_offset", summaryOffsetSchema()},
                       {"levels_offset", summaryOffsetSchema()},
                       {"objects_offset", summaryOffsetSchema()}}),
                {"document_id"}));
  catalog.append(operation(
      "scene.settings", "Set explicit frame rate, pixels and physical inches.",
      merge(documentSchema(), settings), {"document_id"}, true));
  catalog.append(
      operation("scene.discard",
                "Discard a detached document and invalidate its handles.",
                documentSchema(), {"document_id"}, true));
}

QJsonObject strictObjectSchema(const QJsonObject& properties, const QJsonArray& required) {
  return {{"type", "object"}, {"additionalProperties", false},
          {"properties", properties}, {"required", required}};
}

QJsonObject speedSchema(bool incoming) {
  return strictObjectSchema(
      {{"frame_delta", numberSchema(incoming ? -MaxFrame : 0, incoming ? 0 : MaxFrame)},
       {"value_delta", numberSchema(-1000000, 1000000)}},
      {"frame_delta", "value_delta"});
}

QJsonObject incomingSchema() {
  QJsonObject speed = speedSchema(true);
  auto properties = speed.value("properties").toObject();
  properties.insert("kind", enumSchema({"speed"}));
  speed["properties"] = properties;
  speed["required"] = QJsonArray{"kind", "frame_delta", "value_delta"};
  return {{"oneOf", QJsonArray{
      strictObjectSchema({{"kind", enumSchema({"none"})}}, {"kind"}), speed,
      strictObjectSchema({{"kind", enumSchema({"ease"})},
                          {"frames", numberSchema(0, MaxFrame)}}, {"kind", "frames"})}}};
}

QJsonObject animationKeySchema() {
  const QJsonObject base{{"frame", integerSchema(0, MaxFrame)},
                         {"value", numberSchema(-1000000, 1000000)},
                         {"step", integerSchema(1, 1000)},
                         {"incoming", incomingSchema()}};
  const QJsonArray required{"frame", "value", "interpolation"};
  QJsonArray withControl = required;
  withControl.append("outgoing");
  return {{"oneOf", QJsonArray{
      strictObjectSchema(merge(base, {{"interpolation", enumSchema({"linear", "constant"})}}),
                          required),
      strictObjectSchema(merge(base, {{"interpolation", enumSchema({"speed_in_out"})},
                                      {"outgoing", speedSchema(false)}}), withControl),
      strictObjectSchema(merge(base, {{"interpolation", enumSchema({"ease_in_out"})},
          {"outgoing", strictObjectSchema({{"frames", numberSchema(0, MaxFrame)}}, {"frames"})}}),
                          withControl)}}};
}

QJsonObject wholeCurveOperation(const QString& name, const QString& description,
                                const QJsonObject& properties,
                                const QJsonArray& required) {
  auto descriptor = operation(name, description, properties, required, true);
  auto schema     = descriptor.value("inputSchema").toObject();
  auto fields     = schema.value("properties").toObject();
  const QJsonObject handle{
      {"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
  for (const auto& field : {"document_id", "operation_id", "expected_revision"})
    fields.insert(field, handle);
  schema.insert("properties", fields);
  descriptor.insert("inputSchema", schema);
  return descriptor;
}

void addWholeCurveOperations(QJsonArray& catalog) {
  const QJsonObject target{
      {"object_id",
       QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
      {"channel",
       QJsonObject{{"enum", QJsonArray{"x", "y", "z", "rotation", "scale_x",
                                       "scale_y", "scale"}}}},
      {"unit", QJsonObject{{"enum", QJsonArray{"inch", "degree", "scalar",
                                               "native_z"}}}}};
  const auto targetSchema =
      strictObjectSchema(target, {"object_id", "channel", "unit"});
  catalog.append(wholeCurveOperation(
      "animation.copy_curve",
      "Copy a supported whole curve into an independently owned destination "
      "channel.",
      merge(documentSchema(),
            {{"source", targetSchema},
             {"destination", targetSchema},
             {"destination_policy", QJsonObject{{"const", "replace_all"}}}}),
      {"document_id", "source", "destination", "destination_policy"}));
  const auto scale =
      strictObjectSchema({{"numerator", integerSchema(1, MaxFrame)},
                          {"denominator", integerSchema(1, MaxFrame)}},
                         {"numerator", "denominator"});
  catalog.append(wholeCurveOperation(
      "animation.retime_curve",
      "Apply exact rational timing to one whole curve, preserving its values "
      "and ownership.",
      merge(merge(documentSchema(), target),
            {{"origin_frame", integerSchema(0, MaxFrame)},
             {"shift_frames", integerSchema(-MaxFrame, MaxFrame)},
             {"scale", scale},
             {"step_policy", QJsonObject{{"const", "scale_exact"}}}}),
      {"document_id", "object_id", "channel", "unit", "origin_frame",
       "shift_frames", "scale", "step_policy"}));
}

void addAnimationOperations(QJsonArray& catalog) {
  QJsonObject object = merge(documentSchema(), {{"object_id", textSchema()}});
  catalog.append(operation(
      "object.create", "Create a native pegbar with a stable object handle.",
      merge(documentSchema(),
            {{"name", textSchema()}, {"kind", enumSchema({"pegbar"})}}),
      {"document_id", "name", "kind"}, true));
  catalog.append(operation(
      "object.parent",
      "Reparent an object and optionally set its parent handle. Local keys "
      "are kept, so stage placement may move; report_frames returns the "
      "world changes.",
      merge(object,
            {{"parent_id", textSchema()},
             {"placement_policy", QJsonObject{{"const", "preserve_local"}}},
             {"parent_handle", automationHandleInputSchema()},
             {"report_frames", automationReportFramesSchema()}}),
      {"document_id", "object_id", "parent_id", "placement_policy"}, true));
  const QJsonObject layer{
      {"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
  const QJsonObject readTargets{{"object_id", textSchema()},
                                {"layer_id", layer},
                                {"fx_id", textSchema()},
                                {"column_id", textSchema()}};
  catalog.append(operation(
      "animation.channels",
      "Discover animation channels, units, bounds, edit admission and the "
      "target and tool for each: the stage channels of object_id (layer_id "
      "lists that offset layer's channels and weight), the numeric parameters "
      "of fx_id, or the time remap curve of a level column_id. Name exactly "
      "one of object_id, fx_id and column_id; effect and remap curves are "
      "read only here.",
      merge(documentSchema(), readTargets), {"document_id"}));
  QJsonObject animation = merge(
      object,
      {{"channel", enumSchema({"x", "y", "z", "rotation", "scale_x", "scale_y",
                               "scale", "path_position", "weight"})},
       {"unit",
        enumSchema({"inch", "degree", "scalar", "native_z", "percent"})},
       {"layer_id", layer}});
  const QJsonArray required{"document_id", "object_id", "channel", "unit"};
  QJsonObject readable =
      merge(documentSchema(),
            merge(readTargets, {{"channel", QJsonObject{{"type", "string"},
                                                        {"minLength", 1},
                                                        {"maxLength", 128}}},
                                {"unit", QJsonObject{{"type", "string"},
                                                     {"minLength", 1},
                                                     {"maxLength", 64}}}}));
  catalog.append(operation(
      "animation.keys",
      "Read native curve keys with explicit units, for a stage channel, an "
      "offset layer curve, an effect parameter (fx_id) or a time remap curve "
      "(column_id); take channel and unit from animation.channels.",
      readable, {"document_id", "channel", "unit"}));
  QJsonArray withFrames = required;
  withFrames.append("frames");
  const QJsonObject frames =
      merge(animation, {{"frames", arraySchema(integerSchema(0, MaxFrame))}});
  catalog.append(operation(
      "animation.sample",
      "Sample effective local channel values with their base and offset-layer "
      "parts at integral or fractional frames; layer_id samples one layer "
      "curve; fx_id and column_id sample an effect parameter or a time remap "
      "curve.",
      merge(readable, {{"frames", arraySchema(numberSchema(0, MaxFrame))}}),
      {"document_id", "channel", "unit", "frames"}));
  catalog.append(operation("animation.delete_keys",
                           "Delete existing keys after complete validation.",
                           frames, withFrames, true));
  QJsonObject stage = animation;
  stage.remove("layer_id");
  stage.insert("channel", enumSchema({"x", "y", "z", "rotation", "scale_x",
                                      "scale_y", "scale"}));
  stage.insert("unit", enumSchema({"inch", "degree", "scalar", "native_z"}));
  catalog.append(operation(
      "animation.insert_breakdown",
      "Insert a key inside a segment as the editor's Set Breakdown Key: "
      "favour_percent (-100 to 200) of the way from the previous key's value "
      "to the next one's, or an explicit value. The segment keeps its "
      "interpolation and step; other keys are unchanged.",
      merge(stage, {{"frame", integerSchema(0, MaxFrame)},
                    {"favour_percent", numberSchema(-100, 200)},
                    {"value", numberSchema(-1000000, 1000000)}}),
      {"document_id", "object_id", "channel", "unit", "frame"}, true));
  const QJsonObject keyFrames{
      {"frames", arraySchema(integerSchema(0, MaxFrame))},
      {"frame_offset", integerSchema(-MaxFrame, MaxFrame)},
      {"value_offset", numberSchema(-2000000, 2000000)}};
  catalog.append(operation(
      "animation.move_keys",
      "Shift existing keys by frame_offset frames and value_offset, as the "
      "editor's key drag: each moved key keeps its interpolation, step and "
      "handles, the key order is kept, and every other key is unchanged. A "
      "shift that lands on or crosses another key, leaves a handle outside "
      "its segment or leaves the bounds is refused.",
      merge(stage, keyFrames),
      {"document_id", "object_id", "channel", "unit", "frames"}, true));
  catalog.append(operation(
      "animation.duplicate_keys",
      "Copy existing keys verbatim to frame_offset frames later (or earlier) "
      "with value_offset added, as the editor's copy and paste. A copy may "
      "not land on an existing key, and no key's incoming control kind may "
      "change; existing keys are unchanged.",
      merge(stage, keyFrames),
      {"document_id", "object_id", "channel", "unit", "frames", "frame_offset"},
      true));
  catalog.append(operation(
      "animation.set_cycle",
      "Turn the editor's Cycle on or off for a stage base channel curve: past "
      "its last key the curve repeats its key span, adding the last-minus-"
      "first key value each repeat. Keys, default value and every sample "
      "inside the key span are unchanged; the result reports cycle.",
      merge(stage, {{"enabled", QJsonObject{{"type", "boolean"}}}}),
      {"document_id", "object_id", "channel", "unit", "enabled"}, true));
  QJsonArray withKeys = required;
  withKeys.append("keys");
  catalog.append(operation(
      "animation.set_keys",
      "Replace sparse keys with explicit steps and independent incoming/outgoing controls.",
      merge(animation, {{"keys", arraySchema(animationKeySchema())}}), withKeys, true));
}

}  // namespace
/// Return the typed catalog for detached scene and animation operations.
QJsonArray sceneOperations() {
  QJsonArray catalog;
  addSceneOperations(catalog);
  addCameraOperations(catalog);
  addLevelOperations(catalog);
  addColumnOperations(catalog);
  addCellOperations(catalog);
  addAnimationOperations(catalog);
  addWholeCurveOperations(catalog);
  return catalog;
}

/// Execute one scene operation against its explicitly addressed document.
///
/// Args:
///   context: Worker-owned project, document registry and path authority.
///   method: Catalog operation name.
///   params: Validated JSON arguments including document-scoped handles.
///
/// Returns:
///   Native inspection data or a typed mutation result.
///
/// Throws:
///   Error: A preflight constraint or native operation fails.
QJsonValue executeScene(Context& context, const QString& method,
                        const QJsonObject& params) {
  static const SceneHandlers handlers = [] {
    SceneHandlers result = {{"project.inspect", inspectProject},
                            {"scene.create", createScene},
                            {"scene.summary", sceneSummary},
                            {"scene.settings", updateSettings},
                            {"scene.discard", discardScene},
                            {"object.create", createObject},
                            {"object.parent", parentObject},
                            {"animation.channels", inspectAnimationChannels},
                            {"animation.keys", inspectKeys},
                            {"animation.copy_curve", copyAnimationCurve},
                            {"animation.retime_curve", retimeAnimationCurve},
                            {"animation.set_keys", setKeys},
                            {"animation.delete_keys", deleteKeys},
                            {"animation.insert_breakdown", insertBreakdown},
                            {"animation.move_keys", moveKeys},
                            {"animation.set_cycle", setCycle},
                            {"animation.duplicate_keys", duplicateKeys},
                            {"animation.sample", sampleAnimation}};
    addCameraHandlers(result);
    addLevelHandlers(result);
    addColumnHandlers(result);
    addCellHandlers(result);
    return result;
  }();
  const auto handler = handlers.find(method);
  if (handler == handlers.end())
    throw Error("unknown_method", "Unknown scene operation: " + method);
  return handler->second(context, params);
}

}  // namespace GroundzeroAutomation
