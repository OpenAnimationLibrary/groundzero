#pragma once

#include "automation_cells.h"

namespace GroundzeroAutomation {

enum class RetimeKind {
  Reverse,
  Swing,
  Step,
  Each,
  Reframe,
  IncreaseStep,
  DecreaseStep,
  ResetStep,
  Repeat,
  RollUp,
  RollDown,
  TimeStretch
};

/// One native timing operation over the same interval of each column.
struct RetimeRequest final {
  RetimeKind kind = RetimeKind::Reverse;
  int start       = 0;
  int length      = 1;
  /// step and each: the factor; reframe: the step.
  int factor = 1;
  /// reframe: native withBlank (-1 keeps blank entries).
  int withBlank       = -1;
  bool extendOverHold = false;
  /// repeat: the last row to fill.
  int until = 0;
  /// time_stretch: the interval's new length.
  int newLength = 1;
};

/// The composite op that runs the native function on one column and its
/// port on the column's model; reframe extends over a hold from `before`.
CellOp retimeOp(const RetimeRequest& request, const CellModel& before);

/// An upper bound on the rows one column can gain.
int retimeRowsAdded(const RetimeRequest& request);

}  // namespace GroundzeroAutomation
