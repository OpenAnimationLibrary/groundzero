#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

class TDoubleParam;
class TStageObject;
class ToonzScene;

namespace GroundzeroAutomation {
class Context;
struct Document;

// Return strict descriptors for the approved detached path operations.
QJsonArray pathOperations();
// Execute a revision-bound native path operation after dispatcher validation.
//
// Args:
//   context: Worker ownership, path permissions and mutation-outcome tracking.
//   method: Registered motion_path or object.path operation name.
//   params: Strict-schema arguments including the current document revision.
// Returns:
//   Bounded native metadata; the dispatcher adds the mutation receipt.
// Throws:
//   Error: Invalid geometry, expired ownership, quotas, locks or native
//   failures.
QJsonValue executePath(Context& context, const QString& method,
                       const QJsonObject& params);

// Register bounded scene-owned path handles before publishing a document.
//
// Args:
//   document: Unpublished document containing a worker-created or verified
//   scene.
// Throws:
//   Error: Unsupported native paths or stage topology before handle
//   publication.
void initializeAutomationPaths(Document& document);

// Require the worker-owned regular path and bounded stage hierarchy profile.
//
// Args:
//   scene: Scene whose path geometry and consumer topology must be admitted.
// Throws:
//   Error: Unsupported geometry, ownership, percentage curves or resource
//   bounds.
void validateAutomationPaths(ToonzScene& scene);

// Refuse a reparenting that would exceed path-scene topology bounds before
// writes.
//
// Args:
//   scene: Owner of both resolved native stage objects.
//   object: Object whose prospective parent changes.
//   parent: Proposed parent; the native tree remains unchanged during
//   validation.
// Throws:
//   Error: Cyclic, missing or excessive-depth prospective ancestry.
void validateAutomationPathParent(ToonzScene& scene, TStageObject* object,
                                  TStageObject* parent);

// Check the target's path profile and, for writes, locks and bounded ancestry.
//
// Args:
//   scene: Native owner used to resolve column locks and parent membership.
//   object: Resolved target whose mode and percentage curve must be supported.
//   writable: Whether locks and native invalidation depth must also be checked.
// Throws:
//   Error: Locked or unsupported target, curve or hierarchy.
void requireAutomationPathTarget(ToonzScene& scene, TStageObject& object,
                                 bool writable);

// Refuse stage-object growth beyond the admitted path scene budget.
void requireAutomationPathObjectGrowth(ToonzScene& scene);

// Require constant/linear, continuous, bounded native percentage content.
void validateAutomationPathPosition(const TDoubleParam& curve);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationPathTests(Context& context);
#endif
}  // namespace GroundzeroAutomation
