#include "automation.h"
#include "automation_fx_profile.h"
#include "automation_macro.h"

#include "tfx.h"
#include "tfxattributes.h"
#include "tmacrofx.h"
#include "tparamcontainer.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/tcolumnfx.h"
#include "toonz/toonzscene.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include <algorithm>
#include <limits>
#include <map>
#include <vector>

namespace GroundzeroAutomation {
namespace {
void require(bool valid, const char* message) {
  if (!valid) throw Error("INVALID_MACRO", QString::fromUtf8(message));
}

bool memberType(TFx* fx) {
  const auto type = fx->getFxType();
  return type == "overFx" || type == "STD_blurFx" || type == "STD_glowFx";
}

QString key(TFx* fx) { return QString::fromStdWString(fx->getFxId()); }

void validateMemberIdentity(TFx* fx) {
  require(
      memberType(fx),
      "Macro members require over, Blur or Glow; expand other effects first.");
  validateAutomationFxProfile(fx);
  const QString id = key(fx);
  require(!id.isEmpty() && !id.contains('_'),
          "Macro member IDs must be nonempty without underscores.");
  const int number = fx->getAttributes()->getId();
  require(
      number > 0 && number < std::numeric_limits<int>::max(),
      "Macro member numeric IDs must leave room for the next native effect.");
  require(fx->getLinkedFx() == fx,
          "Linked member parameter groups are unsupported.");
  require(fx->getInputPortCount() <= 128,
          "Macro member has too many input ports.");
  for (int index = 0; index < fx->getInputPortCount(); ++index)
    require(fx->getInputPortName(index).find('_') == std::string::npos,
            "Macro member input names cannot contain underscores.");
}

std::set<TFx*> membersOf(TMacroFx* macro, const std::set<TFx*>& top,
                         std::set<TFx*>& owned) {
  const auto& values = macro->getFxs();
  require(values.size() >= 2 && values.size() <= 64,
          "Macro requires 2 through 64 members.");
  std::set<TFx*> members;
  for (const auto& value : values) {
    TFx* fx = value.getPointer();
    require(fx && !top.count(fx) && owned.insert(fx).second,
            "Macro members must have one owner and cannot also be top-level "
            "effects.");
    validateMemberIdentity(fx);
    members.insert(fx);
  }
  require(members.count(macro->getRoot()),
          "Macro root is not one of its owned members.");
  return members;
}

void validateReachability(TMacroFx* macro, const std::set<TFx*>& members) {
  std::set<TFx*> visited, active;
  std::vector<std::pair<TFx*, bool>> pending{{macro->getRoot(), false}};
  while (!pending.empty()) {
    auto [fx, leaving] = pending.back();
    pending.pop_back();
    if (leaving) {
      active.erase(fx);
      continue;
    }
    require(!active.count(fx), "Macro contains an internal cycle.");
    if (!visited.insert(fx).second) continue;
    active.insert(fx);
    pending.emplace_back(fx, true);
    for (int index = 0; index < fx->getInputPortCount(); ++index) {
      TFx* source = fx->getInputPort(index)->getFx();
      if (members.count(source)) pending.emplace_back(source, false);
    }
  }
  require(visited == members,
          "Macro members are not one connected directed acyclic graph.");
}

void validateParameters(TMacroFx* macro) {
  require(macro->getLinkedFx() == macro,
          "Linked macro parameter groups are unsupported.");
  std::set<TParam*> parameters;
  for (const auto& member : macro->getFxs()) {
    auto* values = member->getParams();
    require(values->getParamCount() <= 128,
            "Macro member parameter count exceeds the profile.");
    for (int index = 0; index < values->getParamCount(); ++index)
      require(parameters.insert(values->getParam(index)).second,
              "Macro members cannot share parameter objects.");
  }
  auto* aliases = macro->getParams();
  require(size_t(aliases->getParamCount()) == parameters.size(),
          "Macro parameter aliases do not match its members.");
  for (int index = 0; index < aliases->getParamCount(); ++index)
    require(
        parameters.erase(aliases->getParam(index)) == 1,
        "Macro parameter alias references a foreign or repeated parameter.");
}

std::set<TFxPort*> boundaryPorts(TMacroFx* macro) {
  require(macro->getInputPortCount() <= 128,
          "Macro exposes more than 128 input ports.");
  std::set<TFxPort*> ports;
  for (int index = 0; index < macro->getInputPortCount(); ++index) {
    auto* port = macro->getInputPort(index);
    require(port && ports.insert(port).second && port->getOwnerFx() == macro,
            "Macro boundary ports must be unique and owned by their macro.");
  }
  return ports;
}

void validateBoundaryName(TMacroFx* macro, TFx* member, int memberPort) {
  auto* port = member->getInputPort(memberPort);
  for (int index = 0; index < macro->getInputPortCount(); ++index) {
    if (macro->getInputPort(index) != port) continue;
    const std::string expected = member->getInputPortName(memberPort) + "_" +
                                 std::to_string(index) + "_" +
                                 key(member).toStdString();
    require(
        macro->getInputPortName(index) == expected,
        "Macro boundary name does not match its exact native member input.");
    return;
  }
  throw Error("INVALID_MACRO",
              "Member input is missing from its macro boundary.");
}

void validateMemberConsumers(TFx* member, const std::set<TFx*>& members) {
  require(member->getOutputConnectionCount() <= 4096,
          "Macro member has too many native output consumers.");
  for (int index = 0; index < member->getOutputConnectionCount(); ++index) {
    auto* port = member->getOutputConnection(index);
    require(
        port && members.count(port->getOwnerFx()),
        "A macro member has an external consumer; connect through its macro.");
  }
}

void validateMemberPorts(TMacroFx* macro, const std::set<TFx*>& members,
                         const std::set<TFx*>& top) {
  auto boundary = boundaryPorts(macro);
  for (TFx* member : members) {
    for (int index = 0; index < member->getInputPortCount(); ++index) {
      auto* port  = member->getInputPort(index);
      TFx* source = port->getFx();
      if (members.count(source)) {
        require(port->getOwnerFx() == member && !boundary.count(port),
                "Internal macro ports must remain owned by their member.");
      } else {
        require(boundary.erase(port) == 1 && port->getOwnerFx() == macro,
                "Every external or null member input must be one exposed macro "
                "port.");
        validateBoundaryName(macro, member, index);
        require(!source || top.count(source),
                "Macro input references an unowned external effect.");
      }
    }
    validateMemberConsumers(member, members);
  }
  require(boundary.empty(),
          "Macro boundary contains a port not owned by any member.");
}

void collectIdentity(TFx* fx, std::map<QString, TFx*>& ids) {
  if (auto* column = dynamic_cast<TZeraryColumnFx*>(fx))
    fx = column->getZeraryFx();
  require(fx, "Generator column has no native effect.");
  const QString id = key(fx).toLower();
  if (id.isEmpty()) return;
  const auto found = ids.emplace(id, fx);
  require(
      found.second || found.first->second == fx,
      "Persistent native effect IDs collide under native lowercase lookup.");
}

std::vector<TMacroFx*> macrosIn(ToonzScene& scene) {
  auto* effects = scene.getXsheet()->getFxDag()->getInternalFxs();
  std::vector<TMacroFx*> macros;
  require(effects->getFxCount() <= 4096,
          "Scene has more than 4096 internal effects.");
  for (int index = 0; index < effects->getFxCount(); ++index)
    if (auto* macro = dynamic_cast<TMacroFx*>(effects->getFx(index)))
      macros.push_back(macro);
  return macros;
}
std::set<TFx*> candidateTopLevel(ToonzScene& scene) {
  std::set<TFx*> top;
  auto* sheet = scene.getXsheet();
  auto* graph = sheet->getFxDag();
  graph->getInternalFxs()->getFxs(top);
  top.insert(graph->getXsheetFx());
  for (int index = 0; index < graph->getOutputFxCount(); ++index)
    top.insert(graph->getOutputFx(index));
  for (int index = 0; index < sheet->getColumnCount(); ++index) {
    auto* column = sheet->getColumn(index);
    if (column && column->getFx()) top.insert(column->getFx());
  }
  return top;
}
}  // namespace

void validateAutomationMacros(ToonzScene& scene,
                              const std::set<TFx*>& topLevel) {
  const auto macros = macrosIn(scene);
  if (macros.empty()) return;
  std::set<TFx*> owned;
  std::map<QString, TFx*> ids;
  for (TFx* fx : topLevel) collectIdentity(fx, ids);
  for (auto* macro : macros) {
    auto members = membersOf(macro, topLevel, owned);
    require(topLevel.size() + owned.size() <= 4096,
            "Scene macro containers and descendants exceed 4096 effects.");
    for (TFx* fx : members) {
      collectIdentity(fx, ids);
      require(!scene.getXsheet()->getFxDag()->getTerminalFxs()->containsFx(fx),
              "Macro members cannot be Xsheet terminals.");
    }
    validateReachability(macro, members);
    validateParameters(macro);
    validateMemberPorts(macro, members, topLevel);
  }
}

void validateAutomationMacroCandidate(ToonzScene& scene, TMacroFx* macro,
                                      const std::set<TFx*>& replaced) {
  auto top = candidateTopLevel(scene);
  for (TFx* fx : replaced) top.erase(fx);
  top.insert(macro);
  size_t count = top.size();
  std::map<QString, TFx*> ids;
  for (TFx* fx : top) collectIdentity(fx, ids);
  for (auto* existing : macrosIn(scene)) {
    count += existing->getFxs().size();
    for (const auto& member : existing->getFxs())
      collectIdentity(member.getPointer(), ids);
  }
  require(
      count + macro->getFxs().size() <= 4096,
      "Macro container and all descendants exceed the 4096-node scene limit.");
  std::set<TFx*> owned;
  const auto members = membersOf(macro, top, owned);
  for (TFx* fx : members) collectIdentity(fx, ids);
  validateReachability(macro, members);
  validateParameters(macro);
  validateMemberPorts(macro, members, top);
}

void indexAutomationMacroMembers(ToonzScene& scene) {
  // The caller validates the whole private loaded scene before any index write.
  const auto macros = macrosIn(scene);
  auto* graph       = scene.getXsheet()->getFxDag();
  for (auto* macro : macros)
    for (const auto& member : macro->getFxs()) {
      TFx* found =
          graph->getFxById(key(member.getPointer()).toLower().toStdWString());
      require(!found || found == member.getPointer(),
              "Loaded macro member has a conflicting index.");
      require(graph->getFxTypeCount(member.getPointer()) <
                  std::numeric_limits<int>::max(),
              "Loaded macro member type counter is exhausted.");
    }
  for (auto* macro : macros)
    for (const auto& member : macro->getFxs()) {
      TFx* fx = member.getPointer();
      graph->updateFxIdTable(fx);
      graph->updateFxTypeTable(fx, std::max(graph->getFxTypeCount(fx),
                                            fx->getAttributes()->getId()));
    }
}
}  // namespace GroundzeroAutomation
