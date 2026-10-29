#include "toonzqt/seexpreditor.h"
#include "toonzqt/paramfield.h"
#include "tenv.h"
#include "tnotanimatableparam.h"
#include "tthread.h"
#include "tundo.h"

#include <QApplication>
#include <QComboBox>
#include <QColorDialog>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
QByteArray preset(const QJsonObject &document) {
  return QJsonDocument(document).toJson(QJsonDocument::Compact);
}
QJsonObject document(const QString &rgb = "[$u,$v,0.5]") {
  return {{"version", 1}, {"rgb", rgb}, {"alpha", "1"}, {"controls", QJsonArray()}};
}
}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication application(argc, argv);
  TThread::init();
  try {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary preset directory unavailable");
    require(QDir().mkpath(directory.filePath("stuff/config")) &&
                QDir().mkpath(directory.filePath("stuff/profiles")),
            "temporary native editor profile unavailable");
    TEnv::setStuffDir(TFilePath(directory.filePath("stuff").toStdWString()));
    SeExprEditor editor;
    editor.setPresetDirectory(directory.path());
    editor.setDocument(QString::fromUtf8(preset(document())), true);
    require(!editor.isModified(), "opening a document made a scene edit");
    auto *rgb = editor.findChild<QPlainTextEdit *>("seExprRgb");
    require(rgb, "native source field missing");
    int commits = 0;
    QString applied;
    QObject::connect(&editor, &SeExprEditor::documentApplied,
                     [&](const QString &value) { ++commits; applied = value; });
    rgb->setPlainText("[");
    require(editor.isModified() && commits == 0, "draft typing committed the document");
    require(!editor.applyDraft() && commits == 0,
            "invalid source replaced the committed expression");
    editor.revertDraft();
    require(!editor.isModified() && rgb->toPlainText() == "[$u,$v,0.5]",
            "Revert failed to restore the committed expression");
    rgb->setPlainText("[0.25,0.5,0.75]");
    require(editor.applyDraft() && commits == 1, "one Apply did not produce one document commit");
    require(!editor.isModified() && editor.applyDraft() && commits == 1,
            "reapplying an unchanged document produced another edit");
    require(QJsonDocument::fromJson(applied.toUtf8()).object().value("rgb").toString() ==
                "[0.25,0.5,0.75]", "committed source did not match the draft");

    // A frame/profile refresh must not discard a pending edit. Revert follows
    // the newly committed host value (for example after native undo).
    rgb->setPlainText("[0.1,0.2,0.3]");
    editor.setDocument(QString::fromUtf8(preset(document("[1,0,0]"))));
    require(rgb->toPlainText() == "[0.1,0.2,0.3]", "host refresh discarded an edited draft");
    editor.revertDraft();
    require(rgb->toPlainText() == "[1,0,0]", "Revert ignored an external undo/update");

    const QJsonObject scalar{{"id", "gain_id"}, {"name", "gain"}, {"label", "Gain"},
                             {"type", "scalar"}, {"value", 0.5}, {"min", 0.}, {"max", 1.}};
    QJsonObject controlled = document("$gain");
    controlled["controls"] = QJsonArray{scalar};
    QString error;
    require(editor.importPreset(preset(controlled), &error) && commits == 1,
            "preset loading committed instead of staging a draft");
    require(editor.validateDraft(), "native scalar control did not bind in the evaluator");
    require(editor.applyDraft() && commits == 2, "controlled preset could not be applied");
    const QByteArray exported = editor.exportPreset();
    const auto envelope = QJsonDocument::fromJson(exported).object();
    require(envelope.value("format").toString() == "groundzero-seexpr" &&
                envelope.value("document").toObject().value("controls").toArray()[0]
                    .toObject().value("id").toString() == "gain_id",
            "preset export lost the stable native control identifier");

    QJsonObject replacement = document("[0,1,0]");
    require(editor.importPreset(preset(replacement), &error), "replacement preset rejected");
    const auto replaced = QJsonDocument::fromJson(editor.document().toUtf8()).object();
    require(replaced.value("archive").toArray().size() == 1 &&
                replaced.value("archive").toArray()[0].toObject().value("id").toString() == "gain_id",
            "preset replacement discarded the old control needed by undo/keyframes");
    require(editor.importPreset(exported, &error), "an archived control could not be restored from its preset");
    require(QJsonDocument::fromJson(editor.document().toUtf8()).object().value("archive").toArray().isEmpty(),
            "restored control remained duplicated in the archive");
    editor.revertDraft();

    auto wrongType = scalar; wrongType["type"] = "string"; wrongType["value"] = "text";
    auto conflict = document(); conflict["controls"] = QJsonArray{wrongType};
    require(!editor.importPreset(preset(conflict), &error),
            "preset changed the type of a persisted native control identifier");
    auto malformed = document(); malformed["version"] = 99;
    require(!editor.importPreset(preset(malformed), &error), "unsupported preset version accepted");
    malformed = document(); malformed["controls"] = QJsonArray{scalar, scalar};
    require(!editor.importPreset(preset(malformed), &error), "duplicate native control identifiers accepted");

    const QString renamed = SeExprEditor::renameVariable(
        "$gain + gain + $gain_more + sprintf(\"gain\") # gain\n$gain", "gain", "amount");
    require(renamed == "$amount + amount + $gain_more + sprintf(\"gain\") # gain\n$amount",
            "variable migration changed strings, comments or unrelated identifiers");
    require(SeExprEditor::renameVariable("$gain + 'gain' + $gain::other", "gain", "amount") ==
                "$amount + 'gain' + $gain::other",
            "variable migration changed a quoted string or another qualified binding");

    // Real animation reference validation can override the generic FX context.
    // Its value preview is supplied by the same native reference evaluator.
    SeExprEditor scalarEditor;
    scalarEditor.setResultType(TSeExpr::Type::Scalar);
    scalarEditor.setSource("1", true);
    scalarEditor.setValidator([](const QString &source) {
      return source == "channel(\"peg.x\")" ? QString() : QString("Invalid native reference");
    });
    scalarEditor.setPreviewEvaluator([](const QString &) { return QString("Native value: 42"); });
    auto *expression = scalarEditor.findChild<QPlainTextEdit *>("seExprRgb");
    expression->setPlainText("channel(\"peg.x\")");
    int scalarCommits = 0;
    QObject::connect(&scalarEditor, &SeExprEditor::sourceApplied,
                     [&](const QString &) { ++scalarCommits; });
    require(scalarEditor.applyDraft() && scalarCommits == 1,
            "native reference callbacks were bypassed by the generic evaluator");
    require(scalarEditor.findChild<QLabel *>("seExprValuePreview")->text() == "Native value: 42",
            "native current-frame preview did not use its host evaluator");
    expression->setPlainText("channel(\"missing\")");
    require(!scalarEditor.applyDraft() && scalarCommits == 1,
            "broken native reference produced a source commit");

    // An unsupported saved version must not become version 1 merely because
    // the editor displays it. The original document remains repairable.
    SeExprEditor future;
    auto futureDocument = document("[0,0,0]");
    futureDocument["version"] = 99;
    future.setDocument(QString::fromUtf8(preset(futureDocument)), true);
    require(!future.isModified() &&
                QJsonDocument::fromJson(future.document().toUtf8()).object()
                    .value("version").toInt() == 99,
            "opening an unsupported document silently changed its version");
    future.findChild<QPlainTextEdit *>("seExprRgb")->setPlainText("[1,1,1]");
    require(!future.applyDraft(), "unsupported document version was downgraded by Apply");
    futureDocument = document("[0,0,0]");
    futureDocument["backend"] = "unknown-backend";
    future.setDocument(QString::fromUtf8(preset(futureDocument)), true);
    require(!future.validateDraft(), "unknown backend silently used the interpreter");
    futureDocument = document("[0,0,0]");
    futureDocument["engine"] = "unsupported-engine";
    future.setDocument(QString::fromUtf8(preset(futureDocument)), true);
    require(!future.validateDraft(), "unknown saved engine was silently evaluated");

    // Every shipped editor preset must use the same variable names as the
    // native effects. In particular Source Color uses src, not another host's
    // differently named image variable.
    SeExprEditor examples;
    auto *presets = examples.findChild<QListWidget *>("seExprPresets");
    require(presets && presets->count() >= 9, "built-in formula presets are missing");
    for (int index = 0; index < presets->count(); ++index) {
      const QByteArray data = presets->item(index)->data(Qt::UserRole).toString().toUtf8();
      require(examples.importPreset(data, &error), "built-in preset import failed");
      if (!examples.validateDraft())
        throw std::runtime_error((QString("Built-in preset %1 failed: %2")
            .arg(presets->item(index)->text(),
                 examples.findChild<QLabel *>("seExprStatus")->text())).toStdString());
    }

    SeExprEditor motionExamples;
    motionExamples.setResultType(TSeExpr::Type::Scalar);
    motionExamples.setSource("1", true);
    auto *motionPresets = motionExamples.findChild<QListWidget *>("seExprPresets");
    require(motionPresets && motionPresets->count() == 4,
            "built-in animation presets are missing");
    QByteArray smoothStep;
    for (int index = 0; index < motionPresets->count(); ++index) {
      const auto data = motionPresets->item(index)->data(Qt::UserRole).toString().toUtf8();
      require(motionExamples.importPreset(data, &error) && motionExamples.validateDraft(),
              "built-in animation preset failed in the real evaluator");
      if (motionPresets->item(index)->text() == "Smooth Step") smoothStep = data;
    }
    require(!smoothStep.isEmpty() && motionExamples.importPreset(smoothStep, &error),
            "Smooth Step animation preset is missing");
    const std::pair<double, const char *> expectedSteps[] = {
        {1.0, "Current value: 0"}, {12.5, "Current value: 0.5"}, {24.0, "Current value: 1"}};
    for (const auto &step : expectedSteps) {
      motionExamples.setContext({{"frame", TSeExpr::Value::scalar(step.first)}});
      require(motionExamples.validateDraft() &&
                  motionExamples.findChild<QLabel *>("seExprValuePreview")->text() == step.second,
              "Smooth Step does not transition from its first to last frame");
    }

    // Imported string annotations expose real file and directory pickers.
    // Cancel preserves the literal string; accepting changes only this draft.
    QFile selectedFile(directory.filePath("sample.txt"));
    require(selectedFile.open(QIODevice::WriteOnly), "path picker fixture unavailable");
    selectedFile.close();
    for (const QString annotation : {QString("file"), QString("directory")}) {
      SeExprEditor pathEditor;
      auto pathDocument = document("[0,0,0]");
      const QString original = "relative/sequence final";
      pathDocument["controls"] = QJsonArray{QJsonObject{
          {"id", "path_id"}, {"name", "path"}, {"label", "Path"},
          {"type", "string"}, {"value", original}, {"annotationType", annotation}}};
      pathEditor.setDocument(QString::fromUtf8(preset(pathDocument)), true);
      // Actual FX context includes the control being edited. It must not be
      // mistaken for a conflicting host binding when its name stays the same.
      pathEditor.setContext({{"path", TSeExpr::Value::string(original.toStdString())}});
      auto *controls = pathEditor.findChild<QTableWidget *>("seExprControls");
      controls->setCurrentCell(0, 0);
      const QString selected = annotation == "file" ? selectedFile.fileName() : directory.path();
      QString pathFailure;
      bool pickerSeen = false;
      int pathCommits = 0;
      QObject::connect(&pathEditor, &SeExprEditor::documentApplied,
                       [&](const QString &) { ++pathCommits; });
      QTimer watchdog(&pathEditor);
      watchdog.setSingleShot(true);
      QObject::connect(&watchdog, &QTimer::timeout, &pathEditor, [&] {
        pathFailure = "native path picker did not complete";
        for (auto *widget : QApplication::topLevelWidgets())
          if (widget->objectName() == "seExprControlPathDialog" ||
              widget->objectName() == "seExprControlDialog")
            if (auto *dialog = qobject_cast<QDialog *>(widget)) dialog->reject();
      });
      watchdog.start(5000);
      QTimer::singleShot(0, &pathEditor, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog || dialog->objectName() != "seExprControlDialog") {
          pathFailure = "native control dialog did not open";
          return;
        }
        auto *kind = dialog->findChild<QComboBox *>("seExprControlStringKind");
        auto *value = dialog->findChild<QLineEdit *>("seExprControlValue");
        auto *browse = dialog->findChild<QPushButton *>("seExprControlBrowse");
        auto *buttons = dialog->findChild<QDialogButtonBox *>("seExprControlButtons");
        if (!kind || !value || !browse || !buttons || kind->currentData().toString() != annotation) {
          pathFailure = "native string annotation did not select its path control";
          dialog->reject();
          return;
        }
        QTimer::singleShot(0, &pathEditor, [&] {
          auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
          if (!picker) { pathFailure = "native path chooser is missing"; return; }
          pickerSeen = true;
          const auto expected = annotation == "file" ? QFileDialog::ExistingFile : QFileDialog::Directory;
          if (picker->fileMode() != expected) pathFailure = "native path chooser mode is wrong";
          picker->reject();
        });
        browse->click();
        if (value->text() != original) pathFailure = "cancel replaced a literal string path";
        if (!pathFailure.isEmpty()) { dialog->reject(); return; }
        QTimer::singleShot(0, &pathEditor, [&] {
          auto *picker = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
          if (!picker) { pathFailure = "native path chooser did not reopen"; return; }
          picker->setDirectory(QFileInfo(selected).absolutePath());
          picker->selectFile(selected);
          if (!QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection))
            pathFailure = "native path chooser could not accept its selection";
        });
        browse->click();
        if (value->text() != selected) pathFailure = "native path chooser lost its selected path";
        if (!pathFailure.isEmpty()) { dialog->reject(); return; }
        buttons->button(QDialogButtonBox::Ok)->click();
      });
      pathEditor.findChild<QPushButton *>("seExprControlEdit")->click();
      watchdog.stop();
      const auto edited = QJsonDocument::fromJson(pathEditor.document().toUtf8())
                              .object().value("controls").toArray()[0].toObject();
      require(pathFailure.isEmpty() && pickerSeen && pathCommits == 0 &&
                  edited.value("value").toString() == selected &&
                  edited.value("annotationType").toString() == annotation,
              "native path annotation picker lost a value or committed the scene");
    }

    // An unchanged display picker must preserve the native HDR source value.
    {
      SeExprEditor colorEditor;
      const QJsonArray original{-0.25, 2.125, 0.123456789012345};
      auto colorDocument = document("$tint");
      colorDocument["controls"] = QJsonArray{QJsonObject{
          {"id", "tint_id"}, {"name", "tint"}, {"label", "Tint"},
          {"type", "color"}, {"value", original}}};
      colorEditor.setDocument(QString::fromUtf8(preset(colorDocument)), true);
      colorEditor.setContext({{"tint", TSeExpr::Value::vector(
          original[0].toDouble(), original[1].toDouble(), original[2].toDouble())}});
      colorEditor.findChild<QTableWidget *>("seExprControls")->setCurrentCell(0, 0);
      QString colorFailure;
      bool pickerSeen = false;
      QTimer watchdog(&colorEditor);
      watchdog.setSingleShot(true);
      QObject::connect(&watchdog, &QTimer::timeout, &colorEditor, [&] {
        colorFailure = "native color dialog did not complete";
        for (auto *widget : QApplication::topLevelWidgets())
          if (auto *dialog = qobject_cast<QDialog *>(widget)) dialog->reject();
      });
      watchdog.start(5000);
      QTimer::singleShot(0, &colorEditor, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) { colorFailure = "native color control dialog is missing"; return; }
        auto *color = dialog->findChild<QPushButton *>("seExprControlColor");
        auto *buttons = dialog->findChild<QDialogButtonBox *>("seExprControlButtons");
        if (!color || !buttons) { colorFailure = "native color controls are missing"; dialog->reject(); return; }
        QTimer::singleShot(0, &colorEditor, [&] {
          auto *picker = qobject_cast<QColorDialog *>(QApplication::activeModalWidget());
          if (!picker) { colorFailure = "native display color picker is missing"; return; }
          pickerSeen = true;
          QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
        });
        color->click();
        buttons->button(QDialogButtonBox::Ok)->click();
      });
      colorEditor.findChild<QPushButton *>("seExprControlEdit")->click();
      watchdog.stop();
      const auto edited = QJsonDocument::fromJson(colorEditor.document().toUtf8())
                              .object().value("controls").toArray()[0].toObject();
      require(colorFailure.isEmpty() && pickerSeen && edited.value("value").toArray() == original,
              "unchanged native color picker lost HDR or precision");
    }

    // Host preview failures are errors, rather than successful preview strings.
    // The same real evaluator that renders expressions drives this boundary.
    SeExprEditor checked;
    checked.setResultType(TSeExpr::Type::Scalar);
    checked.setSource("1", true);
    checked.setCheckedPreviewEvaluator([](const QString &source, QString &value,
                                         QString &diagnostic) {
      TSeExpr::Options options;
      options.resultType = TSeExpr::Type::Scalar;
      TSeExpr::Program program;
      TSeExpr::Value result;
      if (!program.compile(source.toUtf8().toStdString(), {}, {}, options) ||
          !program.evaluate(result)) {
        diagnostic = QString::fromStdString(program.errorMessage());
        return false;
      }
      value = QString::number(result.number[0], 'g', 12);
      return true;
    });
    int checkedCommits = 0;
    QObject::connect(&checked, &SeExprEditor::sourceApplied,
                     [&](const QString &) { ++checkedCommits; });
    auto *checkedSource = checked.findChild<QPlainTextEdit *>("seExprRgb");
    checkedSource->setPlainText("1/0");
    require(!checked.applyDraft() && checkedCommits == 0 &&
                checked.findChild<QLabel *>("seExprValuePreview")->text().isEmpty(),
            "runtime preview failure committed a formula or looked successful");
    checkedSource->setPlainText("2+2");
    require(checked.applyDraft() && checkedCommits == 1 &&
                checked.findChild<QLabel *>("seExprValuePreview")->text() == "4",
            "preview error prevented a later valid formula from applying");

    // Cached native parameter pages reuse StringParamField widgets across FX.
    // A pending draft belongs to its original parameter, while a frame refresh
    // of that parameter must retain it. Apply uses the real native undo path.
    TStringParamP actualA = new TStringParam(
        QString::fromUtf8(preset(document("[1,0,0]"))).toStdWString());
    TStringParamP currentA = TParamP(actualA->clone());
    TStringParamP actualB = new TStringParam(
        QString::fromUtf8(preset(document("[0,1,0]"))).toStdWString());
    TStringParamP currentB = TParamP(actualB->clone());
    for (const auto &parameter : {actualA, currentA, actualB, currentB})
      parameter->setName("seexprDocument");
    StringParamField field(nullptr, "Formula", actualA);
    field.setParam(currentA, actualA, 0);
    auto *nativeEditor = field.findChild<SeExprEditor *>();
    require(nativeEditor != nullptr, "native expression parameter did not create its editor");
    auto *nativeSource = nativeEditor->findChild<QPlainTextEdit *>("seExprRgb");
    nativeSource->setPlainText("[0.2,0.3,0.4]");
    field.update(1);
    require(nativeEditor->isModified() && nativeSource->toPlainText() == "[0.2,0.3,0.4]",
            "same-parameter frame refresh discarded its pending formula");
    field.setParam(currentB, actualB, 1);
    require(!nativeEditor->isModified() && nativeSource->toPlainText() == "[0,1,0]",
            "a pending formula leaked into another native parameter");
    TUndoManager *undo = TUndoManager::manager();
    undo->reset();
    const auto beforeB = actualB->getValue();
    nativeSource->setPlainText("[0,0,1]");
    require(nativeEditor->applyDraft() && undo->getHistoryCount() == 1 &&
                actualB->getValue() == currentB->getValue(),
            "native Apply did not create exactly one undoable parameter change");
    require(QJsonDocument::fromJson(QString::fromStdWString(actualB->getValue()).toUtf8())
                .object().value("rgb").toString() == "[0,0,1]",
            "native Apply changed a different parameter");
    require(QJsonDocument::fromJson(QString::fromStdWString(actualA->getValue()).toUtf8())
                .object().value("rgb").toString() == "[1,0,0]",
            "editing the second parameter changed the first parameter");
    undo->undo(); field.update(1);
    require(actualB->getValue() == beforeB && nativeSource->toPlainText() == "[0,1,0]",
            "native document undo did not restore the previous source");
    undo->redo(); field.update(1);
    require(nativeSource->toPlainText() == "[0,0,1]", "native document redo did not restore the source");
    undo->reset();

    // Exercise the actual editor button and modal range viewer. Each open
    // freezes its draft context, even if a host refresh changes the editor
    // while the dialog processes events. Preview must never apply a formula.
    SeExprEditor ranged;
    ranged.setResultType(TSeExpr::Type::Scalar);
    ranged.setSource("$frame*2", true);
    auto *rangedSource = ranged.findChild<QPlainTextEdit *>("seExprRgb");
    int rangeFactories = 0, rangeCommits = 0;
    ranged.setRangeEvaluatorFactory([&](const QString &source) -> SeExprEditor::RangeEvaluator {
      ++rangeFactories;
      auto program = std::make_shared<TSeExpr::Program>();
      TSeExpr::Options options;
      options.resultType = TSeExpr::Type::Scalar;
      const bool compiled = program->compile(source.toUtf8().toStdString(),
          {{"frame", TSeExpr::Value::scalar(1)}}, {}, options);
      return [program, compiled](double frame, std::vector<double> &values, QString &error) {
        TSeExpr::Value result;
        if (!compiled || !program->setScalar("frame", frame) || !program->evaluate(result)) {
          error = QString::fromStdString(program->errorMessage());
          return false;
        }
        values = result.number;
        return true;
      };
    });
    QObject::connect(&ranged, &SeExprEditor::sourceApplied,
                     [&](const QString &) { ++rangeCommits; });
    QString rangeFailure;
    bool rangeDialogSeen = false;
    QTimer::singleShot(0, &ranged, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog || dialog->objectName() != "seExprRangeDialog") {
        rangeFailure = "native range dialog did not open";
        return;
      }
      rangeDialogSeen = true;
      auto *last = dialog->findChild<QDoubleSpinBox *>("seExprRangeEnd");
      auto *runRange = dialog->findChild<QPushButton *>("seExprRangeRun");
      auto *samples = dialog->findChild<QTableWidget *>("seExprRangeSamples");
      if (!last || !runRange || !samples) rangeFailure = "native range controls are missing";
      else {
        last->setValue(3);
        runRange->click();
        rangedSource->setPlainText("$frame*3");
        runRange->click();
        if (samples->rowCount() != 3 || !samples->item(2, 1) ||
            samples->item(2, 1)->text().toDouble() != 6.0)
          rangeFailure = "range preview did not retain its original draft context";
      }
      dialog->reject();
    });
    QTimer::singleShot(10000, &ranged, [&] {
      for (auto *widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "seExprRangeDialog") {
          rangeFailure = "native range dialog did not complete its acceptance check";
          if (auto *dialog = qobject_cast<QDialog *>(widget)) dialog->reject();
        }
    });
    ranged.findChild<QPushButton *>("seExprRangePreview")->click();
    require(rangeDialogSeen && rangeFailure.isEmpty() && rangeFactories == 1 && rangeCommits == 0,
            "native editor range preview failed or committed a formula");

    std::cout << "seexpr-editor: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "seexpr-editor: FAIL: " << error.what() << '\n';
    return 1;
  }
}
