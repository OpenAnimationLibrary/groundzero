#include "toonzqt/seexprrangeviewer.h"
#include "tdoubleparam.h"
#include "texpression.h"
#include "tseexpr.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"

#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void expectNear(double actual, double expected) {
  require(std::isfinite(actual) && std::abs(actual - expected) < 1e-10,
          "range sample differs from the native expression value");
}

void actualEvaluation() {
  TSeExpr::Program program;
  require(program.compile("[$frame,$frame*2,$frame*3]",
                          {{"frame", TSeExpr::Value::scalar(1)}}),
          "vector range formula did not compile");
  const auto evaluator = [&](double frame, std::vector<double> &values, QString &error) {
    if (!program.setScalar("frame", frame)) return false;
    TSeExpr::Value value;
    if (!program.evaluate(value)) { error = QString::fromStdString(program.errorMessage()); return false; }
    values = value.number;
    return true;
  };
  const auto result = SeExprRangeViewer::evaluate({1, 3, .5}, evaluator);
  require(result.error.isEmpty() && !result.cancelled && result.samples.size() == 5,
          "fractional-frame range did not contain the requested samples");
  for (int row = 0; row < result.samples.size(); ++row) {
    const auto &sample = result.samples[row];
    require(sample.error.isEmpty() && sample.values.size() == 3,
            "valid vector sample lost its numeric components");
    expectNear(sample.frame, 1 + row * .5);
    for (int component = 0; component < 3; ++component)
      expectNear(sample.values[component], sample.frame * (component + 1));
  }
  bool cancel = false;
  const auto partial = SeExprRangeViewer::evaluate({1, 8, 1}, evaluator,
      [&] { return cancel; }, [&](int completed, int) { if (completed == 2) cancel = true; });
  require(partial.cancelled && partial.samples.size() == 2,
          "cancellation did not retain only completed preview samples");

  int calls = 0;
  const auto bounded = [&](double, std::vector<double> &, QString &) { ++calls; return true; };
  require(!SeExprRangeViewer::evaluate({1, 600, 1}, bounded).error.isEmpty() && calls == 0,
          "oversized range invoked the host before rejecting its sample count");
  require(!SeExprRangeViewer::evaluate({2, 1, 1}, bounded).error.isEmpty() && calls == 0,
          "decreasing range was not rejected before host evaluation");
  require(!SeExprRangeViewer::evaluate({1, 2, 0}, bounded).error.isEmpty() && calls == 0,
          "zero step was not rejected before host evaluation");
  require(!SeExprRangeViewer::evaluate({1, 2, std::numeric_limits<double>::quiet_NaN()}, bounded)
              .error.isEmpty(), "nonfinite range input was accepted");

  const auto failures = SeExprRangeViewer::evaluate({1, 3, 1},
      [](double frame, std::vector<double> &values, QString &error) {
        if (frame == 2) { error = "Fixture invalid frame"; return false; }
        values = {frame}; return true;
      });
  require(failures.samples.size() == 3 && failures.samples[0].error.isEmpty() &&
              failures.samples[1].error == "Fixture invalid frame" &&
              failures.samples[1].values.empty() && failures.samples[2].error.isEmpty(),
          "failed-frame preview omitted diagnostics or discarded later valid frames");
  const auto nonfinite = SeExprRangeViewer::evaluate({1, 1, 1},
      [](double, std::vector<double> &values, QString &) {
        values = {std::numeric_limits<double>::infinity()}; return true;
      });
  require(!nonfinite.samples[0].error.isEmpty() && nonfinite.samples[0].values.empty(),
          "nonfinite graph values were accepted");
}

void nativeReferences() {
  TXsheet sheet;
  auto *horizontal = sheet.getStageObject(TStageObjectId::ColumnId(0))
                         ->getParam(TStageObject::T_X);
  horizontal->setMeasureName("");
  horizontal->setValue(0, 3);
  horizontal->setValue(1, 5);
  const int keys = horizontal->getKeyframeCount();
  TExpression draft;
  draft.setGrammar(sheet.getStageObjectTree()->getGrammar());
  draft.setText(TExpression::makeSeExprText("channel(\"col1.x\")*2"));
  require(draft.isValid(), "native reference range draft did not bind");
  const auto result = SeExprRangeViewer::evaluate({1, 2, 1},
      [&](double frame, std::vector<double> &values, QString &error) {
        double value = 0;
        if (!draft.evaluate(frame - 1, frame, frame, nullptr, value)) {
          error = QString::fromStdString(draft.getError()); return false;
        }
        values = {value}; return true;
      });
  require(result.samples.size() == 2 && result.samples[0].error.isEmpty() &&
              result.samples[1].error.isEmpty(), "native range reference failed");
  expectNear(result.samples[0].values[0], 6);
  expectNear(result.samples[1].values[0], 10);
  require(horizontal->getKeyframeCount() == keys,
          "range preview changed native keyframe ownership");
  expectNear(horizontal->getValue(0), 3);
  expectNear(horizontal->getValue(1), 5);
}

void actualDialog() {
  bool acted = false, timedOut = false;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog || dialog->objectName() != "seExprRangeDialog") return;
    auto *from = dialog->findChild<QDoubleSpinBox *>("seExprRangeStart");
    auto *to = dialog->findChild<QDoubleSpinBox *>("seExprRangeEnd");
    auto *step = dialog->findChild<QDoubleSpinBox *>("seExprRangeStep");
    auto *run = dialog->findChild<QPushButton *>("seExprRangeRun");
    auto *table = dialog->findChild<QTableWidget *>("seExprRangeSamples");
    auto *status = dialog->findChild<QLabel *>("seExprRangeStatus");
    if (!from || !to || !step || !run || !table || !status) { dialog->reject(); return; }
    from->setValue(1); to->setValue(3); step->setValue(1);
    run->click();
    acted = table->rowCount() == 3 && table->columnCount() == 3 &&
            table->item(1, 1) && table->item(1, 1)->text() == "4" &&
            status->text().contains("3");
    dialog->reject();
  });
  QTimer watchdog;
  watchdog.setSingleShot(true);
  QObject::connect(&watchdog, &QTimer::timeout, [&] {
    timedOut = true;
    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
  });
  watchdog.start(3000);
  SeExprRangeViewer::show(nullptr,
      [](double frame, std::vector<double> &values, QString &) {
        values = {frame * 2}; return true;
      });
  watchdog.stop();
  require(acted && !timedOut, "native range dialog did not display requested evaluated samples");
}

void ownerClosureDuringPreview() {
  QPointer<QWidget> owner = new QWidget;
  int evaluated = 0;
  QTimer::singleShot(0, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog || dialog->objectName() != "seExprRangeDialog") return;
    auto *run = dialog->findChild<QPushButton *>("seExprRangeRun");
    if (run) run->click();
  });
  QTimer watchdog;
  watchdog.setSingleShot(true);
  QObject::connect(&watchdog, &QTimer::timeout, [&] { if (owner) delete owner; });
  watchdog.start(3000);
  SeExprRangeViewer::show(owner,
      [&](double frame, std::vector<double> &values, QString &) {
        ++evaluated;
        values = {frame};
        if (evaluated == 1)
          QTimer::singleShot(0, [&] { if (owner) delete owner; });
        return true;
      }, 1, 48, 1);
  watchdog.stop();
  require(!owner && evaluated == 1,
          "closing the host did not cancel range evaluation at the event boundary");
}
}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication application(argc, argv);
  try {
    actualEvaluation(); nativeReferences(); actualDialog(); ownerClosureDuringPreview();
    std::cout << "seexpr_range: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "seexpr_range: FAIL: " << error.what() << '\n';
  }
  return 1;
}
