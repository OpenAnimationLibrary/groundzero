#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <string>

class TStageObject;

namespace GroundzeroAutomation {
class Context;
struct Document;

// Return strict descriptors for stage-object duplicate, rename, delete,
// dependency preflight and schematic grouping.
QJsonArray objectOperations();

// Execute a revision-bound object lifecycle operation after dispatcher
// validation.
//
// Args:
//   context: Worker document registry and mutation-outcome tracking.
//   method: Registered object lifecycle or grouping operation.
//   params: Strict-schema arguments including the current document revision.
// Returns:
//   Bounded object, dependency, mapping and group records.
// Throws:
//   Error: Unsupported targets, locks, dependents, budgets or stale handles.
QJsonValue executeObject(Context& context, const QString& method,
                         const QJsonObject& params);

// Input schema of object.parent's optional world_policy.
QJsonObject automationWorldPolicySchema();

// Reparent an object under its preflighted parent and handle with the
// requested placement_policy.
//
// Args:
//   context: Marks the start of native mutation.
//   document: Owner of both objects.
//   object: The reparented child, already checked for cycles and depth.
//   parent: Its new parent.
//   handle: The validated parent handle string on the new parent.
//   params: object.parent arguments: placement_policy and world_policy.
// Returns:
//   world_preservation for preserve_world; empty for preserve_local.
// Throws:
//   Error: world_policy without preserve_world, or a world placement that
//   cannot be kept exactly (with a bake proposal) or baked.
QJsonObject automationApplyParent(Context& context, Document& document,
                                  TStageObject& object, TStageObject& parent,
                                  const std::string& handle,
                                  const QJsonObject& params);
}  // namespace GroundzeroAutomation
