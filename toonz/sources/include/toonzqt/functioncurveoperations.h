#pragma once

#ifndef FUNCTIONCURVEOPERATIONS_H
#define FUNCTIONCURVEOPERATIONS_H

#include "tdoubleparam.h"
#include <QString>
#include <cstddef>
#include <functional>
#include <set>
#include <utility>
#include <vector>

#undef DVAPI
#ifdef TOONZQT_EXPORTS
#define DVAPI DV_EXPORT_API
#else
#define DVAPI DV_IMPORT_API
#endif

// Native, non-mutating plans used by the Function Editor's preview and apply
// flows. A plan owns its snapshots. apply() rejects stale or locked targets.
namespace FunctionCurveOperations {
struct Selection {
  TDoubleParamP curve;
  std::set<int> indices;
};
struct Change {
  TDoubleParamP curve;
  TDoubleParamP before;
  TDoubleParamP after;
  // Unit preferences live outside TDoubleParam snapshots. Record their mapping
  // so a preference change cannot reinterpret an already-previewed amount.
  QString displayUnit;
  double displayZero = 0, displayOne = 1;
};
struct Plan {
  std::vector<Change> changes;
  QString error;
  bool cancelled = false;
  size_t sampleCount = 0;
  size_t removedKeys = 0;
  double maximumError = 0;
  bool valid() const { return error.isEmpty() && !cancelled; }
};
struct Range {
  double first = 0, last = 0, step = 1;
};
enum class ValueMode { Set, Offset };
using Cancel = std::function<bool()>;

DVAPI double displayValue(const TDoubleParam *curve, double nativeValue);
DVAPI Plan planValues(const std::vector<Selection> &selection, ValueMode mode,
                     double displayValue);
DVAPI Plan planFrameOffset(const std::vector<Selection> &selection,
                          double frames);
DVAPI Plan planRetime(const std::vector<Selection> &selection, double pivot,
                     double positiveScale);
// Map the bounds of the complete multi-channel selection to exact destinations.
// A selection at one frame can only be translated to one destination frame.
DVAPI Plan planRetimeToRange(const std::vector<Selection> &selection,
                            double targetFirst, double targetLast);
DVAPI Plan planQuantize(const std::vector<Selection> &selection, double pivot,
                       double positiveInterval);
DVAPI Plan planBreakdown(const std::vector<Selection> &selection, double frame,
                        double favour);

// Include last even when step does not divide the range. Partial range bounds
// within a curve must be existing keys, preserving the surrounding segments.
// Bake evaluates the snapshot before making any replacement keys. The resulting
// linear curve is exact at the requested samples, not between samples.
DVAPI Plan planBake(const std::vector<TDoubleParamP> &curves, const Range &range,
                   const Cancel &cancel = Cancel());

// Simplify selected interior keys in linear, unstepped spans only. Other
// interpolation types, unselected keys, range endpoints and driven boundaries
// stay intact. Error is checked at every original key and requested sample in
// display units against an immutable original, not against successive edits.
DVAPI Plan planReduce(const std::vector<Selection> &selection,
                     const Range &range, double tolerance,
                     const Cancel &cancel = Cancel());

// Replace the target keys with a native numeric preset, adapting its duration
// and amplitude around its first value. Formula/file presets must first be
// baked explicitly; importing foreign channel bindings is never implicit.
DVAPI Plan planPreset(TDoubleParam *target, const TDoubleParam *preset,
                     double start, double duration, double amplitude,
                     double displayOffset);
DVAPI Plan planRestore(
    const std::vector<std::pair<TDoubleParamP, TDoubleParamP>> &snapshots);

DVAPI bool matchesSnapshot(const TDoubleParam *curve,
                          const TDoubleParam *snapshot);
DVAPI bool apply(const Plan &plan, const QString &history, QString &error);
}  // namespace FunctionCurveOperations
#endif
