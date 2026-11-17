#include "automation.h"
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

namespace GroundzeroAutomation {
namespace {
QString key(TFx* fx) {
  if (!fx) return {};
  if (auto* column = dynamic_cast<TZeraryColumnFx*>(fx))
    fx = column->getZeraryFx();
  return QString::fromStdWString(fx->getFxId());
}

QJsonObject ports(TFx* fx) {
  QJsonObject result;
  for (int index = 0; index < fx->getInputPortCount(); ++index)
    result[QString::fromStdString(fx->getInputPortName(index))] =
        key(fx->getInputPort(index)->getFx());
  return result;
}

QJsonObject memberState(TFx* fx, const QString& scratch, qint64& bytes) {
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
      {"ports", ports(fx)},
      {"parameters", QString::fromLatin1(QCryptographicHash::hash(
                                             preset, QCryptographicHash::Sha256)
                                             .toHex())}};
}

QJsonObject macroState(TMacroFx* macro, const QString& scratch, qint64& bytes) {
  QJsonObject members;
  for (const auto& member : macro->getFxs())
    members[key(member.getPointer())] =
        memberState(member.getPointer(), scratch, bytes);
  return {{"name", QString::fromStdWString(macro->getName())},
          {"number", macro->getAttributes()->getId()},
          {"enabled", macro->getAttributes()->isEnabled()},
          {"native_fx_version", macro->getFxVersion()},
          {"root", key(macro->getRoot())},
          {"ports", ports(macro)},
          {"members", members}};
}

QJsonObject graphState(FxDag* graph) {
  QJsonObject result;
  auto* effects = graph->getInternalFxs();
  for (int index = 0; index < effects->getFxCount(); ++index) {
    TFx* fx = effects->getFx(index);
    result[key(fx)] =
        QJsonObject{{"ports", ports(fx)},
                    {"terminal", graph->getTerminalFxs()->containsFx(fx)}};
  }
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    result["output-" + QString::number(index)] =
        ports(graph->getOutputFx(index));
  return result;
}
}  // namespace

QByteArray automationMacroFingerprint(ToonzScene& scene,
                                      const QString& scratchRoot) {
  auto* graph   = scene.getXsheet()->getFxDag();
  auto* effects = graph->getInternalFxs();
  bool present  = false;
  for (int index = 0; index < effects->getFxCount(); ++index)
    if (dynamic_cast<TMacroFx*>(effects->getFx(index))) present = true;
  if (!present) return {};
  validateAutomationFxGraph(scene);
  QTemporaryDir scratch(scratchRoot + "/.mcp-macro-verify-XXXXXX");
  if (!scratch.isValid())
    throw Error(
        "io_error",
        "Cannot create a private macro roundtrip verification directory.");
  qint64 bytes = 0;
  QJsonObject macros;
  for (int index = 0; index < effects->getFxCount(); ++index)
    if (auto* macro = dynamic_cast<TMacroFx*>(effects->getFx(index)))
      macros[key(macro)] =
          macroState(macro, scratch.path() + "/member.preset", bytes);
  const QJsonObject state{{"macros", macros},
                          {"graph", graphState(graph)},
                          {"current_output", key(graph->getCurrentOutputFx())}};
  const QByteArray serialized =
      QJsonDocument(state).toJson(QJsonDocument::Compact);
  if (serialized.size() > 1024 * 1024)
    throw Error("RESOURCE_LIMIT",
                "Macro roundtrip state exceeds the 1 MiB verification limit.");
  return QCryptographicHash::hash(serialized, QCryptographicHash::Sha256);
}
}  // namespace GroundzeroAutomation
