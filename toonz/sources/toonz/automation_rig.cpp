#include "automation_rig.h"
#include "automation.h"
#include "automation_animation.h"
#include "automation_hook.h"

#include "tdoubleparam.h"
#include "tstream.h"
#include "toonz/stageunits.h"
#include "toonz/toonzscene.h"
#include "toonz/tpinnedrangeset.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshsimplelevel.h"

#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxObjects            = 4096;
constexpr int MaxDepth              = 64;
constexpr int MaxLayers             = 16;
constexpr int MaxSceneLayers        = 1024;
constexpr int MaxPairs              = 512;
constexpr int DefaultPage           = 128;
constexpr int MaxBindings           = 512;
constexpr int MaxInspectionBytes    = 1024 * 1024;
constexpr double MaxWeight          = 10;
constexpr double MaxValue           = 1000000;
constexpr double MaxPivotCoordinate = 1000;
constexpr double PreservedInch      = 1e-6;
constexpr double PreservedLinear    = 1e-9;

using Channel = TStageObject::Channel;
using Layer   = TStageObject::OffsetLayer;

struct ChannelInfo final {
  const char* name;
  Channel native;
  const char* unit;
};

const ChannelInfo Channels[] = {{"x", TStageObject::T_X, "inch"},
                                {"y", TStageObject::T_Y, "inch"},
                                {"z", TStageObject::T_Z, "native_z"},
                                {"rotation", TStageObject::T_Angle, "degree"},
                                {"scale_x", TStageObject::T_ScaleX, "scalar"},
                                {"scale_y", TStageObject::T_ScaleY, "scalar"},
                                {"scale", TStageObject::T_Scale, "scalar"}};

const Channel Unexposed[] = {TStageObject::T_SO,     TStageObject::T_ShearX,
                             TStageObject::T_ShearY, TStageObject::T_RotX,
                             TStageObject::T_RotY,   TStageObject::T_Path,
                             TStageObject::T_FocusZ, TStageObject::T_Aperture};

struct Pivot final {
  TPointD center;
  TPointD offset;
};

[[noreturn]] void invalid(const QString& message,
                          const QJsonObject& details = {}) {
  throw Error("invalid_params", message, details);
}

TStageObjectTree& stageOf(ToonzScene& scene) {
  return *scene.getXsheet()->getStageObjectTree();
}

int depthOf(TStageObjectTree& stage, const TStageObject& object) {
  int depth = 0;
  for (TStageObjectId parent = object.getParent();
       parent != TStageObjectId::NoneId;) {
    if (++depth > MaxDepth)
      throw Error("resource_limit",
                  "Stage hierarchies allow at most 64 parent edges and no "
                  "cycles.");
    const TStageObject* next = stage.getStageObject(parent, false);
    if (!next) invalid("A stage object has an unknown parent.");
    parent = next->getParent();
  }
  return depth;
}

bool letterHandle(const std::string& handle) {
  return handle.size() == 1 && (('A' <= handle[0] && handle[0] <= 'Z') ||
                                ('a' <= handle[0] && handle[0] <= 'z'));
}

int hookNumber(const std::string& handle) {
  if (handle.size() < 2 || handle.size() > 3 || handle[0] != 'H' ||
      handle[1] == '0')
    return 0;
  int number = 0;
  for (std::size_t index = 1; index < handle.size(); ++index) {
    if (handle[index] < '0' || handle[index] > '9') return 0;
    number = number * 10 + (handle[index] - '0');
  }
  return number;
}

TPointD letterOffset(char letter) {
  if ('A' <= letter && letter <= 'Z') return TPointD(8.0 * (letter - 'B'), 0);
  return TPointD(4.0 * (letter - 'b'), 0);
}

TPointD handlePosition(TStageObjectTree& stage, const TStageObjectId& id,
                       const std::string& handle, int row) {
  if (hookNumber(handle)) return stage.getHandlePos(id, handle, row);
  return letterOffset(handle[0]);
}

std::string handleString(const QJsonObject& input) {
  if (input.value("kind") == "hook")
    return "H" + std::to_string(input.value("hook_number").toInt());
  return input.value("letter").toString().toStdString();
}

bool parentIsColumn(const TStageObject& object) {
  const TStageObjectId parent = object.getParent();
  return parent != TStageObjectId::NoneId && parent.isColumn();
}

void validateHandles(const TStageObject& object) {
  const std::string& own = object.getHandle();
  if (!letterHandle(own) && !(hookNumber(own) && object.getId().isColumn()))
    invalid("Own handles must be peg-hole letters, or H1-H99 on columns.",
            {{"handle", QString::fromStdString(own)}});
  const std::string& parent = object.getParentHandle();
  if (!letterHandle(parent) && !(hookNumber(parent) && parentIsColumn(object)))
    invalid(
        "Parent handles must be peg-hole letters, or H1-H99 under a column.",
        {{"parent_handle", QString::fromStdString(parent)}});
}

void requireName(const QString& name) {
  const auto characters = name.toUcs4();
  if (characters.isEmpty() || characters.size() > 256)
    invalid("Names require between 1 and 256 Unicode characters.");
  for (const uint character : characters)
    if (character < 0x20 || (character >= 0x7f && character < 0xa0))
      invalid("Names cannot contain control characters.");
}

void validateBoundedCurve(const TDoubleParam& curve, double bound) {
  requireSupportedAnimationCurve(curve);
  const double fallback = curve.getDefaultValue();
  if (!std::isfinite(fallback) || std::abs(fallback) > bound)
    invalid("An offset curve default is outside its bounds.");
  for (int index = 0; index < curve.getKeyframeCount(); ++index)
    if (std::abs(curve.getKeyframe(index).m_value) > bound)
      invalid("An offset curve key is outside its bounds.");
}

void validateLayer(const Layer& layer) {
  requireName(QString::fromStdWString(layer.m_name));
  if (!layer.m_weight) invalid("An offset layer has no weight curve.");
  validateBoundedCurve(*layer.m_weight, MaxWeight);
  for (const auto& channel : Channels) {
    const TDoubleParam* curve = layer.m_channels[channel.native].getPointer();
    if (!curve) invalid("An offset layer is missing an exposed channel.");
    validateBoundedCurve(*curve, MaxValue);
  }
  for (const Channel channel : Unexposed) {
    const TDoubleParam* curve = layer.m_channels[channel].getPointer();
    if (curve &&
        (curve->getKeyframeCount() != 0 || curve->getDefaultValue() != 0))
      invalid(
          "SO, shear and tilt offsets are not exposed and must stay at 0 "
          "without keys.");
  }
}

bool followsPath(const TStageObject& object) {
  const int status = object.getStatus() & TStageObject::STATUS_MASK;
  return object.getSpline() || status == TStageObject::PATH ||
         status == TStageObject::PATH_AIM;
}

bool isIk(const TStageObject& object) {
  return (object.getStatus() & TStageObject::STATUS_MASK) == TStageObject::IK;
}

void validateObjectLayers(const TStageObject& object, int& total) {
  const int count = object.getOffsetLayerCount();
  if (count == 0) return;
  if (object.getId().isTable() || followsPath(object))
    throw Error("unsupported_target",
                "The table and objects that follow a path cannot carry "
                "offset layers.");
  if (count > MaxLayers)
    throw Error("resource_limit", "An object holds at most 16 offset layers.");
  total += count;
  if (total > MaxSceneLayers)
    throw Error("resource_limit", "A scene holds at most 1024 offset layers.");
  std::set<std::wstring> names;
  for (int index = 0; index < count; ++index) {
    const Layer& layer = object.getOffsetLayer(index);
    validateLayer(layer);
    if (!names.insert(layer.m_name).second)
      invalid("Offset layer names must be unique on their object.");
  }
}

void validateObject(TStageObject& object, int& layers) {
  if (isIk(object) || object.getPinnedRangeSet()->getRangeCount() != 0)
    throw Error("unsupported_target",
                "IK status and pinned ranges are not admitted in this "
                "profile.");
  if (object.getPlasticSkeletonDeformation())
    throw Error("unsupported_target",
                "Plastic deformations are not admitted in this profile.");
  if (object.hasPublishedChannels())
    throw Error("unsupported_target",
                "Published channels are not admitted in this profile.");
  validateHandles(object);
  validateObjectLayers(object, layers);
}

bool objectLocked(ToonzScene& scene, const TStageObject& object) {
  const TStageObjectId id = object.getId();
  TXsheet* sheet          = scene.getXsheet();
  bool locked             = object.isLocked();
  if (id.isColumn()) {
    const TXshColumn* column = sheet->getColumn(id.getIndex());
    locked                   = locked || (column && column->isLocked());
  }
  if (id.isCamera()) locked = locked || sheet->isCameraColumnLocked();
  return locked;
}

void requireUnlocked(ToonzScene& scene, const TStageObject& object) {
  if (objectLocked(scene, object))
    throw Error("locked", "Unlock the object and its column first.");
}

Document& rigDocument(Context& context, const QJsonObject& params) {
  auto& document = context.document(requiredString(params, "document_id"));
  if (document.revisionText() != requiredString(params, "expected_revision"))
    throw Error(
        "revision_conflict",
        "The document changed; inspect its current revision first.",
        {{"document_id", document.id}, {"revision", document.revisionText()}});
  validateAutomationRig(*document.scene);
  return document;
}

QJsonObject pointJson(const TPointD& point) {
  return {{"x", point.x + 0.0}, {"y", point.y + 0.0}};
}

QJsonObject affineJson(const TAffine& affine) {
  return {{"a11", affine.a11},
          {"a12", affine.a12},
          {"a21", affine.a21},
          {"a22", affine.a22},
          {"dx_inch", affine.a13 / Stage::inch},
          {"dy_inch", affine.a23 / Stage::inch}};
}

QString registerLayer(Document& document, const TDoubleParamP& weight) {
  for (auto entry = document.offsetLayers.constBegin();
       entry != document.offsetLayers.constEnd(); ++entry)
    if (entry.value().getPointer() == weight.getPointer()) return entry.key();
  const QString id =
      "layer_" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  document.offsetLayers.insert(id, weight);
  return id;
}

QString layerId(Document& document, const Layer& layer) {
  for (auto entry = document.offsetLayers.constBegin();
       entry != document.offsetLayers.constEnd(); ++entry)
    if (entry.value().getPointer() == layer.m_weight.getPointer())
      return entry.key();
  throw Error("invalid_handle", "An offset layer has no published handle.");
}

int sceneLayers(TStageObjectTree& stage) {
  int total = 0;
  for (int index = 0; index < stage.getStageObjectCount(); ++index)
    total += stage.getStageObject(index)->getOffsetLayerCount();
  return total;
}

QJsonObject curveSummary(const TDoubleParam& curve) {
  return {{"default_value", curve.getDefaultValue()},
          {"key_count", curve.getKeyframeCount()}};
}

QJsonObject layerRecord(Document& document, TStageObject& object, int index) {
  const Layer& layer = object.getOffsetLayer(index);
  QJsonArray channels;
  for (const auto& channel : Channels) {
    auto summary = curveSummary(*layer.m_channels[channel.native]);
    summary.insert("channel", channel.name);
    summary.insert("unit", channel.unit);
    channels.append(summary);
  }
  return {{"layer_id", layerId(document, layer)},
          {"object_id", document.idForObject(&object)},
          {"name", QString::fromStdWString(layer.m_name)},
          {"index", index},
          {"enabled", layer.m_enabled},
          {"weight", curveSummary(*layer.m_weight)},
          {"channels", channels}};
}

std::vector<TStageObject*> sortedChildren(const TStageObject& object) {
  std::vector<TStageObject*> children(object.getChildren().begin(),
                                      object.getChildren().end());
  std::sort(children.begin(), children.end(),
            [](const TStageObject* left, const TStageObject* right) {
              return left->getId() < right->getId();
            });
  return children;
}

QString kindName(const TStageObjectId& id) {
  if (id.isColumn()) return "column";
  if (id.isCamera()) return "camera";
  if (id.isPegbar()) return "pegbar";
  return "table";
}

QString statusName(const TStageObject& object) {
  const int status = object.getStatus() & TStageObject::STATUS_MASK;
  if (status == TStageObject::PATH) return "path";
  if (status == TStageObject::PATH_AIM) return "path_aim";
  return "xy";
}

const std::vector<TXshSimpleLevel*>& exposedLevels(
    const AutomationExposures& exposures, int column) {
  static const std::vector<TXshSimpleLevel*> none;
  const auto found = exposures.levels.find(column);
  return found == exposures.levels.end() ? none : found->second;
}

void appendBindings(Document& document, const AutomationExposures& exposures,
                    const QString& role, int number, int column,
                    QJsonArray& bindings) {
  for (TXshSimpleLevel* level : exposedLevels(exposures, column))
    bindings.append(
        QJsonObject{{"role", role},
                    {"hook_number", number},
                    {"level_id", document.idForLevel(level)},
                    {"hook_defined", automationHookDefined(*level, number)}});
}

QJsonArray hookBindings(Document& document,
                        const AutomationExposures& exposures,
                        const TStageObject& object) {
  QJsonArray bindings;
  const int own = hookNumber(object.getHandle());
  if (own && object.getId().isColumn())
    appendBindings(document, exposures, "own_handle", own,
                   object.getId().getIndex(), bindings);
  const int parent = hookNumber(object.getParentHandle());
  if (parent && parentIsColumn(object))
    appendBindings(document, exposures, "parent_handle", parent,
                   object.getParent().getIndex(), bindings);
  if (bindings.size() > MaxBindings)
    throw Error("resource_limit", "An object has more than 512 bindings.");
  return bindings;
}

QJsonObject objectRecord(Document& document,
                         const AutomationExposures& exposures,
                         TStageObject& object) {
  auto& stage             = stageOf(*document.scene);
  const TStageObjectId id = object.getId();
  QJsonValue column;
  QJsonArray levels, children;
  if (id.isColumn()) {
    if (TXshColumn* native =
            document.scene->getXsheet()->getColumn(id.getIndex()))
      column = document.idForColumn(native);
    for (TXshSimpleLevel* level : exposedLevels(exposures, id.getIndex()))
      levels.append(document.idForLevel(level));
  }
  for (TStageObject* child : sortedChildren(object))
    children.append(document.idForObject(child));
  TStageObject* parent = object.getParent() == TStageObjectId::NoneId
                             ? nullptr
                             : stage.getStageObject(object.getParent(), false);
  TPointD center, offset;
  object.getCenterAndOffset(center, offset);
  return {{"object_id", document.idForObject(&object)},
          {"kind", kindName(id)},
          {"name", QString::fromStdString(object.getName())},
          {"column_id", column},
          {"parent_id",
           parent ? QJsonValue(document.idForObject(parent)) : QJsonValue()},
          {"depth", depthOf(stage, object)},
          {"children", children},
          {"handle", automationHandleRecord(object.getHandle())},
          {"parent_handle", automationHandleRecord(object.getParentHandle())},
          {"pivot", QJsonObject{{"center", pointJson(center)},
                                {"offset", pointJson(offset)}}},
          {"status", statusName(object)},
          {"locked", objectLocked(*document.scene, object)},
          {"levels_exposed", levels},
          {"hook_bindings", hookBindings(document, exposures, object)},
          {"offset_layers", automationLayerSummaries(document, object)}};
}

bool inSubtree(TStageObjectTree& stage, const TStageObject& object,
               const TStageObject& root) {
  const TStageObject* current = &object;
  for (int depth = 0; current && depth <= MaxDepth; ++depth) {
    if (current == &root) return true;
    const TStageObjectId parent = current->getParent();
    current                     = parent == TStageObjectId::NoneId
                                      ? nullptr
                                      : stage.getStageObject(parent, false);
  }
  return false;
}

std::vector<TStageObject*> listedObjects(Document& document,
                                         const QJsonObject& params) {
  auto& stage = stageOf(*document.scene);
  const TStageObject* root =
      params.contains("root_object_id")
          ? document.object(requiredString(params, "root_object_id"))
          : nullptr;
  std::vector<TStageObject*> objects;
  for (int index = 0; index < stage.getStageObjectCount(); ++index) {
    TStageObject* object = stage.getStageObject(index);
    if (!root || inSubtree(stage, *object, *root)) objects.push_back(object);
  }
  return objects;
}

QJsonValue inspectRig(Context& context, const QJsonObject& params) {
  auto& document       = rigDocument(context, params);
  const auto exposures = automationExposures(*document.scene);
  const auto objects   = listedObjects(document, params);
  const int total      = static_cast<int>(objects.size());
  const int start      = params.value("start_index").toInt(0);
  const int limit      = params.value("limit").toInt(DefaultPage);
  if (start > 0 && start >= total)
    invalid("start_index is beyond the last listed object.");
  QJsonArray records;
  qint64 bytes = 4096;
  for (int index = start; index < std::min(total, start + limit); ++index) {
    const auto record = objectRecord(document, exposures, *objects[index]);
    bytes += QJsonDocument(record).toJson(QJsonDocument::Compact).size() + 1;
    if (bytes > MaxInspectionBytes)
      throw Error("resource_limit",
                  "The rig page exceeds 1 MiB; retry with a smaller limit.");
    records.append(record);
  }
  const int next = start + static_cast<int>(records.size());
  return QJsonObject{
      {"document_id", document.id},
      {"revision", document.revisionText()},
      {"total_objects", total},
      {"start_index", start},
      {"next_index", next < total ? QJsonValue(next) : QJsonValue()},
      {"objects", records}};
}

void refreshPlacements(ToonzScene& scene) {
  auto& stage = stageOf(scene);
  for (int index = 0; index < stage.getStageObjectCount(); ++index) {
    TStageObject* object = stage.getStageObject(index);
    if (object->getParent() == TStageObjectId::NoneId) object->invalidate();
  }
}

QJsonArray channelValues(const TStageObject& object, double frame) {
  QJsonArray channels;
  for (const auto& channel : Channels) {
    const double base   = object.getParam(channel.native)->getValue(frame);
    const double offset = object.getOffsetValue(channel.native, frame);
    channels.append(QJsonObject{{"channel", channel.name},
                                {"unit", channel.unit},
                                {"base", base},
                                {"offset", offset},
                                {"effective", base + offset}});
  }
  return channels;
}

QJsonObject sample(Document& document, TStageObject& object, int frame) {
  auto& stage             = stageOf(*document.scene);
  const TAffine placement = object.getPlacement(frame);
  const double time       = object.paramsTime(frame);
  const int row           = static_cast<int>(time);
  const TPointD own =
      handlePosition(stage, object.getId(), object.getHandle(), row);
  TPointD center, offset;
  object.getCenterAndOffset(center, offset);
  const TPointD pivot =
      placement * ((center + own) * Stage::inch) * (1.0 / Stage::inch);
  QJsonValue attachment;
  if (object.getParent() != TStageObjectId::NoneId)
    attachment = pointJson(handlePosition(stage, object.getParent(),
                                          object.getParentHandle(), row));
  return {{"object_id", document.idForObject(&object)},
          {"frame", frame},
          {"placement", affineJson(placement)},
          {"pivot_stage", pointJson(pivot)},
          {"own_handle_local", pointJson(own)},
          {"parent_attachment_local", attachment},
          {"channels", channelValues(object, time)}};
}

std::vector<TStageObject*> evaluatedObjects(Document& document,
                                            const QJsonObject& params) {
  std::vector<TStageObject*> objects;
  QSet<QString> seen;
  for (const auto& value : params.value("object_ids").toArray()) {
    if (seen.contains(value.toString())) invalid("object_ids must be unique.");
    seen.insert(value.toString());
    TStageObject* object = document.object(value.toString());
    if (isIk(*object))
      throw Error("unsupported_target", "IK evaluation is not qualified.");
    objects.push_back(object);
  }
  return objects;
}

std::vector<int> uniqueFrames(const QJsonArray& values, const QString& field) {
  std::vector<int> frames;
  QSet<int> seen;
  for (const auto& value : values) {
    if (seen.contains(value.toInt())) invalid(field + " must be unique.");
    seen.insert(value.toInt());
    frames.push_back(value.toInt());
  }
  return frames;
}

QJsonValue evaluateRig(Context& context, const QJsonObject& params) {
  auto& document     = rigDocument(context, params);
  const auto objects = evaluatedObjects(document, params);
  const auto frames  = uniqueFrames(params.value("frames").toArray(), "frames");
  if (objects.size() * frames.size() > MaxPairs)
    throw Error("resource_limit", "Evaluate at most 512 object-frame pairs.");
  refreshPlacements(*document.scene);
  QJsonArray samples;
  for (TStageObject* object : objects)
    for (const int frame : frames)
      samples.append(sample(document, *object, frame));
  const QJsonObject result{{"document_id", document.id},
                           {"revision", document.revisionText()},
                           {"unit", "inch"},
                           {"space", "stage"},
                           {"samples", samples}};
  if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() >
      MaxInspectionBytes)
    throw Error("resource_limit", "The evaluation exceeds 1 MiB.");
  return result;
}

void requireColumnHook(Document& document, int column, int number) {
  TStageObject* owner =
      stageOf(*document.scene)
          .getStageObject(TStageObjectId::ColumnId(column), false);
  if (owner && owner->getPlasticSkeletonDeformation())
    throw Error("unsupported_target",
                "Hook handles on Plastic-deformed columns are not admitted.");
  const auto exposures = automationExposures(*document.scene);
  QJsonArray missing;
  for (TXshSimpleLevel* level : exposedLevels(exposures, column))
    if (!automationHookDefined(*level, number))
      missing.append(document.idForLevel(level));
  if (!missing.isEmpty())
    invalid("The hook number has no keys in every level the column exposes.",
            {{"hook_number", number}, {"levels", missing}});
}

void requireOwnHandle(Document& document, const TStageObject& object,
                      const std::string& handle) {
  const int number = hookNumber(handle);
  if (!number) return;
  if (!object.getId().isColumn())
    invalid("Hook handles require a column object.");
  requireColumnHook(document, object.getId().getIndex(), number);
}

void requireParentHandle(Document& document, const TStageObjectId& parent,
                         const std::string& handle) {
  const int number = hookNumber(handle);
  if (!number) return;
  if (parent == TStageObjectId::NoneId || !parent.isColumn())
    invalid("Hook parent handles require a column parent.");
  requireColumnHook(document, parent.getIndex(), number);
}

QJsonValue setHandles(Context& context, const QJsonObject& params) {
  auto& document       = rigDocument(context, params);
  auto& scene          = *document.scene;
  TStageObject* object = document.object(requiredString(params, "object_id"));
  const TStageObjectId id = object->getId();
  if (id.isTable() || id.isCamera() || object->getPlasticSkeletonDeformation())
    throw Error("unsupported_target",
                "Handles are set on pegbars and non-Plastic columns only.");
  if (!params.contains("handle") && !params.contains("parent_handle"))
    invalid("Supply handle, parent_handle or both.");
  requireUnlocked(scene, *object);
  const auto frames = automationReportFrames(params);
  std::string own = object->getHandle(), parent = object->getParentHandle();
  if (params.contains("handle")) {
    own = handleString(params.value("handle").toObject());
    requireOwnHandle(document, *object, own);
  }
  if (params.contains("parent_handle")) {
    parent = handleString(params.value("parent_handle").toObject());
    requireParentHandle(document, object->getParent(), parent);
  }
  TPointD center, offset;
  object->getCenterAndOffset(center, offset);
  const auto before        = automationPlacements(scene, *object, frames);
  const bool ownChanged    = own != object->getHandle();
  const bool parentChanged = parent != object->getParentHandle();
  if (ownChanged || parentChanged) context.markMutationStarted();
  if (ownChanged) object->setHandle(own);
  if (parentChanged) object->setParentHandle(parent);
  QJsonObject result{
      {"object_id", params.value("object_id")},
      {"handle", automationHandleRecord(own)},
      {"parent_handle", automationHandleRecord(parent)},
      {"pivot_reset",
       QJsonObject{{"occurred", ownChanged && hookNumber(own) != 0},
                   {"previous_center", pointJson(center)},
                   {"previous_offset", pointJson(offset)}}},
      {"keys_rewritten", 0}};
  insertAutomationWorldChanges(result, frames, before,
                               automationPlacements(scene, *object, frames));
  return result;
}

Pivot pivotRoundTrip(const Pivot& pivot, const QString& file) {
  const TFilePath path(file.toStdWString());
  {
    TOStream output(path);
    output.child("center") << pivot.center.x << pivot.center.y
                           << -pivot.offset.x << -pivot.offset.y;
  }
  TIStream input(path);
  std::string tag;
  if (!input || !input.matchTag(tag) || tag != "center")
    throw Error("native_error", "Native pivot serialization failed.");
  Pivot result;
  input >> result.center.x >> result.center.y >> result.offset.x >>
      result.offset.y;
  result.offset = -result.offset;
  return result;
}

Pivot canonicalPivot(Context& context, const Pivot& pivot) {
  QTemporaryDir directory(context.workspace() + "/pivot-canonical-XXXXXX");
  if (!directory.isValid())
    throw Error("io_error", "Cannot create private pivot preparation storage.");
  const Pivot first  = pivotRoundTrip(pivot, directory.filePath("first.xml"));
  const Pivot second = pivotRoundTrip(first, directory.filePath("second.xml"));
  if (first.center != second.center || first.offset != second.offset)
    invalid("Native pivot coordinates are not stable.");
  return first;
}

QJsonValue setPivot(Context& context, const QJsonObject& params) {
  auto& document       = rigDocument(context, params);
  auto& scene          = *document.scene;
  TStageObject* object = document.object(requiredString(params, "object_id"));
  if (!object->getId().isPegbar() && !object->getId().isColumn())
    throw Error("unsupported_target", "Pivots are set on pegbars and columns.");
  requireUnlocked(scene, *object);
  const auto frames = automationReportFrames(params);
  const auto input  = params.value("center").toObject();
  const TPointD wanted(input.value("x").toDouble(),
                       input.value("y").toDouble());
  Pivot current;
  object->getCenterAndOffset(current.center, current.offset);
  const Pivot requested = canonicalPivot(context, {wanted, current.offset});
  const double error =
      std::hypot(requested.center.x - wanted.x, requested.center.y - wanted.y);
  const double limit = params.value("max_coordinate_error_inch").toDouble();
  if (error > limit)
    throw Error(
        "invalid_params",
        "Native pivot precision exceeds the caller's coordinate error limit.",
        {{"max_input_error_inch", error}, {"maximum", limit}});
  const auto before = automationPlacements(scene, *object, frames);
  if (requested.center != current.center) {
    context.markMutationStarted();
    const auto policy = params.value("policy").toObject();
    if (policy.value("kind") == "preserve_world_at_frame") {
      const int frame = policy.value("frame").toInt();
      object->setCenter(
          frame,
          requested.center + handlePosition(stageOf(scene), object->getId(),
                                            object->getHandle(), frame));
    } else {
      object->setCenterAndOffset(requested.center, current.offset);
    }
    Pivot moved;
    object->getCenterAndOffset(moved.center, moved.offset);
    const Pivot stored = canonicalPivot(context, moved);
    object->setCenterAndOffset(stored.center, stored.offset);
  }
  Pivot stored;
  object->getCenterAndOffset(stored.center, stored.offset);
  QJsonObject result{{"object_id", params.value("object_id")},
                     {"unit", "inch"},
                     {"center", pointJson(stored.center)},
                     {"offset", pointJson(stored.offset)},
                     {"max_input_error_inch", error},
                     {"keys_rewritten", 0}};
  insertAutomationWorldChanges(result, frames, before,
                               automationPlacements(scene, *object, frames));
  return result;
}

void requireLayerOwner(ToonzScene& scene, const TStageObject& object) {
  if (object.getId().isTable() || followsPath(object) || isIk(object))
    throw Error("unsupported_target",
                "Offset layers need a pegbar, column or camera that does not "
                "follow a path.");
  requireUnlocked(scene, object);
}

QString layerName(const QJsonObject& params) {
  const QString name = params.value("name").toString();
  requireName(name);
  return name;
}

QJsonValue createLayer(Context& context, const QJsonObject& params) {
  auto& document       = rigDocument(context, params);
  auto& stage          = stageOf(*document.scene);
  TStageObject* object = document.object(requiredString(params, "object_id"));
  requireLayerOwner(*document.scene, *object);
  const QString name = layerName(params);
  if (object->findOffsetLayer(name.toStdWString()) >= 0)
    throw Error("name_conflict", "The object already has a layer named so.");
  const int count = object->getOffsetLayerCount();
  if (count >= MaxLayers)
    throw Error("resource_limit", "An object holds at most 16 offset layers.");
  if (sceneLayers(stage) >= MaxSceneLayers)
    throw Error("resource_limit", "A scene holds at most 1024 offset layers.");
  const int index = params.value("index").toInt(count);
  if (index > count) invalid("index must be at most the current layer count.");
  Layer layer     = object->createOffsetLayer(name.toStdWString());
  layer.m_enabled = params.value("enabled").toBool(true);
  if (params.contains("weight"))
    layer.m_weight->setDefaultValue(params.value("weight").toDouble());
  context.markMutationStarted();
  object->insertOffsetLayer(index, layer);
  registerLayer(document, layer.m_weight);
  return QJsonObject{{"layer", layerRecord(document, *object, index)}};
}

QJsonValue updateLayer(Context& context, const QJsonObject& params) {
  auto& document = rigDocument(context, params);
  const auto target =
      automationLayer(document, params.value("layer_id").toString());
  TStageObject& object = *target.object;
  requireUnlocked(*document.scene, object);
  if (!params.contains("name") && !params.contains("enabled") &&
      !params.contains("index"))
    invalid("Supply name, enabled or index.");
  const Layer& layer = object.getOffsetLayer(target.index);
  const std::wstring name =
      params.contains("name") ? layerName(params).toStdWString() : layer.m_name;
  if (name != layer.m_name && object.findOffsetLayer(name) >= 0)
    throw Error("name_conflict", "The object already has a layer named so.");
  const bool enabled = params.value("enabled").toBool(layer.m_enabled);
  const int index    = params.value("index").toInt(target.index);
  if (index >= object.getOffsetLayerCount())
    invalid("index must name an existing layer position.");
  if (name == layer.m_name && enabled == layer.m_enabled &&
      index == target.index)
    return QJsonObject{{"layer", layerRecord(document, object, index)}};
  context.markMutationStarted();
  if (name != layer.m_name) object.renameOffsetLayer(target.index, name);
  if (enabled != object.getOffsetLayer(target.index).m_enabled)
    object.setOffsetLayerEnabled(target.index, enabled);
  if (index != target.index)
    object.insertOffsetLayer(index, object.removeOffsetLayer(target.index));
  return QJsonObject{{"layer", layerRecord(document, object, index)}};
}

bool contributes(const TDoubleParam& curve) {
  return curve.getKeyframeCount() != 0 || curve.getDefaultValue() != 0;
}

QJsonValue deleteLayer(Context& context, const QJsonObject& params) {
  auto& document       = rigDocument(context, params);
  const QString id     = params.value("layer_id").toString();
  const auto target    = automationLayer(document, id);
  TStageObject& object = *target.object;
  requireUnlocked(*document.scene, object);
  const Layer layer = object.getOffsetLayer(target.index);
  int keys          = layer.m_weight->getKeyframeCount();
  for (int channel = 0; channel != TStageObject::T_ChannelCount; ++channel)
    if (layer.m_channels[channel])
      keys += layer.m_channels[channel]->getKeyframeCount();
  QJsonArray changed;
  if (layer.m_enabled && contributes(*layer.m_weight))
    for (const auto& channel : Channels)
      if (contributes(*layer.m_channels[channel.native]))
        changed.append(channel.name);
  context.markMutationStarted();
  object.removeOffsetLayer(target.index);
  document.offsetLayers.remove(id);
  return QJsonObject{{"layer_id", id},
                     {"object_id", document.idForObject(&object)},
                     {"removed_name", QString::fromStdWString(layer.m_name)},
                     {"removed_key_count", keys},
                     {"channels_changed", changed}};
}

QJsonObject handleSchema() {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
}

QJsonObject integerSchema(int minimum, int maximum) {
  return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}};
}

QJsonObject strictSchema(const QJsonObject& properties,
                         const QJsonArray& required) {
  return {{"type", "object"},
          {"properties", properties},
          {"required", required},
          {"additionalProperties", false}};
}

QJsonObject arraySchema(const QJsonObject& items, int minimum, int maximum) {
  return {{"type", "array"},
          {"items", items},
          {"minItems", minimum},
          {"maxItems", maximum}};
}

QJsonObject constant(const QString& value) { return {{"const", value}}; }

void addTool(QJsonArray& catalog, const QString& name,
             const QString& description, QJsonObject fields,
             QJsonArray required, bool writes) {
  fields.insert("document_id", handleSchema());
  fields.insert("expected_revision", handleSchema());
  required.prepend("expected_revision");
  required.prepend("document_id");
  if (writes) {
    fields.insert("operation_id", handleSchema());
    required.insert(2, "operation_id");
  }
  auto tool = operation(name, description, {}, {}, writes);
  tool.insert("inputSchema", strictSchema(fields, required));
  catalog.append(tool);
}

QJsonObject pivotFields() {
  const QJsonObject coordinate{{"type", "number"},
                               {"minimum", -MaxPivotCoordinate},
                               {"maximum", MaxPivotCoordinate}};
  const QJsonObject preserve =
      strictSchema({{"kind", constant("preserve_world_at_frame")},
                    {"frame", integerSchema(0, 1000000)}},
                   {"kind", "frame"});
  const QJsonObject keep =
      strictSchema({{"kind", constant("keep_offset")}}, {"kind"});
  return {{"object_id", handleSchema()},
          {"unit", constant("inch")},
          {"center",
           strictSchema({{"x", coordinate}, {"y", coordinate}}, {"x", "y"})},
          {"policy", QJsonObject{{"oneOf", QJsonArray{preserve, keep}}}},
          {"coordinate_policy", constant("native_roundtrip")},
          {"max_coordinate_error_inch",
           QJsonObject{{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
          {"report_frames", automationReportFramesSchema()}};
}

QJsonObject layerFields(bool create) {
  QJsonObject fields{
      {"name",
       QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
      {"index", integerSchema(0, MaxLayers - 1)},
      {"enabled", QJsonObject{{"type", "boolean"}}}};
  if (create) {
    fields.insert("object_id", handleSchema());
    fields.insert("weight", QJsonObject{{"type", "number"},
                                        {"minimum", -MaxWeight},
                                        {"maximum", MaxWeight}});
  } else {
    fields.insert("layer_id", handleSchema());
  }
  return fields;
}

void addStageTools(QJsonArray& catalog) {
  addTool(catalog, "rig.inspect",
          "Inspect the actual stage-object hierarchy: parents, handles, "
          "pivots, hook bindings per exposed level and offset-layer "
          "summaries. No synthetic bone objects.",
          {{"root_object_id", handleSchema()},
           {"start_index", integerSchema(0, MaxObjects - 1)},
           {"limit", integerSchema(1, 512)}},
          {}, false);
  addTool(catalog, "rig.evaluate",
          "Evaluate placements at integer frames: stage placement affine in "
          "inches, pivot in stage space, attachment point in the parent, and "
          "base/offset/effective channels.",
          {{"object_ids", arraySchema(handleSchema(), 1, 64)},
           {"frames", arraySchema(integerSchema(0, 1000000), 1, 64)}},
          {"object_ids", "frames"}, false);
  addTool(catalog, "object.handles.set",
          "Set an object's own handle (pivot origin: peg hole or column hook) "
          "and/or its parent handle (attachment point on the parent). Native "
          "semantics; no compensation.",
          {{"object_id", handleSchema()},
           {"placement_policy", constant("native_no_compensation")},
           {"report_frames", automationReportFramesSchema()},
           {"handle", automationHandleInputSchema()},
           {"parent_handle", automationHandleInputSchema()}},
          {"object_id", "placement_policy", "report_frames"}, true);
  addTool(catalog, "object.pivot.set",
          "Set the stored pivot center (relative to the own handle). Either "
          "preserve stage placement at one frame (native center move) or keep "
          "the offset (object moves).",
          pivotFields(),
          {"object_id", "unit", "center", "policy", "coordinate_policy",
           "max_coordinate_error_inch", "report_frames"},
          true);
}

void addLayerTools(QJsonArray& catalog) {
  addTool(catalog, "offset_layer.create",
          "Add an additive offset layer (weight x curves added to base "
          "channels; scale offsets add to factors). Names must be unique on "
          "the object; no automatic renaming.",
          layerFields(true), {"object_id", "name"}, true);
  addTool(catalog, "offset_layer.update",
          "Rename, enable/disable or reorder an offset layer. Its curves are "
          "unchanged.",
          layerFields(false), {"layer_id"}, true);
  addTool(catalog, "offset_layer.delete",
          "Remove an offset layer and its curves from its object.",
          {{"layer_id", handleSchema()}}, {"layer_id"}, true);
}

}  // namespace

QJsonObject automationHandleInputSchema() {
  QJsonArray letters;
  for (char letter = 'A'; letter <= 'Z'; ++letter)
    letters.append(QString(QChar(letter)));
  for (char letter = 'a'; letter <= 'z'; ++letter)
    letters.append(QString(QChar(letter)));
  const QJsonObject peg = strictSchema(
      {{"kind", constant("peg_hole")},
       {"letter", QJsonObject{{"type", "string"}, {"enum", letters}}}},
      {"kind", "letter"});
  const QJsonObject hook = strictSchema(
      {{"kind", constant("hook")}, {"hook_number", integerSchema(1, 99)}},
      {"kind", "hook_number"});
  return {{"oneOf", QJsonArray{peg, hook}}};
}

QJsonObject automationReportFramesSchema() {
  return arraySchema(integerSchema(0, 1000000), 1, 64);
}

void validateAutomationRig(ToonzScene& scene) {
  auto& stage = stageOf(scene);
  if (stage.getStageObjectCount() > MaxObjects)
    throw Error("resource_limit", "A scene holds at most 4096 stage objects.");
  int layers = 0;
  for (int index = 0; index < stage.getStageObjectCount(); ++index) {
    TStageObject* object = stage.getStageObject(index);
    depthOf(stage, *object);
    validateObject(*object, layers);
  }
}

void initializeAutomationRig(Document& document) {
  auto& stage = stageOf(*document.scene);
  for (int index = 0; index < stage.getStageObjectCount(); ++index) {
    TStageObject* object = stage.getStageObject(index);
    for (int layer = 0; layer < object->getOffsetLayerCount(); ++layer)
      registerLayer(document, object->getOffsetLayer(layer).m_weight);
  }
}

void validateAutomationStageParent(ToonzScene& scene, TStageObject& object,
                                   TStageObject& parent) {
  auto& stage    = stageOf(scene);
  const int base = depthOf(stage, parent) + 1;
  std::vector<std::pair<const TStageObject*, int>> pending{{&object, 0}};
  int visited = 0;
  while (!pending.empty()) {
    const auto [current, depth] = pending.back();
    pending.pop_back();
    if (current == &parent)
      throw Error("cycle", "The requested parent would create a cycle.");
    if (base + depth > MaxDepth)
      throw Error("resource_limit",
                  "Stage hierarchies allow at most 64 parent edges.");
    if (++visited > MaxObjects)
      throw Error("resource_limit", "The subtree exceeds 4096 objects.");
    for (const TStageObject* child : current->getChildren())
      pending.push_back({child, depth + 1});
  }
}

std::string automationParentHandle(Document& document, TStageObject& object,
                                   TStageObject& parent,
                                   const QJsonValue& requested) {
  const std::string handle = requested.isObject()
                                 ? handleString(requested.toObject())
                                 : object.getParentHandle();
  if (parent.getPlasticSkeletonDeformation() && hookNumber(handle))
    throw Error("unsupported_target",
                "Hook handles on Plastic-deformed columns are not admitted.");
  requireParentHandle(document, parent.getId(), handle);
  return handle;
}

QJsonObject automationHandleRecord(const std::string& handle) {
  const QString native = QString::fromStdString(handle);
  if (const int number = hookNumber(handle))
    return {{"kind", "hook"}, {"native", native}, {"hook_number", number}};
  return {{"kind", "peg_hole"},
          {"native", native},
          {"letter", native},
          {"static_offset_inch", pointJson(letterOffset(handle[0]))}};
}

std::vector<int> automationReportFrames(const QJsonObject& params) {
  if (!params.contains("report_frames")) return {};
  return uniqueFrames(params.value("report_frames").toArray(), "report_frames");
}

std::vector<TAffine> automationPlacements(ToonzScene& scene,
                                          TStageObject& object,
                                          const std::vector<int>& frames) {
  depthOf(stageOf(scene), object);
  refreshPlacements(scene);
  std::vector<TAffine> placements;
  for (const int frame : frames)
    placements.push_back(object.getPlacement(frame));
  return placements;
}

void insertAutomationWorldChanges(QJsonObject& result,
                                  const std::vector<int>& frames,
                                  const std::vector<TAffine>& before,
                                  const std::vector<TAffine>& after) {
  if (frames.empty()) return;
  QJsonArray changes;
  bool preservedAll = true;
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const TAffine& first  = before[index];
    const TAffine& second = after[index];
    const double translation =
        std::hypot(second.a13 - first.a13, second.a23 - first.a23) /
        Stage::inch;
    const double linear = std::max(
        {std::abs(second.a11 - first.a11), std::abs(second.a12 - first.a12),
         std::abs(second.a21 - first.a21), std::abs(second.a22 - first.a22)});
    const bool preserved =
        translation <= PreservedInch && linear <= PreservedLinear;
    preservedAll = preservedAll && preserved;
    changes.append(QJsonObject{{"frame", frames[index]},
                               {"before", affineJson(first)},
                               {"after", affineJson(second)},
                               {"max_translation_delta_inch", translation},
                               {"max_linear_delta", linear},
                               {"preserved", preserved}});
  }
  result.insert("world_changes", changes);
  result.insert("world_placement_preserved", preservedAll);
}

AutomationLayer automationLayer(Document& document, const QString& layerId) {
  const auto found = document.offsetLayers.constFind(layerId);
  if (found != document.offsetLayers.constEnd()) {
    auto& stage = stageOf(*document.scene);
    for (int index = 0; index < stage.getStageObjectCount(); ++index) {
      TStageObject* object = stage.getStageObject(index);
      for (int layer = 0; layer < object->getOffsetLayerCount(); ++layer)
        if (object->getOffsetLayer(layer).m_weight.getPointer() ==
            found.value().getPointer())
          return {object, layer};
    }
  }
  throw Error("invalid_handle",
              "The offset layer handle is unknown, deleted or belongs to "
              "another document.");
}

QJsonArray automationLayerSummaries(Document& document,
                                    const TStageObject& object) {
  QJsonArray layers;
  for (int index = 0; index < object.getOffsetLayerCount(); ++index) {
    const Layer& layer = object.getOffsetLayer(index);
    layers.append(QJsonObject{{"layer_id", layerId(document, layer)},
                              {"name", QString::fromStdWString(layer.m_name)},
                              {"index", index},
                              {"enabled", layer.m_enabled}});
  }
  return layers;
}

QJsonArray rigOperations() {
  QJsonArray catalog;
  addStageTools(catalog);
  addLayerTools(catalog);
  return catalog;
}

QJsonValue executeRig(Context& context, const QString& method,
                      const QJsonObject& params) {
  static const std::map<QString, QJsonValue (*)(Context&, const QJsonObject&)>
      handlers{{"rig.inspect", inspectRig},
               {"rig.evaluate", evaluateRig},
               {"object.handles.set", setHandles},
               {"object.pivot.set", setPivot},
               {"offset_layer.create", createLayer},
               {"offset_layer.update", updateLayer},
               {"offset_layer.delete", deleteLayer}};
  const auto found = handlers.find(method);
  if (found == handlers.end())
    throw Error("unknown_method", "Unknown detached rig operation.");
  return found->second(context, params);
}

}  // namespace GroundzeroAutomation
