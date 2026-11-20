#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace GroundzeroAutomation {
class Context;

// Return strict descriptors for lip-sync timing import and application.
QJsonArray lipsyncOperations();

// Execute a lip-sync operation after dispatcher validation.
//
// Args:
//   context: Worker documents, path permissions and mutation tracking.
//   method: Registered lipsync operation name.
//   params: Strict-schema arguments; apply also carries the document revision.
// Returns:
//   An immutable analysis, a page of its cues, or the applied cell range.
// Throws:
//   Error: Malformed timing files, unmapped shapes, missing drawings, timing
//   or link changes, locks and limits.
QJsonValue executeLipsync(Context& context, const QString& method,
                          const QJsonObject& params);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationLipsyncTests(Context& context);
#endif
}  // namespace GroundzeroAutomation
