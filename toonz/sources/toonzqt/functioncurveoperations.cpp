#include "toonzqt/functioncurveoperations.h"
#include "toonzqt/functioncurvestate.h"
#include "toonz/doubleparamcmd.h"

#include "tdoublekeyframe.h"
#include "texpression.h"
#include "tseexpr.h"
#include "tundo.h"
#include "tunit.h"

#include <QObject>
#include <QCoreApplication>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>

namespace FunctionCurveOperations {
namespace {
constexpr size_t MaximumSamples = 100001;
constexpr size_t MaximumTotalSamples = 1000000;
constexpr size_t MaximumReductionWork = 5000000;
constexpr double FrameSeparation = 1e-8;

QString message(const char *source) {
  return QCoreApplication::translate("FunctionCurveOperations", source);
}

const TUnit *unit(const TDoubleParam *curve) {
  return curve && curve->getMeasure() ? curve->getMeasure()->getCurrentUnit()
                                    : nullptr;
}

double nativeValue(const TDoubleParam *curve, double value) {
  const TUnit *u = unit(curve);
  return u ? u->convertFrom(value) : value;
}

bool cancelled(Plan &plan, const Cancel &cancel) {
  if (cancel && cancel()) {
    plan.cancelled = true;
    plan.changes.clear();
    return true;
  }
  return false;
}

bool fail(Plan &plan, const QString &error) {
  plan.error = error;
  plan.changes.clear();
  return false;
}

Change snapshot(TDoubleParam *curve) {
  Change change;
  change.curve  = curve;
  change.before = new TDoubleParam(*curve);
  change.after  = new TDoubleParam(*curve);
  change.displayZero = displayValue(curve, 0);
  change.displayOne = displayValue(curve, 1);
  if (unit(curve))
    change.displayUnit = QString::fromStdWString(unit(curve)->getDefaultExtension());
  return change;
}

bool finiteFrame(double frame) {
  return std::isfinite(frame) &&
         frame > -double((std::numeric_limits<int>::max)()) &&
         frame < double((std::numeric_limits<int>::max)());
}

bool validValue(TDoubleParam *curve, double value) {
  if (!std::isfinite(value)) return false;
  double minimum = 0, maximum = 0, step = 0;
  curve->getValueRange(minimum, maximum, step);
  if (value < minimum || value > maximum) return false;
  const auto &choices = curve->getUIChoices();
  return choices.empty() ||
         (value >= 0 && value < double(choices.size()) &&
          value == std::floor(value));
}

bool finiteKey(const TDoubleKeyframe &key) {
  return finiteFrame(key.m_frame) && std::isfinite(key.m_value) &&
         std::isfinite(key.m_speedIn.x) && std::isfinite(key.m_speedIn.y) &&
         std::isfinite(key.m_speedOut.x) && std::isfinite(key.m_speedOut.y) &&
         std::isfinite(key.m_similarShapeOffset);
}

bool normalize(const std::vector<Selection> &input,
               std::vector<Selection> &output, Plan &plan) {
  std::map<TDoubleParam *, std::set<int>> merged;
  for (const auto &selection : input) {
    if (!selection.curve)
      return fail(plan, message("A selected channel is no longer available."));
    auto &indices = merged[selection.curve.getPointer()];
    for (int index : selection.indices) {
      if (index < 0 || index >= selection.curve->getKeyframeCount())
        return fail(plan, message("The key selection changed. Select the keys again."));
      indices.insert(index);
    }
  }
  for (const auto &entry : merged)
    if (!entry.second.empty()) output.push_back({entry.first, entry.second});
  if (output.empty()) return fail(plan, message("Select at least one keyframe."));
  return true;
}

bool samples(const Range &range, std::vector<double> &frames, Plan &plan) {
  if (!finiteFrame(range.first) || !finiteFrame(range.last) ||
      !std::isfinite(range.step) || range.step <= 0 || range.last < range.first)
    return fail(plan, message("Enter a finite increasing frame range and a positive sample step."));
  const double count = std::floor((range.last - range.first) / range.step);
  if (!std::isfinite(count) || count >= double(MaximumSamples))
    return fail(plan, message("The range exceeds 100001 samples. Increase the sample step."));
  for (size_t i = 0; i <= size_t(count); ++i) {
    const double frame = range.first + double(i) * range.step;
    if (frame > range.last) break;
    if (!frames.empty() && frame - frames.back() <= FrameSeparation)
      return fail(plan, message("The sample step is too small to produce distinct keyframes."));
    frames.push_back(frame);
  }
  if (frames.empty() || frames.back() != range.last) {
    if (!frames.empty() && range.last - frames.back() <= FrameSeparation)
      frames.back() = range.last;
    else
      frames.push_back(range.last);
  }
  if (frames.size() > MaximumSamples)
    return fail(plan, message("The range exceeds 100001 samples. Increase the sample step."));
  return true;
}

bool drivenEndpoint(TDoubleParam *curve, int index) {
  // Values at the end of an expression/file segment can come from its left
  // limit rather than from stored m_value. Editing it must never appear to
  // succeed while the evaluator continues to ignore the edited value.
  return !TDoubleKeyframe::isKeyframeBased(curve->getKeyframe(index).m_type) ||
         (index > 0 && !TDoubleKeyframe::isKeyframeBased(
                           curve->getKeyframe(index - 1).m_type));
}

QString effectiveUnit(const TDoubleParam *curve, const TDoubleKeyframe &key) {
  const TMeasure *measure = curve->getMeasure();
  if (!measure) return QString();
  const TUnit *resolved = key.m_unitName.empty()
                             ? nullptr
                             : measure->getUnit(QString::fromStdString(key.m_unitName)
                                                    .toStdWString());
  if (!resolved) resolved = measure->getCurrentUnit();
  return resolved ? QString::fromStdWString(resolved->getDefaultExtension())
                  : QString();
}

bool sameKey(const TDoubleParam *a, const TDoubleKeyframe &x,
             const TDoubleParam *b, const TDoubleKeyframe &y) {
  return x.m_frame == y.m_frame && x.m_value == y.m_value &&
         x.m_type == y.m_type && x.m_prevType == y.m_prevType &&
         x.m_isKeyframe == y.m_isKeyframe && x.m_step == y.m_step &&
         x.m_speedIn == y.m_speedIn && x.m_speedOut == y.m_speedOut &&
         x.m_linkedHandles == y.m_linkedHandles &&
         x.m_expressionText == y.m_expressionText &&
         x.m_fileParams.m_path == y.m_fileParams.m_path &&
         x.m_fileParams.m_fieldIndex == y.m_fileParams.m_fieldIndex &&
         x.m_similarShapeOffset == y.m_similarShapeOffset &&
         effectiveUnit(a, x) == effectiveUnit(b, y);
}

void appendIfChanged(Plan &plan, const Change &change) {
  if (!matchesSnapshot(change.before.getPointer(), change.after.getPointer()))
    plan.changes.push_back(change);
}

Plan timePlan(const std::vector<Selection> &input, double pivot, double scale,
              double offset) {
  Plan plan;
  if (!finiteFrame(pivot) || !std::isfinite(scale) || scale <= 0 ||
      !std::isfinite(offset)) {
    fail(plan, message("Enter a finite pivot and offset and a positive time scale."));
    return plan;
  }
  std::vector<Selection> selections;
  if (!normalize(input, selections, plan)) return plan;
  for (const auto &selection : selections) {
    Change change = snapshot(selection.curve.getPointer());
    TDoubleParam *source = change.before.getPointer();
    std::map<int, TDoubleKeyframe> keys;
    for (int i = 0; i < source->getKeyframeCount(); ++i) {
      auto key = source->getKeyframe(i);
      if (selection.indices.count(i))
        key.m_frame = pivot + (key.m_frame - pivot) * scale + offset;
      if (!finiteFrame(key.m_frame) ||
          (i && key.m_frame - keys.at(i - 1).m_frame <= FrameSeparation)) {
        fail(plan, message("The requested timing would collide with or cross another key. No keys were changed."));
        return plan;
      }
      keys.emplace(i, key);
    }
    // Keep tangent/ease lengths proportional to each changed segment. The
    // neighboring endpoint may be unselected; its tangent is part of that
    // segment and is included in the same snapshot/undo.
    for (int i = 0; i + 1 < source->getKeyframeCount(); ++i) {
      const auto &old = source->getKeyframe(i);
      if (old.m_type != TDoubleKeyframe::SpeedInOut &&
          old.m_type != TDoubleKeyframe::EaseInOut)
        continue;
      const double ratio = (keys.at(i + 1).m_frame - keys.at(i).m_frame) /
                           (source->keyframeIndexToFrame(i + 1) - old.m_frame);
      keys.at(i).m_speedOut.x *= ratio;
      keys.at(i + 1).m_speedIn.x *= ratio;
      if (!std::isfinite(keys.at(i).m_speedOut.x) ||
          !std::isfinite(keys.at(i + 1).m_speedIn.x)) {
        fail(plan, message("The requested timing exceeds the numeric handle range."));
        return plan;
      }
    }
    change.after->setKeyframes(keys);
    appendIfChanged(plan, change);
  }
  return plan;
}

// TDoubleParam::getValue deliberately uses a safe fallback for invalid SeExpr
// during GUI painting. A bake must reject that failure instead of keying the
// fallback. Validate and evaluate the expressions independently with the same
// owner, grammar, units and native segment coordinates before using getValue.
bool validateExpressions(TDoubleParam *curve, double frame, QString &error,
                         std::map<int, std::unique_ptr<TExpression>> &cache) {
  const int count = curve->getKeyframeCount();
  if (count < 2) return true;
  double sample = frame;
  const double first = curve->keyframeIndexToFrame(0);
  const double last = curve->keyframeIndexToFrame(count - 1);
  if (sample < first) sample = first;
  if (curve->isCycleEnabled() && sample >= last && last > first)
    sample = first + std::fmod(sample - first, last - first);
  else if (sample > last)
    sample = last;
  int index = curve->getPrevKeyframe(sample);
  if (curve->isKeyframe(sample)) ++index;
  index = std::max(0, std::min(index, count - 2));
  const auto &key = curve->getKeyframe(index);
  if (key.m_type == TDoubleKeyframe::File) {
    QFileInfo file(key.m_fileParams.m_path.getQString());
    if (!file.isFile() || !file.isReadable() || key.m_fileParams.m_fieldIndex < 0) {
      error = message("The sampled file channel has no readable data file or valid field.");
      return false;
    }
    return true;
  }
  if (key.m_type != TDoubleKeyframe::Expression &&
      key.m_type != TDoubleKeyframe::SimilarShape)
    return true;
  auto &cached = cache[index];
  if (!cached) {
    cached = std::make_unique<TExpression>();
    cached->setGrammar(curve->getGrammar());
    cached->setOwnerParameter(curve);
    cached->setText(key.m_expressionText);
  }
  TExpression &expression = *cached;
  if (!expression.isValid()) {
    error = message("An expression cannot be baked: %1")
                .arg(QString::fromStdString(expression.getError()));
    return false;
  }
  const TMeasure *measure = curve->getMeasure();
  const TUnit *evaluationUnit = measure && !key.m_unitName.empty()
                                   ? measure->getUnit(QString::fromStdString(key.m_unitName)
                                                          .toStdWString())
                                   : nullptr;
  if (!evaluationUnit) evaluationUnit = unit(curve);
  const double end = curve->keyframeIndexToFrame(index + 1);
  if (key.m_type == TDoubleKeyframe::SimilarShape)
    sample += key.m_similarShapeOffset;
  const double t = (sample - key.m_frame) / (end - key.m_frame);
  double value = 0;
  if (!expression.evaluate(t, sample + 1, sample - key.m_frame + 1,
                           evaluationUnit, value) || !std::isfinite(value)) {
    error = message("Expression evaluation failed at frame %1: %2")
                .arg(sample + 1)
                .arg(QString::fromStdString(expression.getError()));
    return false;
  }
  if (key.m_type == TDoubleKeyframe::SimilarShape) {
    for (double endpoint : {key.m_frame + key.m_similarShapeOffset,
                             end + key.m_similarShapeOffset}) {
      if (!expression.evaluate((endpoint - key.m_frame) / (end - key.m_frame), endpoint + 1,
                               endpoint - key.m_frame + 1, evaluationUnit, value) ||
          !std::isfinite(value)) {
        error = message("A Similar Shape endpoint cannot be evaluated.");
        return false;
      }
    }
  }
  return true;
}

class CurveUndo final : public TUndo {
  std::vector<Change> m_changes;
  QString m_history;

 public:
  CurveUndo(std::vector<Change> changes, QString history)
      : m_changes(std::move(changes)), m_history(std::move(history)) {}
  void undo() const override {
    for (const auto &change : m_changes) change.curve->copy(change.before.getPointer());
  }
  void redo() const override {
    for (const auto &change : m_changes) change.curve->copy(change.after.getPointer());
  }
  int getSize() const override {
    TINT64 bytes = sizeof(*this);
    for (const auto &change : m_changes)
      for (const auto &curve : {change.before, change.after}) {
        bytes += sizeof(TDoubleParam);
        for (int i = 0; i < curve->getKeyframeCount(); ++i) {
          const auto &key = curve->getKeyframe(i);
          bytes += sizeof(TDoubleKeyframe) + key.m_expressionText.size() +
                   key.m_unitName.size() +
                   key.m_fileParams.m_path.getWideString().size() * sizeof(wchar_t);
        }
      }
    return clampSize(bytes);
  }
  QString getHistoryString() override { return m_history; }
};

template <class Build>
Plan guarded(Build build) {
  try {
    return build();
  } catch (const std::exception &) {
    Plan plan;
    fail(plan, message("The curve operation could not be evaluated. No keys were changed."));
    return plan;
  } catch (...) {
    Plan plan;
    fail(plan, message("The curve operation could not be evaluated. No keys were changed."));
    return plan;
  }
}
}  // namespace

double displayValue(const TDoubleParam *curve, double value) {
  const TUnit *u = unit(curve);
  return u ? u->convertTo(value) : value;
}

bool matchesSnapshot(const TDoubleParam *curve, const TDoubleParam *snapshot) {
  if (!curve || !snapshot || curve->getName() != snapshot->getName() ||
      curve->getGrammar() != snapshot->getGrammar() ||
      curve->getMeasureName() != snapshot->getMeasureName() ||
      curve->getDefaultValue() != snapshot->getDefaultValue() ||
      curve->isCycleEnabled() != snapshot->isCycleEnabled() ||
      curve->getUIChoices() != snapshot->getUIChoices() ||
      curve->isUICheckbox() != snapshot->isUICheckbox() ||
      curve->getKeyframeCount() != snapshot->getKeyframeCount())
    return false;
  double amin = 0, amax = 0, astep = 0, bmin = 0, bmax = 0, bstep = 0;
  if (curve->getValueRange(amin, amax, astep) !=
          snapshot->getValueRange(bmin, bmax, bstep) ||
      amin != bmin || amax != bmax)
    return false;
  amin = amax = bmin = bmax = 0;
  if (curve->getUISliderRange(amin, amax) != snapshot->getUISliderRange(bmin, bmax) ||
      amin != bmin || amax != bmax)
    return false;
  for (int i = 0; i < curve->getKeyframeCount(); ++i)
    if (!sameKey(curve, curve->getKeyframe(i), snapshot, snapshot->getKeyframe(i)))
      return false;
  return true;
}

static Plan buildValues(const std::vector<Selection> &input, ValueMode mode, double value) {
  Plan plan;
  if (!std::isfinite(value)) {
    fail(plan, message("Enter a finite numeric value."));
    return plan;
  }
  std::vector<Selection> selections;
  if (!normalize(input, selections, plan)) return plan;
  for (const auto &selection : selections) {
    Change change = snapshot(selection.curve.getPointer());
    std::map<int, TDoubleKeyframe> keys;
    for (int index : selection.indices) {
      if (drivenEndpoint(change.before.getPointer(), index)) {
        fail(plan, message("The selection includes expression, file or Similar Shape driven keys. Bake them before changing their numeric values."));
        return plan;
      }
      auto key = change.before->getKeyframe(index);
      const double requested = mode == ValueMode::Set
                                   ? value
                                   : displayValue(change.before.getPointer(), key.m_value) + value;
      key.m_value = nativeValue(change.before.getPointer(), requested);
      if (!validValue(change.before.getPointer(), key.m_value)) {
        fail(plan, message("A requested value is outside the channel's valid range or discrete choices."));
        return plan;
      }
      keys.emplace(index, key);
    }
    change.after->setKeyframes(keys);
    appendIfChanged(plan, change);
  }
  return plan;
}

Plan planFrameOffset(const std::vector<Selection> &selection, double frames) {
  return guarded([&] { return timePlan(selection, 0, 1, frames); });
}

Plan planRetime(const std::vector<Selection> &selection, double pivot, double scale) {
  return guarded([&] { return timePlan(selection, pivot, scale, 0); });
}

static Plan buildBreakdown(const std::vector<Selection> &input, double frame,
                   double favour) {
  Plan plan;
  if (!finiteFrame(frame) || !std::isfinite(favour)) {
    fail(plan, message("Enter a finite breakdown frame and favour percentage."));
    return plan;
  }
  std::vector<Selection> selections;
  if (!normalize(input, selections, plan)) return plan;
  for (const auto &selection : selections) {
    Change change = snapshot(selection.curve.getPointer());
    auto *curve = change.before.getPointer();
    const int previous = curve->getPrevKeyframe(frame);
    const int next = curve->getNextKeyframe(frame);
    if (curve->isKeyframe(frame) || previous < 0 || next < 0 ||
        drivenEndpoint(curve, previous) || drivenEndpoint(curve, next)) {
      fail(plan, message("Each selected channel needs numeric keys before and after an empty breakdown frame. Driven keys must be baked first."));
      return plan;
    }
    const double from = curve->getValue(curve->keyframeIndexToFrame(previous));
    const double to = curve->getValue(curve->keyframeIndexToFrame(next));
    const double value = from + (to - from) * favour;
    if (!validValue(curve, value)) {
      fail(plan, message("The breakdown or overshoot exceeds a channel's valid numeric range."));
      return plan;
    }
    // Use native splitting rules, but never add a preview mutation to undo.
    KeyframeSetter setter(change.after.getPointer(), -1, false);
    setter.createKeyframe(frame);
    setter.setValue(value);
    appendIfChanged(plan, change);
  }
  return plan;
}

static Plan buildBake(const std::vector<TDoubleParamP> &input, const Range &range,
              const Cancel &cancel) {
  Plan plan;
  std::vector<double> frames;
  if (!samples(range, frames, plan)) return plan;
  std::set<TDoubleParam *> seen;
  for (const auto &curve : input) {
    if (cancelled(plan, cancel)) return plan;
    if (!curve) {
      fail(plan, message("A selected channel is no longer available."));
      return plan;
    }
    if (!seen.insert(curve.getPointer()).second) continue;
    if (plan.sampleCount + frames.size() > MaximumTotalSamples) {
      fail(plan, message("This bake exceeds one million samples. Bake fewer channels or increase the step."));
      return plan;
    }
    Change change = snapshot(curve.getPointer());
    TDoubleParamP evaluation = new TDoubleParam(*curve);
    std::map<int, std::unique_ptr<TExpression>> expressions;
    const int count = curve->getKeyframeCount();
    if (count) {
      const double first = curve->keyframeIndexToFrame(0);
      const double last = curve->keyframeIndexToFrame(count - 1);
      for (double bound : {range.first, range.last})
        if (bound > first && bound < last && !curve->isKeyframe(bound)) {
          fail(plan, message("For a partial bake, choose existing keyframes as range boundaries. This preserves the surrounding animation."));
          return plan;
        }
      if (curve->isCycleEnabled() && (range.first < first || range.last > last)) {
        fail(plan, message("Bake a cyclic curve within its existing first and last keys to preserve its cycle outside the range."));
        return plan;
      }
    }
    std::vector<TDoubleKeyframe> baked;
    for (double frame : frames) {
      if (cancelled(plan, cancel)) return plan;
      QString error;
      const auto failures = TSeExpr::failureCount();
      if (!validateExpressions(evaluation.getPointer(), frame, error, expressions)) {
        fail(plan, error);
        return plan;
      }
      const double value = evaluation->getValue(frame);
      if (TSeExpr::failureCount() != failures) {
        fail(plan, message("A referenced expression failed during sampling. Its fallback value was not baked."));
        return plan;
      }
      if (!validValue(curve.getPointer(), value)) {
        fail(plan, message("A sampled value is nonfinite or outside the channel's valid range."));
        return plan;
      }
      TDoubleKeyframe key(frame, value);
      key.m_isKeyframe = true;
      key.m_prevType = TDoubleKeyframe::Linear;
      key.m_linkedHandles = false;
      if (frame == range.first && curve->isKeyframe(frame)) {
        const int index = curve->getClosestKeyframe(frame);
        key.m_speedIn = curve->getKeyframe(index).m_speedIn;
        if (index > 0 && curve->getKeyframe(index - 1).m_type == TDoubleKeyframe::SpeedInOut)
          key.m_speedIn = curve->getSpeedIn(index);
      }
      if (frame == range.last && curve->isKeyframe(frame)) {
        const int index = curve->getClosestKeyframe(frame);
        key = curve->getKeyframe(index);
        key.m_value = value;
        // Freeze the boundary's outgoing handle before replacing its incoming
        // interpolation, otherwise linked handles can change the next segment.
        if (key.m_type == TDoubleKeyframe::SpeedInOut)
          key.m_speedOut = curve->getSpeedOut(index);
        key.m_linkedHandles = false;
      }
      baked.push_back(key);
      ++plan.sampleCount;
    }
    for (int i = count - 1; i >= 0; --i) {
      const double frame = change.after->keyframeIndexToFrame(i);
      if (frame >= range.first && frame <= range.last) change.after->deleteKeyframe(frame);
    }
    for (const auto &key : baked) change.after->setKeyframe(key);
    // Discontinuities next to a driven boundary can override stored values in
    // TDoubleParam. Refuse such a partial bake rather than advertise false
    // sample equivalence. A larger range resolves the ambiguity.
    for (size_t i = 0; i < frames.size(); ++i) {
      if (cancelled(plan, cancel)) return plan;
      const double actual = change.after->getValue(frames[i]);
      if (actual != baked[i].m_value) {
        fail(plan, message("The surrounding driven animation changes a baked boundary. Extend the bake range to include that segment."));
        return plan;
      }
    }
    appendIfChanged(plan, change);
  }
  if (seen.empty()) fail(plan, message("Select at least one channel to bake."));
  return plan;
}

static Plan buildReduce(const std::vector<Selection> &input, const Range &range,
                double tolerance, const Cancel &cancel) {
  Plan plan;
  if (!std::isfinite(tolerance) || tolerance < 0) {
    fail(plan, message("Enter a finite, nonnegative error tolerance."));
    return plan;
  }
  std::vector<double> frames;
  if (!samples(range, frames, plan)) return plan;
  std::vector<Selection> selections;
  if (!normalize(input, selections, plan)) return plan;
  size_t work = 0;
  for (const auto &selection : selections) {
    if (cancelled(plan, cancel)) return plan;
    Change change = snapshot(selection.curve.getPointer());
    TDoubleParam *source = change.before.getPointer();
    const int count = source->getKeyframeCount();
    if (source->isCycleEnabled() && count > 1 &&
        (range.first < source->keyframeIndexToFrame(0) ||
         range.last > source->keyframeIndexToFrame(count - 1))) {
      fail(plan, message("Reduce a cyclic curve within its existing first and last keys."));
      return plan;
    }
    std::vector<bool> protectedKeys(count, true), keep(count, true);
    for (int i = 1; i + 1 < count; ++i) {
      const auto &previous = source->getKeyframe(i - 1);
      const auto &key = source->getKeyframe(i);
      const auto &next = source->getKeyframe(i + 1);
      const bool eligible = selection.indices.count(i) &&
                            key.m_frame > range.first && key.m_frame < range.last &&
                            previous.m_type == TDoubleKeyframe::Linear &&
                            key.m_type == TDoubleKeyframe::Linear &&
                            previous.m_step == 1 && key.m_step == 1 &&
                            TDoubleKeyframe::isKeyframeBased(next.m_type) &&
                            (i < 2 || TDoubleKeyframe::isKeyframeBased(
                                          source->getKeyframe(i - 2).m_type));
      protectedKeys[i] = !eligible;
      keep[i] = !eligible;
    }
    int left = 0;
    while (left + 1 < count) {
      int right = left + 1;
      while (right < count - 1 && !protectedKeys[right]) ++right;
      std::vector<std::pair<int, int>> stack{{left, right}};
      while (!stack.empty()) {
        if (cancelled(plan, cancel)) return plan;
        const auto span = stack.back();
        stack.pop_back();
        const auto &a = source->getKeyframe(span.first);
        const auto &b = source->getKeyframe(span.second);
        double worst = tolerance;
        int split = -1;
        for (int i = span.first + 1; i < span.second; ++i) {
          if (++work > MaximumReductionWork) {
            fail(plan, message("The reduction is too complex. Select a smaller range."));
            return plan;
          }
          const auto &key = source->getKeyframe(i);
          const double t = (key.m_frame - a.m_frame) / (b.m_frame - a.m_frame);
          const double interpolated = (1 - t) * a.m_value + t * b.m_value;
          const double error = std::abs(displayValue(source, key.m_value) -
                                        displayValue(source, interpolated));
          if (!std::isfinite(error)) {
            fail(plan, message("A curve contains a nonfinite value."));
            return plan;
          }
          if (error > worst) { worst = error; split = i; }
        }
        if (split >= 0) {
          keep[split] = true;
          stack.push_back({span.first, split});
          stack.push_back({split, span.second});
        }
      }
      left = right;
    }
    for (int i = count - 1; i >= 0; --i)
      if (!keep[i]) {
        change.after->deleteKeyframe(source->keyframeIndexToFrame(i));
        ++plan.removedKeys;
      }
    std::set<double> oracle(frames.begin(), frames.end());
    for (int i = 0; i < count; ++i) {
      const double frame = source->keyframeIndexToFrame(i);
      if (frame >= range.first && frame <= range.last) oracle.insert(frame);
    }
    if (plan.sampleCount + oracle.size() > MaximumTotalSamples) {
      fail(plan, message("The reduction exceeds one million validation samples. Select fewer channels."));
      return plan;
    }
    std::map<int, std::unique_ptr<TExpression>> expressions;
    for (double frame : oracle) {
      if (cancelled(plan, cancel)) return plan;
      const auto failures = TSeExpr::failureCount();
      QString errorText;
      if (!validateExpressions(source, frame, errorText, expressions)) {
        fail(plan, errorText);
        return plan;
      }
      const double error = std::abs(displayValue(source, source->getValue(frame)) -
                                    displayValue(source, change.after->getValue(frame)));
      if (TSeExpr::failureCount() != failures || !std::isfinite(error) || error > tolerance) {
        fail(plan, message("The reduced curve exceeds the requested error tolerance. No keys were changed."));
        return plan;
      }
      plan.maximumError = std::max(plan.maximumError, error);
      ++plan.sampleCount;
    }
    appendIfChanged(plan, change);
  }
  return plan;
}

static Plan buildPreset(TDoubleParam *target, const TDoubleParam *preset, double start,
                double duration, double amplitude, double offset) {
  Plan plan;
  if (!target || !preset || !preset->hasKeyframes() || !finiteFrame(start) ||
      !std::isfinite(duration) || duration < 0 || !std::isfinite(amplitude) ||
      !std::isfinite(offset)) {
    fail(plan, message("Choose a numeric curve preset and finite timing and amplitude settings."));
    return plan;
  }
  if (target->getMeasureName() != preset->getMeasureName()) {
    fail(plan, message("The preset and target use different measures. Choose a compatible channel."));
    return plan;
  }
  const int count = preset->getKeyframeCount();
  const double first = preset->keyframeIndexToFrame(0);
  const double oldDuration = preset->keyframeIndexToFrame(count - 1) - first;
  if (count > 1 && (duration <= 0 || oldDuration <= 0)) {
    fail(plan, message("A multi-key preset requires a positive duration."));
    return plan;
  }
  const double scale = count > 1 ? duration / oldDuration : 1;
  Change change = snapshot(target);
  change.after->clearKeyframes();
  const double initial = preset->getKeyframe(0).m_value;
  const double nativeOffset = nativeValue(target, offset) - nativeValue(target, 0);
  double previousFrame = -std::numeric_limits<double>::infinity();
  for (int i = 0; i < count; ++i) {
    auto key = preset->getKeyframe(i);
    if (!TDoubleKeyframe::isKeyframeBased(key.m_type)) {
      fail(plan, message("Bake expression, file and Similar Shape presets before applying them to another channel."));
      return plan;
    }
    key.m_frame = start + (key.m_frame - first) * scale;
    key.m_value = initial + (key.m_value - initial) * amplitude + nativeOffset;
    if (!finiteFrame(key.m_frame) || key.m_frame - previousFrame <= FrameSeparation ||
        !validValue(target, key.m_value)) {
      fail(plan, message("The adapted preset exceeds the target's frame or value range."));
      return plan;
    }
    if (key.m_type == TDoubleKeyframe::SpeedInOut ||
        key.m_type == TDoubleKeyframe::EaseInOut)
      key.m_speedOut.x *= scale;
    if (i > 0 && (preset->getKeyframe(i - 1).m_type == TDoubleKeyframe::SpeedInOut ||
                  preset->getKeyframe(i - 1).m_type == TDoubleKeyframe::EaseInOut))
      key.m_speedIn.x *= scale;
    key.m_speedIn.y *= amplitude;
    key.m_speedOut.y *= amplitude;
    if (!finiteKey(key)) {
      fail(plan, message("The adapted preset exceeds the numeric handle range."));
      return plan;
    }
    change.after->setKeyframe(key);
    previousFrame = key.m_frame;
  }
  change.after->enableCycle(preset->isCycleEnabled());
  appendIfChanged(plan, change);
  return plan;
}

Plan planValues(const std::vector<Selection> &selection, ValueMode mode, double value) {
  return guarded([&] { return buildValues(selection, mode, value); });
}
Plan planBreakdown(const std::vector<Selection> &selection, double frame, double favour) {
  return guarded([&] { return buildBreakdown(selection, frame, favour); });
}
Plan planBake(const std::vector<TDoubleParamP> &curves, const Range &range,
              const Cancel &cancel) {
  return guarded([&] { return buildBake(curves, range, cancel); });
}
Plan planReduce(const std::vector<Selection> &selection, const Range &range,
                double tolerance, const Cancel &cancel) {
  return guarded([&] { return buildReduce(selection, range, tolerance, cancel); });
}
Plan planPreset(TDoubleParam *target, const TDoubleParam *preset, double start,
                double duration, double amplitude, double offset) {
  return guarded([&] { return buildPreset(target, preset, start, duration, amplitude, offset); });
}

Plan planRestore(const std::vector<std::pair<TDoubleParamP, TDoubleParamP>> &snapshots) {
  return guarded([&] {
    Plan plan;
    std::set<TDoubleParam *> seen;
    for (const auto &entry : snapshots) {
      if (!entry.first || !entry.second ||
          !seen.insert(entry.first.getPointer()).second ||
          entry.first->getGrammar() != entry.second->getGrammar()) {
        fail(plan, message("The comparison snapshot no longer belongs to these channels."));
        return plan;
      }
      Change change = snapshot(entry.first.getPointer());
      change.after = new TDoubleParam(*entry.second);
      appendIfChanged(plan, change);
    }
    return plan;
  });
}

bool apply(const Plan &plan, const QString &history, QString &error) {
  error.clear();
  if (!plan.valid()) {
    error = plan.cancelled ? message("The operation was cancelled.") : plan.error;
    return false;
  }
  std::set<TDoubleParam *> seen;
  for (const auto &change : plan.changes) {
    if (!change.curve || !change.before || !change.after ||
        !seen.insert(change.curve.getPointer()).second) {
      error = message("The curve edit plan is invalid.");
      return false;
    }
    if (FunctionCurveState::isLocked(change.curve.getPointer())) {
      error = message("A destination channel is locked. No channels were changed.");
      return false;
    }
    const TUnit *currentUnit = unit(change.curve.getPointer());
    const QString name = currentUnit
                             ? QString::fromStdWString(currentUnit->getDefaultExtension())
                             : QString();
    if (name != change.displayUnit ||
        displayValue(change.curve.getPointer(), 0) != change.displayZero ||
        displayValue(change.curve.getPointer(), 1) != change.displayOne) {
      error = message("The display units changed after the preview. Preview the edit again.");
      return false;
    }
    if (!matchesSnapshot(change.curve.getPointer(), change.before.getPointer())) {
      error = message("The animation changed after the preview. Preview the edit again.");
      return false;
    }
    double previous = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < change.after->getKeyframeCount(); ++i) {
      const auto &key = change.after->getKeyframe(i);
      if (!finiteKey(key) || key.m_frame - previous <= FrameSeparation) {
        error = message("The preview contains invalid keyframe positions or values.");
        return false;
      }
      previous = key.m_frame;
    }
  }
  if (plan.changes.empty()) return true;
  std::vector<Change> committed = plan.changes;
  for (auto &change : committed) {
    change.before = new TDoubleParam(*change.before);
    change.after = new TDoubleParam(*change.after);
  }
  auto undo = std::make_unique<CurveUndo>(std::move(committed), history);
  try {
    undo->redo();
  } catch (...) {
    try {
      undo->undo();
      error = message("The curve edit could not be applied. The original keys were restored.");
    } catch (...) {
      error = message("The curve edit and its restoration failed. Check the affected channels before saving.");
    }
    return false;
  }
  TUndoManager::manager()->add(undo.release());
  return true;
}
}  // namespace FunctionCurveOperations
