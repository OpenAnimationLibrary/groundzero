#include "automation.h"
#include "automation_fx.h"
#include "automation_fx_journal.h"

#include "tfx.h"
#include "tmacrofx.h"
#include "tparamcontainer.h"
#include "tzeraryfx.h"
#include "toonz/tcolumnfx.h"

#include <QJsonArray>

namespace GroundzeroAutomation {
namespace {

constexpr int kMaximumLinkedMembers = 64;

// The effect whose parameters a node owns: a generator column's effect.
TFx* nativeOf(TFx* node) { return automationParameterFx(node); }

int ringSize(TFx* native) {
  int size    = 1;
  TFx* member = native->getLinkedFx();
  while (member && member != native && size <= 1024) {
    ++size;
    member = member->getLinkedFx();
  }
  return size;
}

bool sameRing(TFx* first, TFx* second) {
  TFx* member = first;
  for (int guard = 0; guard < 1024; ++guard) {
    if (member == second) return true;
    member = member->getLinkedFx();
    if (!member || member == first) return false;
  }
  return false;
}

// A node the editor can link: an admitted builtin effect or generator with
// public parameters; never the Xsheet node, an output, a macro, or a level,
// palette or sound column.
void requireLinkable(Document& document, TFx* node) {
  const bool column =
      dynamic_cast<TColumnFx*>(node) && !dynamic_cast<TZeraryColumnFx*>(node);
  const bool excluded = column || dynamic_cast<TOutputFx*>(node) ||
                        dynamic_cast<TMacroFx*>(node) ||
                        dynamic_cast<TXsheetFx*>(node);
  TFx* native         = excluded ? nullptr : nativeOf(node);
  if (excluded ||
      !isAutomationFxTypeSupported(
          QString::fromStdString(native->getFxType())) ||
      native->getParams()->getParamCount() == 0)
    throw Error("unsupported_link",
                "Only admitted builtin effects and generators with parameters "
                "can be linked.",
                {{"fx_id", document.idForFx(node)}});
}

QStringList parameterNames(TFx* native) {
  QStringList names;
  TParamContainer* params = native->getParams();
  for (int index = 0; index < params->getParamCount(); ++index)
    names.append(QString::fromStdString(params->getParamName(index)));
  return names;
}

void requireCompatible(TFx* native, TFx* target) {
  if (native->getFxType() != target->getFxType() ||
      native->getFxVersion() != target->getFxVersion() ||
      parameterNames(native) != parameterNames(target))
    throw Error(
        "link_incompatible",
        "Linked effects share every parameter, so they must have the "
        "same type, native version and parameters.",
        {{"type", QString::fromStdString(native->getFxType())},
         {"linked_to_type", QString::fromStdString(target->getFxType())}});
}

QJsonValue addLink(Context& context, Document& document,
                   const QJsonObject& params) {
  const QString id = requiredString(params, "fx_id");
  TFx* node        = automationOwnedFx(document, id);
  TFx* targetNode =
      automationOwnedFx(document, requiredString(params, "linked_to_fx_id"));
  if (node == targetNode)
    throw Error("invalid_params", "An effect cannot be linked to itself.");
  requireLinkable(document, node);
  requireLinkable(document, targetNode);
  TFx* native = nativeOf(node);
  TFx* target = nativeOf(targetNode);
  requireCompatible(native, target);
  if (sameRing(native, target))
    return QJsonObject{
        {"changed", false},
        {"fx_id", id},
        {"linked_group", automationFxLinkedGroup(document, node)},
        {"overwritten_parameters", QJsonArray()},
        {"affected_fx_ids", QJsonArray()}};
  if (native->getLinkedFx() != native)
    throw Error("already_linked",
                "The effect already shares parameters with another group; "
                "unlink it first.",
                {{"group_id", automationFxLinkedGroup(document, node)
                                  .toObject()
                                  .value("group_id")}});
  if (ringSize(target) >= kMaximumLinkedMembers)
    throw Error("resource_limit", "A linked group holds at most 64 effects.",
                {{"limit", "linked_members"}});
  automationRequireColumnEditable(document, node);
  automationRequireUnlockedCurves(document, node);
  const QStringList overwritten =
      automationFxDifferingParameters(node, targetNode);
  context.markMutationStarted();
  native->linkParams(target);
  if (!sameRing(native, target) ||
      native->getParams()->getParam(0) != target->getParams()->getParam(0))
    throw Error("NATIVE_FAILURE",
                "Native parameter linking did not take effect.");
  return QJsonObject{
      {"changed", true},
      {"fx_id", id},
      {"linked_group", automationFxLinkedGroup(document, node)},
      {"overwritten_parameters", QJsonArray::fromStringList(overwritten)},
      {"affected_fx_ids", QJsonArray{id}}};
}

// Any other member of node's ring, to describe what remains of the group.
TFx* otherMember(Document& document, TFx* native) {
  TFx* member = native->getLinkedFx();
  if (!member || member == native) return nullptr;
  if (auto* zerary = dynamic_cast<TZeraryFx*>(member))
    if (zerary->getColumnFx()) return zerary->getColumnFx();
  Q_UNUSED(document);
  return member;
}

QJsonValue removeLink(Context& context, Document& document,
                      const QJsonObject& params) {
  const QString id = requiredString(params, "fx_id");
  TFx* node        = automationOwnedFx(document, id);
  TFx* native      = nativeOf(node);
  if (native->getLinkedFx() == native)
    return QJsonObject{{"changed", false},
                       {"fx_id", id},
                       {"group_before", QJsonValue()},
                       {"remaining_group", QJsonValue()}};
  automationRequireColumnEditable(document, node);
  const QJsonValue before = automationFxLinkedGroup(document, node);
  TFx* other              = otherMember(document, native);
  context.markMutationStarted();
  native->unlinkParams();
  if (native->getLinkedFx() != native)
    throw Error("NATIVE_FAILURE",
                "Native parameter unlinking did not take effect.");
  return QJsonObject{
      {"changed", true},
      {"fx_id", id},
      {"group_before", before},
      {"remaining_group",
       other ? automationFxLinkedGroup(document, other) : QJsonValue()}};
}

QJsonObject idSchema() {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
}

}  // namespace

QJsonArray fxLinkOperations() {
  const QJsonObject effect{{"document_id", idSchema()}, {"fx_id", idSchema()}};
  QJsonObject add        = effect;
  add["linked_to_fx_id"] = idSchema();
  return {
      operation("fx.link.add",
                "Link an effect's parameters to another effect of the same "
                "type and version, as the editor's linked effects: it takes "
                "that group's values and keys, and every later parameter edit "
                "of one member changes them all. Connections, name and "
                "placement stay.",
                add, {"document_id", "fx_id", "linked_to_fx_id"}, true),
      operation("fx.link.remove",
                "Give a linked effect its own copy of the shared parameters, "
                "with equal values and keys; the other members stay linked.",
                effect, {"document_id", "fx_id"}, true)};
}

QJsonValue executeFxLink(Context& context, Document& document,
                         const QString& method, const QJsonObject& params) {
  if (method == "fx.link.add") return addLink(context, document, params);
  if (method == "fx.link.remove") return removeLink(context, document, params);
  throw Error("METHOD_NOT_FOUND", "Unknown FX link operation.");
}

}  // namespace GroundzeroAutomation
