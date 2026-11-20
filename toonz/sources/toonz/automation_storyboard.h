#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

class ToonzScene;

namespace GroundzeroAutomation {
class Context;

// Return strict descriptors for the detached storyboard panel operations.
QJsonArray storyboardOperations();

// Execute a revision-bound storyboard panel operation after validation.
//
// Args:
//   context: Worker documents and mutation-outcome tracking.
//   method: Registered storyboard operation name.
//   params: Strict-schema arguments including the document revision.
// Returns:
//   Panel descriptions; the dispatcher adds the receipt fields.
// Throws:
//   Error: Missing or duplicate panels, invalid text, limits or a stale
//   revision.
QJsonValue executeStoryboard(Context& context, const QString& method,
                             const QJsonObject& params);

// Require the root navigation tags to fit the panel profile that save,
// render and reopen admit.
void validateAutomationStoryboard(ToonzScene& scene);

// SHA-256 of the root navigation tags (frame, caption, color and notes), or
// an empty string when the scene has no tags.
QString automationTagsDigest(ToonzScene& scene);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationStoryboardTests(Context& context);
#endif
}  // namespace GroundzeroAutomation
