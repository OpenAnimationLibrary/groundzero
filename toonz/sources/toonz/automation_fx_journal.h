#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

class TFx;

namespace GroundzeroAutomation {

class Context;
struct Document;

// The FX graph journal of a document: a snapshot of its top sheet's graph
// after every write that advanced the revision, and the states each such
// write changed, so that a delta between two revisions is a diff of
// snapshots rather than something each tool describes about itself. It
// also owns the document's creation order of effects and the handles of
// schematic groups and linked groups.
class FxGraphJournal;

// Before a write that names a document: start the document's journal with
// a baseline snapshot of its current graph when it has none.
void prepareFxGraphJournal(Context& context, const QString& documentId);

// After a write: when it advanced the revision, record one step and, for an
// FX graph tool, set the result's graph_delta to that step; when it kept the
// revision, an FX graph tool's graph_delta is null. advanced is false for a
// no-op; result is null for a write that failed after it began.
void recordFxGraphStep(Context& context, const QString& documentId,
                       const QString& tool, const QJsonObject& params,
                       bool advanced, QJsonObject* result);

// fx.graph.delta: the net change of the top sheet's graph from from_revision
// to the current revision, a page at a time.
QJsonValue automationFxGraphDelta(Document& document,
                                  const QJsonObject& params);

// fx.graph's delta_window: how far back fx.graph.delta can reach.
QJsonObject automationFxDeltaWindow(Document& document);

// The document's creation sequence of an effect handle, assigned when the
// journal first sees the effect.
quint64 automationFxSequence(Document& document, TFx* fx);

// Opaque handles of a sheet's native schematic group ids and of linked
// parameter groups, stable for the document's lifetime.
QString automationFxGroupHandle(Document& document, int nativeGroupId);
// The native group id of a handle, or 0 for a handle this document never
// issued.
int automationFxGroupIdOrZero(Document& document, const QString& handle);
// The current top-sheet graph as the journal sees it: "nodes" maps each
// effect handle to its state (type, name, enabled, terminal, position,
// group_ids innermost first, linked_group_id, port_names, parameter digests);
// "linked_groups" maps a linked group handle to its members in creation
// order; "groups" maps a schematic group handle to its name and member count.
QJsonObject automationFxGraphView(Document& document);

#ifdef BUILD_UI_PORT_TESTS
// Test builds snapshot after every write and refuse one whose effect
// declaration wrongly says it cannot change the FX graph; false measures the
// shipping path, which does not.
void setFxJournalVerification(bool verify);
#endif

// The schematic groups of the top sheet, for fx.graph: id, name, members in
// creation order, member count and the enclosing group.
QJsonArray automationFxGroups(Document& document);

// The fx-graph toolset: schematic groups and placement.
QJsonArray fxGraphOperations();
QJsonValue executeFxGraph(Context& context, Document& document,
                          const QString& method, const QJsonObject& params);

}  // namespace GroundzeroAutomation
