#include "automation.h"
#include "automation_fx.h"
#include "automation_macro.h"

#include "tfx.h"
#include "tfxattributes.h"
#include "tmacrofx.h"
#include "trasterfx.h"
#include "tparamcontainer.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include <algorithm>
#include <limits>
#include <map>
#include <vector>

namespace GroundzeroAutomation {
namespace {
#ifdef BUILD_UI_PORT_TESTS
int failurePoint = 0;
#endif

void checkpoint(int point) {
#ifdef BUILD_UI_PORT_TESTS
  if (failurePoint == point)
    throw Error("NATIVE_FAILURE", "Injected native macro publication failure.");
#else
  (void)point;
#endif
}

void require(bool valid, const char* message) {
  if (!valid) throw Error("INVALID_MACRO", QString::fromUtf8(message));
}

QString key(TFx* fx) { return QString::fromStdWString(fx->getFxId()); }
FxDag* dag(Document& document) {
  return document.scene->getXsheet()->getFxDag();
}

TFxP memberCopy(TFx* source) {
  const QString type = QString::fromStdString(source->getFxType());
  require(type == "overFx" || type == "STD_blurFx" || type == "STD_glowFx",
          "Macro grouping supports only over, Blur and Glow effects.");
  require(source->getLinkedFx() == source,
          "Expand linked parameter groups before macro grouping.");
  TFxP copy = makeAutomationEffect(type);
  // Group ports beyond the constructor's, in array order, so connections
  // copy by name (fx.port_group.set grows over groups).
  for (int index = copy->getInputPortCount();
       index < source->getInputPortCount(); ++index) {
    const int group = source->getInputPort(index)->getGroupIndex();
    require(group >= 0 && copy->addInputPort(source->getInputPortName(index),
                                             new TRasterFxPort, group),
            "A macro member's grown port group could not be copied.");
  }
  copy->setFxVersion(source->getFxVersion());
  copy->getParams()->copy(source->getParams());
  copy->setName(source->getName());
  copy->setFxId(source->getFxId());
  copy->getAttributes()->setId(source->getAttributes()->getId());
  copy->getAttributes()->enable(source->getAttributes()->isEnabled());
  copy->setNewIdentifier();
  return copy;
}

std::set<TFx*> reachable(TFx* root, const std::set<TFx*>& selected) {
  std::set<TFx*> visited;
  std::vector<TFx*> pending{root};
  while (!pending.empty()) {
    TFx* fx = pending.back();
    pending.pop_back();
    if (!visited.insert(fx).second) continue;
    for (int index = 0; index < fx->getInputPortCount(); ++index) {
      TFx* source = fx->getInputPort(index)->getFx();
      if (selected.count(source)) pending.push_back(source);
    }
  }
  return visited;
}

std::vector<TFxP> selection(Document& document, const QJsonArray& ids) {
  require(ids.size() >= 2 && ids.size() <= 64,
          "Choose 2 through 64 distinct macro members.");
  std::vector<TFxP> members;
  std::set<TFx*> selected;
  for (const auto& id : ids) {
    TFx* fx = document.fx(id.toString());
    require(
        dag(document)->getInternalFxs()->containsFx(fx),
        "Macro members must be currently owned top-level internal effects.");
    require(selected.insert(fx).second,
            "Macro member selection contains a repeated handle.");
    if (fx->getAttributes()->isGrouped())
      throw Error("INVALID_MACRO",
                  "Ungroup schematic groups before making a macro of their "
                  "members.",
                  {{"reason", "grouped_member"}, {"fx_id", id.toString()}});
    members.emplace_back(fx);
  }
  return members;
}

TFx* selectionRoot(const std::vector<TFxP>& members, FxDag* graph) {
  TFxP root;
  std::vector<TFxP> roots, leaves;
  require(TMacroFx::analyze(members, root, roots, leaves),
          "Selection needs one root with no non-root external consumers.");
  std::set<TFx*> selected;
  for (const auto& member : members) selected.insert(member.getPointer());
  require(reachable(root.getPointer(), selected) == selected,
          "Macro selection must be one connected directed acyclic graph.");
  for (const auto& member : members)
    require(member == root ||
                !graph->getTerminalFxs()->containsFx(member.getPointer()),
            "Non-root macro members cannot remain Xsheet terminals.");
  return root.getPointer();
}

struct Boundary {
  TFxPort* port;
  TFx* source;
};
struct Candidate {
  TFxP macro;
  TFx* originalRoot;
  std::vector<TFxP> originals;
  std::vector<Boundary> boundaries;
};

void connectCopies(Candidate& candidate, const std::map<TFx*, TFxP>& copies) {
  for (const auto& source : candidate.originals) {
    TFx* target = copies.at(source.getPointer()).getPointer();
    for (int index = 0; index < source->getInputPortCount(); ++index) {
      TFxPort* input   = source->getInputPort(index);
      TFxPort* output  = target->getInputPort(source->getInputPortName(index));
      const auto found = copies.find(input->getFx());
      if (found != copies.end())
        output->setFx(found->second.getPointer());
      else
        candidate.boundaries.push_back({output, input->getFx()});
    }
  }
  require(candidate.boundaries.size() <= 128,
          "Macro exposes more than 128 boundary ports.");
}

Candidate prepare(Document& document, const QJsonObject& params) {
  validateAutomationFxGraph(*document.scene);
  Candidate result;
  result.originals    = selection(document, params.value("fx_ids").toArray());
  result.originalRoot = selectionRoot(result.originals, dag(document));
  std::map<TFx*, TFxP> copies;
  std::vector<TFxP> values;
  for (const auto& source : result.originals) {
    TFxP copy = memberCopy(source.getPointer());
    copies.emplace(source.getPointer(), copy);
    values.push_back(copy);
  }
  connectCopies(result, copies);
  result.macro = TMacroFx::create(values);
  auto* macro  = dynamic_cast<TMacroFx*>(result.macro.getPointer());
  require(
      macro && macro->getRoot() == copies.at(result.originalRoot).getPointer(),
      "Native macro construction changed its expected root.");
  require(macro->getInputPortCount() == int(result.boundaries.size()),
          "Native macro construction lost a boundary port.");
  macro->setName(automationFxName(params).toStdWString());
  for (const auto& boundary : result.boundaries)
    require(boundary.port->getOwnerFx() == macro && !boundary.port->getFx(),
            "Private macro candidate does not own its disconnected boundary.");
  std::set<TFx*> replaced;
  for (const auto& original : result.originals)
    replaced.insert(original.getPointer());
  validateAutomationMacroCandidate(*document.scene, macro, replaced);
  return result;
}

struct SavedPort {
  TFxPort* port;
  TFx* source;
  TFx* owner;
};
struct SavedNode {
  TFxP fx;
  bool internal;
  bool terminal;
};

class GraphBackup final {
public:
  GraphBackup(Document& document, const std::vector<TFxP>& nodes)
      : m_document(document), m_handles(document.effects) {
    std::set<TFxPort*> seen;
    for (const auto& node : nodes) {
      TFx* fx = node.getPointer();
      m_nodes.push_back({node, graph()->getInternalFxs()->containsFx(fx),
                         graph()->getTerminalFxs()->containsFx(fx)});
      m_counters.emplace(fx->getFxType(),
                         std::make_pair(node, graph()->getFxTypeCount(fx)));
      for (int index = 0; index < fx->getInputPortCount(); ++index)
        savePort(fx->getInputPort(index), seen);
      for (int index = 0; index < fx->getOutputConnectionCount(); ++index)
        savePort(fx->getOutputConnection(index), seen);
    }
  }

  void restore() {
    for (const auto& entry : m_ports) entry.port->setFx(nullptr);
    for (const auto& entry : m_nodes)
      graph()->removeFxId(entry.fx.getPointer());
    restoreMembership();
    for (const auto& entry : m_ports) {
      entry.port->setOwnerFx(entry.owner);
      entry.port->setFx(entry.source);
    }
    for (const auto& entry : m_counters)
      graph()->updateFxTypeTable(entry.second.first.getPointer(),
                                 entry.second.second);
    m_document.effects = m_handles;
  }

private:
  FxDag* graph() { return dag(m_document); }
  void savePort(TFxPort* port, std::set<TFxPort*>& seen) {
    if (seen.insert(port).second)
      m_ports.push_back({port, port->getFx(), port->getOwnerFx()});
  }
  void restoreMembership() {
    for (const auto& entry : m_nodes) {
      TFx* fx = entry.fx.getPointer();
      if (entry.internal)
        graph()->getInternalFxs()->addFx(fx);
      else
        graph()->getInternalFxs()->removeFx(fx);
      if (entry.terminal)
        graph()->addToXsheet(fx);
      else
        graph()->removeFromXsheet(fx);
      if (entry.internal) graph()->updateFxIdTable(fx);
    }
    restoreMemberIndexes();
  }
  void restoreMemberIndexes() {
    for (const auto& entry : m_nodes) {
      if (!entry.internal) continue;
      auto* macro = dynamic_cast<TMacroFx*>(entry.fx.getPointer());
      if (!macro) continue;
      for (const auto& member : macro->getFxs())
        graph()->updateFxIdTable(member.getPointer());
    }
  }

  Document& m_document;
  QHash<QString, TFx*> m_handles;
  std::vector<SavedNode> m_nodes;
  std::vector<SavedPort> m_ports;
  std::map<std::string, std::pair<TFxP, int>> m_counters;
};

void forget(Document& document, TFx* fx) {
  for (auto entry = document.effects.begin();
       entry != document.effects.end();) {
    if (entry.value() == fx)
      entry = document.effects.erase(entry);
    else
      ++entry;
  }
}

void publish(Candidate& candidate, Document& document) {
  auto* graph = dag(document);
  auto* macro = static_cast<TMacroFx*>(candidate.macro.getPointer());
  graph->assignUniqueId(macro);
  macro->setNewIdentifier();
  graph->getInternalFxs()->addFx(macro);
  macro->getAttributes()->setDagNodePos(
      candidate.originalRoot->getAttributes()->getDagNodePos());
  checkpoint(1);
  for (const auto& boundary : candidate.boundaries)
    boundary.port->setFx(boundary.source);
  while (candidate.originalRoot->getOutputConnectionCount())
    candidate.originalRoot->getOutputConnection(0)->setFx(macro);
  if (graph->getTerminalFxs()->containsFx(candidate.originalRoot))
    graph->addToXsheet(macro);
  checkpoint(2);
  for (const auto& original : candidate.originals) {
    TFx* fx = original.getPointer();
    graph->removeFromXsheet(fx);
    graph->removeFxId(fx);
    graph->getInternalFxs()->removeFx(fx);
    fx->disconnectAll();
    forget(document, fx);
  }
  checkpoint(3);
  for (const auto& member : macro->getFxs())
    graph->updateFxIdTable(member.getPointer());
  checkpoint(7);
}

QJsonValue createMacro(Context& context, Document& document,
                       const QJsonObject& params) {
  Candidate candidate = prepare(document, params);
  auto nodes          = candidate.originals;
  nodes.push_back(candidate.macro);
  GraphBackup backup(document, nodes);
  auto* macro = static_cast<TMacroFx*>(candidate.macro.getPointer());
  require(
      dag(document)->getFxTypeCount(macro) < std::numeric_limits<int>::max(),
      "Macro type counter is exhausted.");
  QJsonArray mapping;
  for (const auto& source : candidate.originals)
    mapping.append(
        QJsonObject{{"source_fx_id", document.idForFx(source.getPointer())},
                    {"member_key", key(source.getPointer())}});
  context.markMutationStarted();
  try {
    publish(candidate, document);
    validateAutomationFxGraph(*document.scene);
    QJsonObject result{{"changed", true},
                       {"node", describeAutomationFx(document, macro)},
                       {"retired_fx_ids", params.value("fx_ids")},
                       {"member_mapping", mapping}};
    checkpoint(8);
    return result;
  } catch (...) {
    backup.restore();
    throw;
  }
}

void expandMembers(TMacroFx* macro, Document& document) {
  auto* graph = dag(document);
  for (const auto& member : macro->getFxs()) {
    graph->getInternalFxs()->addFx(member.getPointer());
    for (int index = 0; index < member->getInputPortCount(); ++index)
      member->getInputPort(index)->setOwnerFx(member.getPointer());
  }
  checkpoint(4);
  TFx* root = macro->getRoot();
  while (macro->getOutputConnectionCount())
    macro->getOutputConnection(0)->setFx(root);
  if (graph->getTerminalFxs()->containsFx(macro)) graph->addToXsheet(root);
  checkpoint(5);
  graph->removeFromXsheet(macro);
  graph->removeFxId(macro);
  graph->getInternalFxs()->removeFx(macro);
  forget(document, macro);
  checkpoint(6);
}

QJsonValue expandMacro(Context& context, Document& document,
                       const QJsonObject& params) {
  validateAutomationFxGraph(*document.scene);
  const QString id = requiredString(params, "fx_id");
  TFxP retained(document.fx(id));
  auto* macro = dynamic_cast<TMacroFx*>(retained.getPointer());
  require(macro && dag(document)->getInternalFxs()->containsFx(macro),
          "Expansion requires an owned qualified top-level macro.");
  auto members = macro->getFxs();
  auto nodes   = members;
  nodes.push_back(retained);
  GraphBackup backup(document, nodes);
  context.markMutationStarted();
  try {
    expandMembers(macro, document);
    validateAutomationFxGraph(*document.scene);
    QJsonArray mapping;
    for (const auto& member : members)
      mapping.append(
          QJsonObject{{"member_key", key(member.getPointer())},
                      {"fx_id", document.idForFx(member.getPointer())}});
    QJsonObject result{{"changed", true},
                       {"retired_macro_fx_id", id},
                       {"root_fx_id", document.idForFx(macro->getRoot())},
                       {"member_mapping", mapping}};
    checkpoint(9);
    return result;
  } catch (...) {
    backup.restore();
    throw;
  }
}

QJsonArray memberPorts(Document& document, TFx* member,
                       const std::set<TFx*>& members) {
  QJsonArray ports;
  for (int index = 0; index < member->getInputPortCount(); ++index) {
    auto* port  = member->getInputPort(index);
    TFx* source = port->getFx();
    QJsonValue internal(QJsonValue::Null), external(QJsonValue::Null);
    if (members.count(source))
      internal = key(source);
    else if (source)
      external = document.idForFx(source);
    ports.append(QJsonObject{
        {"name", QString::fromStdString(member->getInputPortName(index))},
        {"type", "raster"},
        {"control", port->isaControlPort()},
        {"connected", bool(source)},
        {"source_member_key", internal},
        {"source_fx_id", external}});
  }
  return ports;
}

QJsonArray inputMapping(Document& document, TMacroFx* macro) {
  QJsonArray mapping;
  for (int index = 0; index < macro->getInputPortCount(); ++index) {
    auto* boundary = macro->getInputPort(index);
    for (const auto& member : macro->getFxs())
      for (int port = 0; port < member->getInputPortCount(); ++port)
        if (member->getInputPort(port) == boundary)
          mapping.append(QJsonObject{
              {"port", QString::fromStdString(macro->getInputPortName(index))},
              {"member_key", key(member.getPointer())},
              {"member_port",
               QString::fromStdString(member->getInputPortName(port))},
              {"source_fx_id",
               boundary->getFx()
                   ? QJsonValue(document.idForFx(boundary->getFx()))
                   : QJsonValue(QJsonValue::Null)}});
  }
  return mapping;
}
}  // namespace

QJsonObject describeAutomationMacro(Document& document, TMacroFx* macro) {
  std::set<TFx*> members;
  for (const auto& member : macro->getFxs())
    members.insert(member.getPointer());
  QJsonArray descriptions;
  for (const auto& member : macro->getFxs())
    descriptions.append(QJsonObject{
        {"member_key", key(member.getPointer())},
        {"type", QString::fromStdString(member->getFxType())},
        {"name", QString::fromStdWString(member->getName())},
        {"enabled", member->getAttributes()->isEnabled()},
        {"native_fx_version", member->getFxVersion()},
        {"ports", memberPorts(document, member.getPointer(), members)}});
  return {{"root_member_key", key(macro->getRoot())},
          {"members", descriptions},
          {"input_mapping", inputMapping(document, macro)}};
}

QJsonArray macroOperations() {
  const QJsonObject text{{"type", "string"}, {"minLength", 1}};
  return {
      operation("fx.macro.create",
                "Group owned raster effects into a native macro.",
                {{"document_id", text},
                 {"fx_ids", QJsonObject{{"type", "array"},
                                        {"items", text},
                                        {"minItems", 2},
                                        {"maxItems", 64}}},
                 {"name", boundedNameSchema(256, true)}},
                {"document_id", "fx_ids", "name"}, true),
      operation("fx.macro.expand",
                "Expand a qualified macro into independently owned effects.",
                {{"document_id", text}, {"fx_id", text}},
                {"document_id", "fx_id"}, true)};
}

QJsonValue executeMacro(Context& context, Document& document,
                        const QString& method, const QJsonObject& params) {
  if (method == "fx.macro.create")
    return createMacro(context, document, params);
  if (method == "fx.macro.expand")
    return expandMacro(context, document, params);
  throw Error("METHOD_NOT_FOUND", "Unknown native macro operation.");
}

#ifdef BUILD_UI_PORT_TESTS
void setAutomationMacroFailurePoint(int point) { failurePoint = point; }
#endif
}  // namespace GroundzeroAutomation
