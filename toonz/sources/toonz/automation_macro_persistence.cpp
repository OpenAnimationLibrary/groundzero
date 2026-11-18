#include "automation.h"
#include <cstdlib>
#include "automation_fx.h"
#include "automation_macro.h"

#include "tfx.h"
#include "tfxattributes.h"
#include "tmacrofx.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include <QCryptographicHash>
#include <QFile>
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

QByteArray automationMacroFingerprint(ToonzScene& scene,
                                      const QString& scratchRoot) {
  auto* graph      = scene.getXsheet()->getFxDag();
  const auto nodes = macroNodes(graph);
  if (nodes.empty()) return {};
  validateAutomationFxGraph(scene);
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
  const QJsonObject state{
      {"macros", macros},
      {"graph", graphState(graph)},
      {"current_output",
       connectionIdentity(graph->getCurrentOutputFx(), graph)}};
  const QByteArray serialized =
      QJsonDocument(state).toJson(QJsonDocument::Compact);
  if (serialized.size() > 1024 * 1024)
    throw Error("RESOURCE_LIMIT",
                "Macro roundtrip state exceeds the 1 MiB verification limit.");
  return QCryptographicHash::hash(serialized, QCryptographicHash::Sha256);
}
}  // namespace GroundzeroAutomation
