#include "automation.h"
#include "automation_fx.h"
#include "automation_files.h"
#include "automation_scene_host.h"
#include "automation_fx_profile.h"
#include "automation_fx_journal.h"
#include "automation_macro.h"
#include "tmacrofx.h"

#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "tfx.h"
#include "tconst.h"
#include "tfxattributes.h"
#include "tnotanimatableparam.h"
#include "tparamcontainer.h"
#include "tparamset.h"
#include "tstream.h"
#include "trasterfx.h"
#include "tzeraryfx.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshzeraryfxcolumn.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <vector>

namespace GroundzeroAutomation {
namespace {

const QStringList& allowedTypes() { return automationFxProfileTypes(); }

QJsonObject stringSchema() { return {{"type", "string"}, {"minLength", 1}}; }

QJsonObject frameSchema() {
  return {{"type", "integer"},
          {"minimum", 0},
          {"maximum", 1000000},
          {"description", "Zero-based scene frame."}};
}

FxDag* dag(Document& document) {
  return document.scene->getXsheet()->getFxDag();
}

TFx* parameterFx(TFx* fx) {
  auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx);
  if (!wrapper) return fx;
  if (!wrapper->getZeraryFx())
    throw Error("INVALID_GRAPH", "Generator column has no effect.");
  return wrapper->getZeraryFx();
}

std::set<TFx*> sheetMembers(TXsheet* sheet) {
  std::set<TFx*> members;
  FxDag* graph = sheet->getFxDag();
  graph->getInternalFxs()->getFxs(members);
  members.insert(graph->getXsheetFx());
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    members.insert(graph->getOutputFx(index));
  for (int index = 0; index < sheet->getColumnCount(); ++index) {
    TXshColumn* column = sheet->getColumn(index);
    if (column && column->getFx()) members.insert(column->getFx());
  }
  return members;
}

std::set<TFx*> sceneMembers(ToonzScene& scene) {
  return sheetMembers(scene.getXsheet());
}

std::set<TFx*> graphMembers(Document& document) {
  return sceneMembers(*document.scene);
}

size_t graphNodeCount(Document& document) {
  const auto members = graphMembers(document);
  size_t count       = members.size();
  for (TFx* fx : members) {
    auto* macro = dynamic_cast<TMacroFx*>(fx);
    if (!macro) continue;
    const size_t children = macro->getFxs().size();
    if (children < 2 || children > 64)
      throw Error("INVALID_MACRO",
                  "Macro members exceed the admitted profile.");
    count += children;
  }
  return count;
}

TFx* ownedFx(Document& document, const QString& id) {
  TFx* fx = document.fx(id);
  if (graphMembers(document).count(fx) == 0) {
    document.effects.remove(id);
    throw Error("STALE_ENTITY", "Effect no longer belongs to this document.",
                {{"fx_id", id}});
  }
  return fx;
}

void requireEditable(TFx* fx) {
  const QString type = QString::fromStdString(parameterFx(fx)->getFxType());
  if (!allowedTypes().contains(type))
    throw Error("UNSUPPORTED_FX",
                "Effect is outside the builtin automation profile.",
                {{"type", type}});
}

TStageObject* columnObject(Document& document, TColumnFx* fx) {
  return document.scene->getXsheet()->getStageObjectTree()->getStageObject(
      TStageObjectId::ColumnId(fx->getColumnIndex()), false);
}

void requireColumnEditable(Document& document, TFx* fx) {
  auto* column = dynamic_cast<TColumnFx*>(fx);
  if (!column) return;
  TStageObject* object = columnObject(document, column);
  if (!object || !column->getXshColumn())
    throw Error("INVALID_GRAPH",
                "Column effect is missing its column or stage object.");
  if (object->isLocked() || column->getXshColumn()->isLocked())
    throw Error("locked",
                "Unlock the column and stage object before modifying it.");
}

// The name of a curve of param that the editor has locked, or a null
// string. A lock is editor session state (TDoubleParam::isEditorLocked); the
// worker refuses to change a locked curve as the Function Editor does.
QString lockedCurve(TParam* param, const QString& name, int depth = 0) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param))
    return number->isEditorLocked() ? name : QString();
  auto* set = dynamic_cast<TParamSet*>(param);
  if (!set || depth > 16) return {};
  for (int index = 0; index < set->getParamCount(); ++index) {
    const QString locked = lockedCurve(
        set->getParam(index).getPointer(),
        name + "." + QString::fromStdString(set->getParamName(index)),
        depth + 1);
    if (!locked.isNull()) return locked;
  }
  return {};
}

void requireUnlockedCurve(Document& document, TFx* fx, TParam* param,
                          const QString& name) {
  const QString channel = lockedCurve(param, name);
  if (channel.isNull()) return;
  throw Error("locked", "The curve is locked in the editor; unlock it first.",
              {{"reason", "curve_locked"},
               {"fx_id", document.idForFx(fx)},
               {"parameter", name},
               {"channel", channel}});
}

void requireUnlockedCurves(Document& document, TFx* fx) {
  TParamContainer* params = parameterFx(fx)->getParams();
  for (int index = 0; index < params->getParamCount(); ++index)
    requireUnlockedCurve(document, fx, params->getParam(index),
                         QString::fromStdString(params->getParamName(index)));
}

// The nodes that share fx's parameters, fx included, in ring order. A
// parameter edit of one is an edit of all, so each must be editable.
std::vector<TFx*> linkedNodes(TFx* fx) {
  TFx* native = parameterFx(fx);
  std::vector<TFx*> nodes{fx};
  TFx* member = native->getLinkedFx();
  for (int guard = 0; member && member != native && guard < 1024; ++guard) {
    auto* zerary = dynamic_cast<TZeraryFx*>(member);
    nodes.push_back(zerary && zerary->getColumnFx() ? zerary->getColumnFx()
                                                    : member);
    member = member->getLinkedFx();
  }
  return nodes;
}

// Every member of fx's linked group is in an unlocked column; the result's
// affected_fx_ids lists them in creation order with the group id.
void requireLinkedEditable(Document& document, TFx* fx) {
  for (TFx* node : linkedNodes(fx)) requireColumnEditable(document, node);
}

void addAffected(Document& document, TFx* fx, QJsonObject& result) {
  const QJsonValue group = automationFxLinkedGroup(document, fx);
  result["linked_group_id"] =
      group.isNull() ? QJsonValue() : group.toObject().value("group_id");
  result["affected_fx_ids"] = group.isNull()
                                  ? QJsonArray{document.idForFx(fx)}
                                  : group.toObject().value("fx_ids").toArray();
}

// Names the affected effect in a constraint failure raised by action.
template <typename Action>
auto constraintForFx(Document& document, TFx* node, Action action) {
  try {
    return action();
  } catch (Error& error) {
    if (error.code == "PARAMETER_CONSTRAINT")
      error.details.insert("fx_id", document.idForFx(node));
    throw;
  }
}

TFxP makeEffect(const QString& type) {
  if (!allowedTypes().contains(type))
    throw Error("UNSUPPORTED_FX",
                "Effect creation requires an allowed builtin type.",
                {{"type", type}});
  std::vector<TFxInfo> registered;
  TFx::listFxs(registered);
  const std::string name = type.toStdString();
  const auto found       = std::find_if(
      registered.begin(), registered.end(),
      [&name](const TFxInfo& info) { return info.m_name == name; });
  if (found == registered.end())
    throw Error("UNAVAILABLE_FX",
                "Builtin effect is not registered in this worker.",
                {{"type", type}});
  // TPersist never falls back to TFx's external-program factory.
  std::unique_ptr<TPersist> object(TPersist::create(name));
  TFx* fx = dynamic_cast<TFx*>(object.get());
  if (!fx || fx->getFxType() != name)
    throw Error("NATIVE_FAILURE",
                "Builtin factory returned an unexpected effect.");
  validateAutomationFxProfile(fx);
  object.release();
  return TFxP(fx);
}

bool typeAvailable(const QString& type) {
  try {
    makeEffect(type);
    return true;
  } catch (const Error& error) {
    if (error.code != "UNAVAILABLE_FX") throw;
    return false;
  }
}

TFxP makeOutput() {
  // The registered factory constructs inside toonzlib; its constructor is not
  // part of that DLL's public API on Windows.
  std::unique_ptr<TPersist> object(TPersist::create("Toonz_outputFx"));
  auto* output = dynamic_cast<TOutputFx*>(object.get());
  if (!output || output->getFxType() != "Toonz_outputFx")
    throw Error("NATIVE_FAILURE",
                "Native output factory returned an unexpected effect.");
  TFxP effect(output);
  object.release();
  return effect;
}

QJsonObject profileSchema(TFx* fx, TParam* param, const QString& name) {
  return automationFxParameterSchema(parameterFx(fx), param,
                                     name.toStdString());
}

QJsonValue discreteValue(TParam* param) {
  if (auto* integer = dynamic_cast<TIntParam*>(param))
    return integer->getValue();
  if (auto* boolean = dynamic_cast<TBoolParam*>(param))
    return boolean->getValue();
  if (auto* enumeration = dynamic_cast<TEnumParam*>(param))
    return enumeration->getValue();
  return QJsonValue();
}

double checkedNumber(TDoubleParam* param, double frame) {
  double value;
  std::string error;
  if (!param->getValueChecked(frame, value, error))
    throw Error("PARAMETER_EVALUATION_FAILED", QString::fromStdString(error));
  return value;
}

QJsonValue parameterValue(TParam* param, double frame) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param))
    return checkedNumber(number, frame);
  if (auto* pixel = dynamic_cast<TPixelParam*>(param)) {
    // Check every channel before using the native clamping/rounding conversion.
    for (int index = 0; index < pixel->getParamCount(); ++index)
      checkedNumber(
          static_cast<TDoubleParam*>(pixel->getParam(index).getPointer()),
          frame);
    const TPixel32 value = pixel->getValue(frame);
    return QJsonArray{value.r, value.g, value.b, value.m};
  }
  if (auto* point = dynamic_cast<TPointParam*>(param))
    return QJsonArray{checkedNumber(point->getX().getPointer(), frame),
                      checkedNumber(point->getY().getPointer(), frame)};
  return discreteValue(param);
}

QJsonObject parameterDescription(TFx* fx, TParam* param, const QString& name,
                                 double frame) {
  const QJsonObject schema = profileSchema(fx, param, name);
  QJsonObject result =
      automationFxParameterFacts(parameterFx(fx), param, name.toStdString());
  result.insert("name", name);
  result.insert("animatable", param->isAnimatable());
  result.insert("supported", !schema.isEmpty());
  result.insert("schema", schema);
  if (schema.isEmpty()) {
    result["unsupported_reason"] =
        "Complex or external-resource parameter is not editable.";
    return result;
  }
  result["value"] = parameterValue(param, frame);
  QJsonArray keys;
  std::set<double> frames;
  param->getKeyframes(frames);
  for (double key : frames) keys.append(key);
  result["keyframes"] = keys;
  return result;
}

QJsonArray parameterDescriptions(TFx* fx, double frame) {
  QJsonArray result;
  TParamContainer* params = parameterFx(fx)->getParams();
  for (int index = 0; index < params->getParamCount(); ++index) {
    const TParamVar* entry = params->getParamVar(index);
    if (entry->isHidden() || entry->isObsolete()) continue;
    result.append(parameterDescription(fx, entry->getParam(),
                                       QString::fromStdString(entry->getName()),
                                       frame));
  }
  return result;
}

// The group a port belongs to and its position in it, or null.
QJsonValue portGroup(TFx* fx, TFxPort* port) {
  const int group = port->getGroupIndex();
  if (group < 0 || group >= fx->dynamicPortGroupsCount()) return QJsonValue();
  const TFxPortDG* ports = fx->dynamicPortGroup(group);
  const auto& members    = ports->ports();
  const auto found       = std::find(members.begin(), members.end(), port);
  return QJsonObject{{"prefix", QString::fromStdString(ports->portsPrefix())},
                     {"position", static_cast<int>(found - members.begin())}};
}

QJsonArray portsDescription(TFx* fx, Document* document = nullptr) {
  QJsonArray ports;
  for (int index = 0; index < fx->getInputPortCount(); ++index) {
    TFxPort* port = fx->getInputPort(index);
    QJsonObject entry{
        {"name", QString::fromStdString(fx->getInputPortName(index))},
        {"type", "raster"},
        {"control", port->isaControlPort()},
        {"connected", port->isConnected()},
        {"group", portGroup(fx, port)}};
    if (document && port->isConnected())
      entry["source_fx_id"] = document->idForFx(port->getFx());
    ports.append(entry);
  }
  return ports;
}

// A node's dynamic port groups: prefix, the native minimum, the most
// fx.port_group.set accepts and whether the worker edits the group.
QJsonArray portGroupsDescription(TFx* fx) {
  QJsonArray groups;
  const int maximum = automationFxMaxGroupPorts(fx->getFxType());
  for (int group = 0; group < fx->dynamicPortGroupsCount(); ++group) {
    const TFxPortDG* ports = fx->dynamicPortGroup(group);
    groups.append(
        QJsonObject{{"prefix", QString::fromStdString(ports->portsPrefix())},
                    {"min_ports", ports->minPortsCount()},
                    {"max_ports", std::max(maximum, ports->minPortsCount())},
                    {"editable", maximum > 0}});
  }
  return groups;
}

QJsonObject catalogEntry(const QString& type, const EffectFactory& factory) {
  TFxP fx = factory(type);
  const std::string plugin = fx->getPluginId();
  QJsonObject entry{
      {"type", type},
      {"available", true},
      {"origin",
       QJsonObject{
           {"kind", "builtin"},
           {"plugin_id", plugin.empty()
                             ? QJsonValue()
                             : QJsonValue(QString::fromStdString(plugin))}}},
      {"hidden", TFx::getFxInfo(type.toStdString()).m_isHidden},
      {"native_fx_version", fx->getFxVersion()},
      {"authoring", QJsonArray{"rename", "enabled.set", "duplicate",
                               "preset.save", "preset.apply"}},
      {"generator", dynamic_cast<TZeraryFx*>(fx.getPointer()) != nullptr},
      {"ports", portsDescription(fx.getPointer())},
      {"port_groups", portGroupsDescription(fx.getPointer())},
      {"parameters", parameterDescriptions(fx.getPointer(), 0)}};
  const QJsonObject profile = automationFxProfileDescription(type);
  for (auto field = profile.begin(); field != profile.end(); ++field)
    entry.insert(field.key(), field.value());
  return entry;
}

QJsonValue effectCatalog(const EffectFactory& factory = makeEffect) {
  QJsonArray effects;
  for (const auto& type : allowedTypes()) {
    try {
      effects.append(catalogEntry(type, factory));
    } catch (const Error& error) {
      if (error.code != "UNAVAILABLE_FX") throw;
      effects.append(QJsonObject{
          {"type", type}, {"available", false}, {"reason", error.message}});
    }
  }
  return QJsonObject{{"profile", automationFxProfileName()},
                     {"menu_categories", "not_reported"},
                     {"external_dependencies", QJsonArray{}},
                     {"effects", effects},
                     {"output_creation_type", "output"}};
}

QJsonArray diagnostics(TFx* fx) { return automationFxProfileDiagnostics(fx); }

// graph is the sheet graph that holds fx; null means the current sheet's.
QJsonObject describeFx(Document& document, TFx* fx, FxDag* graph = nullptr) {
  if (!graph) graph = dag(document);
  TFx* native = parameterFx(fx);
  QJsonObject result{{"fx_id", document.idForFx(fx)},
                     {"type", QString::fromStdString(native->getFxType())},
                     {"name", QString::fromStdWString(native->getName())},
                     {"enabled", fx->getAttributes()->isEnabled() &&
                                     native->getAttributes()->isEnabled()},
                     {"native_fx_version", native->getFxVersion()},
                     {"ports", portsDescription(fx, &document)},
                     {"port_groups", portGroupsDescription(fx)},
                     {"terminal", graph->getTerminalFxs()->containsFx(fx)},
                     {"diagnostics", diagnostics(native)}};
  if (auto* column = dynamic_cast<TColumnFx*>(fx)) {
    result["column_id"]         = document.idForColumn(column->getXshColumn());
    result["column_index"]      = column->getColumnIndex();
    result["generator_wrapper"] = dynamic_cast<TZeraryColumnFx*>(fx) != nullptr;
  }
  if (auto* macro = dynamic_cast<TMacroFx*>(fx))
    result["macro"] = describeAutomationMacro(document, macro);
  return result;
}

// Read-only documents describe effects outside the profile without their
// parameters or diagnostics.
QJsonObject reducedFx(Document& document, TFx* fx, FxDag* graph) {
  auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx);
  TFx* native = wrapper && wrapper->getZeraryFx() ? wrapper->getZeraryFx() : fx;
  QJsonObject result{{"fx_id", document.idForFx(fx)},
                     {"type", QString::fromStdString(native->getFxType())},
                     {"name", QString::fromStdWString(native->getName())},
                     {"enabled", fx->getAttributes()->isEnabled() &&
                                     native->getAttributes()->isEnabled()},
                     {"ports", portsDescription(fx, &document)},
                     {"terminal", graph->getTerminalFxs()->containsFx(fx)}};
  if (auto* column = dynamic_cast<TColumnFx*>(fx)) {
    TXshColumn* owner = column->getXshColumn();
    result["column_id"] =
        owner ? QJsonValue(document.idForColumn(owner)) : QJsonValue();
    result["column_index"]      = column->getColumnIndex();
    result["generator_wrapper"] = wrapper != nullptr;
  }
  return result;
}

QJsonValue nodePosition(TFx* fx) {
  const TPointD point = fx->getAttributes()->getDagNodePos();
  if (point == TConst::nowhere) return QJsonValue();
  return QJsonObject{{"x", point.x}, {"y", point.y}};
}

// Schematic placement, groups (innermost first) and the linked group, read
// from the journal's view of the top sheet.
void addLayout(QJsonObject& entry, const QJsonObject& view) {
  const QJsonObject state = view.value("nodes")
                                .toObject()
                                .value(entry.value("fx_id").toString())
                                .toObject();
  entry.insert("position", state.value("position"));
  entry.insert("group_ids", state.value("group_ids").toArray());
  entry.insert("linked_group_id", state.value("linked_group_id"));
}

QJsonArray linkedGroups(const QJsonObject& view) {
  QJsonArray groups;
  const QJsonObject linked = view.value("linked_groups").toObject();
  for (auto it = linked.begin(); it != linked.end(); ++it)
    groups.append(QJsonObject{{"group_id", it.key()}, {"fx_ids", it.value()}});
  return groups;
}

std::vector<TFx*> creationOrder(Document& document, TXsheet* sheet) {
  const std::set<TFx*> members = sheetMembers(sheet);
  std::vector<std::pair<quint64, TFx*>> ordered;
  for (TFx* fx : members)
    if (fx) ordered.push_back({automationFxSequence(document, fx), fx});
  std::sort(ordered.begin(), ordered.end());
  std::vector<TFx*> result;
  for (const auto& entry : ordered) result.push_back(entry.second);
  return result;
}

QJsonObject graphEntry(Document& document, TFx* fx, FxDag* graph) {
  QJsonObject entry;
  if (!document.readOnly) {
    entry = describeFx(document, fx, graph);
  } else {
    try {
      entry = fxAdmitted(fx) ? describeFx(document, fx, graph)
                             : reducedFx(document, fx, graph);
    } catch (const Error&) {
      entry = reducedFx(document, fx, graph);
    }
  }
  entry.insert("admission", fxAdmission(document, fx));
  return entry;
}

// sheet_path names a sub-xsheet instance; without it, the current sheet.
constexpr int kDefaultGraphPage   = 64;
constexpr int kMaximumGraphPage   = 512;
constexpr int kMaximumPageBytes   = 1024 * 1024;
constexpr int kMaximumGraphNodes  = 4096;
constexpr int kReadOnlyGraphNodes = 16384;

// A page after the first, or any page naming expected_revision, is bound to
// the revision the caller started from.
void requireGraphRevision(Document& document, const QJsonObject& params,
                          int offset) {
  const bool named = params.contains("expected_revision");
  if (!named && offset == 0) return;
  if (named &&
      params.value("expected_revision").toString() == document.revisionText())
    return;
  throw Error("stale_revision",
              named ? "The document changed since the first page; read "
                      "fx.graph again from offset 0."
                    : "Pages after the first need expected_revision.",
              {{"revision", document.revisionText()}});
}

// Every node a document counts toward its limit: effects and macro members.
int countedNodes(const std::vector<TFx*>& effects) {
  int count = 0;
  for (TFx* fx : effects) {
    ++count;
    if (auto* macro = dynamic_cast<TMacroFx*>(fx))
      count += static_cast<int>(macro->getFxs().size());
  }
  return count;
}

// What constrains a write to an effect's parameters, at most eight reasons:
// a locked column or an effect outside the profile refuses it, and a linked
// group (via its id) makes it a write to every member.
QJsonArray editConstraints(Document& document, TFx* fx,
                           const QJsonObject& entry) {
  QJsonArray constraints;
  auto add = [&constraints](const char* kind, const QJsonValue& via) {
    constraints.append(QJsonObject{
        {"kind", kind}, {"via", via}, {"parameters", QJsonValue()}});
  };
  const QJsonValue linked = automationFxLinkedGroup(document, fx);
  if (linked.isObject())
    add("linked_parameters", linked.toObject().value("group_id"));
  if (auto* column = dynamic_cast<TColumnFx*>(fx)) {
    TStageObject* object = columnObject(document, column);
    if ((object && object->isLocked()) ||
        (column->getXshColumn() && column->getXshColumn()->isLocked()))
      add("locked_column", entry.value("column_id"));
  }
  const QJsonValue admission = entry.value("admission");
  if (admission.isObject() && !admission.toObject().value("editable").toBool())
    add("outside_profile", QJsonValue());
  return constraints;
}

// The schematic and linked groups that have a member on this page.
QJsonArray pageGroups(const QJsonArray& groups, const QSet<QString>& page) {
  QJsonArray result;
  for (const QJsonValue& value : groups)
    for (const QJsonValue& member : value.toObject().value("fx_ids").toArray())
      if (page.contains(member.toString())) {
        result.append(value);
        break;
      }
  return result;
}

QJsonObject graphPage(Document& document, TXsheet* sheet, int offset,
                      int limit) {
  FxDag* graph   = sheet->getFxDag();
  const bool top = sheet == document.scene->getXsheet();
  const QJsonObject view =
      top ? automationFxGraphView(document) : QJsonObject();
  const std::vector<TFx*> ordered = creationOrder(document, sheet);
  QJsonArray effects;
  QSet<QString> page;
  for (size_t index = offset; index < ordered.size() && effects.size() < limit;
       ++index) {
    QJsonObject entry = graphEntry(document, ordered[index], graph);
    if (top) addLayout(entry, view);
    entry.insert("edit_constraints",
                 editConstraints(document, ordered[index], entry));
    page.insert(entry.value("fx_id").toString());
    effects.append(entry);
  }
  const int next = offset + effects.size();
  QJsonObject result{
      {"document_id", document.id},
      {"revision", document.revisionText()},
      {"effects", effects},
      {"total_effects", static_cast<int>(ordered.size())},
      {"node_count", countedNodes(ordered)},
      {"offset", offset},
      {"limit", limit},
      {"next_offset", next < static_cast<int>(ordered.size()) ? QJsonValue(next)
                                                              : QJsonValue()},
      {"limits", QJsonObject{{"nodes", document.readOnly ? kReadOnlyGraphNodes
                                                         : kMaximumGraphNodes},
                             {"page_bytes", kMaximumPageBytes}}},
      {"linked_groups",
       top ? pageGroups(linkedGroups(view), page) : QJsonArray()}};
  if (top)
    result.insert("groups", pageGroups(automationFxGroups(document), page));
  return result;
}

// sheet_path names a sub-xsheet instance; without it, the current sheet.
// Effects come a page at a time in creation order; next_offset is null on
// the last page.
QJsonValue inspectGraph(Document& document, const QJsonObject& params) {
  TXsheet* sheet =
      params.contains("sheet_path")
          ? resolveSheetPath(document, params.value("sheet_path").toArray())
          : document.scene->getXsheet();
  const int offset = integerParam(params, "offset", 0, 16384, 0);
  const int limit =
      integerParam(params, "limit", 1, kMaximumGraphPage, kDefaultGraphPage);
  requireGraphRevision(document, params, offset);
  if (!document.readOnly) validateAutomationFxGraph(*document.scene);
  FxDag* graph       = sheet->getFxDag();
  TFx* output        = graph->getCurrentOutputFx();
  QJsonObject result = graphPage(document, sheet, offset, limit);
  result.insert("xsheet_fx_id", document.idForFx(graph->getXsheetFx()));
  result.insert("current_output_fx_id",
                output ? QJsonValue(document.idForFx(output)) : QJsonValue());
  result.insert("delta_window", automationFxDeltaWindow(document));
  if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() >
      kMaximumPageBytes)
    throw Error("resource_limit",
                "This fx.graph page exceeds 1 MiB; ask for a smaller limit.",
                {{"limit", "page_bytes"}});
  return result;
}

void assignGrammar(TParam* param, TXsheet* sheet) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param)) {
    sheet->getStageObjectTree()->setGrammar(number);
  } else if (auto* parameters = dynamic_cast<TParamSet*>(param)) {
    for (int index = 0; index < parameters->getParamCount(); ++index)
      assignGrammar(parameters->getParam(index).getPointer(), sheet);
  }
}

TFx* insertGenerator(Document& document, TFx* fx, int frameCount) {
  TXsheet* sheet             = document.scene->getXsheet();
  TXshZeraryFxColumnP column = new TXshZeraryFxColumn(frameCount);
  TZeraryColumnFx* wrapper   = column->getZeraryColumnFx();
  wrapper->setZeraryFx(fx);
  const int index = sheet->getColumnCount();
  sheet->insertColumn(index, column.getPointer());
  // New nodes are deliberately disconnected; routing is a separate explicit
  // operation.
  dag(document)->removeFromXsheet(wrapper);
  sheet->updateFrameCount();
  if (sheet->getColumn(index) != column.getPointer() ||
      wrapper->getZeraryFx() != fx)
    throw Error("NATIVE_FAILURE",
                "Generator column insertion did not take effect.");
  return wrapper;
}

TFx* insertEffect(Context& context, Document& document, TFx* fx, int frames) {
  TXsheet* sheet = document.scene->getXsheet();
  for (int index = 0; index < fx->getParams()->getParamCount(); ++index)
    assignGrammar(fx->getParams()->getParam(index), sheet);
  context.markMutationStarted();
  dag(document)->assignUniqueId(fx);
  fx->setNewIdentifier();
  if (auto* output = dynamic_cast<TOutputFx*>(fx))
    dag(document)->addOutputFx(output);
  else if (dynamic_cast<TZeraryFx*>(fx))
    fx = insertGenerator(document, fx, frames);
  else
    dag(document)->getInternalFxs()->addFx(fx);
  if (graphMembers(document).count(fx) == 0)
    throw Error("NATIVE_FAILURE",
                "Created effect was not inserted into the scene graph.");
  return fx;
}

QString authoringName(const QJsonObject& params);

QJsonValue createEffect(Context& context, Document& document,
                        const QJsonObject& params) {
  const QString type = requiredString(params, "type");
  const int frames   = integerParam(params, "frame_count", 1, 10000, 1);
  if (graphNodeCount(document) >= 4096)
    throw Error("RESOURCE_LIMIT",
                "The automation graph is limited to 4096 effect nodes.");
  TFxP effect = type == "output" ? makeOutput() : makeEffect(type);
  if (params.contains("name"))
    effect->setName(authoringName(params).toStdWString());
  return describeFx(
      document, insertEffect(context, document, effect.getPointer(), frames));
}

QString authoringName(const QJsonObject& params) {
  const QString name = requiredString(params, "name");
  if (name.toUcs4().size() > 256)
    throw Error("INVALID_PARAMS",
                "Effect names are limited to 256 Unicode characters.");
  for (QChar character : name)
    if (character.isNull() || character.category() == QChar::Other_Control)
      throw Error("INVALID_PARAMS",
                  "Effect names cannot contain control characters.");
  return name;
}

QJsonValue renameEffect(Context& context, Document& document,
                        const QJsonObject& params) {
  TFx* fx = ownedFx(document, requiredString(params, "fx_id"));
  if (!dynamic_cast<TMacroFx*>(fx)) requireEditable(fx);
  requireColumnEditable(document, fx);
  const auto name    = authoringName(params).toStdWString();
  TFx* native        = parameterFx(fx);
  const bool changed = native->getName() != name;
  if (changed) {
    context.markMutationStarted();
    native->setName(name);
    if (native->getName() != name)
      throw Error("NATIVE_FAILURE",
                  "Native effect rename did not take effect.");
  }
  auto result       = describeFx(document, fx);
  result["changed"] = changed;
  return result;
}

QJsonValue enableEffect(Context& context, Document& document,
                        const QJsonObject& params) {
  TFx* fx = ownedFx(document, requiredString(params, "fx_id"));
  requireEditable(fx);
  requireColumnEditable(document, fx);
  if (!params.value("enabled").isBool())
    throw Error("INVALID_PARAMS", "enabled must be a boolean.");
  const bool enabled = params.value("enabled").toBool();
  TFx* native        = parameterFx(fx);
  const bool changed = fx->getAttributes()->isEnabled() != enabled ||
                       native->getAttributes()->isEnabled() != enabled;
  if (changed) {
    context.markMutationStarted();
    fx->getAttributes()->enable(enabled);
    native->getAttributes()->enable(enabled);
  }
  if (fx->getAttributes()->isEnabled() != enabled ||
      native->getAttributes()->isEnabled() != enabled)
    throw Error("NATIVE_FAILURE",
                "Native effect enable state did not take effect.");
  auto result       = describeFx(document, fx);
  result["enabled"] = enabled;
  result["changed"] = changed;
  return result;
}

QJsonValue duplicateEffect(Context& context, Document& document,
                           const QJsonObject& params) {
  TFx* source = ownedFx(document, requiredString(params, "fx_id"));
  requireEditable(source);
  requireColumnEditable(document, source);
  const int frames = integerParam(params, "frame_count", 1, 10000, 1);
  if (graphNodeCount(document) >= 4096)
    throw Error("RESOURCE_LIMIT",
                "The automation graph is limited to 4096 effect nodes.");
  TFx* native = parameterFx(source);
  if (params.contains("frame_count") && !dynamic_cast<TZeraryFx*>(native))
    throw Error("INVALID_PARAMS",
                "frame_count applies only to a duplicated generator.");
  const auto type = QString::fromStdString(native->getFxType());
  const auto name = params.contains("name")
                        ? authoringName(params).toStdWString()
                        : native->getName();
  constraintForFx(document, source,
                  [native] { validateAutomationFxProfile(native); });
  TFxP copy = makeEffect(type);
  copy->setFxVersion(native->getFxVersion());
  copy->getParams()->copy(native->getParams());
  copy->setName(name);
  const bool enabled = source->getAttributes()->isEnabled() &&
                       native->getAttributes()->isEnabled();
  copy->getAttributes()->enable(enabled);
  TFx* inserted = insertEffect(context, document, copy.getPointer(), frames);
  inserted->getAttributes()->enable(enabled);
  auto result            = describeFx(document, inserted);
  result["source_fx_id"] = document.idForFx(source);
  result["enabled"]      = enabled;
  result["changed"]      = true;
  return result;
}

constexpr qint64 maximumPresetBytes = 1024 * 1024;

struct PublishedPreset final {
  QJsonObject manifest;
  QByteArray manifestBytes;
};

std::map<QString, PublishedPreset> publishedPresets;

QByteArray presetDigest(const QByteArray& bytes) {
  return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

void requirePresetKeyBudget(TFx* fx) {
  std::vector<TParam*> pending;
  for (int index = 0; index < fx->getParams()->getParamCount(); ++index)
    pending.push_back(fx->getParams()->getParam(index));
  qint64 keys = 0;
  int visited = 0;
  while (!pending.empty()) {
    TParam* param = pending.back();
    pending.pop_back();
    if (++visited > 4096)
      throw Error("RESOURCE_LIMIT", "Preset parameter structure is too large.");
    if (auto* number = dynamic_cast<TDoubleParam*>(param))
      keys += number->getKeyframeCount();
    if (auto* set = dynamic_cast<TParamSet*>(param))
      for (int index = 0; index < set->getParamCount(); ++index)
        pending.push_back(set->getParam(index).getPointer());
    if (keys > 4096)
      throw Error("RESOURCE_LIMIT", "Presets are limited to 4096 scalar keys.");
  }
}

QByteArray readPresetFile(const QString& path) {
  const QFileInfo info(path);
  if (!info.isFile() || info.isSymLink() ||
      info.canonicalFilePath() != QDir::cleanPath(path))
    throw Error("INVALID_PRESET", "Preset files must be regular owned files.");
  if (info.size() > maximumPresetBytes)
    throw Error("RESOURCE_LIMIT", "Preset files are limited to 1 MiB.");
  QFile file;
  if (openApprovedFile(file, path) != OpenResult::Opened)
    throw Error("PRESET_READ_FAILED", "Cannot open the preset file.");
  const QByteArray bytes = file.read(maximumPresetBytes + 1);
  if (file.error() != QFileDevice::NoError || bytes.size() != info.size())
    throw Error("PRESET_READ_FAILED", "Cannot read the complete preset file.");
  return bytes;
}

void writePresetFile(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
      file.write(bytes) != bytes.size() || !file.flush())
    throw Error("PRESET_WRITE_FAILED", "Cannot write the private preset file.");
}

QByteArray saveNativePreset(TFx* fx, const QString& path) {
  validateAutomationFxProfile(fx);
  requirePresetKeyBudget(fx);
  try {
    TOStream stream(TFilePath(path.toStdWString()));
    if (!stream)
      throw Error("PRESET_WRITE_FAILED",
                  "Cannot create a native preset stream.");
    stream.setFullPrecision(true);
    fx->savePreset(stream);
  } catch (const TException&) {
    throw Error("PRESET_WRITE_FAILED", "Native preset serialization failed.");
  }
  return readPresetFile(path);
}

TFxP verifiedPreset(Document& document, TFx* source, const QByteArray& bytes,
                    const QString& staging) {
  TFxP candidate = makeEffect(QString::fromStdString(source->getFxType()));
  candidate->setFxVersion(source->getFxVersion());
  for (int index = 0; index < candidate->getParams()->getParamCount(); ++index)
    assignGrammar(candidate->getParams()->getParam(index),
                  document.scene->getXsheet());
  const QString path = staging + "/input.preset";
  writePresetFile(path, bytes);
  try {
    TIStream stream(TFilePath(path.toStdWString()));
    if (!stream)
      throw Error("INVALID_PRESET", "Cannot open the native preset stream.");
    candidate->loadPreset(stream);
  } catch (const TException&) {
    throw Error("INVALID_PRESET", "Native preset loading failed.");
  }
  if (saveNativePreset(candidate.getPointer(), staging + "/verified.preset") !=
      bytes)
    throw Error(
        "INVALID_PRESET",
        "The native preset did not preserve every serialized parameter.");
  return candidate;
}

QJsonObject presetManifest(Document& document, TFx* native,
                           const QByteArray& bytes) {
  return {{"format_version", 1},
          {"type", QString::fromStdString(native->getFxType())},
          {"native_fx_version", native->getFxVersion()},
          {"sha256", QString::fromLatin1(presetDigest(bytes))},
          {"size_bytes", bytes.size()},
          {"source_revision", document.revisionText()}};
}

QString newPresetDirectory(Context& context, const QJsonObject& params) {
  const QString requested = requiredString(params, "directory");
  if (!QDir::isAbsolutePath(requested))
    throw Error("INVALID_PARAMS",
                "Preset directory must be an absolute workspace path.");
  const QString path = context.resolvePath(requested, true);
  const QFileInfo target(path);
  if (target.exists() || target.isSymLink())
    throw Error(
        "ALREADY_EXISTS",
        "Choose a new preset directory; existing paths are not overwritten.");
  if (!QDir(target.absolutePath()).exists())
    throw Error("MISSING_PARENT", "Create the preset parent directory first.");
  if (!QFileInfo(target.absolutePath()).isWritable())
    throw Error("PRESET_WRITE_FAILED",
                "The preset parent directory is not writable.");
  if (publishedPresets.size() >= 128)
    throw Error("RESOURCE_LIMIT",
                "This worker has reached its 128 published preset limit.");
  return path;
}

QJsonValue savePreset(Context& context, Document& document,
                      const QJsonObject& params) {
  TFx* fx = ownedFx(document, requiredString(params, "fx_id"));
  requireEditable(fx);
  TFx* native             = parameterFx(fx);
  const QString directory = newPresetDirectory(context, params);
  PrivateDirectory staging(context.runtimeDirectory + "/preset-XXXXXX");
  if (!staging.isValid())
    throw Error("PRESET_WRITE_FAILED",
                "Cannot create a private preset staging directory.");
  const QByteArray bytes =
      saveNativePreset(native, staging.path() + "/effect.preset");
  PrivateDirectory verification(context.scratchDirectory() +
                                "/preset-check-XXXXXX");
  if (!verification.isValid())
    throw Error("PRESET_WRITE_FAILED",
                "Cannot create a private preset verification directory.");
  verifiedPreset(document, native, bytes, verification.path());
  const QJsonObject manifest = presetManifest(document, native, bytes);
  const QByteArray manifestBytes =
      QJsonDocument(manifest).toJson(QJsonDocument::Compact);
  writePresetFile(staging.path() + "/manifest.json", manifestBytes);
  publishStaged(context, staging.path(), directory, "PRESET_WRITE_FAILED",
                "Cannot publish the verified preset directory.");
  staging.setAutoRemove(false);
  publishedPresets.emplace(directory, PublishedPreset{manifest, manifestBytes});
  return QJsonObject{{"preset_directory", directory},
                     {"manifest", manifest},
                     {"fx_id", document.idForFx(fx)}};
}

void requirePresetIdentity(const QJsonObject& manifest, TFx* native) {
  if (manifest.value("type") != QString::fromStdString(native->getFxType()) ||
      manifest.value("native_fx_version") != native->getFxVersion())
    throw Error("INCOMPATIBLE_PRESET",
                "Preset effect type and native version must match the target.");
}

QByteArray admittedPreset(Context& context, TFx* native,
                          const QJsonObject& params) {
  const QString requested = requiredString(params, "directory");
  if (!QDir::isAbsolutePath(requested))
    throw Error("INVALID_PARAMS",
                "Preset directory must be an absolute workspace path.");
  const QString directory = context.resolvePath(requested, false);
  const auto found        = publishedPresets.find(directory);
  if (found == publishedPresets.end())
    throw Error("UNREGISTERED_PRESET",
                "Apply only a preset published by this worker.");
  const auto& preset = found->second;
  if (readPresetFile(directory + "/manifest.json") != preset.manifestBytes)
    throw Error("INVALID_PRESET",
                "The preset manifest changed after publication.");
  if (QDir(directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                QDir::NoDotAndDotDot) !=
      QStringList{"effect.preset", "manifest.json"})
    throw Error("INVALID_PRESET",
                "The preset directory contains unexpected entries.");
  const auto& manifest = preset.manifest;
  requirePresetIdentity(manifest, native);
  const QByteArray bytes = readPresetFile(directory + "/effect.preset");
  if (bytes.size() != manifest.value("size_bytes").toInt() ||
      QString::fromLatin1(presetDigest(bytes)) !=
          manifest.value("sha256").toString())
    throw Error("INVALID_PRESET",
                "The native preset changed after publication.");
  return bytes;
}

void replacePresetParameters(TFx* target, TFx* candidate, TFx* backup,
                             const QByteArray& bytes, const QString& staging) {
  try {
    target->getParams()->copy(candidate->getParams());
    if (saveNativePreset(target, staging + "/applied.preset") != bytes)
      throw Error("NATIVE_FAILURE",
                  "Applied preset failed its native parameter readback.");
  } catch (...) {
    target->getParams()->copy(backup->getParams());
    throw;
  }
}

QJsonValue applyPreset(Context& context, Document& document,
                       const QJsonObject& params) {
  TFx* fx = ownedFx(document, requiredString(params, "fx_id"));
  requireEditable(fx);
  requireLinkedEditable(document, fx);
  requireUnlockedCurves(document, fx);
  TFx* native            = parameterFx(fx);
  const QByteArray bytes = admittedPreset(context, native, params);
  PrivateDirectory staging(context.scratchDirectory() + "/preset-apply-XXXXXX");
  if (!staging.isValid())
    throw Error("PRESET_WRITE_FAILED",
                "Cannot create a private preset verification directory.");
  TFxP candidate = constraintForFx(document, fx, [&] {
    return verifiedPreset(document, native, bytes, staging.path());
  });
  const bool changed =
      saveNativePreset(native, staging.path() + "/previous.preset") != bytes;
  if (changed) {
    TFxP backup = makeEffect(QString::fromStdString(native->getFxType()));
    backup->setFxVersion(native->getFxVersion());
    backup->getParams()->copy(native->getParams());
    context.markMutationStarted();
    replacePresetParameters(native, candidate.getPointer(), backup.getPointer(),
                            bytes, staging.path());
  }
  QJsonObject result{{"changed", changed},
                     {"fx_id", document.idForFx(fx)},
                     {"sha256", QString::fromLatin1(presetDigest(bytes))}};
  addAffected(document, fx, result);
  result.remove("linked_group_id");
  return result;
}

void appendInputs(TFx* fx, FxDag* graph, std::vector<TFx*>& pending) {
  if (fx == graph->getXsheetFx()) {
    TFxSet* terminals = graph->getTerminalFxs();
    for (int index = 0; index < terminals->getFxCount(); ++index)
      pending.push_back(terminals->getFx(index));
    return;
  }
  fx = parameterFx(fx);
  for (int index = 0; index < fx->getInputPortCount(); ++index) {
    TFx* input = fx->getInputPort(index)->getFx();
    if (input) pending.push_back(input);
  }
}

bool dependsOn(TFx* fx, TFx* target, FxDag* graph) {
  std::vector<TFx*> pending{fx};
  std::set<TFx*> visited;
  while (!pending.empty()) {
    TFx* current = pending.back();
    pending.pop_back();
    if (current == target) return true;
    if (!visited.insert(current).second) continue;
    appendInputs(current, graph, pending);
  }
  return false;
}

void validateColumnOwnership(TColumnFx* fx, TXsheet* sheet) {
  TXshColumn* column = fx->getXshColumn();
  if (!column)
    throw Error("INVALID_GRAPH",
                "Column effect is missing a scene-owned column.");
  const int index = fx->getColumnIndex();
  if (index < 0 || index >= sheet->getColumnCount())
    throw Error("INVALID_GRAPH",
                "Column effect is missing a scene-owned column.");
  if (sheet->getColumn(index) != column || column->getFx() != fx)
    throw Error("INVALID_GRAPH",
                "Column effect and scene column ownership disagree.");
  if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx)) {
    requireEditable(wrapper);
    if (wrapper->getZeraryFx()->getColumnFx() != wrapper)
      throw Error("INVALID_GRAPH",
                  "Generator does not belong to its column wrapper.");
    validateAutomationFxProfile(wrapper->getZeraryFx());
  }
}

void validateOutputOwnership(TOutputFx* fx, FxDag* graph) {
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    if (graph->getOutputFx(index) == fx) return;
  throw Error("INVALID_GRAPH",
              "Output effect is not registered as a scene output.");
}

void validateNode(TFx* fx, TXsheet* sheet) {
  FxDag* graph = sheet->getFxDag();
  if (fx == graph->getXsheetFx()) return;
  if (auto* output = dynamic_cast<TOutputFx*>(fx)) {
    validateOutputOwnership(output, graph);
    return;
  }
  if (auto* column = dynamic_cast<TColumnFx*>(fx)) {
    validateColumnOwnership(column, sheet);
    return;
  }
  if (dynamic_cast<TMacroFx*>(fx)) return;
  if (dynamic_cast<TZeraryFx*>(fx))
    throw Error("INVALID_GRAPH",
                "A generator must be owned by a scene column wrapper.");
  requireEditable(fx);
  validateAutomationFxProfile(fx);
}

void validateEdges(const std::set<TFx*>& members, FxDag* graph) {
  std::set<TFx*> visited, active;
  for (TFx* root : members) {
    std::vector<std::pair<TFx*, bool>> pending{{root, false}};
    while (!pending.empty()) {
      const auto entry = pending.back();
      pending.pop_back();
      TFx* fx = entry.first;
      if (entry.second) {
        active.erase(fx);
        continue;
      }
      if (active.count(fx))
        throw Error("GRAPH_CYCLE", "Native effect graph contains a cycle.");
      if (!visited.insert(fx).second) continue;
      active.insert(fx);
      pending.emplace_back(fx, true);
      std::vector<TFx*> inputs;
      appendInputs(fx, graph, inputs);
      for (TFx* input : inputs) {
        if (members.count(input) == 0)
          throw Error("INVALID_GRAPH",
                      "An input references an effect outside the scene graph.");
        pending.emplace_back(input, false);
      }
    }
  }
}

TFxPort* editablePort(TFx* fx, const QString& name) {
  if (!dynamic_cast<TOutputFx*>(fx) && !dynamic_cast<TMacroFx*>(fx))
    requireEditable(fx);
  TFxPort* port = fx->getInputPort(name.toStdString());
  if (port) return port;
  QJsonObject details{{"port", name}};
  for (int group = 0; group < fx->dynamicPortGroupsCount(); ++group) {
    const QString prefix =
        QString::fromStdString(fx->dynamicPortGroup(group)->portsPrefix());
    if (!name.startsWith(prefix)) continue;
    details["port_group"] = prefix;
    details["use"]        = "fx.port_group.set";
  }
  throw Error("UNKNOWN_PORT", "Effect has no input port with this exact name.",
              details);
}

void validateConnection(FxDag* graph, TFx* destination, TFxPort* port,
                        TFx* source) {
  if (!source) return;
  if (dynamic_cast<TOutputFx*>(source))
    throw Error("INVALID_CONNECTION",
                "An output node cannot be used as an input source.");
  if (!dynamic_cast<TRasterFx*>(source) || !port->accepts(source))
    throw Error("PORT_TYPE_MISMATCH",
                "Input port does not accept this source effect.");
  if (dependsOn(source, destination, graph))
    throw Error(
        "GRAPH_CYCLE",
        "Connection would create a cycle, including Xsheet terminal links.");
}

// A native connection that does not take effect is undone: the port gets its
// previous source back, and a refusal that changed nothing is reported as one
// rather than as a partial change. The test fault point native:fx-connect
// makes the native call leave the port empty.
void setPortSource(Context& context, Document& document, TFxPort* port,
                   TFx* source) {
  TFxP previous      = port->getFx();
  const bool started = context.mutationStarted();
  context.markMutationStarted();
  try {
    port->setFx(writeFaultFires(context, "native:fx-connect") ? nullptr
                                                              : source);
    if (port->getFx() == source) return;
  } catch (...) {
    port->setFx(previous.getPointer());
    throw;
  }
  port->setFx(previous.getPointer());
  const bool restored = port->getFx() == previous.getPointer();
  if (restored && !started) context.resetMutationStarted();
  throw Error("NATIVE_FAILURE", "Native port connection did not take effect.",
              {{"reason", "silent_refusal"},
               {"restored", restored},
               {"previous_source_fx_id",
                previous ? QJsonValue(document.idForFx(previous.getPointer()))
                         : QJsonValue()}});
}

QJsonValue connectEffect(Context& context, Document& document,
                         const QJsonObject& params, bool disconnect) {
  validateAutomationFxGraph(*document.scene);
  TFx* destination = ownedFx(document, requiredString(params, "fx_id"));
  TFxPort* port    = editablePort(destination, requiredString(params, "port"));
  TFx* source = disconnect
                    ? nullptr
                    : ownedFx(document, requiredString(params, "source_fx_id"));
  validateConnection(dag(document), destination, port, source);
  if (port->getFx() == source) return describeFx(document, destination);
  setPortSource(context, document, port, source);
  return describeFx(document, destination);
}

// The ports of one dynamic group of fx, in port array order.
std::vector<std::pair<std::string, TFxPort*>> groupPorts(TFx* fx, int group) {
  std::vector<std::pair<std::string, TFxPort*>> ports;
  for (int index = 0; index < fx->getInputPortCount(); ++index)
    if (fx->getInputPort(index)->getGroupIndex() == group)
      ports.push_back({fx->getInputPortName(index), fx->getInputPort(index)});
  return ports;
}

int editableGroup(TFx* fx, const QString& prefix) {
  QJsonArray prefixes;
  const bool editable = automationFxMaxGroupPorts(fx->getFxType()) > 0;
  for (int group = 0; group < fx->dynamicPortGroupsCount(); ++group) {
    const QString name =
        QString::fromStdString(fx->dynamicPortGroup(group)->portsPrefix());
    if (editable) prefixes.append(name);
    if (editable && name == prefix) return group;
  }
  throw Error("unknown_port_group",
              "The effect has no editable dynamic port group with this "
              "prefix.",
              {{"group", prefix}, {"groups", prefixes}});
}

std::vector<TFx*> groupSources(Document& document, TFx* fx, TFxPort* sample,
                               const QJsonArray& ids, int maximum) {
  if (ids.size() > maximum)
    throw Error("resource_limit", "Too many sources for this port group.",
                {{"limit", "group_ports"}, {"maximum", maximum}});
  std::vector<TFx*> sources;
  for (const QJsonValue& id : ids) {
    TFx* source = ownedFx(document, id.toString());
    validateConnection(dag(document), fx, sample, source);
    sources.push_back(source);
  }
  return sources;
}

// The editor's normal form: ports reused in array order, port i fed by
// sources[i], one empty port after them, at least the group's minimum.
int targetPortCount(int minimum, int maximum, int sources) {
  return std::max(minimum, std::min(sources + 1, maximum));
}

bool groupMatches(TFx* fx, int group, const std::vector<TFx*>& sources,
                  int count) {
  const auto ports = groupPorts(fx, group);
  if (static_cast<int>(ports.size()) != count) return false;
  for (int index = 0; index < count; ++index)
    if (ports[index].second->getFx() !=
        (index < static_cast<int>(sources.size()) ? sources[index] : nullptr))
      return false;
  return true;
}

// Grows the group to count ports with the editor's names (prefix plus the
// first free number from the group size up), checking each name first: the
// native addInputPort deletes a port whose name exists.
QJsonArray growGroup(TFx* fx, int group, int count) {
  const std::string prefix = fx->dynamicPortGroup(group)->portsPrefix();
  QJsonArray added;
  while (static_cast<int>(groupPorts(fx, group).size()) < count) {
    int number = static_cast<int>(groupPorts(fx, group).size()) + 1;
    while (fx->getInputPort(prefix + std::to_string(number))) ++number;
    if (!fx->addInputPort(prefix + std::to_string(number), new TRasterFxPort,
                          group))
      throw Error("NATIVE_FAILURE",
                  "Native port creation did not take effect.");
    added.append(QString::fromStdString(prefix + std::to_string(number)));
  }
  return added;
}

QJsonArray shrinkGroup(TFx* fx, int group, int count) {
  QJsonArray removed;
  for (auto ports                                    = groupPorts(fx, group);
       static_cast<int>(ports.size()) > count; ports = groupPorts(fx, group)) {
    ports.back().second->setFx(nullptr);
    if (!fx->removeInputPort(ports.back().first))
      throw Error("NATIVE_FAILURE", "Native port removal did not take effect.");
    removed.append(QString::fromStdString(ports.back().first));
  }
  return removed;
}

QJsonValue setPortGroup(Context& context, Document& document,
                        const QJsonObject& params) {
  validateAutomationFxGraph(*document.scene);
  TFx* fx = ownedFx(document, requiredString(params, "fx_id"));
  if (dynamic_cast<TMacroFx*>(fx))
    throw Error("unknown_port_group",
                "Expand the macro to edit its members' port groups.");
  requireEditable(fx);
  requireColumnEditable(document, fx);
  const int group   = editableGroup(fx, requiredString(params, "group"));
  const int minimum = fx->dynamicPortGroup(group)->minPortsCount();
  const int maximum = automationFxMaxGroupPorts(fx->getFxType());
  const auto sources =
      groupSources(document, fx, groupPorts(fx, group).front().second,
                   params.value("sources").toArray(), maximum);
  const int count    = targetPortCount(minimum, maximum, int(sources.size()));
  const bool changed = !groupMatches(fx, group, sources, count);
  QJsonArray added, removed;
  if (changed) {
    context.markMutationStarted();
    added            = growGroup(fx, group, count);
    removed          = shrinkGroup(fx, group, count);
    const auto ports = groupPorts(fx, group);
    for (int index = 0; index < count; ++index)
      ports[index].second->setFx(index < int(sources.size()) ? sources[index]
                                                             : nullptr);
    if (!groupMatches(fx, group, sources, count))
      throw Error("NATIVE_FAILURE",
                  "Native port group edit did not take effect.");
  }
  QJsonArray ports;
  for (const auto& port : groupPorts(fx, group))
    ports.append(QJsonObject{
        {"name", QString::fromStdString(port.first)},
        {"source_fx_id", port.second->getFx() ? QJsonValue(document.idForFx(
                                                    port.second->getFx()))
                                              : QJsonValue()}});
  return QJsonObject{{"changed", changed},
                     {"fx_id", document.idForFx(fx)},
                     {"group", params.value("group")},
                     {"ports", ports},
                     {"ports_added", added},
                     {"ports_removed", removed},
                     {"min_ports", minimum},
                     {"max_ports", maximum}};
}

void requireTerminalEditable(TFx* fx) {
  if (!dynamic_cast<TColumnFx*>(fx) && !dynamic_cast<TMacroFx*>(fx))
    requireEditable(fx);
}

QJsonValue setTerminal(Context& context, Document& document,
                       const QJsonObject& params) {
  TFx* fx = ownedFx(document, requiredString(params, "fx_id"));
  validateAutomationFxGraph(*document.scene);
  requireTerminalEditable(fx);
  requireColumnEditable(document, fx);
  if (!params.value("enabled").isBool())
    throw Error("INVALID_PARAMS", "enabled must be a boolean.");
  const bool enabled = params.value("enabled").toBool();
  FxDag* graph       = dag(document);
  if (enabled && dependsOn(fx, graph->getXsheetFx(), graph))
    throw Error("GRAPH_CYCLE", "Xsheet terminal link would create a cycle.");
  if (graph->getTerminalFxs()->containsFx(fx) == enabled)
    return describeFx(document, fx);
  context.markMutationStarted();
  if (enabled)
    graph->addToXsheet(fx);
  else
    graph->removeFromXsheet(fx);
  if (graph->getTerminalFxs()->containsFx(fx) != enabled)
    throw Error("NATIVE_FAILURE",
                "Native Xsheet terminal update did not take effect.");
  return describeFx(document, fx);
}

QJsonValue selectOutput(Context& context, Document& document,
                        const QJsonObject& params) {
  TFx* fx      = ownedFx(document, requiredString(params, "fx_id"));
  auto* output = dynamic_cast<TOutputFx*>(fx);
  if (!output)
    throw Error("INVALID_OUTPUT",
                "Selected effect is not a scene output node.");
  if (dag(document)->getCurrentOutputFx() == output)
    return QJsonObject{{"current_output_fx_id", document.idForFx(output)}};
  context.markMutationStarted();
  dag(document)->setCurrentOutputFx(output);
  if (dag(document)->getCurrentOutputFx() != output)
    throw Error("NATIVE_FAILURE",
                "Native output selection did not take effect.");
  return QJsonObject{{"current_output_fx_id", document.idForFx(output)}};
}

template <typename T>
void forgetEntity(QHash<QString, T*>& handles, T* value) {
  for (auto entry = handles.begin(); entry != handles.end();) {
    if (entry.value() == value)
      entry = handles.erase(entry);
    else
      ++entry;
  }
}

void removeGenerator(Document& document, TZeraryColumnFx* fx) {
  TXshColumn* column = fx->getXshColumn();
  dag(document)->removeFxId(fx->getZeraryFx());
  forgetEntity(document.effects, static_cast<TFx*>(fx->getZeraryFx()));
  forgetEntity(document.columns, column);
  forgetEntity(document.objects, columnObject(document, fx));
  TXsheet* sheet = document.scene->getXsheet();
  sheet->removeColumn(fx->getColumnIndex());
  sheet->updateFrameCount();
}

void requireDeletable(Document& document, TFx* fx) {
  if (dynamic_cast<TOutputFx*>(fx)) {
    if (dag(document)->getOutputFxCount() == 1)
      throw Error("LAST_OUTPUT",
                  "The scene must retain at least one output node.");
    return;
  }
  requireEditable(fx);
  requireColumnEditable(document, fx);
  if (auto* generator = dynamic_cast<TZeraryColumnFx*>(fx)) {
    if (columnObject(document, generator)->hasChildren())
      throw Error(
          "has_dependents",
          "Reparent the generator column's children before deleting it.");
  }
}

QJsonValue deleteEffect(Context& context, Document& document,
                        const QJsonObject& params) {
  const QString id = requiredString(params, "fx_id");
  TFxP effect      = ownedFx(document, id);
  TFx* fx          = effect.getPointer();
  auto* output     = dynamic_cast<TOutputFx*>(fx);
  requireDeletable(document, fx);
  context.markMutationStarted();
  // A member leaving the graph is unlinked first, as the editor does, so no
  // ring keeps an effect the scene no longer saves.
  TFx* native = output ? fx : parameterFx(fx);
  if (native->getLinkedFx() != native) native->unlinkParams();
  while (fx->getOutputConnectionCount() > 0)
    fx->getOutputConnection(0)->setFx(nullptr);
  fx->disconnectAll();
  dag(document)->removeFxId(fx);
  dag(document)->removeFromXsheet(fx);
  if (output)
    dag(document)->removeOutputFx(output);
  else if (auto* generator = dynamic_cast<TZeraryColumnFx*>(fx))
    removeGenerator(document, generator);
  else
    dag(document)->getInternalFxs()->removeFx(fx);
  if (graphMembers(document).count(fx) != 0)
    throw Error("NATIVE_FAILURE",
                "Native effect deletion did not remove graph membership.");
  forgetEntity(document.effects, fx);
  return QJsonObject{{"deleted_fx_id", id}};
}

TParam* editableParameter(TFx* fx, const QString& name) {
  requireEditable(fx);
  TParamVar* entry =
      parameterFx(fx)->getParams()->getParamVar(name.toStdString());
  if (!entry || entry->isHidden() || entry->isObsolete())
    throw Error("UNKNOWN_PARAMETER",
                "Effect has no public parameter with this exact name.",
                {{"parameter", name}});
  if (profileSchema(fx, entry->getParam(), name).isEmpty())
    throw Error("UNSUPPORTED_PARAMETER",
                "Complex or external-resource parameter is not editable.");
  return entry->getParam();
}

void validateScalar(const QJsonValue& value, const QJsonObject& schema) {
  validateAutomationFxNumber(value, schema);
  if (schema.contains("enum") &&
      !schema.value("enum").toArray().contains(value))
    throw Error("OUT_OF_RANGE",
                "Parameter value is not a native enumeration choice.");
}

void validateValue(const QJsonValue& value, const QJsonObject& schema) {
  const QString type = schema.value("type").toString();
  if (type == "boolean") {
    if (!value.isBool())
      throw Error("INVALID_PARAMS", "Parameter value must be a boolean.");
    return;
  }
  if (type == "array") {
    const int count = schema.value("minItems").toInt();
    if (!value.isArray() || value.toArray().size() != count)
      throw Error("INVALID_PARAMS", "Parameter array has the wrong size.",
                  {{"size", count}});
    const QJsonArray components = value.toArray();
    const QJsonArray prefix     = schema.value("prefixItems").toArray();
    for (int index = 0; index < components.size(); ++index) {
      validateAutomationFxNumber(components[index],
                                 schema.value("items").toObject());
      const QJsonObject item = prefix.at(index).toObject();
      if (item.contains("const") && components[index] != item.value("const"))
        throw Error("OUT_OF_RANGE",
                    "Parameter component must equal its constant.",
                    {{"index", index}, {"const", item.value("const")}});
    }
    return;
  }
  validateScalar(value, schema);
}

void setNumber(TDoubleParam* param, double value, int frame, bool key) {
  if (!key) {
    param->setDefaultValue(value);
    return;
  }
  TDoubleKeyframe keyframe(frame, value);
  keyframe.m_isKeyframe = true;
  keyframe.m_prevType   = TDoubleKeyframe::Linear;
  param->setKeyframe(keyframe);
}

void setDiscrete(TParam* param, const QJsonValue& value) {
  if (auto* integer = dynamic_cast<TIntParam*>(param))
    integer->setValue(value.toInt());
  else if (auto* boolean = dynamic_cast<TBoolParam*>(param))
    boolean->setValue(value.toBool());
  else if (auto* enumeration = dynamic_cast<TEnumParam*>(param))
    enumeration->setValue(value.toInt());
  else
    throw Error("UNSUPPORTED_PARAMETER",
                "Parameter has no supported native setter.");
}

void applyParameter(TParam* param, const QJsonValue& value, int frame,
                    bool key) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param)) {
    setNumber(number, value.toDouble(), frame, key);
    return;
  }
  if (auto* parameters = dynamic_cast<TParamSet*>(param)) {
    const QJsonArray values = value.toArray();
    for (int index = 0; index < values.size(); ++index) {
      auto* number =
          dynamic_cast<TDoubleParam*>(parameters->getParam(index).getPointer());
      if (!number)
        throw Error("UNSUPPORTED_PARAMETER",
                    "Compound parameter has a nonnumeric channel.");
      const double scale = dynamic_cast<TPixelParam*>(param) ? 255.0 : 1.0;
      setNumber(number, values[index].toDouble() / scale, frame, key);
    }
    return;
  }
  setDiscrete(param, value);
}

bool unchangedNumber(TDoubleParam* param, double value, int frame, bool key) {
  if (!key) return param->getDefaultValue() == value;
  if (!param->isKeyframe(frame)) return false;
  const TDoubleKeyframe& current = param->getKeyframeAt(frame);
  return current.m_value == value &&
         current.m_type == TDoubleKeyframe::Linear && current.m_step == 1;
}

bool unchangedParameter(TParam* param, const QJsonValue& value, int frame,
                        bool key) {
  if (auto* number = dynamic_cast<TDoubleParam*>(param))
    return unchangedNumber(number, value.toDouble(), frame, key);
  if (auto* parameters = dynamic_cast<TParamSet*>(param)) {
    const QJsonArray values = value.toArray();
    const double scale      = dynamic_cast<TPixelParam*>(param) ? 255.0 : 1.0;
    for (int index = 0; index < values.size(); ++index) {
      auto* number =
          dynamic_cast<TDoubleParam*>(parameters->getParam(index).getPointer());
      if (!number || !unchangedNumber(number, values[index].toDouble() / scale,
                                      frame, key))
        return false;
    }
    return true;
  }
  return discreteValue(param) == value;
}

void validateParameterEdit(TParam* param, bool key) {
  if (key && !param->isAnimatable())
    throw Error("NOT_ANIMATABLE",
                "Parameter cannot have keyframes; use fx.parameter.set.");
  if (!key && param->hasKeyframes())
    throw Error("ANIMATED_PARAMETER",
                "Parameter has keyframes; use fx.parameter.set_key.");
}

void verifyParameterEdit(TParam* param, const QJsonValue& value, int frame,
                         bool key) {
  if (parameterValue(param, frame) != value ||
      (key && !param->isKeyframe(frame)))
    throw Error("NATIVE_FAILURE",
                "Native parameter update failed its readback check.");
}

// Applies the edit to a detached copy first, so key limits and
// cross-parameter constraints reject it before the document changes.
void validateEditedProfile(TFx* fx, const QString& name,
                           const QJsonValue& value, int frame, bool key) {
  TFx* native    = parameterFx(fx);
  TFxP candidate = native->clone(false);
  candidate->setFxVersion(native->getFxVersion());
  applyParameter(candidate->getParams()->getParam(name.toStdString()), value,
                 frame, key);
  validateAutomationFxProfile(candidate.getPointer());
}

QJsonValue changeParameter(Context& context, Document& document,
                           const QJsonObject& params, bool key) {
  TFx* fx            = ownedFx(document, requiredString(params, "fx_id"));
  const QString name = requiredString(params, "parameter");
  TParam* param      = editableParameter(fx, name);
  requireLinkedEditable(document, fx);
  requireUnlockedCurve(document, fx, param, name);
  const int frame        = integerParam(params, "frame", 0, 1000000, 0);
  const QJsonValue value = params.value("value");
  validateValue(value, profileSchema(fx, param, name));
  validateParameterEdit(param, key);
  constraintForFx(document, fx,
                  [&] { validateEditedProfile(fx, name, value, frame, key); });
  if (unchangedParameter(param, value, frame, key)) {
    QJsonObject result = parameterDescription(fx, param, name, frame);
    addAffected(document, fx, result);
    return result;
  }
  TParamP backup = param->clone();
  context.markMutationStarted();
  try {
    applyParameter(param, value, frame, key);
    verifyParameterEdit(param, value, frame, key);
  } catch (...) {
    param->copy(backup.getPointer());
    throw;
  }
  QJsonObject result = parameterDescription(fx, param, name, frame);
  addAffected(document, fx, result);
  return result;
}

// Why a write to fx's parameters is refused or reaches further than fx, for
// fx.parameter.get: a locked column of any linked member, an effect outside
// the profile, or a linked group whose members all change; null otherwise.
QJsonValue parameterConstraint(Document& document, TFx* fx) {
  for (TFx* node : linkedNodes(fx)) {
    try {
      requireColumnEditable(document, node);
    } catch (const Error&) {
      auto* column = static_cast<TColumnFx*>(node);
      return QJsonObject{
          {"kind", "locked_column"},
          {"via", column->getXshColumn()
                      ? QJsonValue(document.idForColumn(column->getXshColumn()))
                      : QJsonValue()}};
    }
  }
  if (!allowedTypes().contains(
          QString::fromStdString(parameterFx(fx)->getFxType())))
    return QJsonObject{{"kind", "outside_profile"}, {"via", QJsonValue()}};
  const QJsonValue linked = automationFxLinkedGroup(document, fx);
  if (linked.isObject())
    return QJsonObject{{"kind", "linked_parameters"},
                       {"via", linked.toObject().value("group_id")}};
  return QJsonValue();
}

QJsonValue getParameters(Context&, Document& document,
                         const QJsonObject& params) {
  TFx* fx = ownedFx(document, requiredString(params, "fx_id"));
  if (dynamic_cast<TMacroFx*>(fx))
    throw Error("UNSUPPORTED_PARAMETER",
                "Expand the macro before reading member parameters.");
  const int frame         = integerParam(params, "frame", 0, 1000000, 0);
  const QJsonValue effect = parameterConstraint(document, fx);
  QJsonArray described;
  for (const QJsonValue& value : parameterDescriptions(fx, frame)) {
    QJsonObject item     = value.toObject();
    const bool supported = item.value("supported").toBool();
    TParam* param        = parameterFx(fx)->getParams()->getParam(
        item.value("name").toString().toStdString());
    item.insert("animated", param->hasKeyframes());
    item.insert("sample", supported ? QJsonValue(QJsonObject{
                                          {"frame", frame},
                                          {"value", item.value("value")}})
                                    : QJsonValue());
    item.insert("edit_constraint",
                !effect.isNull() || supported
                    ? effect
                    : QJsonValue(QJsonObject{{"kind", "unsupported_type"},
                                             {"via", QJsonValue()}}));
    described.append(item);
  }
  TFx* native = parameterFx(fx);
  return QJsonObject{{"fx_id", document.idForFx(fx)},
                     {"revision", document.revisionText()},
                     {"type", QString::fromStdString(native->getFxType())},
                     {"native_fx_version", native->getFxVersion()},
                     {"frame", frame},
                     {"parameters", described}};
}

QJsonArray authoringOperations() {
  const QJsonObject effect{{"document_id", stringSchema()},
                           {"fx_id", stringSchema()}};
  QJsonObject rename = effect;
  rename["name"]        = boundedNameSchema(256, true);
  QJsonObject enable    = effect;
  enable["enabled"]     = QJsonObject{{"type", "boolean"}};
  QJsonObject duplicate = rename;
  duplicate["frame_count"] =
      QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}};
  QJsonObject preset  = effect;
  preset["directory"] = stringSchema();
  const auto save =
      operation("fx.preset.save",
                "Publish a new native preset package; keep the "
                "document revision unchanged.",
                preset, {"document_id", "fx_id", "directory"}, true);
  return {
      operation("fx.rename", "Rename an admitted builtin effect.", rename,
                {"document_id", "fx_id", "name"}, true),
      operation("fx.enabled.set",
                "Set native enable/bypass state for a builtin effect.", enable,
                {"document_id", "fx_id", "enabled"}, true),
      operation(
          "fx.duplicate",
          "Copy independent parameters into a new disconnected builtin effect. "
          "Generator exposures begin at frame 0; frame_count defaults to 1.",
          duplicate, {"document_id", "fx_id"}, true),
      save,
      operation("fx.preset.apply",
                "Apply an unchanged preset published by this worker to the "
                "same native "
                "effect type/version; preserve names, graph connections and "
                "exposures.",
                preset, {"document_id", "fx_id", "directory"}, true)};
}

}  // namespace

TFxP makeAutomationEffect(const QString& type) { return makeEffect(type); }

QJsonValue automationFxCatalog(const EffectFactory& factory) {
  return effectCatalog(factory);
}

QJsonObject describeAutomationFx(Document& document, TFx* fx) {
  return describeFx(document, fx);
}

QByteArray automationFxPresetBytes(TFx* fx, const QString& path) {
  return saveNativePreset(fx, path);
}

QString automationFxName(const QJsonObject& params) {
  return authoringName(params);
}

bool isAutomationFxTypeSupported(const QString& type) {
  return allowedTypes().contains(type);
}

bool isAutomationFxTypeAvailable(const QString& type) {
  return allowedTypes().contains(type) && typeAvailable(type);
}

namespace {
// I-2: a parameter ring links 2 to 64 effects of one type and version, all
// in the scene graph; a ring that a hostile file built otherwise is refused.
void validateLinkRings(const std::set<TFx*>& members) {
  std::set<TFx*> natives;
  for (TFx* fx : members)
    if (!dynamic_cast<TMacroFx*>(fx) && !dynamic_cast<TOutputFx*>(fx))
      natives.insert(parameterFx(fx));
  for (TFx* native : natives) {
    TFx* member = native->getLinkedFx();
    int size    = 1;
    for (; member && member != native; member = member->getLinkedFx()) {
      if (++size > 64 || natives.count(member) == 0 ||
          member->getFxType() != native->getFxType() ||
          member->getFxVersion() != native->getFxVersion())
        throw Error("INVALID_GRAPH",
                    "A linked parameter group links effects outside the "
                    "graph, of different types, or more than 64.");
    }
    if (!member)
      throw Error("INVALID_GRAPH", "A linked parameter group is not a ring.");
  }
}
}  // namespace

void validateAutomationFxGraph(ToonzScene& scene) {
  const std::set<TFx*> members = sceneMembers(scene);
  if (members.size() > 4096)
    throw Error("RESOURCE_LIMIT",
                "The automation graph is limited to 4096 effect nodes.");
  TXsheet* sheet = scene.getXsheet();
  FxDag* graph   = sheet->getFxDag();
  validateAutomationMacros(scene, members);
  for (TFx* fx : members) validateNode(fx, sheet);
  validateLinkRings(members);
  validateEdges(members, graph);
}

QJsonArray fxRecipeOperations() {
  const QJsonArray categories{
      "background", "title",       "transition", "atmosphere", "cel_finishing",
      "matte",      "stylization", "light",      "particles",  "procedural",
      "shot",       "rig",         "delivery"};
  return {operation(
      "fx.recipe.list",
      "List static recipes (FX, shot, rig and delivery): atomic tool-call "
      "steps that run with the existing tools, with brief, required "
      "capabilities, ownership, deliverables labelled editable, approximated "
      "or baked, and a review the client must complete before it reports "
      "success. An argument whose whole value is ${input.<name>} or "
      "${steps.<id>.<path>} (dotted fields and array indexes) takes that JSON "
      "value; a step with poll repeats until its field is terminal; supply "
      "expected_revision and operation_id per call where a tool asks for "
      "them. Check frames assume the input defaults.",
      {{"recipe_id", stringSchema()},
       {"category", QJsonObject{{"type", "string"}, {"enum", categories}}}},
      {})};
}

namespace {
QJsonObject revisionSchema() {
  return {{"type", "string"},
          {"minLength", 1},
          {"maxLength", 80},
          {"pattern", "[0-9]{1,20}"}};
}

QJsonObject graphSchema() {
  return {
      {"document_id", stringSchema()},
      {"sheet_path", sheetPathSchema()},
      {"expected_revision",
       QJsonObject{{"type", "string"}, {"minLength", 1}, {"maxLength", 80}}},
      {"offset",
       QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 16384}}},
      {"limit",
       QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 512}}}};
}

QJsonObject deltaSchema() {
  return {
      {"document_id", stringSchema()},
      {"from_revision", revisionSchema()},
      {"expected_revision", revisionSchema()},
      {"sheet_path", sheetPathSchema()},
      {"offset",
       QJsonObject{{"type", "integer"}, {"minimum", 0}, {"maximum", 65536}}},
      {"limit",
       QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 512}}}};
}
}  // namespace

QJsonArray fxOperations() {
  const QJsonObject effect{{"document_id", stringSchema()},
                           {"fx_id", stringSchema()}};
  QJsonObject creation{
      {"document_id", stringSchema()},
      {"type", stringSchema()},
      {"name", boundedNameSchema(256, true)},
      {"frame_count",
       QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}}};
  QJsonObject port           = effect;
  port["port"]               = stringSchema();
  QJsonObject connection     = port;
  connection["source_fx_id"] = stringSchema();
  QJsonObject terminal       = effect;
  terminal["enabled"]        = QJsonObject{{"type", "boolean"}};
  QJsonObject parameters     = effect;
  parameters["frame"]        = frameSchema();
  QJsonObject change         = effect;
  change["parameter"]        = stringSchema();
  change["value"]            = QJsonObject{
      {"description", "Value must match the native parameter schema."}};
  QJsonObject key = change;
  key["frame"]    = frameSchema();
  QJsonArray operations{
      operation("fx.catalog",
                "Discover allowed builtin effects and native typed parameters.",
                {}, {}),
      operation(
          "fx.graph",
          "Inspect graph, generator columns, ports, terminals, and "
          "current output; sheet_path inspects a sub-xsheet instance "
          "named by scene.inventory. Effects come in creation order, 64 a "
          "page by default (limit up to 512); repeat with next_offset and "
          "expected_revision until next_offset is null.",
          graphSchema(), {"document_id"}),
      operation(
          "fx.graph.delta",
          "Report what changed in the top sheet's FX graph between "
          "from_revision and the current revision: created, removed and "
          "changed effects, edges, terminal links, the current output, "
          "linked groups and schematic groups, net of intermediate steps. "
          "Pages are bound to the current revision; next_offset is null on "
          "the last page.",
          deltaSchema(), {"document_id", "from_revision"}),
      operation(
          "fx.create",
          "Create a disconnected builtin effect, generator column, or output.",
          creation, {"document_id", "type"}, true),
      operation(
          "fx.delete",
          "Delete an effect and disconnect its edges; retain the last output.",
          effect, {"document_id", "fx_id"}, true),
      operation(
          "fx.connect",
          "Connect a named input after checking ownership, type, and cycles.",
          connection, {"document_id", "fx_id", "port", "source_fx_id"}, true),
      operation("fx.disconnect", "Disconnect an effect's named input port.",
                port, {"document_id", "fx_id", "port"}, true),
      operation("fx.terminal.set",
                "Connect or disconnect an effect from the Xsheet composite.",
                terminal, {"document_id", "fx_id", "enabled"}, true),
      operation("fx.output.select", "Select a scene output node for rendering.",
                effect, {"document_id", "fx_id"}, true),
      operation(
          "fx.parameter.get",
          "Read public parameter schemas, values, and keyframe positions.",
          parameters, {"document_id", "fx_id"}),
      operation(
          "fx.parameter.set",
          "Set a supported unanimated parameter with native range checks.",
          change, {"document_id", "fx_id", "parameter", "value"}, true),
      operation(
          "fx.parameter.set_key",
          "Set a linear key on an animatable parameter; frames start at 0.",
          key, {"document_id", "fx_id", "parameter", "value", "frame"}, true)};
  for (const auto& entry : authoringOperations()) operations.append(entry);
  for (const auto& entry : macroOperations()) operations.append(entry);
  return operations;
}

QJsonValue executeFx(Context& context, const QString& method,
                     const QJsonObject& params) {
  if (method == "fx.catalog") return effectCatalog();
  if (method == "fx.recipe.list") return listAutomationFxRecipes(params);
  Document& document = context.document(requiredString(params, "document_id"));
  if (method.startsWith("fx.macro."))
    return executeMacro(context, document, method, params);
  if (method == "fx.port_group.set")
    return setPortGroup(context, document, params);
  if (method.startsWith("fx.link."))
    return executeFxLink(context, document, method, params);
  if (method.startsWith("fx.group.") || method == "fx.layout.set")
    return executeFxGraph(context, document, method, params);
  if (method == "fx.graph.delta")
    return automationFxGraphDelta(document, params);
  using Handler = QJsonValue (*)(Context&, Document&, const QJsonObject&);
  static const std::map<QString, Handler> handlers = {
      {"fx.create", createEffect},
      {"fx.rename", renameEffect},
      {"fx.enabled.set", enableEffect},
      {"fx.duplicate", duplicateEffect},
      {"fx.preset.save", savePreset},
      {"fx.preset.apply", applyPreset},
      {"fx.delete", deleteEffect},
      {"fx.terminal.set", setTerminal},
      {"fx.output.select", selectOutput},
      {"fx.parameter.get", getParameters},
      {"fx.connect",
       [](Context& ctx, Document& doc, const QJsonObject& args) {
         return connectEffect(ctx, doc, args, false);
       }},
      {"fx.disconnect",
       [](Context& ctx, Document& doc, const QJsonObject& args) {
         return connectEffect(ctx, doc, args, true);
       }},
      {"fx.parameter.set",
       [](Context& ctx, Document& doc, const QJsonObject& args) {
         return changeParameter(ctx, doc, args, false);
       }},
      {"fx.parameter.set_key",
       [](Context& ctx, Document& doc, const QJsonObject& args) {
         return changeParameter(ctx, doc, args, true);
       }},
      {"fx.graph", [](Context&, Document& doc, const QJsonObject& args) {
         return inspectGraph(doc, args);
       }}};
  const auto handler = handlers.find(method);
  if (handler == handlers.end())
    throw Error("METHOD_NOT_FOUND", "Unknown FX operation.");
  return handler->second(context, document, params);
}

void automationRequireColumnEditable(Document& document, TFx* fx) {
  requireColumnEditable(document, fx);
}

void automationRequireUnlockedCurves(Document& document, TFx* fx) {
  requireUnlockedCurves(document, fx);
}

TFx* automationOwnedFx(Document& document, const QString& id) {
  return ownedFx(document, id);
}

TFx* automationParameterFx(TFx* fx) { return parameterFx(fx); }

namespace {

QJsonObject channelDetails(Document& document,
                           const AutomationFxChannel& channel) {
  return {{"fx_id", document.idForFx(channel.fx)},
          {"parameter", channel.parameter},
          {"channel", channel.channel}};
}

// The curve of channel in native, an effect of the same type: a clone or the
// effect itself.
TDoubleParam* channelCurve(TFx* native, const AutomationFxChannel& channel) {
  TParam* param =
      native->getParams()->getParam(channel.parameter.toStdString());
  if (channel.component < 0) return dynamic_cast<TDoubleParam*>(param);
  auto* set = dynamic_cast<TParamSet*>(param);
  if (!set || channel.component >= set->getParamCount()) return nullptr;
  return dynamic_cast<TDoubleParam*>(
      set->getParam(channel.component).getPointer());
}

}  // namespace

void automationRequireFxChannelWritable(Document& document,
                                        const AutomationFxChannel& channel) {
  TParam* param = editableParameter(channel.fx, channel.parameter);
  auto* pixel   = dynamic_cast<TPixelParam*>(param);
  if (pixel && channel.component == 3 && !pixel->isMatteEnabled())
    throw Error("UNSUPPORTED_PARAMETER",
                "This color has no native matte: the effect never reads its "
                "alpha channel.",
                channelDetails(document, channel));
  requireLinkedEditable(document, channel.fx);
  if (!channel.curve->isEditorLocked()) return;
  QJsonObject details = channelDetails(document, channel);
  details.insert("reason", "curve_locked");
  throw Error("locked", "The curve is locked in the editor; unlock it first.",
              details);
}

void automationValidateFxChannelEdit(
    Document& document, const AutomationFxChannel& channel,
    const std::function<void(TDoubleParam&)>& edit) {
  TFx* native    = parameterFx(channel.fx);
  TFxP candidate = native->clone(false);
  candidate->setFxVersion(native->getFxVersion());
  TDoubleParam* curve = channelCurve(candidate.getPointer(), channel);
  if (!curve)
    throw Error("NATIVE_FAILURE", "The effect's copy lacks the edited channel.",
                channelDetails(document, channel));
  edit(*curve);
  try {
    validateAutomationFxProfile(candidate.getPointer());
  } catch (Error& error) {
    error.details.insert("fx_id", document.idForFx(channel.fx));
    throw;
  }
}

void automationAddFxAffected(Document& document, TFx* fx, QJsonObject& result) {
  result.insert("fx_id", document.idForFx(fx));
  addAffected(document, fx, result);
}

}  // namespace GroundzeroAutomation
