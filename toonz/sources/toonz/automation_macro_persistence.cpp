#include "automation.h"
#include <cstdlib>
#include "automation_fx.h"
#include "automation_macro.h"

#include "tconst.h"
#include "tfx.h"
#include "tfxattributes.h"
#include "tmacrofx.h"
#include "tzeraryfx.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <vector>

namespace GroundzeroAutomation {
namespace {
QString key(TFx* fx) { return QString::fromStdWString(fx->getFxId()); }

QJsonValue connectionIdentity(TFx* fx, FxDag* graph) {
  if (!fx) return QJsonValue(QJsonValue::Null);
  if (fx == graph->getXsheetFx()) return QJsonObject{{"kind", "xsheet"}};
  if (auto* column = dynamic_cast<TColumnFx*>(fx))
    return QJsonObject{{"kind", "column"}, {"index", column->getColumnIndex()}};
  if (dynamic_cast<TOutputFx*>(fx)) {
    for (int index = 0; index < graph->getOutputFxCount(); ++index)
      if (graph->getOutputFx(index) == fx)
        return QJsonObject{{"kind", "output"}, {"index", index}};
    throw Error("INVALID_MACRO", "Output identity is not owned by this scene.");
  }
  const QString id = key(fx);
  if (id.isEmpty())
    throw Error("INVALID_MACRO",
                "Macro-connected internal effect has no persistent native ID.");
  return QJsonObject{{"kind", "effect"}, {"id", id}};
}

QJsonObject ports(TFx* fx, FxDag* graph) {
  QJsonObject result;
  for (int index = 0; index < fx->getInputPortCount(); ++index)
    result[QString::fromStdString(fx->getInputPortName(index))] =
        connectionIdentity(fx->getInputPort(index)->getFx(), graph);
  return result;
}

QJsonObject memberState(TFx* fx, const QString& scratch, qint64& bytes,
                        FxDag* graph) {
  const QByteArray preset = automationFxPresetBytes(fx, scratch);
  bytes += preset.size();
  if (bytes > 256 * 1024 * 1024)
    throw Error(
        "RESOURCE_LIMIT",
        "Macro preset verification exceeds the scene package byte budget.");
  if (!QFile::remove(scratch))
    throw Error("io_error",
                "Cannot remove the private macro preset verification file.");
  return {
      {"type", QString::fromStdString(fx->getFxType())},
      {"name", QString::fromStdWString(fx->getName())},
      {"number", fx->getAttributes()->getId()},
      {"enabled", fx->getAttributes()->isEnabled()},
      {"native_fx_version", fx->getFxVersion()},
      {"ports", ports(fx, graph)},
      {"parameters", QString::fromLatin1(QCryptographicHash::hash(
                                             preset, QCryptographicHash::Sha256)
                                             .toHex())}};
}

QJsonObject macroState(TMacroFx* macro, const QString& scratch, qint64& bytes,
                       FxDag* graph) {
  QJsonObject members;
  for (const auto& member : macro->getFxs())
    members[key(member.getPointer())] =
        memberState(member.getPointer(), scratch, bytes, graph);
  return {{"name", QString::fromStdWString(macro->getName())},
          {"number", macro->getAttributes()->getId()},
          {"enabled", macro->getAttributes()->isEnabled()},
          {"native_fx_version", macro->getFxVersion()},
          {"root", key(macro->getRoot())},
          {"ports", ports(macro, graph)},
          {"members", members}};
}

QJsonObject graphState(FxDag* graph) {
  QJsonObject result;
  auto* effects = graph->getInternalFxs();
  for (int index = 0; index < effects->getFxCount(); ++index) {
    TFx* fx = effects->getFx(index);
    result[key(fx)] =
        QJsonObject{{"ports", ports(fx, graph)},
                    {"terminal", graph->getTerminalFxs()->containsFx(fx)}};
  }
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    result["output-" + QString::number(index)] =
        ports(graph->getOutputFx(index), graph);
  return result;
}
// A position as the scene file stores it (six significant digits), so a
// saved and a reopened scene give the same value.
QJsonValue storedPosition(TFx* fx) {
  const TPointD point = fx->getAttributes()->getDagNodePos();
  if (point == TConst::nowhere) return QJsonValue();
  return QJsonArray{QString::number(point.x, 'g', 6).toDouble(),
                    QString::number(point.y, 'g', 6).toDouble()};
}

QJsonArray ringIdentities(TFx* fx, FxDag* graph) {
  QStringList members;
  TFx* member = fx;
  for (int guard = 0; guard < 1024; ++guard) {
    TFx* owner = member;
    if (auto* zerary = dynamic_cast<TZeraryFx*>(member))
      if (zerary->getColumnFx()) owner = zerary->getColumnFx();
    members.append(QString::fromUtf8(
        QJsonDocument(QJsonArray{connectionIdentity(owner, graph)})
            .toJson(QJsonDocument::Compact)));
    member = member->getLinkedFx();
    if (!member || member == fx) break;
  }
  members.sort();
  return QJsonArray::fromStringList(members);
}

// The schematic structure of one node when it differs from a new node's:
// placement, group stacks and names, the parameter ring, grown port groups.
QJsonObject nodeStructure(TFx* fx, FxDag* graph) {
  QJsonObject state;
  const QJsonValue position = storedPosition(fx);
  if (!position.isNull()) state["position"] = position;
  TFxAttributes* attributes = fx->getAttributes();
  if (attributes->isGrouped()) {
    QJsonArray ids, names;
    for (int id : attributes->getGroupIdStack()) ids.append(id);
    for (const std::wstring& name : attributes->getGroupNameStack())
      names.append(QString::fromStdWString(name));
    state["groups"]      = ids;
    state["group_names"] = names;
  }
  TFx* native = fx;
  if (auto* wrapper = dynamic_cast<TZeraryColumnFx*>(fx))
    if (wrapper->getZeraryFx()) native = wrapper->getZeraryFx();
  if (native->getLinkedFx() != native)
    state["linked"] = ringIdentities(native, graph);
  bool grown = false;
  for (int group = 0; group < fx->dynamicPortGroupsCount(); ++group)
    grown |= fx->dynamicPortGroup(group)->ports().size() >
             size_t(fx->dynamicPortGroup(group)->minPortsCount());
  if (grown) {
    QJsonArray names;
    for (int index = 0; index < fx->getInputPortCount(); ++index)
      names.append(QString::fromStdString(fx->getInputPortName(index)));
    state["ports"] = names;
  }
  return state;
}

// Every top-sheet node's structure that differs from a new node's, keyed by
// its persistent identity; empty for a scene the worker never placed,
// grouped, linked or grew.
QJsonObject structureState(ToonzScene& scene) {
  TXsheet* sheet = scene.getXsheet();
  FxDag* graph   = sheet->getFxDag();
  std::vector<TFx*> nodes{graph->getXsheetFx()};
  auto* effects = graph->getInternalFxs();
  for (int index = 0; index < effects->getFxCount(); ++index)
    nodes.push_back(effects->getFx(index));
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    nodes.push_back(graph->getOutputFx(index));
  for (int index = 0; index < sheet->getColumnCount(); ++index)
    if (sheet->getColumn(index) && sheet->getColumn(index)->getFx())
      nodes.push_back(sheet->getColumn(index)->getFx());
  QJsonObject result;
  for (TFx* fx : nodes) {
    const QJsonObject state = nodeStructure(fx, graph);
    if (state.isEmpty()) continue;
    result[QString::fromUtf8(
        QJsonDocument(QJsonArray{connectionIdentity(fx, graph)})
            .toJson(QJsonDocument::Compact))] = state;
  }
  return result;
}

std::vector<TMacroFx*> macroNodes(FxDag* graph) {
  auto* effects = graph->getInternalFxs();
  if (effects->getFxCount() > 4096)
    throw Error("RESOURCE_LIMIT",
                "Macro inspection exceeds the 4096-node scene limit.");
  std::vector<TMacroFx*> result;
  for (int index = 0; index < effects->getFxCount(); ++index)
    if (auto* macro = dynamic_cast<TMacroFx*>(effects->getFx(index)))
      result.push_back(macro);
  return result;
}
}  // namespace

#ifdef BUILD_UI_PORT_TESTS
namespace {
bool macroVerifyCrash = false;
}
void setMacroVerifyCrash(bool armed) { macroVerifyCrash = armed; }
#endif

namespace {
QJsonObject macroGraphState(ToonzScene& scene,
                            const std::vector<TMacroFx*>& nodes,
                            const QString& scratchRoot) {
  auto* graph = scene.getXsheet()->getFxDag();
  QTemporaryDir scratch(scratchRoot + "/macro-verify-XXXXXX");
  if (!scratch.isValid())
    throw Error(
        "io_error",
        "Cannot create a private macro roundtrip verification directory.");
#ifdef BUILD_UI_PORT_TESTS
  if (macroVerifyCrash) std::_Exit(137);
#endif
  qint64 bytes = 0;
  QJsonObject macros;
  for (auto* macro : nodes)
    macros[key(macro)] =
        macroState(macro, scratch.path() + "/member.preset", bytes, graph);
  return {{"macros", macros},
          {"graph", graphState(graph)},
          {"current_output",
           connectionIdentity(graph->getCurrentOutputFx(), graph)}};
}
}  // namespace

QByteArray automationMacroFingerprint(ToonzScene& scene,
                                      const QString& scratchRoot) {
  auto* graph                 = scene.getXsheet()->getFxDag();
  const auto nodes            = macroNodes(graph);
  const QJsonObject structure = structureState(scene);
  if (nodes.empty() && structure.isEmpty()) return {};
  validateAutomationFxGraph(scene);
  QJsonObject state;
  if (!nodes.empty()) state = macroGraphState(scene, nodes, scratchRoot);
  // Added only when present, so packages without schematic structure keep
  // the fingerprint earlier builds recorded.
  if (!structure.isEmpty()) state["structure"] = structure;
  const QByteArray serialized =
      QJsonDocument(state).toJson(QJsonDocument::Compact);
  if (serialized.size() > 1024 * 1024)
    throw Error(
        "RESOURCE_LIMIT",
        "The FX graph roundtrip state (macros, links, port groups, schematic "
        "groups and placement) exceeds the 1 MiB verification limit.");
  return QCryptographicHash::hash(serialized, QCryptographicHash::Sha256);
}
}  // namespace GroundzeroAutomation
