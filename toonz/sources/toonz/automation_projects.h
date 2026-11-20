#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace GroundzeroAutomation {
class Context;

// Return strict descriptors for project.create and project.duplicate.
QJsonArray projectWriteOperations();

// Write a native project definition in the workspace after verifying it.
//
// Args:
//   context: Worker owning the workspace, the parse helpers and the project
//   registry.
//   method: project.create or project.duplicate.
//   params: Strict-schema arguments.
// Returns:
//   The project.inspect record of the new project.
// Throws:
//   Error: already_exists, name_conflict, not_found, invalid_project, or a
//   staged write error that names its stage; nothing is published then.
QJsonValue executeProjectWrite(Context& context, const QString& method,
                               const QJsonObject& params);

// The binding scene.create records for a draft created in a project: the
// project's record (read through the parse helper) and the explicit
// identity a document reports.
struct DraftProject final {
  QJsonObject binding;
  QJsonValue sceneDefaults;
};

// Read a listed or created project for a new draft.
DraftProject draftProject(Context& context, const QString& projectId);

}  // namespace GroundzeroAutomation
