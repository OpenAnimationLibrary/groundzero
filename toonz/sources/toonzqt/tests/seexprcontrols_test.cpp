#include "toonzqt/seexprcontrols.h"
#include "tseexpr.h"
#include "tseexprspectrum.h"

#include <QApplication>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

std::string bytes(const QString &text) { return text.toUtf8().toStdString(); }

QString literal(const QJsonValue &value) {
  if (value.isArray()) {
    const auto values = value.toArray();
    QStringList components;
    for (const auto component : values)
      components << QString::number(component.toDouble(), 'g', 17);
    return '[' + components.join(',') + ']';
  }
  return QString::number(value.toDouble(), 'g', 17);
}

TSeExpr::Value evaluate(const QString &source, const QJsonArray &controls = {},
                       TSeExpr::Type type = TSeExpr::Type::Scalar,
                       double u = 0.25) {
  std::vector<TSeExpr::Variable> variables{{"u", TSeExpr::Value::scalar(u)}};
  std::vector<TSeExpr::Function> functions;
  for (const auto item : controls) {
    const auto control = item.toObject();
    const auto name = bytes(control.value("name").toString());
    const auto controlType = control.value("type").toString();
    if (controlType == "curve" || controlType == "colorcurve") {
      const bool color = controlType == "colorcurve";
      QString expression = color ? "ccurve($position" : "curve($position";
      for (const auto entry : control.value("points").toArray()) {
        const auto point = entry.toArray();
        expression += ',' + literal(point[0]) + ',' + literal(point[1]) + ',' +
                      (point.size() == 3 ? literal(point[2]) : QString("1"));
      }
      expression += ')';
      auto program = std::make_shared<TSeExpr::Program>();
      TSeExpr::Options options;
      options.resultType = color ? TSeExpr::Type::Vector : TSeExpr::Type::Scalar;
      require(program->compile(bytes(expression), {{"position", TSeExpr::Value::scalar(u)}}, {}, options),
              "imported curve declaration did not compile");
      TSeExpr::Function function;
      function.name = name;
      function.argumentTypes = {TSeExpr::Type::Scalar};
      function.resultType = options.resultType;
      function.evaluate = [program](const std::vector<TSeExpr::Value> &args) {
        if (!program->setScalar("position", args[0].number[0]))
          throw std::runtime_error("curve position binding failed");
        TSeExpr::Value value;
        if (!program->evaluate(value)) throw std::runtime_error(program->errorMessage());
        return value;
      };
      functions.push_back(std::move(function));
    } else if (controlType == "string") {
      variables.push_back({name, TSeExpr::Value::string(bytes(control.value("value").toString()))});
    } else if (controlType == "vector" || controlType == "color") {
      const auto components = control.value("value").toArray();
      variables.push_back({name, TSeExpr::Value::vector(components[0].toDouble(),
          components[1].toDouble(), components[2].toDouble())});
    } else {
      variables.push_back({name, TSeExpr::Value::scalar(control.value("value").toDouble())});
    }
  }
  TSeExpr::Options options;
  options.resultType = type;
  TSeExpr::Program program;
  require(program.compile(bytes(source), variables, functions, options),
          "annotation formula did not compile in the real runtime");
  TSeExpr::Value result;
  require(program.evaluate(result), "annotation formula evaluation failed");
  return result;
}

void same(const TSeExpr::Value &a, const TSeExpr::Value &b) {
  require(a.type == b.type, "annotation import changed result type");
  if (a.type == TSeExpr::Type::String) {
    require(a.text == b.text, "annotation import changed a string value");
    return;
  }
  require(a.number.size() == b.number.size(), "annotation import changed result dimensions");
  for (std::size_t i = 0; i < a.number.size(); ++i)
    require(std::abs(a.number[i] - b.number[i]) < 1e-10,
            "annotation import changed an evaluated result");
}

QDialog *visibleDialog(const QString &name) {
  for (QWidget *widget : QApplication::topLevelWidgets())
    if (auto *dialog = qobject_cast<QDialog *>(widget))
      if (dialog->isVisible() && dialog->objectName() == name) return dialog;
  return nullptr;
}

bool click(QDialog *dialog, const QString &name, QDialogButtonBox::StandardButton button) {
  auto *buttons = dialog->findChild<QDialogButtonBox *>(name);
  if (!buttons || !buttons->button(button)) return false;
  buttons->button(button)->click();
  return true;
}

bool runDialog(const QString &name, const std::function<bool()> &run,
               const std::function<bool(QDialog *)> &action) {
  bool found = false, acted = false, timedOut = false;
  QTimer callback, watchdog;
  callback.setSingleShot(true); watchdog.setSingleShot(true);
  QObject::connect(&callback, &QTimer::timeout, [&] {
    auto *dialog = visibleDialog(name);
    found = dialog != nullptr;
    if (dialog) {
      acted = action(dialog);
      if (!acted) dialog->reject();
    }
  });
  QObject::connect(&watchdog, &QTimer::timeout, [&] {
    timedOut = true;
    for (QWidget *widget : QApplication::topLevelWidgets())
      if (auto *dialog = qobject_cast<QDialog *>(widget)) dialog->reject();
  });
  callback.start(0); watchdog.start(3000);
  const bool accepted = run();
  watchdog.stop();
  require(found && acted && !timedOut, "native control dialog interaction failed or timed out");
  return accepted;
}

void annotationCases() {
  const QString source =
      "$gain=2; #0,4\n"
      "$tint=[0.1,0.2,0.3]; #0.0,2.0\n"
      "$caption=\"é # $gain=7;\"; #string Caption\n"
      "$untouched=sin($u); # keep this computed assignment\n"
      "$gain*2+$tint[1]+$untouched";
  const auto imported = SeExprControls::importAnnotations(source, {}, "rgb");
  require(imported.diagnostics.isEmpty() && imported.controls.size() == 3,
          "literal controls and annotations were not imported");
  require(imported.source.contains("#0,4") &&
              imported.source.contains("# keep this computed assignment") &&
              imported.source.contains("$untouched=sin($u)"),
          "annotation import changed comments or computed expressions");
  require(imported.controls[0].toObject().value("integer").toBool(),
          "integer range annotation was lost");
  require(imported.controls[1].toObject().value("type").toString() == "vector",
          "numeric vector range was treated as a color swatch");
  require(imported.controls[2].toObject().value("value").toString() == "é # $gain=7;",
          "string annotation parsing changed literal punctuation or Unicode");
  same(evaluate(source), evaluate(imported.source, imported.controls));

  const QString repeated = "$a=1; $saved=$a; $a=2; $saved*10+$a";
  const auto repeatedImport = SeExprControls::importAnnotations(repeated, {}, "rgb");
  require(repeatedImport.controls.size() == 2 &&
              repeatedImport.controls[0].toObject().value("name") !=
                  repeatedImport.controls[1].toObject().value("name"),
          "repeated assignments collapsed into one native control");
  same(evaluate(repeated), evaluate(repeatedImport.source, repeatedImport.controls));
  const auto changedFirst = SeExprControls::importAnnotations(
      "$a=sin($u); $saved=$a; $a=2; $saved*10+$a", repeatedImport.controls, "rgb");
  require(changedFirst.controls[1].toObject().value("id") ==
              repeatedImport.controls[1].toObject().value("id"),
          "an unsupported earlier assignment shifted a later control identity");
  const auto alpha = SeExprControls::importAnnotations("$a=3; $a", repeatedImport.controls, "alpha");
  require(alpha.controls.size() == 3, "RGB and alpha annotations shared an unintended binding");
  const auto gainChanged = SeExprControls::importAnnotations(
      "$gain=3; #0,4\n$gain", imported.controls, "rgb");
  require(gainChanged.controls[0].toObject().value("id") ==
              imported.controls[0].toObject().value("id"),
          "changing a literal discarded its persisted native control identity");
  const QString collision = "_annot_a=7; $a=1; _annot_a+$a";
  const auto collisionImport = SeExprControls::importAnnotations(collision);
  same(evaluate(collision), evaluate(collisionImport.source, collisionImport.controls));

  const QString ramp = "$r=curve($u,0,0,0,0.25,0.5,2,0.75,0.8,4,1,1,1); $r";
  const auto rampImport = SeExprControls::importAnnotations(ramp);
  const auto stops = rampImport.controls[0].toObject().value("points").toArray();
  require(stops[0].toArray()[2].toInt() == 0 && stops[1].toArray()[2].toInt() == 2 &&
              stops[2].toArray()[2].toInt() == 4,
          "curve interpolation modes were not preserved");
  for (double u : {0.0, 0.1, 0.25, 0.5, 0.75, 1.0})
    same(evaluate(ramp, {}, TSeExpr::Type::Scalar, u),
         evaluate(rampImport.source, rampImport.controls, TSeExpr::Type::Scalar, u));
  const QString swatch = "$c=swatch($u,[1,0,0],[0,1,0],[0,0,1]); $c";
  const auto swatchImport = SeExprControls::importAnnotations(swatch);
  require(swatchImport.controls[0].toObject().value("swatchCount").toInt() == 3,
          "palette swatch cardinality was lost");
  for (double u : {-1.0, 0.0, 0.2, 1.0 / 3, 0.5, 2.0 / 3, 0.9, 1.0, 2.0})
    same(evaluate(swatch, {}, TSeExpr::Type::Vector, u),
         evaluate(swatchImport.source, swatchImport.controls, TSeExpr::Type::Vector, u));
  const QString outOfBounds = "$r=curve($u,-1,0,1,1,1,1); $r";
  require(SeExprControls::importAnnotations(outOfBounds).source == outOfBounds,
          "a curve outside native control bounds was silently changed");
}

void swatchCases() {
  const QJsonArray first{2.25, -0.5, 0.1234567890123};
  const QJsonArray second{0.1, 0.2, 0.3};
  QJsonObject control{{"id", "swatch_id"}, {"name", "swatches"},
      {"type", "colorcurve"}, {"presentation", "swatches"}, {"swatchCount", 2},
      {"points", QJsonArray{QJsonArray{0.0, first, 0}, QJsonArray{0.5, second, 0},
                             QJsonArray{1.0, second, 0}}}, {"customMetadata", "retain"}};
  const auto original = control;
  require(!runDialog("seExprSwatchDialog", [&] { return SeExprControls::editSwatches(nullptr, control); },
      [](QDialog *dialog) { return click(dialog, "seExprSwatchButtons", QDialogButtonBox::Cancel); }),
      "cancelled swatch edit was accepted");
  require(control == original, "Cancel changed persisted swatch controls");
  require(runDialog("seExprSwatchDialog", [&] { return SeExprControls::editSwatches(nullptr, control); },
      [](QDialog *dialog) { return click(dialog, "seExprSwatchButtons", QDialogButtonBox::Ok); }),
      "valid swatch edit could not be accepted");
  require(control == original, "opening and accepting swatches changed HDR colors or metadata");

  bool colorCancelled = false;
  require(runDialog("seExprSwatchDialog", [&] { return SeExprControls::editSwatches(nullptr, control); },
      [&](QDialog *dialog) {
        auto *table = dialog->findChild<QTableWidget *>("seExprSwatchTable");
        if (!table || table->rowCount() != 2) return false;
        QTimer::singleShot(0, [&] {
          for (QWidget *widget : QApplication::topLevelWidgets())
            if (auto *color = qobject_cast<QColorDialog *>(widget)) {
              if (color->isVisible()) { colorCancelled = true; color->reject(); }
            }
        });
        QMetaObject::invokeMethod(table, "cellDoubleClicked", Qt::DirectConnection,
                                 Q_ARG(int, 0), Q_ARG(int, 1));
        return click(dialog, "seExprSwatchButtons", QDialogButtonBox::Ok);
      }), "swatch color-picker cancellation broke the parent dialog");
  require(colorCancelled && control == original,
          "cancelling the real color picker changed a stored color");

  require(runDialog("seExprSwatchDialog", [&] { return SeExprControls::editSwatches(nullptr, control); },
      [](QDialog *dialog) {
        auto *table = dialog->findChild<QTableWidget *>("seExprSwatchTable");
        auto *down = dialog->findChild<QPushButton *>("seExprSwatchDown");
        auto *add = dialog->findChild<QPushButton *>("seExprSwatchAdd");
        auto *remove = dialog->findChild<QPushButton *>("seExprSwatchRemove");
        if (!table || !down || !add || !remove) return false;
        table->setCurrentCell(0, 1); down->click();
        add->click();
        if (table->rowCount() != 3) return false;
        table->setCurrentCell(2, 1); remove->click();
        return table->rowCount() == 2 && click(dialog, "seExprSwatchButtons", QDialogButtonBox::Ok);
      }), "native swatch add/remove/reorder failed");
  const auto points = control.value("points").toArray();
  require(points[0].toArray()[1] == second && points[1].toArray()[1] == first &&
              points[1].toArray()[0].toDouble() == 0.5 && points[2].toArray()[0].toDouble() == 1.0,
          "reordering palette colors changed their bins or lost values");
}

void spectrumCases() {
  TSeExprSpectrum::Parameters params;
  require(TSeExprSpectrum::sampleCount() == 1993, "upstream spectrum grid cardinality changed");
  const auto points = SeExprControls::spectrumPoints(SeExprControls::defaultSpectrumParameters(), 9);
  require(points.size() == 9, "requested spectrum preview samples missing");
  for (int i = 0; i < points.size(); ++i) {
    const auto point = points[i].toArray();
    const double position = point[0].toDouble();
    require(std::abs(point[1].toDouble() - TSeExprSpectrum::value(params, position)) < 1e-12,
            "spectrum preview differs from the native render utility");
  }
  // Independent expression of the upstream sampled spectrum at its centre.
  const double k = std::exp(995.0 * 0.01 - 5.0);
  const double peak = std::sqrt(2.0 / 6.0);
  const double length = 100.0 / 9.8;
  const auto power = [](double x) { return std::exp(-1.0 / (x * x)) / std::pow(x, 6.0); };
  const double expected = 0.95 / std::pow(power(peak), 0.1) *
      std::pow(power(k) * std::exp(-std::pow(k / length * 0.01, 2.0)), 0.1);
  require(std::abs(TSeExprSpectrum::value(params, 0.5) - expected) < 1e-12,
          "native spectrum no longer follows the upstream logarithmic grid");
  const auto grid = TSeExprSpectrum::gridRange(params);
  auto moreResolution = params; moreResolution.resolution = 12;
  require(grid[0] < grid[1] && TSeExprSpectrum::gridRange(moreResolution)[1] > grid[1],
          "spectrum resolution did not update the grid extent");
  params.amplitude = 0;
  require(TSeExprSpectrum::value(params, 0) == 0 && TSeExprSpectrum::value(params, 1) == 0,
          "zero-amplitude spectrum was not finite and empty");
  params.windSpeed = 0;
  require(!TSeExprSpectrum::valid(params), "invalid spectrum parameters were accepted");
  auto invalid = SeExprControls::defaultSpectrumParameters(); invalid["windSpeed"] = 0;
  require(SeExprControls::spectrumPoints(invalid).isEmpty(), "invalid spectrum produced a preview");

  auto metadata = SeExprControls::defaultSpectrumParameters();
  metadata["windAngle"] = -15.0; metadata["flowDirection"] = QJsonArray{1.0, 2.0, -3.0};
  metadata["time"] = 12.0; metadata["customMetadata"] = "retain";
  QJsonObject control{{"id", "spectrum_id"}, {"name", "water"}, {"spectrum", metadata}};
  const auto original = control;
  require(!runDialog("seExprSpectrumDialog", [&] { return SeExprControls::editSpectrum(nullptr, control); },
      [](QDialog *dialog) { return click(dialog, "seExprSpectrumButtons", QDialogButtonBox::Cancel); }),
      "cancelled spectrum edit was accepted");
  require(control == original, "Cancel changed spectrum metadata");
  require(runDialog("seExprSpectrumDialog", [&] { return SeExprControls::editSpectrum(nullptr, control); },
      [](QDialog *dialog) {
        auto *wind = dialog->findChild<QDoubleSpinBox *>("seExprSpectrum_windSpeed");
        auto *resolution = dialog->findChild<QDoubleSpinBox *>("seExprSpectrum_resolution");
        if (!wind || !resolution) return false;
        wind->setValue(15.0); resolution->setValue(12);
        return click(dialog, "seExprSpectrumButtons", QDialogButtonBox::Ok);
      }), "native spectrum edit failed");
  const auto saved = control.value("spectrum").toObject();
  require(saved.value("windSpeed").toDouble() == 15 && saved.value("resolution").toInt() == 12 &&
              saved.value("flowDirection") == metadata.value("flowDirection") &&
              saved.value("windAngle") == metadata.value("windAngle") &&
              saved.value("time") == metadata.value("time") &&
              saved.value("customMetadata").toString() == "retain",
          "spectrum editing lost unrelated parameters or metadata");
  require(control.value("points").toArray().size() == 64,
          "spectrum preview exceeded native curve control bounds");
}
}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication application(argc, argv);
  application.setQuitOnLastWindowClosed(false);
  try {
    annotationCases(); swatchCases(); spectrumCases();
    std::cout << "seexpr-controls: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "seexpr-controls: FAIL: " << error.what() << '\n';
    return 1;
  }
}
