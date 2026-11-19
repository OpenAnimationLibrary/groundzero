#include "automation_object.h"
#include "automation.h"
#include "automation_hook.h"
#include "automation_rig.h"

#include "tconst.h"
#include "tfx.h"
#include "toonz/fxdag.h"
#include "toonz/poselibrary.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheet.h"
#include "toonz/txshlevel.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/stageobjectsdata.h"

#include <QMap>
#include <QSet>

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int MaxObjects      = 4096;
constexpr int MaxColumns      = 4096;
constexpr int MaxSceneLayers  = 1024;
constexpr int MaxDependents   = 4096;
constexpr int MaxGroupMembers = 256;
constexpr int MaxGroups       = 4096;
constexpr int MaxGroupNumber  = 1000000000;
constexpr int MaxDetail       = 512;

TStageObjectTree& stageOf(ToonzScene& scene) {
  return *scene.getXsheet()->getStageObjectTree();
}

QString kindOf(const TStageObjectId& id) {
  if (id.isColumn()) return "column";
  if (id.isCamera()) return "camera";
  if (id.isPegbar()) return "pegbar";
  return "table";
}

QString describe(const TStageObject& object) {
  return (kindOf(object.getId()) + " " +
          QString::fromStdString(object.getName()))
      .left(MaxDetail);
}

void appendDependent(QJsonArray& list, const QString& kind,
                     const QJsonValue& object, const QString& detail) {
  if (list.size() >= MaxDependents)
    throw Error("resource_limit", "More than 4096 dependents were found.");
  list.append(QJsonObject{{"kind", kind},
                          {"object_id", object},
                          {"detail", detail.left(MaxDetail)}});
}

void appendPoseEntries(ToonzScene& scene, const TStageObjectId& id,
                       QJsonArray& list) {
  const PoseLibrary* poses = scene.getPoseLibrary();
  if (!poses) return;
  for (const auto& entry : poses->getBase())
    if (entry.m_objectId == id)
      appendDependent(
          list, "pose_entry", QJsonValue(),
          "base pose channel " + QString::fromStdString(entry.m_channel));
  for (int index = 0; index < poses->getPoseCount(); ++index) {
    const auto& pose = poses->getPose(index);
    for (const auto& entry : pose.m_entries)
      if (entry.m_objectId == id)
        appendDependent(list, "pose_entry", QJsonValue(),
                        "pose " + pose.m_name + " channel " +
                            QString::fromStdString(entry.m_channel));
  }
}

// Everything that depends on a stage object: its children, the hook parent
// handles they hold on it, the motion paths evaluated in its space and the
// pose entries that name it. Published controls, IK pins, Plastic handles and
// deformer chains are outside the admitted profile, so none can exist.
QJsonArray objectDependents(Document& document, TStageObject& target) {
  QJsonArray list;
  for (TStageObject* child : automationChildren(target)) {
    const QString id = document.idForObject(child);
    appendDependent(list, "child_object", id, describe(*child));
    if (const int hook = automationHookNumber(child->getParentHandle()))
      appendDependent(list, "child_parent_handle_hook_user", id,
                      "parent handle H" + QString::number(hook));
    if (automationFollowsPath(*child))
      appendDependent(list, "path_consumer", id,
                      "follows a motion path in this object's space");
  }
  appendPoseEntries(*document.scene, target.getId(), list);
  return list;
}

QJsonArray hookDependents(Document& document, const QJsonObject& target) {
  TXshLevel* native      = document.level(requiredString(target, "level_id"));
  TXshSimpleLevel* level = native->getSimpleLevel();
  if (!level) throw Error("invalid_params", "Hooks belong to drawing levels.");
  const int number     = target.value("hook_number").toInt();
  const auto exposures = automationExposures(*document.scene);
  QJsonArray list;
  for (const auto& value :
       automationHookUsers(document, exposures, *level, number)) {
    const auto user    = value.toObject();
    const QString hook = "H" + QString::number(number);
    if (user.value("role") == "own_handle")
      appendDependent(list, "own_handle_hook_user", user.value("object_id"),
                      "own handle " + hook + " on column " +
                          user.value("column_id").toString());
    else
      appendDependent(list, "child_parent_handle_hook_user",
                      user.value("child_object_id"),
                      "parent handle " + hook + " on column " +
                          user.value("column_id").toString());
  }
  return list;
}

QJsonArray targetDependents(Document& document, const QJsonObject& target) {
  const QString kind = target.value("kind").toString();
  if (kind == "object")
    return objectDependents(
        document, *document.object(requiredString(target, "object_id")));
  if (kind == "hook") return hookDependents(document, target);
  if (kind == "layer") {
    // Weight controls are the only layer consumers, and published channels
    // are not admitted, so a live layer has none.
    automationLayer(document, requiredString(target, "layer_id"));
    return {};
  }
  throw Error("invalid_handle",
              "The control handle is unknown: published controls are not "
              "admitted in this profile.");
}

QJsonValue inspectDependents(Context& context, const QJsonObject& params) {
  auto& document = automationRigDocument(context, params);
  return QJsonObject{
      {"document_id", document.id},
      {"revision", document.revisionText()},
      {"dependents",
       targetDependents(document, params.value("target").toObject())},
      {"complete", true}};
}

QJsonValue renameObject(Context& context, const QJsonObject& params) {
  auto& document       = automationRigDocument(context, params);
  TStageObject* object = document.object(requiredString(params, "object_id"));
  const TStageObjectId id = object->getId();
  if (!id.isPegbar() && !id.isColumn())
    throw Error("unsupported_target", "Only pegbars and columns are renamed.");
  const QString name = params.value("name").toString();
  requireAutomationName(name);
  requireAutomationUnlocked(*document.scene, *object);
  if (QString::fromStdString(object->getName()) != name) {
    context.markMutationStarted();
    object->setName(name.toStdString());
    if (QString::fromStdString(object->getName()) != name)
      throw Error("native_error", "The native object kept another name.");
  }
  return QJsonObject{{"object_id", document.idForObject(object)},
                     {"name", QString::fromStdString(object->getName())}};
}

struct Subtree final {
  std::vector<TStageObject*> objects;
  std::set<int> columns;
  int layers = 0;
};

Subtree collectSubtree(TStageObject& root, bool whole) {
  Subtree subtree;
  std::vector<TStageObject*> pending{&root};
  while (!pending.empty()) {
    TStageObject* current = pending.back();
    pending.pop_back();
    if (static_cast<int>(subtree.objects.size()) >= MaxObjects)
      throw Error("resource_limit", "The subtree exceeds 4096 objects.");
    subtree.objects.push_back(current);
    subtree.layers += current->getOffsetLayerCount();
    if (current->getId().isColumn())
      subtree.columns.insert(current->getId().getIndex());
    if (!whole) break;
    const auto children = automationChildren(*current);
    pending.insert(pending.end(), children.rbegin(), children.rend());
  }
  return subtree;
}

void requireDuplicable(ToonzScene& scene, const TStageObject& object) {
  const TStageObjectId id = object.getId();
  if (id.isTable() || id.isCamera())
    throw Error("unsupported_target",
                "Only pegbars and level columns are duplicated.",
                {{"object", describe(object)}});
  if (automationFollowsPath(object))
    throw Error("unsupported_target",
                "Objects that follow a motion path are duplicated with "
                "motion_path.duplicate, not object.duplicate.",
                {{"object", describe(object)}});
  if (!id.isColumn()) return;
  const TXshColumn* column = scene.getXsheet()->getColumn(id.getIndex());
  if (!column || column->getColumnType() != TXshColumn::eLevelType)
    throw Error("unsupported_target",
                "Only level columns are duplicated with their stage objects.",
                {{"object", describe(object)}});
}

void requireBudgets(ToonzScene& scene, const Subtree& subtree) {
  auto& stage      = stageOf(scene);
  const int adding = static_cast<int>(subtree.objects.size());
  if (stage.getStageObjectCount() + adding > MaxObjects)
    throw Error("resource_limit", "A scene holds at most 4096 stage objects.");
  if (scene.getXsheet()->getColumnCount() +
          static_cast<int>(subtree.columns.size()) >
      MaxColumns)
    throw Error("resource_limit", "A scene holds at most 4096 columns.");
  int layers = 0;
  for (int index = 0; index < stage.getStageObjectCount(); ++index)
    layers += stage.getStageObject(index)->getOffsetLayerCount();
  if (layers + subtree.layers > MaxSceneLayers)
    throw Error("resource_limit", "A scene holds at most 1024 offset layers.");
}

struct Restored final {
  QMap<TStageObjectId, TStageObjectId> ids;
  QMap<TFx*, TFx*> effects;
};

// Copy and paste the objects the way the stage schematic does
// (StageObjectSelection::copySelection and pasteSelection): independent
// curves and layers, cloned level columns that share their levels, the
// effects fed only by the copied columns, and pasted columns appended to the
// sheet so no existing column moves.
Restored pasteCopies(ToonzScene& scene, const Subtree& subtree) {
  TXsheet* sheet = scene.getXsheet();
  std::vector<TStageObjectId> ids;
  for (const TStageObject* object : subtree.objects)
    ids.push_back(object->getId());
  const int stored =
      StageObjectsData::eDoClone | StageObjectsData::eResetFxDagPositions;
  StageObjectsData data;
  data.storeObjects(ids, sheet, stored);
  if (!subtree.columns.empty())
    data.storeColumnFxs(subtree.columns, sheet, stored);
  std::set<int> indices{sheet->getColumnCount()};
  std::list<int> splines;
  Restored restored;
  data.restoreObjects(
      indices, splines, sheet,
      StageObjectsData::eDoClone | StageObjectsData::eRebindReferences,
      restored.ids, restored.effects, TConst::nowhere);
  return restored;
}

QJsonValue nullableColumn(Document& document, const TStageObject& object) {
  if (!object.getId().isColumn()) return QJsonValue();
  return document.idForColumn(
      document.scene->getXsheet()->getColumn(object.getId().getIndex()));
}

QJsonArray layerMap(Document& document, const TStageObject& source,
                    const TStageObject& copy) {
  const auto from = automationLayerSummaries(document, source);
  const auto to   = automationLayerSummaries(document, copy);
  QJsonArray map;
  for (int index = 0; index < from.size() && index < to.size(); ++index)
    map.append(QJsonObject{
        {"source_layer_id", from[index].toObject().value("layer_id")},
        {"layer_id", to[index].toObject().value("layer_id")}});
  return map;
}

QJsonObject copyRecord(Document& document, TStageObject& source,
                       TStageObject& copy) {
  QJsonObject record{{"source_object_id", document.idForObject(&source)},
                     {"object_id", document.idForObject(&copy)},
                     {"kind", kindOf(copy.getId())},
                     {"source_column_id", nullableColumn(document, source)},
                     {"column_id", nullableColumn(document, copy)}};
  if (copy.getId().isColumn()) {
    TXsheet* sheet = document.scene->getXsheet();
    TFx* fx        = sheet->getColumn(copy.getId().getIndex())->getFx();
    record.insert("output_connected",
                  fx && sheet->getFxDag()->getTerminalFxs()->containsFx(fx));
  }
  return record;
}

QJsonArray copiedEffects(Document& document, const Restored& restored) {
  QJsonArray effects;
  TFxSet* internal = document.scene->getXsheet()->getFxDag()->getInternalFxs();
  for (TFx* fx : restored.effects)
    if (fx && internal->containsFx(fx)) effects.append(document.idForFx(fx));
  return effects;
}

QString duplicationScope(const QJsonObject& params) {
  const QString scope = params.value("scope").toString("object");
  if (scope == "subtree" && !params.contains("level_sharing"))
    throw Error("invalid_params",
                "A subtree copy requires level_sharing: copied columns "
                "expose the same levels as their sources.");
  if (scope == "object" && params.contains("level_sharing"))
    throw Error("invalid_params",
                "level_sharing applies only to scope subtree.");
  return scope;
}

QJsonValue duplicateObject(Context& context, const QJsonObject& params) {
  auto& document      = automationRigDocument(context, params);
  auto& scene         = *document.scene;
  TStageObject* root  = document.object(requiredString(params, "object_id"));
  const QString scope = duplicationScope(params);
  if (scope == "object" && !root->getId().isPegbar())
    throw Error("unsupported_kind",
                "scope object duplicates a pegbar; use scope subtree with "
                "level_sharing for columns.");
  const QString name = params.value("name").toString();
  requireAutomationName(name);
  const Subtree subtree = collectSubtree(*root, scope == "subtree");
  for (const TStageObject* object : subtree.objects)
    requireDuplicable(scene, *object);
  requireBudgets(scene, subtree);
  const TStageObjectId parent = root->getParent();
  const std::string handle    = root->getParentHandle();
  context.markMutationStarted();
  const Restored restored = pasteCopies(scene, subtree);
  auto& stage             = stageOf(scene);
  QJsonArray copies;
  TStageObject* copyRoot = nullptr;
  for (TStageObject* source : subtree.objects) {
    TStageObject* copy =
        stage.getStageObject(restored.ids.value(source->getId()), false);
    if (!copy)
      throw Error("native_error", "A native copy is missing.",
                  {{"object", describe(*source)}});
    if (!copyRoot) copyRoot = copy;
    registerAutomationLayers(document, *copy);
    copies.append(copyRecord(document, *source, *copy));
  }
  copyRoot->setParent(parent);
  copyRoot->setParentHandle(handle);
  copyRoot->setName(name.toStdString());
  validateAutomationRig(scene);
  const auto exposures = automationExposures(scene);
  return QJsonObject{
      {"object", automationObjectRecord(document, exposures, *copyRoot)},
      {"source_object_id", document.idForObject(root)},
      {"scope", scope},
      {"layer_map", layerMap(document, *root, *copyRoot)},
      {"control_map", QJsonArray()},
      {"objects", copies},
      {"copied_fx_ids", copiedEffects(document, restored)}};
}

bool blocksDeletion(const QJsonValue& dependent) {
  const QString kind = dependent.toObject().value("kind").toString();
  return kind != "child_object" && kind != "path_consumer" &&
         kind != "child_parent_handle_hook_user";
}

void requireDeletable(Document& document, TStageObject& object, bool reattach) {
  const QJsonArray dependents = objectDependents(document, object);
  const bool blocked =
      std::any_of(dependents.begin(), dependents.end(), blocksDeletion);
  if (blocked || (!reattach && !dependents.isEmpty()))
    throw Error("has_dependents",
                blocked ? "Pose entries name this object."
                        : "The object has children; choose "
                          "children_policy reattach_to_parent.",
                {{"dependents", dependents}});
  auto& stage                = stageOf(*document.scene);
  const TStageObject* parent = stage.getStageObject(object.getParent(), false);
  for (const TStageObject* child : automationChildren(object)) {
    requireAutomationUnlocked(*document.scene, *child);
    if (parent && parent->getId().isColumn() && child->getId().isPegbar())
      throw Error("has_dependents",
                  "A pegbar child cannot move under a column parent.",
                  {{"dependents", dependents}});
  }
}

QJsonValue deleteObject(Context& context, const QJsonObject& params) {
  auto& document       = automationRigDocument(context, params);
  auto& scene          = *document.scene;
  TStageObject* object = document.object(requiredString(params, "object_id"));
  if (!object->getId().isPegbar())
    throw Error("unsupported_kind",
                "object.delete removes pegbars; columns use column.delete.");
  requireAutomationUnlocked(scene, *object);
  const bool reattach = params.value("children_policy") == "reattach_to_parent";
  requireDeletable(document, *object, reattach);
  const auto frames   = automationReportFrames(params);
  const auto children = automationChildren(*object);
  std::vector<std::vector<TAffine>> before;
  for (TStageObject* child : children)
    before.push_back(automationPlacements(scene, *child, frames));
  const QString objectId    = document.idForObject(object);
  const TStageObjectId id   = object->getId();
  const TStageObjectId into = object->getParent();
  const QJsonArray layers   = automationLayerSummaries(document, *object);
  context.markMutationStarted();
  for (const auto& layer : layers)
    document.offsetLayers.remove(layer.toObject().value("layer_id").toString());
  // RemovePegbarNodeUndo::redo: children move to the removed object's parent
  // and keep their parent handles and local keys.
  for (TStageObject* child : children) child->setParent(into);
  stageOf(scene).removeStageObject(id);
  document.objects.remove(objectId);
  QJsonArray reattached;
  for (std::size_t index = 0; index < children.size(); ++index) {
    TStageObject* child = children[index];
    QJsonObject changes;
    insertAutomationWorldChanges(changes, frames, before[index],
                                 automationPlacements(scene, *child, frames));
    TStageObject* parent = stageOf(scene).getStageObject(child->getParent());
    reattached.append(
        QJsonObject{{"object_id", document.idForObject(child)},
                    {"parent_id", document.idForObject(parent)},
                    {"world_changes", changes.value("world_changes")}});
  }
  return QJsonObject{{"object_id", objectId}, {"reattached", reattached}};
}

// Schematic groups: each object keeps a stack of group numbers, innermost
// first; the top of the stack is the group the stage schematic shows it in.
std::vector<TStageObject*> groupMembers(ToonzScene& scene, int group) {
  std::vector<TStageObject*> members;
  auto& stage = stageOf(scene);
  for (int index = 0; index < stage.getStageObjectCount(); ++index) {
    TStageObject* object = stage.getStageObject(index);
    if (object->isContainedInGroup(group)) members.push_back(object);
  }
  return members;
}

std::vector<TStageObject*> groupedObjects(Document& document,
                                          const QJsonArray& ids) {
  std::vector<TStageObject*> objects;
  QSet<TStageObject*> seen;
  for (const auto& value : ids) {
    TStageObject* object = document.object(value.toString());
    if (seen.contains(object))
      throw Error("invalid_params", "object_ids must be unique.");
    if (object->getId().isTable())
      throw Error("unsupported_target", "The table cannot be grouped.");
    seen.insert(object);
    objects.push_back(object);
  }
  return objects;
}

// The stage schematic groups whole nodes: an object already shown inside a
// group can only be grouped together with every other member of that group.
void requireWholeGroups(Document& document,
                        const std::vector<TStageObject*>& objects) {
  const QSet<TStageObject*> chosen(objects.begin(), objects.end());
  for (TStageObject* object : objects) {
    const int group = object->getGroupId();
    if (group <= 0) continue;
    QJsonArray missing;
    for (TStageObject* member : groupMembers(*document.scene, group))
      if (member->getGroupId() == group && !chosen.contains(member))
        missing.append(document.idForObject(member));
    if (!missing.isEmpty())
      throw Error("invalid_params",
                  "Group whole schematic groups: include every member of "
                  "group " +
                      QString::number(group) + ".",
                  {{"group_number", group}, {"missing_object_ids", missing}});
  }
}

QJsonObject groupRecord(Document& document, int group) {
  QJsonArray members;
  QString name;
  QJsonValue parent;
  for (TStageObject* member : groupMembers(*document.scene, group)) {
    members.append(document.idForObject(member));
    const auto ids     = member->getGroupIdStack();
    const auto names   = member->getGroupNameStack();
    const int position = ids.indexOf(group);
    if (position < names.size())
      name = QString::fromStdWString(names[position]);
    if (position + 1 < ids.size()) parent = ids[position + 1];
  }
  return {{"group_number", group},
          {"name", name},
          {"object_ids", members},
          {"parent_group_number", parent}};
}

QJsonValue createGroup(Context& context, const QJsonObject& params) {
  auto& document = automationRigDocument(context, params);
  const auto objects =
      groupedObjects(document, params.value("object_ids").toArray());
  const QString name = params.value("name").toString();
  requireAutomationName(name);
  requireWholeGroups(document, objects);
  auto& stage = stageOf(*document.scene);
  context.markMutationStarted();
  // TStageObjectCmd::group with the caller's name instead of "Group <n>".
  const int group = stage.getNewGroupId();
  for (TStageObject* object : objects) {
    object->setGroupId(group);
    object->setGroupName(name.toStdWString());
  }
  return QJsonObject{{"group", groupRecord(document, group)}};
}

int groupNumber(Document& document, const QJsonObject& params) {
  const int group = params.value("group_number").toInt();
  if (groupMembers(*document.scene, group).empty())
    throw Error("not_found", "No object belongs to this group.",
                {{"group_number", group}});
  return group;
}

QJsonValue dissolveGroup(Context& context, const QJsonObject& params) {
  auto& document     = automationRigDocument(context, params);
  const int group    = groupNumber(document, params);
  const auto members = groupMembers(*document.scene, group);
  for (TStageObject* member : members)
    if (member->getGroupId() != group)
      throw Error("invalid_params",
                  "The group is nested in another group; dissolve the "
                  "enclosing group first.",
                  {{"group_number", group}});
  QJsonArray ids;
  for (TStageObject* member : members) ids.append(document.idForObject(member));
  context.markMutationStarted();
  // TStageObjectCmd::ungroup.
  for (TStageObject* member : members) {
    member->removeGroupName(-1);
    member->removeGroupId();
  }
  return QJsonObject{{"group_number", group}, {"object_ids", ids}};
}

QJsonValue renameGroup(Context& context, const QJsonObject& params) {
  auto& document     = automationRigDocument(context, params);
  const int group    = groupNumber(document, params);
  const QString name = params.value("name").toString();
  requireAutomationName(name);
  if (groupRecord(document, group).value("name").toString() != name) {
    context.markMutationStarted();
    // TStageObjectCmd::renameGroup at each member's stack position.
    for (TStageObject* member : groupMembers(*document.scene, group)) {
      const int position = member->getGroupIdStack().indexOf(group);
      member->removeGroupName(position);
      member->setGroupName(name.toStdWString(), position);
    }
  }
  return QJsonObject{{"group", groupRecord(document, group)}};
}

QJsonValue listGroups(Context& context, const QJsonObject& params) {
  auto& document = automationRigDocument(context, params);
  auto& stage    = stageOf(*document.scene);
  std::set<int> numbers;
  for (int index = 0; index < stage.getStageObjectCount(); ++index)
    for (const int group : stage.getStageObject(index)->getGroupIdStack()) {
      numbers.insert(group);
      if (static_cast<int>(numbers.size()) > MaxGroups)
        throw Error("resource_limit", "A scene holds at most 4096 groups.");
    }
  QJsonArray groups;
  for (const int group : numbers) groups.append(groupRecord(document, group));
  return QJsonObject{{"document_id", document.id},
                     {"revision", document.revisionText()},
                     {"groups", groups}};
}

QJsonObject nameSchema() {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}};
}

QJsonObject groupNumberSchema() {
  return {{"type", "integer"}, {"minimum", 1}, {"maximum", MaxGroupNumber}};
}

QJsonObject targetSchema() {
  const auto id      = automationIdSchema();
  const auto variant = [](const QString& kind, QJsonObject fields,
                          QJsonArray required) {
    fields.insert("kind", QJsonObject{{"const", kind}});
    required.prepend("kind");
    return automationStrictSchema(fields, required);
  };
  return {
      {"oneOf",
       QJsonArray{variant("object", {{"object_id", id}}, {"object_id"}),
                  variant("hook",
                          {{"level_id", id},
                           {"hook_number", QJsonObject{{"type", "integer"},
                                                       {"minimum", 1},
                                                       {"maximum", 99}}}},
                          {"level_id", "hook_number"}),
                  variant("layer", {{"layer_id", id}}, {"layer_id"}),
                  variant("control", {{"control_id", id}}, {"control_id"})}}};
}

void addLifecycleTools(QJsonArray& catalog) {
  const auto id = automationIdSchema();
  addAutomationRigTool(
      catalog, "rig.dependents",
      "List what depends on an object, hook, offset layer or control: "
      "children, hook users, motion-path consumers and pose entries. "
      "object.delete runs the same scan.",
      {{"target", targetSchema()}}, {"target"}, false);
  addAutomationRigTool(
      catalog, "object.duplicate",
      "Duplicate a pegbar (scope object) or a pegbar or level column with "
      "everything below it (scope subtree) as the stage schematic's copy and "
      "paste does: independent curves and offset layers, copied columns that "
      "share their levels, same parent and handles.",
      {{"object_id", id},
       {"name", nameSchema()},
       {"scope", QJsonObject{{"type", "string"},
                             {"enum", QJsonArray{"object", "subtree"}}}},
       {"level_sharing", QJsonObject{{"const", "shared"}}}},
      {"object_id", "name"}, true);
  addAutomationRigTool(catalog, "object.rename",
                       "Rename a pegbar or column stage object. Names need "
                       "not be unique.",
                       {{"object_id", id}, {"name", nameSchema()}},
                       {"object_id", "name"}, true);
  addAutomationRigTool(
      catalog, "object.delete",
      "Delete a pegbar after the rig.dependents preflight. Children are "
      "refused or re-attached to its parent with their local keys, so their "
      "stage placement may move; report_frames returns the changes.",
      {{"object_id", id},
       {"children_policy",
        QJsonObject{{"type", "string"},
                    {"enum", QJsonArray{"refuse", "reattach_to_parent"}}}},
       {"report_frames", automationReportFramesSchema()}},
      {"object_id", "children_policy", "report_frames"}, true);
}

void addGroupTools(QJsonArray& catalog) {
  addAutomationRigTool(catalog, "object.groups",
                       "List the stage schematic's object groups with their "
                       "names, members and enclosing group.",
                       {}, {}, false);
  addAutomationRigTool(
      catalog, "object.group",
      "Group stage objects in the stage schematic under a name. Grouping "
      "changes no parent or placement; whole existing groups only.",
      {{"object_ids", QJsonObject{{"type", "array"},
                                  {"items", automationIdSchema()},
                                  {"minItems", 2},
                                  {"maxItems", MaxGroupMembers}}},
       {"name", nameSchema()}},
      {"object_ids", "name"}, true);
  addAutomationRigTool(
      catalog, "object.group.rename", "Rename a stage schematic group.",
      {{"group_number", groupNumberSchema()}, {"name", nameSchema()}},
      {"group_number", "name"}, true);
  addAutomationRigTool(
      catalog, "object.ungroup",
      "Dissolve an outermost stage schematic group; its members keep their "
      "parents, placement and any inner groups.",
      {{"group_number", groupNumberSchema()}}, {"group_number"}, true);
}

}  // namespace

QJsonArray objectOperations() {
  QJsonArray catalog;
  addLifecycleTools(catalog);
  addGroupTools(catalog);
  return catalog;
}

QJsonValue executeObject(Context& context, const QString& method,
                         const QJsonObject& params) {
  static const std::map<QString, QJsonValue (*)(Context&, const QJsonObject&)>
      handlers{{"rig.dependents", inspectDependents},
               {"object.duplicate", duplicateObject},
               {"object.rename", renameObject},
               {"object.delete", deleteObject},
               {"object.groups", listGroups},
               {"object.group", createGroup},
               {"object.group.rename", renameGroup},
               {"object.ungroup", dissolveGroup}};
  const auto found = handlers.find(method);
  if (found == handlers.end())
    throw Error("unknown_method", "Unknown detached rig operation.");
  return found->second(context, params);
}

}  // namespace GroundzeroAutomation
