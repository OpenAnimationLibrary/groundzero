#pragma once

#include "automation.h"

#include "tfilepath.h"
#include "toonz/txshcell.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <set>
#include <vector>

class TXsheet;
class TXshCellColumn;
class TXshChildLevel;
class TXshSimpleLevel;

namespace GroundzeroAutomation {

/// The exposures of one level in one column of one sheet.
struct LevelUsageColumn final {
  TXsheet* sheet = nullptr;
  /// The sub-xsheet level that owns the sheet; null for the top sheet.
  TXshChildLevel* sheetLevel = nullptr;
  int index                  = 0;
  TXshCellColumn* column     = nullptr;
  bool locked                = false;
  /// Rows whose cell exposes the level (and one of the frames, when given).
  std::vector<int> rows;
};

/// Every exposure of a level under the top sheet and in every sub-xsheet of
/// the cast. The scan stops at 1,000,000 cells or 128 sheets and then says
/// it is incomplete; no write may rely on an incomplete scan.
struct LevelUsage final {
  bool complete = true;
  QString reason;
  qint64 cells = 0;
  int sheets   = 0;
  std::vector<LevelUsageColumn> columns;
  bool locked() const;
};

/// Scan the exposures of a level, optionally of some of its drawings only.
///
/// Args:
///   document: The document whose top sheet and cast are walked.
///   level: The level whose exposures are collected.
///   frames: When not null, only exposures of these drawings count.
/// Returns:
///   The exposures per column with locks and the scan's completeness.
LevelUsage scanLevelUsage(Document& document, TXshLevel* level,
                          const std::set<TFrameId>* frames = nullptr);

/// The usage report of the contract: complete, cells, columns, sheets and at
/// most 256 rows; top-sheet rows carry the column handle.
QJsonObject levelUsageJson(Document& document, const LevelUsage& usage);

/// Require a complete scan with no locked exposing column.
///
/// Throws:
///   Error: resource_limit for an incomplete scan; locked with every locked
///     exposing column listed.
void requireEditableUsage(Document& document, const LevelUsage& usage);

/// Rewrite every exposure a scan found, without shifting other cells: to an
/// empty cell when `replacement` is empty, else to `replacement`'s level
/// with the same drawing id (or `frame` when it is not empty). The caller
/// has checked every bound and marked the mutation started.
///
/// Returns:
///   The number of cells rewritten.
/// Throws:
///   Error: native_error when a native write is refused or the column
///     differs from the expected cells afterwards.
qint64 rewriteLevelUsage(const LevelUsage& usage, TXshLevel* replacement,
                         const TFrameId& frame = TFrameId());

/// The `settings` record of level.inspect for a simple level.
QJsonObject levelSettingsReport(Document& document, TXshSimpleLevel* level,
                                const LevelUsage& usage);

/// Descriptors of level.settings.set, level.rename, level.duplicate and
/// level.remove (toolset sheet).
QJsonArray levelLifecycleOperations();

/// Run one of the level lifecycle tools.
QJsonValue executeLevelLifecycle(Context& context, const QString& method,
                                 const QJsonObject& params);

/// Run the level lifecycle domain tests.
void runAutomationLevelLifecycleTests(Context& context);

}  // namespace GroundzeroAutomation
