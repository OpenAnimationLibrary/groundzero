#include "automation.h"
#include "automation_fx.h"
#include "automation_fx_journal.h"

#include "tconst.h"
#include "tfx.h"
#include "tfxattributes.h"
#include "tmacrofx.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace GroundzeroAutomation {
namespace {

constexpr int kMaximumGroupDepth   = 16;
constexpr int kMaximumGroups       = 1024;
constexpr int kMaximumGroupListing = 512;

FxDag* graphOf(Document& document) {
  return document.scene->getXsheet()->getFxDag();
}

// Every node of the top sheet, the Xsheet node and outputs included, in
// creation order.
std::vector<TFx*> sheetNodes(Document& document) {
  TXsheet* sheet = document.scene->getXsheet();
  FxDag* graph   = sheet->getFxDag();
  std::set<TFx*> nodes;
  graph->getInternalFxs()->getFxs(nodes);
  nodes.insert(graph->getXsheetFx());
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    nodes.insert(graph->getOutputFx(index));
  for (int index = 0; index < sheet->getColumnCount(); ++index) {
    TXshColumn* column = sheet->getColumn(index);
    if (column && column->getFx()) nodes.insert(column->getFx());
  }
  nodes.erase(nullptr);
  std::vector<TFx*> ordered(nodes.begin(), nodes.end());
  std::sort(ordered.begin(), ordered.end(), [&document](TFx* a, TFx* b) {
    return automationFxSequence(document, a) <
           automationFxSequence(document, b);
  });
  return ordered;
}

// The effects whose attributes carry a node's groups: the node, and the
// members of a macro.
std::vector<TFx*> carriers(TFx* node) {
  std::vector<TFx*> result{node};
  if (auto* macro = dynamic_cast<TMacroFx*>(node))
    for (const auto& member : macro->getFxs())
      result.push_back(member.getPointer());
  return result;
}

bool isUngroupable(Document& document, TFx* fx) {
  return fx == graphOf(document)->getXsheetFx() ||
         dynamic_cast<TOutputFx*>(fx) != nullptr;
}

QStack<int> stackOf(TFx* fx) { return fx->getAttributes()->getGroupIdStack(); }

[[noreturn]] void invalidGroup(const QString& reason, const QString& message,
                               QJsonObject details = {}) {
  details.insert("reason", reason);
  throw Error("invalid_group", message, details);
}

std::vector<TFx*> groupSelection(Document& document, const QJsonArray& ids) {
  std::vector<TFx*> members;
  QSet<QString> seen;
  for (const QJsonValue& value : ids) {
    const QString id = value.toString();
    if (seen.contains(id))
      invalidGroup("duplicate_member", "Each effect can be listed once.",
                   {{"fx_id", id}});
    seen.insert(id);
    TFx* fx = automationOwnedFx(document, id);
    if (isUngroupable(document, fx))
      invalidGroup("unsupported_member",
                   "The Xsheet node and output nodes cannot be grouped.",
                   {{"fx_id", id}});
    members.push_back(fx);
  }
  return members;
}

// A new group encloses whole groups: every node that shares a listed node's
// outermost group is listed too.
void requireClosedSelection(Document& document,
                            const std::vector<TFx*>& members) {
  const std::set<TFx*> listed(members.begin(), members.end());
  const std::vector<TFx*> nodes = sheetNodes(document);
  for (TFx* fx : members) {
    const QStack<int> stack = stackOf(fx);
    if (stack.isEmpty()) continue;
    for (TFx* node : nodes)
      if (!listed.count(node) && stackOf(node).contains(stack.last()))
        invalidGroup(
            "splits_group",
            "List every member of the enclosing group, or none.",
            {{"group_id", automationFxGroupHandle(document, stack.last())}});
  }
}

std::set<int> groupIdsInUse(Document& document) {
  std::set<int> ids;
  for (TFx* node : sheetNodes(document))
    for (TFx* carrier : carriers(node))
      for (int id : stackOf(carrier)) ids.insert(id);
  return ids;
}

void requireGroupCapacity(Document& document,
                          const std::vector<TFx*>& members) {
  for (TFx* fx : members)
    for (TFx* carrier : carriers(fx))
      if (stackOf(carrier).size() >= kMaximumGroupDepth)
        invalidGroup("depth_limit", "Groups nest at most 16 deep.",
                     {{"fx_id", document.idForFx(fx)}});
  if (static_cast<int>(groupIdsInUse(document).size()) >= kMaximumGroups)
    throw Error("resource_limit", "A document holds at most 1024 groups.",
                {{"limit", "groups"}});
}

void requireUnlocked(Document& document, const std::vector<TFx*>& nodes) {
  for (TFx* fx : nodes) automationRequireColumnEditable(document, fx);
}

// One more than every id stored in any stack: the native counter is rebuilt
// at load from terminal and internal effects only, so it can be too low.
int nextGroupId(Document& document) {
  const std::set<int> used = groupIdsInUse(document);
  return used.empty() ? 1 : std::max(*used.rbegin(), 0) + 1;
}

void raiseGroupCounter(Document& document, int id) {
  FxDag* graph = graphOf(document);
  for (int guard = 0; guard < id + 1; ++guard)
    if (graph->getNewGroupId() >= id) return;
}

// Inserts the group as the outermost one and leaves every group closed, the
// state a load produces.
void pushGroup(TFx* fx, int id, const std::wstring& name) {
  TFxAttributes* attributes = fx->getAttributes();
  const int position        = attributes->getGroupIdStack().size();
  attributes->setGroupId(id, position);
  attributes->setGroupName(name, position);
  attributes->closeEditingGroup(id);
}

std::vector<TFx*> membersOf(Document& document, int id) {
  std::vector<TFx*> members;
  for (TFx* node : sheetNodes(document))
    if (stackOf(node).contains(id)) members.push_back(node);
  return members;
}

QJsonObject describeGroup(Document& document, int id) {
  const std::vector<TFx*> members = membersOf(document, id);
  QJsonArray handles;
  for (TFx* fx : members)
    if (handles.size() < kMaximumGroupListing)
      handles.append(document.idForFx(fx));
  QJsonValue parent, name;
  if (!members.empty()) {
    TFxAttributes* attributes = members.front()->getAttributes();
    const QStack<int> stack   = attributes->getGroupIdStack();
    const int index           = stack.indexOf(id);
    name =
        QString::fromStdWString(attributes->getGroupNameStack().value(index));
    if (index + 1 < stack.size())
      parent = automationFxGroupHandle(document, stack[index + 1]);
  }
  return {{"group_id", automationFxGroupHandle(document, id)},
          {"name", name},
          {"fx_ids", handles},
          {"member_count", static_cast<int>(members.size())},
          {"parent_group_id", parent}};
}

QJsonValue createGroup(Context& context, Document& document,
                       const QJsonObject& params) {
  const std::vector<TFx*> members =
      groupSelection(document, params.value("fx_ids").toArray());
  requireClosedSelection(document, members);
  requireGroupCapacity(document, members);
  requireUnlocked(document, members);
  const int id            = nextGroupId(document);
  const std::wstring name = params.contains("name")
                                ? automationFxName(params).toStdWString()
                                : L"Group " + std::to_wstring(id);
  context.markMutationStarted();
  for (TFx* fx : members)
    for (TFx* carrier : carriers(fx)) pushGroup(carrier, id, name);
  raiseGroupCounter(document, id);
  for (TFx* fx : members)
    if (stackOf(fx).isEmpty() || stackOf(fx).last() != id)
      throw Error("NATIVE_FAILURE", "Native grouping did not take effect.");
  return QJsonObject{{"changed", true}, {"group", describeGroup(document, id)}};
}

int existingGroup(Document& document, const QJsonObject& params) {
  const QString handle = requiredString(params, "group_id");
  const int id         = automationFxGroupIdOrZero(document, handle);
  if (id == 0 || !groupIdsInUse(document).count(id))
    throw Error("unknown_group", "No schematic group has this handle.",
                {{"group_id", handle}});
  return id;
}

// Every effect whose stack holds the group: nodes and macro members.
std::vector<TFx*> holders(Document& document, int id) {
  std::vector<TFx*> result;
  for (TFx* node : sheetNodes(document))
    for (TFx* carrier : carriers(node))
      if (stackOf(carrier).contains(id)) result.push_back(carrier);
  return result;
}

void requireNamedStacks(const std::vector<TFx*>& effects) {
  for (TFx* fx : effects)
    if (fx->getAttributes()->getGroupNameStack().size() != stackOf(fx).size())
      throw Error("INVALID_GRAPH",
                  "An effect's group names do not match its group ids.");
}

QJsonValue removeGroup(Context& context, Document& document,
                       const QJsonObject& params) {
  const int id                    = existingGroup(document, params);
  const std::vector<TFx*> effects = holders(document, id);
  requireNamedStacks(effects);
  requireUnlocked(document, membersOf(document, id));
  const QJsonObject before = describeGroup(document, id);
  context.markMutationStarted();
  for (TFx* fx : effects) {
    const int index = stackOf(fx).indexOf(id);
    fx->getAttributes()->removeGroupId(index);
    fx->getAttributes()->removeGroupName(index);
  }
  if (!holders(document, id).empty())
    throw Error("NATIVE_FAILURE", "Native ungrouping did not take effect.");
  return QJsonObject{{"changed", true},
                     {"group_id", before.value("group_id")},
                     {"name", before.value("name")},
                     {"former_member_count", before.value("member_count")}};
}

bool renames(const std::vector<TFx*>& effects, int id,
             const std::wstring& name) {
  for (TFx* fx : effects) {
    const int index = stackOf(fx).indexOf(id);
    if (fx->getAttributes()->getGroupNameStack().value(index) != name)
      return true;
  }
  return false;
}

QJsonValue renameGroup(Context& context, Document& document,
                       const QJsonObject& params) {
  const int id                    = existingGroup(document, params);
  const std::vector<TFx*> effects = holders(document, id);
  requireNamedStacks(effects);
  const std::wstring name  = automationFxName(params).toStdWString();
  const QJsonObject before = describeGroup(document, id);
  const bool changed       = renames(effects, id, name);
  if (changed) {
    requireUnlocked(document, membersOf(document, id));
    context.markMutationStarted();
  }
  for (TFx* fx : effects) {
    const int index = stackOf(fx).indexOf(id);
    if (fx->getAttributes()->getGroupNameStack().value(index) == name) continue;
    fx->getAttributes()->removeGroupName(index);
    fx->getAttributes()->setGroupName(name, index);
  }
  if (renames(effects, id, name))
    throw Error("NATIVE_FAILURE", "Native group rename did not take effect.");
  return QJsonObject{{"changed", changed},
                     {"group_id", before.value("group_id")},
                     {"name", QString::fromStdWString(name)},
                     {"name_before", before.value("name")}};
}

TPointD requestedPosition(const QJsonValue& value) {
  if (value.isNull()) return TConst::nowhere;
  const QJsonObject point = value.toObject();
  const double x          = point.value("x").toDouble();
  const double y          = point.value("y").toDouble();
  if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e6 ||
      std::abs(y) > 1e6)
    throw Error("invalid_params",
                "Positions are finite and within 1,000,000 of the origin.");
  // The scene file keeps six significant digits; store what reloads exactly.
  return TPointD(QString::number(x, 'g', 6).toDouble(),
                 QString::number(y, 'g', 6).toDouble());
}

QJsonValue positionValue(const TPointD& point) {
  if (point == TConst::nowhere) return QJsonValue();
  return QJsonObject{{"x", point.x}, {"y", point.y}};
}

std::vector<std::pair<TFx*, TPointD>> layoutRequest(Document& document,
                                                    const QJsonObject& params) {
  std::vector<std::pair<TFx*, TPointD>> changes;
  QSet<QString> seen;
  for (const QJsonValue& value : params.value("positions").toArray()) {
    const QJsonObject item = value.toObject();
    const QString id       = requiredString(item, "fx_id");
    if (seen.contains(id))
      throw Error("invalid_params", "Each effect can be placed once.",
                  {{"fx_id", id}});
    seen.insert(id);
    TFx* fx = automationOwnedFx(document, id);
    automationRequireColumnEditable(document, fx);
    changes.push_back({fx, requestedPosition(item.value("position"))});
  }
  return changes;
}

QJsonValue setLayout(Context& context, Document& document,
                     const QJsonObject& params) {
  const auto changes = layoutRequest(document, params);
  int changed        = 0;
  QJsonArray positions;
  for (const auto& change : changes) {
    TFxAttributes* attributes = change.first->getAttributes();
    if (attributes->getDagNodePos() != change.second) {
      context.markMutationStarted();
      attributes->setDagNodePos(change.second);
      ++changed;
    }
    if (attributes->getDagNodePos() != change.second)
      throw Error("NATIVE_FAILURE", "Native placement did not take effect.");
    positions.append(QJsonObject{{"fx_id", document.idForFx(change.first)},
                                 {"position", positionValue(change.second)}});
  }
  return QJsonObject{{"changed_count", changed}, {"positions", positions}};
}

QJsonObject idSchema() {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
}

QJsonObject groupIdSchema() {
  return {{"type", "string"},
          {"minLength", 1},
          {"maxLength", 128},
          {"pattern", "fxgroup_[0-9a-f-]{36}"}};
}

QJsonObject positionSchema() {
  const QJsonObject coordinate{
      {"type", "number"}, {"minimum", -1000000}, {"maximum", 1000000}};
  const QJsonObject point{
      {"type", "object"},
      {"properties", QJsonObject{{"x", coordinate}, {"y", coordinate}}},
      {"required", QJsonArray{"x", "y"}},
      {"additionalProperties", false}};
  return {{"oneOf", QJsonArray{point, QJsonObject{{"type", "null"}}}}};
}

QJsonObject layoutSchema() {
  return {{"type", "array"},
          {"minItems", 1},
          {"maxItems", 256},
          {"items", QJsonObject{{"type", "object"},
                                {"properties",
                                 QJsonObject{{"fx_id", idSchema()},
                                             {"position", positionSchema()}}},
                                {"required", QJsonArray{"fx_id", "position"}},
                                {"additionalProperties", false}}}};
}

}  // namespace

QJsonArray automationFxGroups(Document& document) {
  QJsonArray groups;
  for (int id : groupIdsInUse(document))
    if (!membersOf(document, id).empty())
      groups.append(describeGroup(document, id));
  return groups;
}

QJsonArray fxGraphOperations() {
  const QJsonObject document{{"document_id", idSchema()}};
  QJsonObject create  = document;
  create["fx_ids"]    = QJsonObject{{"type", "array"},
                                    {"items", idSchema()},
                                    {"minItems", 2},
                                    {"maxItems", 512}};
  create["name"]      = boundedNameSchema(256, true);
  QJsonObject remove  = document;
  remove["group_id"]  = groupIdSchema();
  QJsonObject rename  = remove;
  rename["name"]      = boundedNameSchema(256, true);
  QJsonObject layout  = document;
  layout["positions"] = layoutSchema();
  QJsonArray operations{
      operation("fx.group.create",
                "Group effects in the FX schematic as the editor's Group "
                "command does: the new group is the outermost group of every "
                "listed effect and of a listed macro's members. Changes no "
                "connection and no render.",
                create, {"document_id", "fx_ids"}, true),
      operation("fx.group.remove",
                "Ungroup: remove one schematic group from every effect that "
                "carries it; enclosing and nested groups stay.",
                remove, {"document_id", "group_id"}, true),
      operation("fx.group.rename", "Rename one schematic group.", rename,
                {"document_id", "group_id", "name"}, true),
      operation("fx.layout.set",
                "Set FX schematic positions; null leaves a node unplaced so "
                "the editor places it when it opens the scene. Positions "
                "change no render.",
                layout, {"document_id", "positions"}, true)};
  QJsonObject ports = document;
  ports["fx_id"]    = idSchema();
  ports["group"]    = idSchema();
  ports["sources"]  = QJsonObject{{"type", "array"},
                                  {"items", idSchema()},
                                  {"minItems", 0},
                                  {"maxItems", 64}};
  operations.append(operation(
      "fx.port_group.set",
      "Set the sources of a dynamic port group in order (index 0 is the "
      "first port, the top of an over's stack). The group takes the editor's "
      "normal form: one empty port after the sources, at least its minimum.",
      ports, {"document_id", "fx_id", "group", "sources"}, true));
  for (const QJsonValue& entry : fxLinkOperations()) operations.append(entry);
  return operations;
}

QJsonValue executeFxGraph(Context& context, Document& document,
                          const QString& method, const QJsonObject& params) {
  if (method == "fx.group.create")
    return createGroup(context, document, params);
  if (method == "fx.group.remove")
    return removeGroup(context, document, params);
  if (method == "fx.group.rename")
    return renameGroup(context, document, params);
  if (method == "fx.layout.set") return setLayout(context, document, params);
  throw Error("METHOD_NOT_FOUND", "Unknown FX graph operation.");
}

}  // namespace GroundzeroAutomation
