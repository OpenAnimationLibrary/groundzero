#include "toonzqt/seexprcontrols.h"
#include "tseexpr.h"
#include "tseexprspectrum.h"
#include "tseexpranimcurve.h"
#include "tdoublekeyframe.h"
#include "tdoubleparam.h"
#include "texpression.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"

#include <QApplication>
#include <QColorDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPixmap>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVariant>

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
  if (value.isString()) {
    const QString json = QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
    return json.mid(1, json.size() - 2);
  }
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
    if (controlType == "curve" || controlType == "colorcurve" || controlType == "animcurve") {
      const bool color = controlType == "colorcurve";
      QString expression = controlType == "animcurve" ? "animCurve($position" :
                           color ? "ccurve($position" : "curve($position";
      if (controlType == "animcurve") {
        expression += ',' + literal(control.value("preInfinity").toString("constant")) +
            ',' + literal(control.value("postInfinity").toString("constant")) +
            ',' + (control.value("weighted").toBool() ? "1" : "0") +
            ',' + literal(control.value("link").toString());
        for (const auto entry : control.value("keys").toArray()) {
          const auto key = entry.toObject();
          expression += ',' + literal(key.value("time")) + ',' + literal(key.value("value")) +
              ',' + literal(key.value("inWeight")) + ',' + literal(key.value("outWeight")) +
              ',' + literal(key.value("inAngle")) + ',' + literal(key.value("outAngle")) +
              ',' + literal(key.value("inTangent").toString("auto")) +
              ',' + literal(key.value("outTangent").toString("auto")) +
              ',' + (key.value("weightsLocked").toBool() ? "1" : "0");
        }
      } else {
        for (const auto entry : control.value("points").toArray()) {
          const auto point = entry.toArray();
          expression += ',' + literal(point[0]) + ',' + literal(point[1]) + ',' +
                        (point.size() == 3 ? literal(point[2]) : QString("1"));
        }
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
  metadata["lengthCutoff"] = 1.2345678901234568e-20;
  metadata["amplitude"] = 1.2345678901234568e9;
  metadata["tileSize"] = 12345678.901234567;
  metadata["windSpeed"] = 10.123456789012346;
  metadata["directionalFactorExponent"] = -1.2345678901234567;
  metadata["windAngle"] = -15.123456789012346;
  metadata["directionalReflectionDamping"] = -12345678.901234567;
  metadata["sharpen"] = -1.2345678901234567;
  metadata["flowDirection"] = QJsonArray{1.2345678901234567, -2.9876543210987654, 1.2345678901234568e-28};
  metadata["time"] = -12345678.901234567;
  metadata["filterWidth"] = -1.2345678901234568e-20;
  metadata["customMetadata"] = "retain";
  QJsonObject control{{"id", "spectrum_id"}, {"name", "water"}, {"spectrum", metadata}};
  const auto original = control;
  require(!runDialog("seExprSpectrumDialog", [&] { return SeExprControls::editSpectrum(nullptr, control); },
      [](QDialog *dialog) { return click(dialog, "seExprSpectrumButtons", QDialogButtonBox::Cancel); }),
      "cancelled spectrum edit was accepted");
  require(control == original, "Cancel changed spectrum metadata");
  require(runDialog("seExprSpectrumDialog", [&] { return SeExprControls::editSpectrum(nullptr, control); },
      [](QDialog *dialog) { return click(dialog, "seExprSpectrumButtons", QDialogButtonBox::Ok); }),
      "unchanged spectrum edit failed");
  require(control.value("spectrum").toObject() == metadata,
          "accepting unchanged spectrum rounded or clamped its original values");
  require(runDialog("seExprSpectrumDialog", [&] { return SeExprControls::editSpectrum(nullptr, control); },
      [](QDialog *dialog) {
        auto *wind = dialog->findChild<QLineEdit *>("seExprSpectrum_windSpeed");
        if (!wind) return false;
        wind->setText("15");
        return click(dialog, "seExprSpectrumButtons", QDialogButtonBox::Ok);
      }), "native spectrum edit failed");
  auto expectedMetadata = metadata;
  expectedMetadata["windSpeed"] = 15.0;
  require(control.value("spectrum").toObject() == expectedMetadata,
          "spectrum editing lost unrelated parameters or metadata");
  require(runDialog("seExprSpectrumDialog", [&] { return SeExprControls::editSpectrum(nullptr, control); },
      [](QDialog *dialog) {
        auto *cutoff = dialog->findChild<QLineEdit *>("seExprSpectrum_lengthCutoff");
        auto *exponent = dialog->findChild<QLineEdit *>("seExprSpectrum_directionalFactorExponent");
        auto *resolution = dialog->findChild<QLineEdit *>("seExprSpectrum_resolution");
        auto *flow = dialog->findChild<QLineEdit *>("seExprSpectrum_flow0");
        if (!cutoff || !exponent || !resolution || !flow) return false;
        cutoff->setText("2.5e-20"); exponent->setText("-2.5");
        resolution->setText("12"); flow->setText("-3.125e-28");
        if (!cutoff->hasAcceptableInput() || !exponent->hasAcceptableInput() ||
            !resolution->hasAcceptableInput() || !flow->hasAcceptableInput()) return false;
        return click(dialog, "seExprSpectrumButtons", QDialogButtonBox::Ok);
      }), "high-precision spectrum values could not be edited");
  expectedMetadata["lengthCutoff"] = 2.5e-20;
  expectedMetadata["directionalFactorExponent"] = -2.5;
  expectedMetadata["resolution"] = 12;
  auto expectedFlow = metadata.value("flowDirection").toArray();
  expectedFlow[0] = -3.125e-28;
  expectedMetadata["flowDirection"] = expectedFlow;
  require(control.value("spectrum").toObject() == expectedMetadata,
          "editing spectrum scientific values changed untouched flow components or metadata");
  require(control.value("points").toArray().size() == 64,
          "spectrum preview exceeded native curve control bounds");

  const auto beforeInvalid = control;
  bool invalidRejected = false;
  require(!runDialog("seExprSpectrumDialog", [&] { return SeExprControls::editSpectrum(nullptr, control); },
      [&](QDialog *dialog) {
        auto *wind = dialog->findChild<QLineEdit *>("seExprSpectrum_windSpeed");
        auto *error = dialog->findChild<QLabel *>("seExprSpectrumError");
        if (!wind || !error) return false;
        wind->setText("0");
        if (!click(dialog, "seExprSpectrumButtons", QDialogButtonBox::Ok)) return false;
        invalidRejected = dialog->isVisible() && control == beforeInvalid && !error->text().isEmpty();
        wind->setText("15");
        if (!error->text().isEmpty()) return false;
        return click(dialog, "seExprSpectrumButtons", QDialogButtonBox::Cancel);
      }), "invalid spectrum edit was applied");
  require(invalidRejected && control == beforeInvalid,
          "invalid spectrum OK did not retain the draft or Cancel changed its source");

  {
    struct RestoreLocale {
      QLocale previous;
      ~RestoreLocale() { QLocale::setDefault(previous); }
    } restoreLocale;
    QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
    require(runDialog("seExprSpectrumDialog", [&] { return SeExprControls::editSpectrum(nullptr, control); },
        [](QDialog *dialog) { return click(dialog, "seExprSpectrumButtons", QDialogButtonBox::Ok); }),
        "unchanged spectrum failed in a decimal-comma locale");
    require(control == beforeInvalid,
            "decimal-comma display changed an untouched spectrum");
    require(runDialog("seExprSpectrumDialog", [&] { return SeExprControls::editSpectrum(nullptr, control); },
        [](QDialog *dialog) {
          auto *cutoff = dialog->findChild<QLineEdit *>("seExprSpectrum_lengthCutoff");
          auto *wind = dialog->findChild<QLineEdit *>("seExprSpectrum_windSpeed");
          if (!cutoff || !wind || !cutoff->text().contains(',')) return false;
          cutoff->setText("3,75e-20"); wind->setText("15,125");
          if (!cutoff->hasAcceptableInput() || !wind->hasAcceptableInput()) return false;
          return click(dialog, "seExprSpectrumButtons", QDialogButtonBox::Ok);
        }), "spectrum could not edit locale-specific scientific values");
    expectedMetadata["lengthCutoff"] = 3.75e-20;
    expectedMetadata["windSpeed"] = 15.125;
    require(control.value("spectrum").toObject() == expectedMetadata,
            "decimal-comma spectrum edit changed untouched metadata or parsed the wrong number");
  }

  auto extremeMetadata = SeExprControls::defaultSpectrumParameters();
  extremeMetadata["windSpeed"] = 1e-200;
  extremeMetadata["tileSize"] = 2.5e-308;
  QJsonObject extremeControl{{"id", "extreme_spectrum"}, {"name", "extreme"},
                            {"spectrum", extremeMetadata}};
  const auto beforeExtreme = extremeControl;
  bool gridWarning = false;
  require(!runDialog("seExprSpectrumDialog", [&] { return SeExprControls::editSpectrum(nullptr, extremeControl); },
      [&](QDialog *dialog) {
        auto *view = dialog->findChild<QWidget *>("seExprSpectrumView");
        auto *error = dialog->findChild<QLabel *>("seExprSpectrumError");
        auto *wind = dialog->findChild<QLineEdit *>("seExprSpectrum_windSpeed");
        auto *tile = dialog->findChild<QLineEdit *>("seExprSpectrum_tileSize");
        if (!view || !error || !wind || !tile || !wind->hasAcceptableInput() ||
            !tile->hasAcceptableInput()) return false;
        gridWarning = error->text() == QObject::tr("The spectrum grid cannot be displayed for these values.");
        // Exercise the actual painter while finite inputs produce a NaN grid.
        const auto preview = view->grab();
        if (preview.isNull()) return false;
        return click(dialog, "seExprSpectrumButtons", QDialogButtonBox::Cancel);
      }), "extreme spectrum cancellation was accepted");
  require(gridWarning && extremeControl == beforeExtreme,
          "non-finite spectrum display grid lacked a warning or changed source values");
}

void animationAnnotationCases() {
  const QString source =
      "$motion=animCurve(sin( # lookup ), comma stays in comment\n$u)+0.5,"
      "# preserve curve metadata comment\n"
      "\"cycleRelative\",\"oscillate\",1,\"col1.ang\","
      "-1.25,2.1234567890123457,0.5,0.75,20,-10,\"fixed\",\"linear\",1,"
      "2.5,3.25,0.25,0.5,-12,30,\"fixed\",\"fixed\",0); # retain outer comment\n$motion";
  const auto imported = SeExprControls::importAnnotations(source, {}, "rgb");
  require(imported.diagnostics.isEmpty() && imported.controls.size() == 1,
          "complete inline animation curve was not imported");
  const auto control = imported.controls[0].toObject();
  const auto keys = control.value("keys").toArray();
  require(control.value("type").toString() == "animcurve" &&
              control.value("preInfinity").toString() == "cycleRelative" &&
              control.value("postInfinity").toString() == "oscillate" &&
              control.value("weighted").toBool() && control.value("link").toString() == "col1.ang" &&
              keys.size() == 2 && keys[0].toObject().value("inAngle").toDouble() == 20 &&
              keys[0].toObject().value("weightsLocked").toBool() &&
              keys[0].toObject().value("id").toString() != keys[1].toObject().value("id").toString(),
          "animation curve import lost infinity, link, tangent, lock or key ownership data");
  require(imported.source.contains("# lookup ), comma stays in comment") &&
              imported.source.contains("# preserve curve metadata comment") &&
              imported.source.contains("# retain outer comment") &&
              imported.source.contains("sin( # lookup ), comma stays in comment\n$u)+0.5"),
          "animation curve import changed its lookup expression or removed comments");
  for (double u : {-12.0, -1.25, 0.0, 0.5, 2.5, 17.0})
    same(evaluate(source, {}, TSeExpr::Type::Scalar, u),
         evaluate(imported.source, imported.controls, TSeExpr::Type::Scalar, u));
  const QString empty = "$motion=animCurve($u,\"constant\",\"linear\",0,\"\"); $motion";
  const auto emptyImport = SeExprControls::importAnnotations(empty);
  require(emptyImport.controls.size() == 1 &&
              emptyImport.controls[0].toObject().value("keys").toArray().isEmpty(),
          "empty animation curve could not be imported");
  same(evaluate(empty), evaluate(emptyImport.source, emptyImport.controls));
  const QString unsupported = "$motion=animCurve($u,\"mirror\",\"constant\",0,\"\"); $motion";
  const auto unsupportedImport = SeExprControls::importAnnotations(unsupported);
  require(unsupportedImport.source == unsupported && unsupportedImport.controls.isEmpty() &&
              !unsupportedImport.diagnostics.isEmpty(),
          "unknown animation infinity alias was silently approximated");
  const QString computed = "$motion=animCurve($u,\"constant\",\"constant\",0,\"\","
      "0,sin($u),1,1,0,0,\"auto\",\"auto\",0); $motion";
  const auto computedImport = SeExprControls::importAnnotations(computed);
  require(computedImport.source == computed && !computedImport.diagnostics.isEmpty(),
          "computed animation key metadata was silently frozen during import");
  auto invalid = SeExprControls::defaultAnimationCurve();
  QString error;
  require(SeExprControls::validateAnimationCurve(invalid, &error),
          "default animation curve did not pass shared validation");
  auto invalidKeys = invalid.value("keys").toArray();
  auto last = invalidKeys[1].toObject(); last["time"] = 0.0; invalidKeys[1] = last;
  invalid["keys"] = invalidKeys;
  require(!SeExprControls::validateAnimationCurve(invalid, &error) && !error.isEmpty(),
          "duplicate animation key times were accepted");
  invalid = SeExprControls::defaultAnimationCurve();
  invalidKeys = invalid.value("keys").toArray(); last = invalidKeys[1].toObject();
  last["id"] = "key_0"; invalidKeys[1] = last; invalid["keys"] = invalidKeys;
  require(!SeExprControls::validateAnimationCurve(invalid, &error),
          "duplicate animation key owners were accepted");
  invalid = SeExprControls::defaultAnimationCurve(); invalid["keys"] = QJsonArray();
  require(SeExprControls::validateAnimationCurve(invalid, &error),
          "empty animation key data was rejected");
  QJsonArray excessive;
  for (int i = 0; i < 65; ++i) excessive.append(QJsonObject{{"time", double(i)}, {"value", 0.0}});
  invalid["keys"] = excessive;
  require(!SeExprControls::validateAnimationCurve(invalid, &error),
          "native animation key bound was not enforced");
}

void animationDialogCases() {
  auto control = SeExprControls::defaultAnimationCurve();
  control["id"] = "motion_id"; control["name"] = "motion";
  control["customMetadata"] = QJsonObject{{"keep", true}};
  auto keys = control.value("keys").toArray();
  auto first = keys[0].toObject(), last = keys[1].toObject();
  first["time"] = -0.12345678901234568; first["value"] = 12345678.901234567;
  first["inWeight"] = 1.2345678901234568e-20; first["outWeight"] = 2.3456789012345679e-20;
  first["inAngle"] = -15.123456789012346; first["outAngle"] = 25.987654321098766;
  first["inTangent"] = "fixed"; first["outTangent"] = "fixed"; first["weightsLocked"] = true;
  first["customKeyMetadata"] = "keep key data";
  last["time"] = 2.3456789012345678; last["value"] = -3.1234567890123457;
  keys[0] = first; keys[1] = last; control["keys"] = keys;
  control["nativeKeys"] = keys;
  const auto original = control;
  require(!runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editAnimationCurve(nullptr, control); },
      [](QDialog *dialog) {
        auto *table = dialog->findChild<QTableWidget *>("seExprAnimationKeys");
        if (!table) return false;
        table->item(0, 1)->setText("123");
        return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Cancel);
      }), "cancelled animation curve edit was accepted");
  require(control == original, "animation curve Cancel changed committed data");
  require(runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editAnimationCurve(nullptr, control); },
      [](QDialog *dialog) { return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Ok); }),
      "unchanged animation curve edit failed");
  require(control == original, "unchanged animation curve edit lost numeric precision or metadata");
  bool lockSynchronized = false;
  require(runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editAnimationCurve(nullptr, control); },
      [&](QDialog *dialog) {
        auto *table = dialog->findChild<QTableWidget *>("seExprAnimationKeys");
        auto *before = dialog->findChild<QComboBox *>("seExprAnimationPreInfinity");
        auto *after = dialog->findChild<QComboBox *>("seExprAnimationPostInfinity");
        auto *weighted = dialog->findChild<QCheckBox *>("seExprAnimationWeighted");
        auto *link = dialog->findChild<QLineEdit *>("seExprAnimationLink");
        if (!table || !before || !after || !weighted || !link) return false;
        before->setCurrentIndex(before->findData("cycleRelative"));
        after->setCurrentIndex(after->findData("oscillate")); weighted->setChecked(true);
        link->setText("col1.ang");
        table->item(0, 0)->setText("-0.25"); table->item(0, 1)->setText("-4.25");
        table->item(0, 2)->setText("0.75");
        lockSynchronized = table->item(0, 3)->text() == "0.75";
        table->item(0, 8)->setCheckState(Qt::Unchecked);
        table->item(0, 3)->setText("2.25");
        table->item(0, 4)->setText("12.5"); table->item(0, 5)->setText("-25");
        table->item(0, 6)->setText("linear"); table->item(0, 7)->setText("stepnext");
        return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Ok);
      }), "complete animation curve authoring failed");
  const auto editedKeys = control.value("keys").toArray();
  const auto editedFirst = editedKeys[0].toObject();
  require(lockSynchronized && control.value("preInfinity").toString() == "cycleRelative" &&
              control.value("postInfinity").toString() == "oscillate" && control.value("weighted").toBool() &&
              control.value("link").toString() == "col1.ang" && editedFirst.value("time").toDouble() == -0.25 &&
              editedFirst.value("value").toDouble() == -4.25 && editedFirst.value("inWeight").toDouble() == 0.75 &&
              editedFirst.value("outWeight").toDouble() == 2.25 && editedFirst.value("inAngle").toDouble() == 12.5 &&
              editedFirst.value("outAngle").toDouble() == -25 && editedFirst.value("inTangent").toString() == "linear" &&
              editedFirst.value("outTangent").toString() == "stepnext" && !editedFirst.value("weightsLocked").toBool() &&
              editedFirst.value("id") == first.value("id") &&
              editedFirst.value("customKeyMetadata") == first.value("customKeyMetadata") && editedKeys[1] == last &&
              control.value("customMetadata") == original.value("customMetadata") &&
              control.value("nativeKeys") == original.value("nativeKeys"),
          "animation curve authoring lost fields, lock behavior, precision, owners or archived metadata");
  require(runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editAnimationCurve(nullptr, control); },
      [](QDialog *dialog) {
        auto *table = dialog->findChild<QTableWidget *>("seExprAnimationKeys");
        const auto push = [&](const char *name) {
          auto *button = dialog->findChild<QPushButton *>(name);
          if (!button) return false; button->click(); return true;
        };
        if (!table) return false;
        table->setCurrentCell(0, 0);
        if (!push("seExprAnimationDown") || !push("seExprAnimationUp") ||
            !push("seExprAnimationDuplicate") || table->rowCount() != 3) return false;
        table->item(2, 0)->setText("4");
        if (!push("seExprAnimationSort") || !push("seExprAnimationAdd") || table->rowCount() != 4) return false;
        table->setCurrentCell(1, 0);
        if (!push("seExprAnimationRemove") || table->rowCount() != 3) return false;
        return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Ok);
      }), "animation key add/duplicate/move/sort/delete operations failed");
  keys = control.value("keys").toArray();
  require(keys.size() == 3 && keys[0] == editedFirst && keys[1].toObject().value("time").toDouble() == 4 &&
              keys[2].toObject().value("time").toDouble() == 5 &&
              keys[1].toObject().value("id") != editedFirst.value("id") &&
              keys[1].toObject().value("id") != keys[2].toObject().value("id") &&
              keys[1].toObject().value("customKeyMetadata") == editedFirst.value("customKeyMetadata"),
          "animation key operations changed identity, copied metadata or ordering");
  const auto beforeInvalid = control;
  bool invalidStayedDraft = false;
  require(!runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editAnimationCurve(nullptr, control); },
      [&](QDialog *dialog) {
        auto *table = dialog->findChild<QTableWidget *>("seExprAnimationKeys");
        auto *error = dialog->findChild<QLabel *>("seExprAnimationError");
        if (!table || !error) return false;
        table->item(1, 0)->setText(table->item(0, 0)->text());
        if (!click(dialog, "seExprAnimationButtons", QDialogButtonBox::Ok)) return false;
        invalidStayedDraft = dialog->isVisible() && control == beforeInvalid && !error->text().isEmpty();
        return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Cancel);
      }), "invalid animation key data was applied");
  require(invalidStayedDraft && control == beforeInvalid,
          "invalid animation curve did not stay a cancellable draft");
}

void inlineAnimationDialogCases() {
  QString source = "$a=2; animCurve( sin($u) + $a , # metadata comment\n"
      "\"constant\",\"constant\",0,\"col1.ang\",0,0,1,1,0,0,\"linear\",\"linear\",0,"
      "1,1,1,1,0,0,\"linear\",\"linear\",0) + $a # outer comment";
  const auto original = source;
  QString error;
  require(!runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editInlineAnimationCurve(nullptr, source, &error); },
      [](QDialog *dialog) { return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Cancel); }),
      "cancelled inline animation curve edit was accepted");
  require(source == original && error.isEmpty(), "inline animation Cancel changed source");
  require(runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editInlineAnimationCurve(nullptr, source, &error); },
      [](QDialog *dialog) {
        auto *table = dialog->findChild<QTableWidget *>("seExprAnimationKeys");
        if (!table) return false;
        table->item(1, 1)->setText("2");
        return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Ok);
      }), "inline animation metadata edit failed");
  require(error.isEmpty() && source.startsWith("$a=2; animCurve( sin($u) + $a ,") &&
              source.endsWith(") + $a # outer comment") && source.contains("# metadata comment") &&
              source.contains("\"col1.ang\""),
          "inline animation edit changed lookup, surrounding source, comments or link");
  require(std::isfinite(evaluate(source).number[0]), "edited inline animation source is unusable");
  QString malformed = "animCurve($u,\"unknown\",\"constant\",0,\"\")";
  const auto beforeMalformed = malformed;
  require(!SeExprControls::editInlineAnimationCurve(nullptr, malformed, &error) &&
              malformed == beforeMalformed && !error.isEmpty(),
          "unsupported inline animation metadata was silently rewritten");
  malformed = "animCurve($u,\"constant\",\"constant\",0,\"\"";
  const auto incomplete = malformed;
  require(!SeExprControls::editInlineAnimationCurve(nullptr, malformed, &error) &&
              malformed == incomplete && !error.isEmpty(),
          "incomplete inline animation call was silently replaced");
  QString withoutCurve = "$u*2+1";
  require(!SeExprControls::editInlineAnimationCurve(nullptr, withoutCurve, &error, false) &&
              withoutCurve == "$u*2+1" && !error.isEmpty(),
          "edit-only inline action replaced a formula which has no animation curve");
  for (const auto &name : {QString("other.animCurve"), QString("other::animCurve")}) {
    QString qualified = "$motion=" + name + "($u,\"constant\",\"constant\",0,\"\"); $motion";
    const auto beforeQualified = qualified;
    require(!SeExprControls::editInlineAnimationCurve(nullptr, qualified, &error, false) &&
                qualified == beforeQualified && !error.isEmpty(),
            "inline animation editor targeted a qualified custom function");
    const auto imported = SeExprControls::importAnnotations(qualified);
    require(imported.source == beforeQualified && imported.controls.isEmpty() && imported.diagnostics.isEmpty(),
            "animation annotation import rewrote a qualified custom function");
  }
  QString fresh;
  require(runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editInlineAnimationCurve(nullptr, fresh, &error); },
      [](QDialog *dialog) { return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Ok); }),
      "explicit new inline animation curve could not be authored");
  TSeExpr::Program program; TSeExpr::Options options; options.resultType = TSeExpr::Type::Scalar;
  TSeExpr::Value value;
  require(program.compile(bytes(fresh), {{"frame", TSeExpr::Value::scalar(0.5)}}, {}, options) &&
              program.evaluate(value) && std::isfinite(value.number[0]),
          "new inline animation authoring did not return a usable expression");
}

void linkedAnimationCurveCases() {
  TXsheet sheet;
  auto *object = sheet.getStageObject(TStageObjectId::ColumnId(0));
  auto *parameter = object->getParam(TStageObject::T_Angle);
  parameter->setMeasureName("scale");
  TDoubleKeyframe first(0, 2), last(4, 10);
  first.m_type = TDoubleKeyframe::Linear; last.m_type = TDoubleKeyframe::Linear;
  parameter->setKeyframe(first); parameter->setKeyframe(last); parameter->enableCycle(true);
  const auto *grammar = sheet.getStageObjectTree()->getGrammar();
  auto control = SeExprControls::defaultAnimationCurve();
  control["name"] = "motion"; control["id"] = "motion_id";
  control["customMetadata"] = "retain linked metadata";
  QString error;
  require(SeExprControls::refreshAnimationCurveLink(grammar, "col1.ang", control, error),
          "native linear linked curve could not be refreshed");
  require(control.value("preInfinity").toString() == "constant" &&
              control.value("postInfinity").toString() == "cycleRelative" &&
              control.value("link").toString() == "col1.ang" &&
              control.value("customMetadata").toString() == "retain linked metadata" &&
              control.value("keys").toArray()[0].toObject().value("time").toDouble() == 1,
          "linked curve refresh lost frame, cycle, link or unrelated metadata");
  const auto compare = [&](const QJsonObject &snapshot) {
    TExpression native;
    native.setGrammar(grammar); native.setText("col1.ang");
    for (double frame : {0.0, 1.0, 1.5, 2.25, 4.75, 5.0, 7.0, 10.0}) {
      double expected = 0;
      require(native.evaluate(frame - 1, frame, frame, nullptr, expected),
              "native linked reference did not evaluate");
      const double actual = evaluate("motion($u)", QJsonArray{snapshot}, TSeExpr::Type::Scalar, frame).number[0];
      require(std::isfinite(actual) && std::abs(actual - expected) <= 1e-6 * std::max(1.0, std::abs(expected)),
              "linked animation snapshot differs from the actual native channel");
    }
  };
  compare(control);
  const auto stable = control.value("keys").toArray();
  require(SeExprControls::refreshAnimationCurveLink(grammar, "col1.ang", control, error) &&
              control.value("keys").toArray()[0].toObject().value("id") == stable[0].toObject().value("id") &&
              control.value("keys").toArray()[1].toObject().value("id") == stable[1].toObject().value("id"),
          "refreshing unchanged native keys changed their ownership");
  parameter->enableCycle(false);
  first.m_type = TDoubleKeyframe::Constant; parameter->setKeyframe(first);
  require(SeExprControls::refreshAnimationCurveLink(grammar, "col1.ang", control, error),
          "native constant linked curve could not be refreshed");
  compare(control);
  first.m_type = TDoubleKeyframe::SpeedInOut;
  first.m_linkedHandles = false; first.m_speedOut = TPointD(1, 3);
  last.m_linkedHandles = false; last.m_speedIn = TPointD(-1, -2);
  parameter->setKeyframe(first); parameter->setKeyframe(last);
  require(SeExprControls::refreshAnimationCurveLink(grammar, "col1.ang", control, error) &&
              control.value("weighted").toBool(),
          "native weighted Bezier linked curve could not be refreshed");
  compare(control);
  const auto beforeInvalid = control;
  for (const auto &reference : {QString("col1.ang+2"), QString("col99.ang"), QString("col1.ang(3)")})
    require(!SeExprControls::refreshAnimationCurveLink(grammar, reference, control, error) &&
                control == beforeInvalid && !error.isEmpty(),
            "computed, missing or time-offset refresh reference was silently copied");
  first.m_type = TDoubleKeyframe::EaseInOut; parameter->setKeyframe(first);
  require(!SeExprControls::refreshAnimationCurveLink(grammar, "col1.ang", control, error) &&
              control == beforeInvalid && !error.isEmpty(),
          "unsupported native interpolation was silently approximated");
  first.m_type = TDoubleKeyframe::Linear; first.m_step = 2; parameter->setKeyframe(first);
  require(!SeExprControls::refreshAnimationCurveLink(grammar, "col1.ang", control, error) && control == beforeInvalid,
          "stepped native timing was silently approximated");
  first.m_step = 1; parameter->setKeyframe(first); parameter->setMeasureName("shear");
  require(!SeExprControls::refreshAnimationCurveLink(grammar, "col1.ang", control, error) && control == beforeInvalid,
          "nonlinear native units were silently approximated");
  parameter->setMeasureName("");
  auto layer = object->createOffsetLayer(L"fixture offset");
  object->insertOffsetLayer(0, layer);
  require(!SeExprControls::refreshAnimationCurveLink(grammar, "col1.ang", control, error) &&
              control == beforeInvalid && !error.isEmpty(),
          "native offset layers were silently discarded from linked refresh");
  object->removeOffsetLayer(0);
  parameter->clearKeyframes(); parameter->setDefaultValue(7.25);
  require(SeExprControls::refreshAnimationCurveLink(grammar, "col1.ang", control, error) &&
              control.value("keys").toArray().size() == 1 &&
              control.value("keys").toArray()[0].toObject().value("value").toDouble() == 7.25,
          "unkeyed native channel default did not survive linked refresh");
}

void linkedAnimationDialogCases() {
  auto control = SeExprControls::defaultAnimationCurve();
  control["name"] = "motion"; control["id"] = "motion_id";
  control["link"] = "col1.ang"; control["customMetadata"] = "retain";
  const auto original = control;
  int calls = 0;
  const SeExprControls::AnimationCurveLinkResolver resolver =
      [&](const QString &link, QJsonObject &draft, QString &) {
    ++calls;
    if (link != "col1.ang") return false;
    auto keys = draft.value("keys").toArray(); auto first = keys[0].toObject();
    first["value"] = 12.5; keys[0] = first; draft["keys"] = keys;
    draft["postInfinity"] = "cycleRelative";
    return true;
  };
  require(!runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editAnimationCurve(nullptr, control, resolver); },
      [](QDialog *dialog) {
        auto *refresh = dialog->findChild<QPushButton *>("seExprAnimationRefresh");
        auto *table = dialog->findChild<QTableWidget *>("seExprAnimationKeys");
        auto *preview = dialog->findChild<QWidget *>("seExprAnimationPreview");
        if (!refresh || !refresh->isEnabled() || !table || !preview) return false;
        refresh->click();
        const auto values = preview->property("sampleValues").toList();
        if (table->item(0, 1)->text() != "12.5" || values.isEmpty() || values[0].toDouble() != 12.5) return false;
        return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Cancel);
      }), "cancelled linked refresh dialog was applied");
  require(control == original && calls == 1, "linked refresh escaped its cancellable draft");
  require(runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editAnimationCurve(nullptr, control, resolver); },
      [](QDialog *dialog) {
        auto *refresh = dialog->findChild<QPushButton *>("seExprAnimationRefresh");
        if (!refresh) return false; refresh->click();
        return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Ok);
      }), "linked refresh dialog could not apply its local snapshot");
  require(calls == 2 && control.value("keys").toArray()[0].toObject().value("value").toDouble() == 12.5 &&
              control.value("postInfinity").toString() == "cycleRelative" &&
              control.value("customMetadata") == original.value("customMetadata"),
          "linked refresh dialog lost snapshot changes or unrelated metadata");
  const auto beforeFailure = control;
  bool failureStayedDraft = false;
  const SeExprControls::AnimationCurveLinkResolver rejected =
      [](const QString &, QJsonObject &draft, QString &error) {
    draft["keys"] = QJsonArray(); error = "Fixture unavailable channel"; return false;
  };
  require(!runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editAnimationCurve(nullptr, control, rejected); },
      [&](QDialog *dialog) {
        auto *refresh = dialog->findChild<QPushButton *>("seExprAnimationRefresh");
        auto *error = dialog->findChild<QLabel *>("seExprAnimationError");
        auto *table = dialog->findChild<QTableWidget *>("seExprAnimationKeys");
        if (!refresh || !error || !table) return false;
        refresh->click();
        failureStayedDraft = error->text() == "Fixture unavailable channel" && table->rowCount() == 2;
        return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Cancel);
      }), "failed linked refresh was applied");
  require(failureStayedDraft && control == beforeFailure,
          "failed linked refresh mutated source or replaced the visible draft");
}

void animationPreviewCases() {
  auto control = SeExprControls::defaultAnimationCurve();
  const auto original = control;
  bool updated = false, invalidCleared = false;
  require(!runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editAnimationCurve(nullptr, control); },
      [&](QDialog *dialog) {
        auto *table = dialog->findChild<QTableWidget *>("seExprAnimationKeys");
        auto *preview = dialog->findChild<QWidget *>("seExprAnimationPreview");
        auto *after = dialog->findChild<QComboBox *>("seExprAnimationPostInfinity");
        if (!table || !preview || !after) return false;
        const int samples = preview->property("sampleCount").toInt();
        const auto frames = preview->property("sampleFrames").toList();
        const auto initial = preview->property("sampleValues").toList();
        if (samples < 129 || samples > 512 || preview->property("keyCount").toInt() != 2 ||
            frames.isEmpty() || frames.first().toDouble() >= 0 || frames.last().toDouble() <= 1 ||
            !preview->property("sampleError").toString().isEmpty() || preview->grab().isNull()) return false;
        table->item(1, 1)->setText("2");
        const auto changed = preview->property("sampleValues").toList();
        after->setCurrentIndex(after->findData("oscillate"));
        updated = changed != initial && preview->property("sampleValues").toList() != changed;
        table->item(1, 1)->setText("invalid");
        const bool rejected = preview->property("sampleCount").toInt() == 0 &&
            !preview->property("sampleError").toString().isEmpty();
        table->item(1, 1)->setText("2");
        invalidCleared = rejected && preview->property("sampleCount").toInt() >= 129 &&
            preview->property("sampleError").toString().isEmpty();
        return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Cancel);
      }), "animation graph preview unexpectedly applied a draft");
  require(updated && invalidCleared && control == original,
          "animation graph did not update values/infinity/errors or escaped its draft");
  for (int keyCount : {0, 1}) {
    control = SeExprControls::defaultAnimationCurve();
    auto key = control.value("keys").toArray()[0].toObject();
    key["time"] = 7.0; key["value"] = 2.25;
    control["keys"] = keyCount ? QJsonArray{key} : QJsonArray();
    bool constant = false;
    require(!runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editAnimationCurve(nullptr, control); },
        [&](QDialog *dialog) {
          auto *preview = dialog->findChild<QWidget *>("seExprAnimationPreview");
          if (!preview || preview->property("keyCount").toInt() != keyCount ||
              !preview->property("sampleError").toString().isEmpty()) return false;
          const auto values = preview->property("sampleValues").toList();
          constant = !values.isEmpty() && values.size() <= 512;
          for (const auto &value : values)
            constant = constant && value.isValid() && value.toDouble() == (keyCount ? 2.25 : 0);
          return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Cancel);
        }), "constant graph preview unexpectedly applied data");
    require(constant, "empty or single-key animation graph changed evaluator semantics");
  }
  control = SeExprControls::defaultAnimationCurve();
  auto keys = control.value("keys").toArray();
  auto first = keys[0].toObject(), last = keys[1].toObject();
  first["value"] = 1e308; last["value"] = std::numeric_limits<double>::max(); last["time"] = 1e308;
  for (auto *key : {&first, &last}) {
    (*key)["inTangent"] = "fixed"; (*key)["outTangent"] = "fixed";
    (*key)["inAngle"] = 45.0; (*key)["outAngle"] = 45.0;
  }
  keys[0] = first; keys[1] = last; control["keys"] = keys; control["postInfinity"] = "linear";
  bool sampleFailureVisible = false;
  require(!runDialog("seExprAnimationCurveDialog", [&] { return SeExprControls::editAnimationCurve(nullptr, control); },
      [&](QDialog *dialog) {
        auto *preview = dialog->findChild<QWidget *>("seExprAnimationPreview");
        auto *error = dialog->findChild<QLabel *>("seExprAnimationError");
        if (!preview || !error) return false;
        sampleFailureVisible = preview->property("sampleCount").toInt() > 0 &&
            !preview->property("sampleError").toString().isEmpty() && !error->text().isEmpty();
        if (preview->grab().isNull()) return false;
        return click(dialog, "seExprAnimationButtons", QDialogButtonBox::Cancel);
      }), "failed graph sample unexpectedly applied data");
  require(sampleFailureVisible, "animation graph concealed per-sample numerical failure");
}
}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication application(argc, argv);
  application.setQuitOnLastWindowClosed(false);
  try {
    annotationCases(); swatchCases(); spectrumCases();
    animationAnnotationCases(); animationDialogCases(); inlineAnimationDialogCases();
    linkedAnimationCurveCases(); linkedAnimationDialogCases();
    animationPreviewCases();
    std::cout << "seexpr-controls: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "seexpr-controls: FAIL: " << error.what() << '\n';
    return 1;
  }
}
