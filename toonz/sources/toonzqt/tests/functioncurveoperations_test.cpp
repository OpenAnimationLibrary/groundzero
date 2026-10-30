#include "toonzqt/functioncurveoperations.h"
#include "tdoublekeyframe.h"
#include "tenv.h"
#include "texpression.h"
#include "tstream.h"
#include "tthread.h"
#include "tundo.h"
#include "tunit.h"
#include "toonz/txsheet.h"
#include "toonz/tstageobjecttree.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace Ops = FunctionCurveOperations;
namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void requireNear(double actual, double expected, double tolerance = 1e-10) {
  require(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
          "native curve differs from independent expected value");
}
TDoubleParamP curve(std::initializer_list<std::pair<double, double>> values) {
  TDoubleParamP result = new TDoubleParam;
  for (const auto &entry : values)
    result->setKeyframe(TDoubleKeyframe(entry.first, entry.second));
  return result;
}
Ops::Selection all(const TDoubleParamP &value) {
  Ops::Selection selected{value, {}};
  for (int i = 0; i < value->getKeyframeCount(); ++i) selected.indices.insert(i);
  return selected;
}
void apply(const Ops::Plan &plan) {
  QString error;
  require(plan.valid(), qPrintable(plan.error));
  require(Ops::apply(plan, "Fixture curve edit", error), qPrintable(error));
}
void numericUnitsAndTransactions() {
  auto *measure = new TMeasure("function_test_affine",
                               new TUnit(L"internal", new TSimpleUnitConverter));
  auto *display = new TUnit(L"display", new TSimpleUnitConverter(2, 10));
  measure->add(display);
  measure->setCurrentUnit(display);
  TMeasureManager::instance()->add(measure);
  auto a = curve({{0, 5}, {3, 9}, {8, 14}});
  a->setMeasureName("function_test_affine");
  auto b = curve({{1, -3}, {4, 7}});
  b->setMeasureName("function_test_affine");
  const auto beforeA = TDoubleParamP(new TDoubleParam(*a));
  const auto beforeB = TDoubleParamP(new TDoubleParam(*b));
  TUndoManager::manager()->reset();
  const auto offset = Ops::planValues({{a, {0, 2}}, {b, {1}}, {a, {2}}},
                                    Ops::ValueMode::Offset, 4);
  require(offset.valid() && offset.changes.size() == 2, "duplicate channel selection was not merged");
  require(Ops::matchesSnapshot(a.getPointer(), beforeA.getPointer()), "planning changed first source");
  require(Ops::matchesSnapshot(b.getPointer(), beforeB.getPointer()), "planning changed second source");
  apply(offset);
  requireNear(a->getKeyframe(0).m_value, 7);
  requireNear(a->getKeyframe(1).m_value, 9);
  requireNear(a->getKeyframe(2).m_value, 16);
  requireNear(b->getKeyframe(1).m_value, 9);
  require(TUndoManager::manager()->getHistoryCount() == 1, "one operation produced multiple undo entries");
  require(TUndoManager::manager()->undo(), "batch undo unavailable");
  require(Ops::matchesSnapshot(a.getPointer(), beforeA.getPointer()) &&
              Ops::matchesSnapshot(b.getPointer(), beforeB.getPointer()),
          "undo did not restore every channel");
  require(TUndoManager::manager()->redo(), "batch redo unavailable");
  requireNear(a->getKeyframe(0).m_value, 7);
  // Preview buffers belong to the caller; later UI reuse must not alter redo.
  for (const auto &change : offset.changes)
    if (change.curve.getPointer() == a.getPointer()) change.after->setValue(0, 999);
  require(TUndoManager::manager()->undo() && TUndoManager::manager()->redo(),
          "snapshot isolation undo/redo unavailable");
  requireNear(a->getKeyframe(0).m_value, 7);
  apply(Ops::planValues({{a, {0, 2}}}, Ops::ValueMode::Set, 28));
  requireNear(a->getKeyframe(0).m_value, 9);
  requireNear(a->getKeyframe(2).m_value, 9);
  const int histories = TUndoManager::manager()->getHistoryCount();
  apply(Ops::planValues({{a, {0, 2}}}, Ops::ValueMode::Set, 28));
  require(TUndoManager::manager()->getHistoryCount() == histories, "unchanged Apply added an undo");
  auto stale = Ops::planValues({all(a), all(b)}, Ops::ValueMode::Set, 20);
  b->setValue(1, 42);
  const auto current = TDoubleParamP(new TDoubleParam(*a));
  QString error;
  require(!Ops::apply(stale, "stale", error) && !error.isEmpty(), "stale plan applied");
  require(Ops::matchesSnapshot(a.getPointer(), current.getPointer()), "stale second channel caused a partial first-channel edit");
  auto locked = Ops::planValues({all(a), all(b)}, Ops::ValueMode::Set, 20);
  b->setEditorLocked(true);
  require(!Ops::apply(locked, "locked", error), "locked destination applied");
  require(Ops::matchesSnapshot(a.getPointer(), current.getPointer()), "locked second destination caused partial mutation");
  b->setEditorLocked(false);
  const auto oldUnits = Ops::planValues({all(a)}, Ops::ValueMode::Offset, 2);
  measure->setCurrentUnit(measure->getUnit(L"internal"));
  require(!Ops::apply(oldUnits, "changed units", error),
          "a preview was applied after display-unit reinterpretation");
  measure->setCurrentUnit(display);
  a->setValueRange(0, 10);
  require(!Ops::planValues({all(a)}, Ops::ValueMode::Set, 100).valid(), "numeric limit ignored");
  require(!Ops::planValues({{a, {50}}}, Ops::ValueMode::Offset, 1).valid(), "stale key index accepted");
  require(!Ops::planValues({all(a)}, Ops::ValueMode::Set,
                           std::numeric_limits<double>::quiet_NaN()).valid(),
          "NaN numeric input accepted");
  auto discrete = curve({{0, 0}, {5, 1}});
  discrete->setUIChoices({"first", "second"});
  require(!Ops::planValues({all(discrete)}, Ops::ValueMode::Set, .5).valid(),
          "discrete channel accepted a fractional choice");
  auto fixed = curve({{0, 3}, {2, 3}});
  fixed->setValueRange(3, 3);
  require(!Ops::planValues({all(fixed)}, Ops::ValueMode::Set, 4).valid(),
          "single-value numeric range was ignored");
}

void timingAndDrivenKeys() {
  TXsheet sheet;
  auto value = curve({{0, 0}, {2, 2}, {4, 4}, {10, 10}});
  auto key = value->getKeyframe(1);
  key.m_type = TDoubleKeyframe::SpeedInOut;
  key.m_speedOut = TPointD(.5, .25);
  value->setKeyframe(1, key);
  key = value->getKeyframe(2);
  key.m_speedIn = TPointD(-.5, -.25);
  value->setKeyframe(2, key);
  const auto before = TDoubleParamP(new TDoubleParam(*value));
  require(!Ops::planFrameOffset({{value, {1}}}, 2).valid(), "colliding offset accepted");
  require(!Ops::planFrameOffset({{value, {1}}}, 4).valid(), "key crossing accepted");
  require(!Ops::planRetime({all(value)}, 0, 0).valid(), "zero time scale accepted");
  auto plan = Ops::planRetime({{value, {1, 2}}}, 2, 2);
  apply(plan);
  requireNear(value->keyframeIndexToFrame(1), 2);
  requireNear(value->keyframeIndexToFrame(2), 6);
  requireNear(value->keyframeIndexToFrame(3), 10);
  requireNear(value->getKeyframe(1).m_speedOut.x, 1);
  requireNear(value->getKeyframe(2).m_speedIn.x, -1);
  require(TUndoManager::manager()->undo(), "retime undo unavailable");
  require(Ops::matchesSnapshot(value.getPointer(), before.getPointer()), "retime undo lost tangent metadata");
  auto quantized = curve({{0, 0}, {1.2, 3}, {4.8, 7}, {10, 10}});
  const auto beforeQuantize = TDoubleParamP(new TDoubleParam(*quantized));
  require(!Ops::planQuantize({all(quantized)}, 0, 0).valid(), "zero quantize grid accepted");
  require(!Ops::planQuantize({{quantized, {1}}}, 0, 10).valid(), "quantization collided with an unselected key");
  apply(Ops::planQuantize({{quantized, {1, 2}}}, 0, 2));
  requireNear(quantized->keyframeIndexToFrame(1), 2); requireNear(quantized->keyframeIndexToFrame(2), 4);
  requireNear(quantized->keyframeIndexToFrame(0), 0); requireNear(quantized->keyframeIndexToFrame(3), 10);
  require(TUndoManager::manager()->undo() && Ops::matchesSnapshot(quantized.getPointer(), beforeQuantize.getPointer()), "quantize undo did not restore original timing");
  apply(Ops::planFrameOffset({{value, {1, 2}}}, .25));
  requireNear(value->keyframeIndexToFrame(1), 2.25);
  requireNear(value->keyframeIndexToFrame(2), 4.25);

  auto formula = curve({{0, 0}, {4, 8}});
  formula->setGrammar(sheet.getStageObjectTree()->getGrammar());
  key = formula->getKeyframe(0);
  key.m_type = TDoubleKeyframe::Expression;
  key.m_expressionText = TExpression::makeSeExprText("frame*2");
  formula->setKeyframe(0, key);
  require(!Ops::planValues({{formula, {0}}}, Ops::ValueMode::Set, 4).valid(),
          "formula key was silently changed to a numeric value");
  require(!Ops::planValues({{formula, {1}}}, Ops::ValueMode::Set, 4).valid(),
          "formula-driven endpoint was treated as a stored numeric value");
  auto moved = Ops::planFrameOffset({all(formula)}, 2);
  require(moved.valid() && moved.changes.front().after->getKeyframe(0).m_expressionText == key.m_expressionText,
          "retime discarded SeExpr source");
  require(moved.changes.front().after->getKeyframe(0).m_type == TDoubleKeyframe::Expression,
          "retime changed expression dialect/type");
  TUndoManager::manager()->reset();
}

void targetRangeTiming(const QString &directory) {
  TXsheet sheet;
  auto a = curve({{0, 0}, {10, 10}, {15, 15}, {20, 20}, {40, 40}});
  auto b = curve({{0, 0}, {12, 12}, {18, 18}, {40, 40}});
  a->setWorkspaceId("range-arm"); b->setWorkspaceId("range-head");
  a->enableCycle(true);
  auto key = a->getKeyframe(1);
  key.m_type = TDoubleKeyframe::SpeedInOut;
  key.m_speedOut = TPointD(2, 3); key.m_linkedHandles = false;
  a->setKeyframe(1, key);
  key = a->getKeyframe(2);
  key.m_type = TDoubleKeyframe::EaseInOut;
  key.m_speedIn = TPointD(-2, -3); key.m_speedOut = TPointD(1, 0);
  a->setKeyframe(2, key);
  key = a->getKeyframe(3); key.m_speedIn = TPointD(-1, 0);
  a->setKeyframe(3, key);
  b->setGrammar(sheet.getStageObjectTree()->getGrammar());
  key = b->getKeyframe(1); key.m_type = TDoubleKeyframe::Expression;
  key.m_expressionText = TExpression::makeSeExprText("frame*2");
  b->setKeyframe(1, key);
  const std::string expression = key.m_expressionText;
  const auto beforeA = TDoubleParamP(new TDoubleParam(*a));
  const auto beforeB = TDoubleParamP(new TDoubleParam(*b));
  const std::vector<Ops::Selection> selected{{a, {1, 2, 3}}, {b, {1, 2}}};
  TUndoManager::manager()->reset();
  const auto plan = Ops::planRetimeToRange(selected, 11.25, 31.75);
  require(plan.valid() && plan.changes.size() == 2, qPrintable(plan.error));
  require(Ops::matchesSnapshot(a.getPointer(), beforeA.getPointer()) &&
              Ops::matchesSnapshot(b.getPointer(), beforeB.getPointer()) &&
              TUndoManager::manager()->getHistoryCount() == 0,
          "target-range preview mutated sources or undo history");
  auto canceled = plan; canceled.cancelled = true;
  QString error;
  require(!Ops::apply(canceled, "canceled range", error) &&
              TUndoManager::manager()->getHistoryCount() == 0,
          "canceled target range committed a transaction");
  apply(plan);
  // Independent numeric oracle: all channels share the source range 10..20.
  // The selected interior keys retain their 1/2, 1/5 and 4/5 positions.
  require(a->keyframeIndexToFrame(1) == 11.25 &&
              a->keyframeIndexToFrame(3) == 31.75,
          "target-range endpoints were rounded or reconstructed inexactly");
  requireNear(a->keyframeIndexToFrame(2), 21.5);
  requireNear(b->keyframeIndexToFrame(1), 15.35);
  requireNear(b->keyframeIndexToFrame(2), 27.65);
  requireNear(a->getKeyframe(1).m_speedOut.x, 4.1);
  requireNear(a->getKeyframe(2).m_speedIn.x, -4.1);
  requireNear(a->getKeyframe(2).m_speedOut.x, 2.05);
  requireNear(a->getKeyframe(3).m_speedIn.x, -2.05);
  requireNear(a->getKeyframe(1).m_speedOut.y, 3);
  require(!a->getKeyframe(1).m_linkedHandles && a->isCycleEnabled(),
          "target range changed handle linkage or curve cycle metadata");
  for (const auto &value : {a, b}) {
    requireNear(value->keyframeIndexToFrame(0), 0);
    requireNear(value->keyframeIndexToFrame(value->getKeyframeCount() - 1), 40);
  }
  require(b->getKeyframe(1).m_type == TDoubleKeyframe::Expression &&
              b->getKeyframe(1).m_expressionText == expression &&
              b->getKeyframe(1).m_value == beforeB->getKeyframe(1).m_value,
          "target retiming replaced an expression or its stored source value");
  require(TUndoManager::manager()->getHistoryCount() == 1,
          "multi-channel range retime did not create exactly one undo");
  const auto afterA = TDoubleParamP(new TDoubleParam(*a));
  const auto afterB = TDoubleParamP(new TDoubleParam(*b));
  apply(Ops::planRetimeToRange(selected, 11.25, 31.75));
  require(TUndoManager::manager()->getHistoryCount() == 1 &&
              Ops::matchesSnapshot(a.getPointer(), afterA.getPointer()) &&
              Ops::matchesSnapshot(b.getPointer(), afterB.getPointer()),
          "repeated target Apply changed timing/handles or added an undo");
  for (int repeat = 0; repeat < 2; ++repeat) {
    require(TUndoManager::manager()->undo() &&
                Ops::matchesSnapshot(a.getPointer(), beforeA.getPointer()) &&
                Ops::matchesSnapshot(b.getPointer(), beforeB.getPointer()),
            "range undo lost timing, driven source or tangent metadata");
    require(TUndoManager::manager()->redo() &&
                Ops::matchesSnapshot(a.getPointer(), afterA.getPointer()) &&
                Ops::matchesSnapshot(b.getPointer(), afterB.getPointer()),
            "range redo differs from the applied snapshot");
  }
  require(a->getWorkspaceId() == "range-arm" &&
              b->getWorkspaceId() == "range-head" &&
              !a->isEditorLocked() && !b->isEditorLocked(),
          "range transactions changed destination identities or session locks");
  auto locked = Ops::planRetimeToRange(selected, 12, 32);
  b->setEditorLocked(true);
  require(!Ops::apply(locked, "locked range", error) &&
              Ops::matchesSnapshot(a.getPointer(), afterA.getPointer()) &&
              Ops::matchesSnapshot(b.getPointer(), afterB.getPointer()) &&
              b->isEditorLocked(),
          "locked range destination caused partial mutation or lost its lock");
  b->setEditorLocked(false);
  auto stale = Ops::planRetimeToRange(selected, 12, 32);
  b->setDefaultValue(7);
  require(!Ops::apply(stale, "stale range", error) &&
              Ops::matchesSnapshot(a.getPointer(), afterA.getPointer()) &&
              b->getDefaultValue() == 7,
          "stale range destination caused partial mutation");
  b->copy(afterB.getPointer());
  require(!Ops::planRetimeToRange(selected, 0, 40).valid(),
          "range endpoint collision with an unselected key was accepted");
  require(!Ops::planRetimeToRange({{a, {1}}}, 25, 25).valid(),
          "range translation crossed an unselected interior key");
  for (const auto &bounds : {std::make_pair(9., 8.), std::make_pair(-1., 20.),
                             std::make_pair(9., 9.), std::make_pair(9., 9. + 1.e-9),
                             std::make_pair(0., std::numeric_limits<double>::infinity()),
                             std::make_pair(std::numeric_limits<double>::quiet_NaN(), 20.)})
    require(!Ops::planRetimeToRange(selected, bounds.first, bounds.second).valid(),
            "invalid, collapsed or indistinguishable target bounds were accepted");
  require(!Ops::planRetimeToRange({}, 1, 2).valid() &&
              !Ops::planRetimeToRange({{a, {99}}}, 1, 2).valid(),
          "empty/stale target-range selection was accepted");
  require(Ops::matchesSnapshot(a.getPointer(), afterA.getPointer()) &&
              Ops::matchesSnapshot(b.getPointer(), afterB.getPointer()) &&
              TUndoManager::manager()->getHistoryCount() == 1,
          "failed target ranges changed sources or undo history");
  const TFilePath file((directory + "/retimed-expression.xml").toStdWString());
  { TOStream output(file); b->saveData(output); }
  TDoubleParamP loaded = new TDoubleParam;
  loaded->setGrammar(sheet.getStageObjectTree()->getGrammar());
  { TIStream input(file); loaded->loadData(input); }
  require(loaded->getWorkspaceId() == "range-head" &&
              loaded->getKeyframeCount() == 4 &&
              loaded->getKeyframe(1).m_expressionText == expression &&
              loaded->getKeyframe(1).m_type == TDoubleKeyframe::Expression,
          "retimed native scene data lost identity or driven source");
  requireNear(loaded->keyframeIndexToFrame(1), 15.35);
  requireNear(loaded->keyframeIndexToFrame(2), 27.65);
  TUndoManager::manager()->reset();

  auto pointA = curve({{0, 0}, {5, 5}, {10, 10}});
  auto pointB = curve({{5, 7}});
  const std::vector<Ops::Selection> points{{pointA, {1}}, {pointB, {0}}};
  require(!Ops::planRetimeToRange(points, 6, 8).valid(),
          "one-frame source selection was stretched into a fabricated range");
  apply(Ops::planRetimeToRange(points, 7.5, 7.5));
  requireNear(pointA->keyframeIndexToFrame(1), 7.5);
  requireNear(pointB->keyframeIndexToFrame(0), 7.5);
  require(TUndoManager::manager()->undo(), "point-range translation has no undo");
  requireNear(pointA->keyframeIndexToFrame(1), 5);
  requireNear(pointB->keyframeIndexToFrame(0), 5);
  TUndoManager::manager()->reset();

  auto precise = curve({{1000000000., 0}, {1000000000.125, 1},
                         {1000000000.5, 2}});
  apply(Ops::planRetimeToRange({all(precise)}, .123456789, .987654321));
  require(precise->keyframeIndexToFrame(0) == .123456789 &&
              precise->keyframeIndexToFrame(2) == .987654321,
          "large source coordinates lost exact fractional destination bounds");
  requireNear(precise->keyframeIndexToFrame(1), .339506172);
  precise->setDefaultValue(.12345678901234567);
  key = precise->getKeyframe(0);
  key.m_type = TDoubleKeyframe::SpeedInOut;
  key.m_value = .23456789012345678;
  key.m_speedOut = TPointD(.012345678901234567, .034567890123456789);
  precise->setKeyframe(0, key);
  key = precise->getKeyframe(1);
  key.m_speedIn = TPointD(-.012345678901234567, -.034567890123456789);
  precise->setKeyframe(1, key);
  for (const bool inheritedPrecision : {false, true}) {
    const TFilePath preciseFile((directory + "/retimed-precision.xml").toStdWString());
    {
      TOStream output(preciseFile);
      output.setFullPrecision(inheritedPrecision);
      precise->saveData(output);
      require(output.setFullPrecision(false) == inheritedPrecision,
              "native curve serialization leaked or disabled the caller's precision mode");
    }
    TDoubleParamP reloaded = new TDoubleParam;
    { TIStream input(preciseFile); reloaded->loadData(input); }
    require(reloaded->getDefaultValue() == precise->getDefaultValue() &&
                reloaded->getKeyframeCount() == 3,
            "fractional curve reload changed its default value or key count");
    for (int i = 0; i < 3; ++i)
      require(reloaded->keyframeIndexToFrame(i) == precise->keyframeIndexToFrame(i),
              "fractional target timing lost precision during native scene serialization");
    require(reloaded->getKeyframe(0).m_value == precise->getKeyframe(0).m_value &&
                reloaded->getKeyframe(0).m_speedOut == precise->getKeyframe(0).m_speedOut &&
                reloaded->getKeyframe(1).m_speedIn == precise->getKeyframe(1).m_speedIn,
            "fractional curve reload rounded values or timing handles");
  }
  TUndoManager::manager()->reset();
}

void bakeSamplesCancellationAndFailures() {
  TXsheet sheet;
  auto formula = curve({{0, 0}, {4, 0}});
  formula->setGrammar(sheet.getStageObjectTree()->getGrammar());
  auto key = formula->getKeyframe(0);
  key.m_type = TDoubleKeyframe::Expression;
  key.m_expressionText = TExpression::makeSeExprText("frame*2");
  formula->setKeyframe(0, key);
  const auto before = TDoubleParamP(new TDoubleParam(*formula));
  const auto plan = Ops::planBake({formula}, {0, 4, 1.5});
  require(plan.valid() && plan.sampleCount == 4, qPrintable(plan.error));
  require(Ops::matchesSnapshot(formula.getPointer(), before.getPointer()), "bake preview changed formula source");
  auto baked = plan.changes.front().after;
  require(baked->getKeyframeCount() == 4, "inclusive terminal sample missing");
  for (double frame : {0.0, 1.5, 3.0, 4.0}) requireNear(baked->getValue(frame), (frame + 1) * 2);
  apply(plan);
  requireNear(formula->getValue(1.5), 5);
  require(TUndoManager::manager()->undo(), "bake undo unavailable");
  require(Ops::matchesSnapshot(formula.getPointer(), before.getPointer()), "bake undo lost source or metadata");
  int calls = 0;
  auto canceled = Ops::planBake({formula}, {0, 4, .5}, [&] { return ++calls == 4; });
  require(canceled.cancelled && canceled.changes.empty(), "cancelled bake exposed applicable partial changes");
  require(Ops::matchesSnapshot(formula.getPointer(), before.getPointer()), "cancelled bake changed source");
  require(!Ops::planBake({formula}, {0, 4, 0}).valid(), "zero bake sample step accepted");
  require(!Ops::planBake({formula}, {0, 4, .000001}).valid(), "unbounded bake sample count accepted");
  require(!Ops::planBake({formula}, {1, 3, 1}).valid(), "partial driven segment was silently changed outside range");
  key.m_expressionText = TExpression::makeSeExprText("unknown_function(frame)");
  formula->setKeyframe(0, key);
  require(!Ops::planBake({formula}, {0, 4, 1}).valid(), "invalid SeExpr fallback was baked as successful values");

  auto outside = curve({{0, 0}, {2, 4}, {4, 8}, {8, 0}});
  auto bounded = Ops::planBake({outside}, {2, 4, .5});
  require(bounded.valid(), qPrintable(bounded.error));
  auto candidate = bounded.changes.front().after;
  requireNear(candidate->getValue(1), 2);
  requireNear(candidate->getValue(6), 4);
  require(candidate->getKeyframe(0).m_frame == 0 &&
              candidate->getKeyframe(candidate->getKeyframeCount() - 1).m_frame == 8,
          "partial bake removed out-of-range keys");
  auto cyclic = curve({{0, 0}, {2, 2}});
  cyclic->enableCycle(true);
  require(!Ops::planBake({cyclic}, {0, 4, 1}).valid(),
          "bake silently changed a cyclic curve's period");
  TUndoManager::manager()->reset();
}

void reductionOracleAndPreservedBoundaries() {
  auto value = curve({{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 4}, {6, 4}, {7, 9}});
  auto key = value->getKeyframe(4);
  key.m_type = TDoubleKeyframe::Constant;
  value->setKeyframe(4, key);
  const auto before = TDoubleParamP(new TDoubleParam(*value));
  auto plan = Ops::planReduce({all(value)}, {0, 7, .25}, 0);
  require(plan.valid() && plan.removedKeys > 0, qPrintable(plan.error));
  auto after = plan.changes.front().after;
  require(after->isKeyframe(4) && after->isKeyframe(5), "hold boundary removed");
  for (int i = 0; i <= 28; ++i) requireNear(after->getValue(i * .25), before->getValue(i * .25), 0);
  require(Ops::matchesSnapshot(value.getPointer(), before.getPointer()), "key reduction preview changed source");
  apply(plan);
  require(TUndoManager::manager()->undo(), "key reduction undo unavailable");
  require(Ops::matchesSnapshot(value.getPointer(), before.getPointer()), "key reduction undo lost retained metadata");
  auto selective = Ops::planReduce({{value, {1, 2}}}, {0, 7, .25}, 0);
  require(selective.valid() && selective.changes.front().after->isKeyframe(3), "unselected key removed");
  auto ranged = Ops::planReduce({all(value)}, {1, 3, .25}, 0);
  require(ranged.valid() && ranged.changes.front().after->isKeyframe(1) &&
              ranged.changes.front().after->isKeyframe(3) &&
              ranged.changes.front().after->isKeyframe(0), "range boundary/outside key removed");
  auto peak = curve({{0, 0}, {1, .4}, {2, 0}, {3, 0}});
  auto reduced = Ops::planReduce({all(peak)}, {0, 3, .25}, .5);
  require(reduced.valid() && reduced.maximumError <= .5, "reduction exceeded tolerance");
  auto strict = Ops::planReduce({all(peak)}, {0, 3, .25}, .1);
  require(strict.valid() && (strict.changes.empty() || strict.changes.front().after->isKeyframe(1)),
          "narrow keyed peak disappeared under strict tolerance");
  int checks = 0;
  auto canceled = Ops::planReduce({all(peak)}, {0, 3, .25}, 1, [&] { return ++checks >= 2; });
  require(canceled.cancelled && canceled.changes.empty(), "cancelled reduction exposed partial edits");
}

void presetAndBreakdown() {
  auto preset = curve({{0, 10}, {2, 20}, {4, 10}});
  auto target = curve({{8, 5}, {9, 7}});
  auto plan = Ops::planPreset(target.getPointer(), preset.getPointer(), 20, 8, 2, 3);
  require(plan.valid(), qPrintable(plan.error));
  auto adapted = plan.changes.front().after;
  requireNear(adapted->getKeyframe(0).m_frame, 20);
  requireNear(adapted->getKeyframe(1).m_frame, 24);
  requireNear(adapted->getKeyframe(2).m_frame, 28);
  requireNear(adapted->getKeyframe(0).m_value, 13);
  requireNear(adapted->getKeyframe(1).m_value, 33);
  requireNear(target->getKeyframe(0).m_frame, 8);
  auto source = curve({{0, 0}, {10, 10}});
  const int histories = TUndoManager::manager()->getHistoryCount();
  auto breakdown = Ops::planBreakdown({all(source)}, 5, 1.5);
  require(breakdown.valid(), qPrintable(breakdown.error));
  require(source->getKeyframeCount() == 2, "breakdown preview inserted source key");
  requireNear(breakdown.changes.front().after->getValue(5), 15);
  require(TUndoManager::manager()->getHistoryCount() == histories, "breakdown preview polluted undo");
  apply(breakdown);
  requireNear(source->getValue(5), 15);
  require(!Ops::planBreakdown({all(source)}, 5, .5).valid(), "occupied breakdown frame accepted");
  auto key = preset->getKeyframe(0);
  key.m_type = TDoubleKeyframe::Expression;
  key.m_expressionText = TExpression::makeSeExprText("frame");
  preset->setKeyframe(0, key);
  require(!Ops::planPreset(target.getPointer(), preset.getPointer(), 20, 8, 2, 3).valid(),
          "foreign expression bindings were implicitly imported as a motion preset");
  require(!Ops::planBreakdown({all(preset)}, 1, .5).valid(), "driven breakdown accepted");
}

void identityAndPersistence(const QString &directory) {
  TDoubleParamP untouched = new TDoubleParam;
  require(untouched->isDefault() && !untouched->hasDataToSave(),
          "untouched default curve acquired persistent data");
  untouched->setEditorLocked(true);
  require(!untouched->hasDataToSave(), "session-only lock changed serialization");
  untouched->setWorkspaceId("fixture-unkeyed-channel");
  require(untouched->isDefault() && untouched->hasDataToSave(),
          "workspace metadata changed value semantics or was omitted from persistence");
  const TFilePath unkeyedPath((directory + "/unkeyed-curve.xml").toStdWString());
  {
    TOStream output(unkeyedPath);
    untouched->saveData(output);
  }
  TDoubleParamP unkeyedReopened = new TDoubleParam;
  {
    TIStream input(unkeyedPath);
    unkeyedReopened->loadData(input);
  }
  require(unkeyedReopened->isDefault() &&
              unkeyedReopened->getKeyframeCount() == 0 &&
              unkeyedReopened->getWorkspaceId() == "fixture-unkeyed-channel" &&
              !unkeyedReopened->isEditorLocked(),
          "unkeyed identity roundtrip changed values, keys, identity or session lock");
  untouched->setWorkspaceId("");
  require(!untouched->hasDataToSave(), "cleared identity retained serialization data");
  auto value = curve({{0, 2}, {4, 8}});
  value->setWorkspaceId("fixture-stable-channel");
  TDoubleParamP duplicate = new TDoubleParam(*value);
  require(duplicate->getWorkspaceId().empty(), "cloned curve inherited another owner's workspace identity");
  const TFilePath path((directory + "/curve.xml").toStdWString());
  {
    TOStream output(path);
    value->saveData(output);
  }
  TDoubleParamP reopened = new TDoubleParam;
  {
    TIStream input(path);
    reopened->loadData(input);
  }
  require(reopened->getWorkspaceId() == value->getWorkspaceId(), "save/reopen lost workspace identity");
  require(reopened->getKeyframeCount() == value->getKeyframeCount(), "save/reopen lost animation");
  apply(Ops::planValues({all(value)}, Ops::ValueMode::Offset, 2));
  require(value->getWorkspaceId() == "fixture-stable-channel", "apply replaced destination identity");
  require(TUndoManager::manager()->undo(), "identity undo unavailable");
  require(value->getWorkspaceId() == "fixture-stable-channel", "undo replaced destination identity");
  QFile old(directory + "/old.xml");
  require(old.open(QIODevice::WriteOnly), "cannot write old default fixture");
  old.write("<default unexpectedOlderReaderAttribute=\"ignored\"> 3 </default>\n");
  old.close();
  TDoubleParamP legacy = new TDoubleParam;
  TIStream input(TFilePath((directory + "/old.xml").toStdWString()));
  legacy->loadData(input);
  requireNear(legacy->getDefaultValue(), 3);
  require(legacy->getWorkspaceId().empty(), "legacy default invented an identity");
}
}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication application(argc, argv);
  TThread::init();
  try {
    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary test profile unavailable");
    require(QDir().mkpath(temporary.filePath("stuff/config")) &&
                QDir().mkpath(temporary.filePath("stuff/profiles")),
            "native test profile folders unavailable");
    TEnv::setStuffDir(TFilePath(temporary.filePath("stuff").toStdWString()));
    numericUnitsAndTransactions();
    timingAndDrivenKeys();
    targetRangeTiming(temporary.path());
    bakeSamplesCancellationAndFailures();
    reductionOracleAndPreservedBoundaries();
    presetAndBreakdown();
    identityAndPersistence(temporary.path());
    TUndoManager::manager()->reset();
    std::cout << "Function curve operations, preview, undo and persistence passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
