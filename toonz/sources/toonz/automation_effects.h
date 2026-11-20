#pragma once

#include "automation.h"

#include <QJsonObject>
#include <QStringList>

namespace GroundzeroAutomation {

// What one tool does to documents, files, jobs and processes. The rest of a
// published effect declaration (undo, no-op, partial failure, retry, grants)
// and the tool's annotations are derived from these fields.
struct ToolEffects {
  QString kind;
  QString document;
  QStringList files;
  QString job;
  QStringList processes;
  bool overwrites = false;
  // Whether the tool can change a document's FX graph: its nodes, edges,
  // terminal links, output, parameters, groups or placement. Only such writes
  // record a snapshot in the FX graph journal. Not published.
  bool fxGraph = false;
};

// The descriptor with its effects, errors and derived annotations. Errors are
// the codes common to the tool's class followed by its own codes.
QJsonObject declaredTool(QJsonObject descriptor, const ToolEffects& effects,
                         const QStringList& errors = {});

// The descriptor declared from the worker's table of tool effects. A tool
// without a row, or one whose row disagrees with its input schema, is an
// internal_error that names the row to add or fix.
QJsonObject declaredTool(const QJsonObject& descriptor);

// The names of the tools the effect table declares, for the census check.
QStringList declaredToolNames();

// The names of the tools automation_output_schemas.json describes.
QStringList outputSchemaNames();

// Whether the effect table says the tool can change a document's FX graph.
bool changesFxGraph(const QString& tool);

// Whether a successful write advances the revision of its document.
bool advancesRevision(const QJsonObject& descriptor);

// A published outputSchema: the success shape or the error envelope.
QJsonObject outputSchema(const QJsonObject& success);
QJsonObject withOutputSchema(QJsonObject descriptor,
                             const QJsonObject& success);
QJsonObject errorEnvelopeSchema();

// Success shapes of the two worker operations.
QJsonObject capabilitiesResultSchema();
QJsonObject documentListResultSchema();
QJsonObject toolDescriptionResultSchema(int maximumTools);

}  // namespace GroundzeroAutomation
