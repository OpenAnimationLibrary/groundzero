#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <map>
#include <vector>

class TFrameId;
class TXshSimpleLevel;
class ToonzScene;

namespace GroundzeroAutomation {
class Context;
struct Document;

// Return strict descriptors for the detached level hook operations.
QJsonArray hookOperations();
// Execute a revision-bound hook read or write after dispatcher validation.
//
// Args:
//   context: Worker document registry and mutation-outcome tracking.
//   method: Registered hook operation name.
//   params: Strict-schema arguments including the current document revision.
// Returns:
//   Canonical hook keys in inches with every exposing column and user.
// Throws:
//   Error: Locked or Plastic users, missing drawings, budgets or precision.
QJsonValue executeHook(Context& context, const QString& method,
                       const QJsonObject& params);

// Level columns of the root sheet and the levels their cells expose.
struct AutomationExposures final {
  std::map<int, std::vector<TXshSimpleLevel*>> levels;
  std::map<TXshSimpleLevel*, std::vector<int>> columns;
};

// Scan at most one million root-sheet cells for level exposures.
AutomationExposures automationExposures(ToonzScene& scene);

// Whether hook_number has at least one key in the level.
bool automationHookDefined(TXshSimpleLevel& level, int hookNumber);

// Require the bounded hook profile of every level: at most 99 hooks, finite
// unsuffixed keys on existing drawings within 1000 inches, no tracker
// regions, uniform raster DPI and the per-level and per-scene key budgets.
void validateAutomationHooks(ToonzScene& scene);

// Write the native hooks sidecar next to a saved level file.
//
// Args:
//   level: Source level whose HookSet is written as TXshSimpleLevel::save
//   does.
//   levelFile: Absolute path of the saved level file.
// Returns:
//   SHA-256 of the canonical hook keys, or null when the level has no hooks.
QJsonValue writeAutomationHooks(TXshSimpleLevel& level,
                                const QString& levelFile);

// Load a reopened level's sidecar and require its published hook digest.
//
// Args:
//   scene: Reopened scene that decodes the level path.
//   level: Level whose drawings were already restored.
//   digest: The manifest digest, or null when no sidecar was published.
// Throws:
//   Error: invalid_package when the sidecar is missing, extra or different.
void readAutomationHooks(ToonzScene& scene, TXshSimpleLevel& level,
                         const QJsonValue& digest);

// Count the hooks keyed on one drawing.
int automationHookKeys(TXshSimpleLevel& level, const TFrameId& frame);

// Refuse removing a drawing whose keys are the last of a hook in use.
void requireAutomationHookRemoval(Document& document, TXshSimpleLevel& level,
                                  const TFrameId& frame);

// Keys expected after a native drawing renumber moves hook-keyed drawings.
struct AutomationHookRenumber final {
  int moved = 0;
  QJsonArray expected;
};

// Refuse suffixed destinations of hook-keyed drawings and record the keys.
AutomationHookRenumber prepareAutomationHookRenumber(
    TXshSimpleLevel& level, const std::map<TFrameId, TFrameId>& mapping);

// Require the native renumber to have moved every hook key as prepared.
void verifyAutomationHookRenumber(TXshSimpleLevel& level,
                                  const AutomationHookRenumber& prepared);
}  // namespace GroundzeroAutomation
