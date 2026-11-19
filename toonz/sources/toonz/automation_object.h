#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

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

}  // namespace GroundzeroAutomation
