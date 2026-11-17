#pragma once

#include "tgeometry.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <string>
#include <vector>

class TStageObject;
class ToonzScene;

namespace GroundzeroAutomation {
class Context;
struct Document;

// Return strict descriptors for rig inspection, handles, pivots and layers.
QJsonArray rigOperations();
// Execute a revision-bound rig operation after dispatcher validation.
//
// Args:
//   context: Worker document registry and mutation-outcome tracking.
//   method: Registered rig, object handle/pivot or offset_layer operation.
//   params: Strict-schema arguments including the current document revision.
// Returns:
//   Bounded native placement, handle and layer records.
// Throws:
//   Error: Unsupported targets, locks, name conflicts, budgets or precision.
QJsonValue executeRig(Context& context, const QString& method,
                      const QJsonObject& params);

// Require the bounded rig profile before native placement is evaluated: at
// most 4096 objects, 64 parent edges without cycles, letter or column hook
// handles, the offset-layer profile, and no IK, pins, published channels or
// Plastic deformation. Hooks are checked by validateAutomationHooks.
void validateAutomationRig(ToonzScene& scene);

// Publish opaque handles for the offset layers of a newly admitted document.
void initializeAutomationRig(Document& document);

// Refuse a prospective parent that would exceed 64 parent edges.
void validateAutomationStageParent(ToonzScene& scene, TStageObject& object,
                                   TStageObject& parent);

// Resolve the parent handle object.parent keeps or sets for a new parent.
//
// Args:
//   document: Owner of both objects.
//   object: The reparented child.
//   parent: Its prospective parent.
//   requested: Optional parent_handle record; undefined keeps the current
//   string.
// Returns:
//   A native handle string valid on the prospective parent.
// Throws:
//   Error: A hook on a non-column parent or a hook missing from its levels.
std::string automationParentHandle(Document& document, TStageObject& object,
                                   TStageObject& parent,
                                   const QJsonValue& requested);

// Input schema of a peg_hole or hook handle record.
QJsonObject automationHandleInputSchema();

// Input schema of unique report_frames (1..64 integer rows).
QJsonObject automationReportFramesSchema();

// Describe a native handle string as a peg hole or hook record.
QJsonObject automationHandleRecord(const std::string& handle);

// Read unique report_frames; empty when the field is absent.
std::vector<int> automationReportFrames(const QJsonObject& params);

// Evaluate fresh stage placements of one object at integer frames.
std::vector<TAffine> automationPlacements(ToonzScene& scene,
                                          TStageObject& object,
                                          const std::vector<int>& frames);

// Add world_changes and world_placement_preserved for reported frames.
void insertAutomationWorldChanges(QJsonObject& result,
                                  const std::vector<int>& frames,
                                  const std::vector<TAffine>& before,
                                  const std::vector<TAffine>& after);

// A live offset layer resolved from its opaque handle.
struct AutomationLayer final {
  TStageObject* object;
  int index;
};

// Resolve a layer_id of this document; stale handles are invalid_handle.
AutomationLayer automationLayer(Document& document, const QString& layerId);

// Summaries of an object's layers in native order with their handles.
QJsonArray automationLayerSummaries(Document& document,
                                    const TStageObject& object);

#ifdef BUILD_UI_PORT_TESTS
void runAutomationRigTests(Context& context);
#endif
}  // namespace GroundzeroAutomation
