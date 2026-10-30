#include "toonzqt/functionworkflow.h"
#include "toonzqt/functionviewer.h"
#include "toonzqt/functiontreeviewer.h"
#include "toonzqt/functionpanel.h"
#include "toonzqt/functionselection.h"
#include "toonzqt/functioncurveoperations.h"
#include "toonz/doubleparamcmd.h"
#include "toonz/toonzfolders.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txsheetexpr.h"
#include "tdoubleparam.h"
#include "texpression.h"
#include "tstream.h"
#include "tundo.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <cmath>
#include <map>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {
namespace Ops = FunctionCurveOperations;
QString tr(const char *text) { return QObject::tr(text); }
QString settingsPath() {
  return (ToonzFolder::getMyModuleDir() + TFilePath("function-editor.ini")).getQString();
}
std::vector<Ops::Selection> selected(FunctionSelection *selection) {
  std::map<TDoubleParam *, std::set<int>> groups;
  for (int n = 0; n < selection->getSelectedKeyframeCount(); ++n) {
    auto key = selection->getSelectedKeyframe(n);
    if (key.first && key.second >= 0 && key.second < key.first->getKeyframeCount())
      groups[key.first].insert(key.second);
  }
  std::vector<Ops::Selection> result;
  for (const auto &entry : groups) result.push_back({entry.first, entry.second});
  return result;
}
std::vector<TDoubleParamP> curves(const std::vector<Ops::Selection> &selection) {
  std::vector<TDoubleParamP> result;
  for (const auto &entry : selection) result.push_back(entry.curve);
  return result;
}
void preview(FunctionPanel *graph, const Ops::Plan &plan, bool before = false) {
  std::vector<std::pair<TDoubleParamP, TDoubleParamP>> overlay;
  if (plan.valid()) for (const auto &change : plan.changes)
    overlay.emplace_back(change.curve, before ? change.before : change.after);
  graph->setComparison(overlay);
}
void restoreOverlay(FunctionViewer *viewer, FunctionPanel *graph,
                    FunctionPanel::ComparisonState saved,
                    bool contextValid) {
  if (!contextValid) { graph->setComparison({}); return; }
  QSet<TDoubleParam *> present;
  for (auto *channel : viewer->findChild<FunctionTreeView *>()->channels()) present.insert(channel->getParam());
  saved.curves.erase(std::remove_if(saved.curves.begin(), saved.curves.end(), [&](const std::pair<TDoubleParamP, TDoubleParamP> &entry) { return !present.contains(entry.first.getPointer()); }), saved.curves.end());
  saved.frozen.erase(std::remove_if(saved.frozen.begin(), saved.frozen.end(), [&](const FunctionPanel::FrozenComparisonCurve &entry) { return !present.contains(entry.owner.getPointer()); }), saved.frozen.end());
  graph->setComparisonState(saved);
}
QDoubleSpinBox *number(QWidget *parent, double value, double low = -1.e9,
                       double high = 1.e9, int decimals = 6) {
  auto *field = new QDoubleSpinBox(parent);
  field->setDecimals(decimals); field->setRange(low, high); field->setValue(value);
  field->setKeyboardTracking(false);
  return field;
}
void showEdit(FunctionViewer *viewer, FunctionPanel *graph) {
  auto savedComparison = graph->comparisonState();
  auto selection = selected(viewer->getSelection());
  if (selection.empty()) {
    QMessageBox::information(viewer, tr("Edit Function Keys"), tr("Select one or more keys in the graph or spreadsheet first.")); return;
  }
  QDialog dialog(viewer); dialog.setWindowTitle(tr("Edit Function Keys"));
  bool contextValid = true;
  if (viewer->getSceneHandle()) QObject::connect(viewer->getSceneHandle(), &TSceneHandle::sceneSwitched, &dialog, [&]() { contextValid = false; dialog.reject(); });
  if (viewer->getXsheetHandle()) QObject::connect(viewer->getXsheetHandle(), &TXsheetHandle::xsheetSwitched, &dialog, [&]() { contextValid = false; dialog.reject(); });
  QObject::connect(viewer, &FunctionViewer::workflowContextChanged, &dialog, [&]() { contextValid = false; dialog.reject(); });
  QObject::connect(graph->getModel(), &QAbstractItemModel::modelAboutToBeReset, &dialog, [&]() { contextValid = false; dialog.reject(); });
  dialog.setObjectName("FunctionKeyEditor");
  QVBoxLayout layout(&dialog);
  double first = 1.e100, last = -first, low = first, high = last;
  size_t count = 0;
  for (const auto &entry : selection) for (int k : entry.indices) {
    auto key = entry.curve->getKeyframe(k);
    first = std::min(first, key.m_frame); last = std::max(last, key.m_frame);
    double value = Ops::displayValue(entry.curve.getPointer(), key.m_value);
    low = std::min(low, value); high = std::max(high, value); ++count;
  }
  auto *summary = new QLabel(tr("%1 keys on %2 channels. Frames %3–%4. %5")
      .arg(qulonglong(count)).arg(selection.size()).arg(first + 1).arg(last + 1)
      .arg(low == high ? tr("Value: %1").arg(low) : tr("Mixed values (%1 to %2)").arg(low).arg(high)));
  summary->setWordWrap(true); layout.addWidget(summary);
  QFormLayout form;
  auto *mode = new QComboBox(&dialog); mode->setObjectName("FunctionEditOperation");
  mode->addItems({tr("Set values"), tr("Offset values"), tr("Offset frames"), tr("Retime around pivot"),
                  tr("Bake evaluated curves"), tr("Reduce linear keys"), tr("Insert breakdown"), tr("Quantize frame grid"),
                  tr("Retime to target range")});
  auto *amount = number(&dialog, 0); amount->setObjectName("FunctionEditAmount");
  auto *start = number(&dialog, first + 1, 1, 1.e9, 9); start->setObjectName("FunctionEditFirstFrame");
  auto *end = number(&dialog, last + 1, 1, 1.e9, 9); end->setObjectName("FunctionEditLastFrame");
  auto *step = number(&dialog, 1, .001, 1.e6); step->setObjectName("FunctionEditInterval");
  auto *pivot = number(&dialog, first + 1, 1); pivot->setObjectName("FunctionEditPivot");
  auto *scale = number(&dialog, 1, .000001, 1.e6);
  auto *favour = new QSlider(Qt::Horizontal, &dialog); favour->setRange(-100, 200); favour->setValue(50); favour->setObjectName("FunctionEditFavour");
  auto *favourLabel = new QLabel(tr("50%"));
  mode->setAccessibleName(tr("Key edit operation")); amount->setAccessibleName(tr("Amount or reduction tolerance"));
  start->setAccessibleName(tr("First frame or breakdown frame")); end->setAccessibleName(tr("Last frame"));
  favour->setAccessibleName(tr("Breakdown favour percent"));
  form.addRow(tr("Operation"), mode); form.addRow(tr("Amount / tolerance"), amount);
  form.addRow(tr("First frame / breakdown frame"), start); form.addRow(tr("Last frame"), end);
  form.addRow(tr("Sample / grid interval (frames)"), step); form.addRow(tr("Pivot frame"), pivot);
  form.addRow(tr("Time scale"), scale); form.addRow(tr("Breakdown / overshoot"), favour); form.addRow(QString(), favourLabel);
  layout.addLayout(&form);
  auto *rangePreview = new QLabel(&dialog);
  rangePreview->setObjectName("FunctionEditRangePreview");
  rangePreview->setWordWrap(true); layout.addWidget(rangePreview);
  auto updateRangePreview = [&]() {
    double from = 1.e100, to = -from;
    for (const auto &entry : selection) for (int index : entry.indices) {
      if (index < 0 || index >= entry.curve->getKeyframeCount()) continue;
      double frame = entry.curve->keyframeIndexToFrame(index);
      from = std::min(from, frame); to = std::max(to, frame);
    }
    rangePreview->setText(from > to ? tr("No selected source frames are available.")
        : tr("Selected source range: %1–%2. Target range: %3–%4. Relative timing across all selected channels is preserved.")
              .arg(from + 1, 0, 'g', 17).arg(to + 1, 0, 'g', 17)
              .arg(start->value(), 0, 'g', 17).arg(end->value(), 0, 'g', 17));
  };
  auto *note = new QLabel(tr("Values use each channel's display units. Baking replaces driven segments explicitly and is exact at the chosen samples. Reduction preserves holds and driven boundaries. Dashed curves show a preview; nothing changes until Apply."));
  note->setWordWrap(true); layout.addWidget(note);
  auto *status = new QLabel; status->setObjectName("FunctionEditStatus");
  status->setWordWrap(true); layout.addWidget(status);
  QDialogButtonBox buttons(QDialogButtonBox::Apply | QDialogButtonBox::Close);
  auto *previewButton = buttons.addButton(tr("Preview"), QDialogButtonBox::ActionRole);
  previewButton->setObjectName("FunctionEditPreview");
  layout.addWidget(&buttons);
  Ops::Plan plan;
  auto updateFields = [&]() {
    int op = mode->currentIndex();
    amount->setEnabled(op <= 2 || op == 5); start->setEnabled((op >= 4 && op <= 6) || op == 8);
    end->setEnabled(op == 4 || op == 5 || op == 8); step->setEnabled(op == 4 || op == 5 || op == 7);
    pivot->setEnabled(op == 3 || op == 7); scale->setEnabled(op == 3); favour->setEnabled(op == 6);
    const QString firstLabel = op == 8 ? tr("Target first frame") : tr("First frame / breakdown frame");
    const QString lastLabel = op == 8 ? tr("Target last frame") : tr("Last frame");
    qobject_cast<QLabel *>(form.labelForField(start))->setText(firstLabel);
    qobject_cast<QLabel *>(form.labelForField(end))->setText(lastLabel);
    start->setAccessibleName(firstLabel); end->setAccessibleName(lastLabel);
    rangePreview->setVisible(op == 8); updateRangePreview();
    status->setText(QString()); graph->setComparison({});
  };
  auto build = [&]() {
    QProgressDialog progress(tr("Sampling selected curves…"), tr("Cancel"), 0, 0, &dialog);
    progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(250);
    auto cancel = [&]() { QApplication::processEvents(); return !contextValid || progress.wasCanceled(); };
    if (!contextValid) { plan = Ops::Plan(); plan.cancelled = true; return; }
    Ops::Range range{start->value() - 1, end->value() - 1, step->value()};
    switch (mode->currentIndex()) {
    case 0: plan = Ops::planValues(selection, Ops::ValueMode::Set, amount->value()); break;
    case 1: plan = Ops::planValues(selection, Ops::ValueMode::Offset, amount->value()); break;
    case 2: plan = Ops::planFrameOffset(selection, amount->value()); break;
    case 3: plan = Ops::planRetime(selection, pivot->value() - 1, scale->value()); break;
    case 4: plan = Ops::planBake(curves(selection), range, cancel); break;
    case 5: plan = Ops::planReduce(selection, range, amount->value(), cancel); break;
    case 6: plan = Ops::planBreakdown(selection, start->value() - 1, favour->value() * .01); break;
    case 7: plan = Ops::planQuantize(selection, pivot->value() - 1, step->value()); break;
    case 8: plan = Ops::planRetimeToRange(selection, range.first, range.last); break;
    }
    preview(graph, plan);
    status->setText(plan.cancelled ? tr("Canceled; no keys changed.") : !plan.valid() ? plan.error
        : tr("Preview: %1 channels; %2 samples; %3 keys removed; maximum sampled error %4.")
            .arg(plan.changes.size()).arg(qulonglong(plan.sampleCount)).arg(qulonglong(plan.removedKeys)).arg(plan.maximumError));
  };
  QObject::connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, updateFields);
  for (auto *field : {start, end})
    QObject::connect(field, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &dialog, [&]() {
      updateRangePreview();
      if (mode->currentIndex() == 8) { graph->setComparison({}); status->clear(); }
    });
  QObject::connect(favour, &QSlider::valueChanged, &dialog, [&](int value) { favourLabel->setText(tr("%1%").arg(value)); });
  QObject::connect(favour, &QSlider::sliderReleased, &dialog, [&]() { if (mode->currentIndex() == 6) build(); });
  QObject::connect(previewButton, &QPushButton::clicked, &dialog, build);
  QObject::connect(buttons.button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&]() {
    build(); if (!plan.valid() || !contextValid) return;
    std::map<TDoubleParam *, std::vector<double>> selectedFrames;
    for (const auto &entry : selection) for (int k : entry.indices)
      selectedFrames[entry.curve.getPointer()].push_back(entry.curve->keyframeIndexToFrame(k));
    QString error;
    if (!Ops::apply(plan, tr("Edit Function Keys"), error)) { status->setText(error); return; }
    if (plan.changes.empty()) {
      status->setText(tr("No keys changed.")); graph->setComparison({}); return;
    }
    auto *keys = viewer->getSelection(); keys->selectNone();
    // Preserve the exact original selection for repeated value/time edits.
    // Baking selects its explicit range; reduction keeps surviving selected keys.
    for (const auto &entry : selection) {
      auto *curve = entry.curve.getPointer();
      if (mode->currentIndex() == 8) {
        // Range retiming preserves ordering/counts, so indices retain the exact
        // original selection without a second floating-point reconstruction.
        for (int index : entry.indices) keys->select(curve, index);
      } else if (mode->currentIndex() == 4) {
        for (int k = 0; k < curve->getKeyframeCount(); ++k) {
          double frame = curve->keyframeIndexToFrame(k);
          if (frame >= start->value() - 1 && frame <= end->value() - 1) keys->select(curve, k);
        }
      } else {
        auto frames = selectedFrames[curve];
        if (mode->currentIndex() == 6) frames.push_back(start->value() - 1);
        for (double frame : frames) {
          if (mode->currentIndex() == 2) frame += amount->value();
          if (mode->currentIndex() == 3) frame = pivot->value() - 1 + (frame - (pivot->value() - 1)) * scale->value();
          if (mode->currentIndex() == 7) frame = pivot->value() - 1 + std::round((frame - (pivot->value() - 1)) / step->value()) * step->value();
          if (curve->isKeyframe(frame)) keys->select(curve, curve->getClosestKeyframe(frame));
        }
      }
    }
    selection = selected(keys);
    updateRangePreview();
    viewer->emitCurveChanged(); graph->refreshNormalization(); graph->update();
    status->setText(tr("Applied as one undo step. Selection is preserved for the next operation."));
    graph->setComparison({});
  });
  QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  updateFields(); dialog.exec(); restoreOverlay(viewer, graph, savedComparison, contextValid);
}

void showSets(FunctionViewer *viewer, FunctionTreeView *tree) {
  QDialog dialog(viewer); dialog.setObjectName("FunctionChannelSets");
  dialog.setWindowTitle(tr("Channel Sets")); QVBoxLayout layout(&dialog);
  bool contextValid = true;
  auto rejectContext = [&]() { contextValid = false; dialog.reject(); };
  if (viewer->getSceneHandle()) QObject::connect(viewer->getSceneHandle(), &TSceneHandle::sceneSwitched, &dialog, rejectContext);
  if (viewer->getXsheetHandle()) QObject::connect(viewer->getXsheetHandle(), &TXsheetHandle::xsheetSwitched, &dialog, rejectContext);
  QObject::connect(viewer, &FunctionViewer::workflowContextChanged, &dialog, rejectContext);
  QObject::connect(tree->model(), &QAbstractItemModel::modelAboutToBeReset, &dialog, rejectContext);
  const QString help = tr("Save visible curves as a named set. Select a set to rename it; F2 edits its name and Enter applies the rename. Closing discards an unfinished name. Channel identities survive channel renaming and scene reload.");
  auto *info = new QLabel(help); info->setObjectName("FunctionChannelSetStatus");
  info->setWordWrap(true); layout.addWidget(info);
  QListWidget list; list.setObjectName("FunctionChannelSetList");
  list.setAccessibleName(tr("Saved channel sets")); layout.addWidget(&list);
  QLineEdit name; name.setObjectName("FunctionChannelSetName");
  name.setAccessibleName(tr("Selected channel set name"));
  auto *nameLabel = new QLabel(tr("Set &name")); nameLabel->setBuddy(&name);
  layout.addWidget(nameLabel); layout.addWidget(&name);
  QSettings settings(settingsPath(), QSettings::IniFormat);
  QJsonArray sets = QJsonDocument::fromJson(settings.value("channelSets").toByteArray()).array();
  QDialogButtonBox buttons(QDialogButtonBox::Close);
  auto *capture = buttons.addButton(tr("&Save visible channels…"), QDialogButtonBox::ActionRole);
  auto *restore = buttons.addButton(tr("S&how set"), QDialogButtonBox::ActionRole);
  auto *rename = buttons.addButton(tr("&Rename set"), QDialogButtonBox::ActionRole);
  auto *remove = buttons.addButton(tr("&Delete set"), QDialogButtonBox::ActionRole);
  capture->setObjectName("FunctionChannelSetSave"); restore->setObjectName("FunctionChannelSetShow");
  rename->setObjectName("FunctionChannelSetRename"); remove->setObjectName("FunctionChannelSetDelete");
  for (auto *button : {capture, restore, rename, remove, buttons.button(QDialogButtonBox::Close)}) button->setAutoDefault(false);
  layout.addWidget(&buttons);
  auto normalizedName = [](const QString &text) { return text.trimmed().normalized(QString::NormalizationForm_C).toCaseFolded(); };
  auto nameError = [&](const QString &text, int excludedRow) {
    if (text.trimmed().isEmpty()) return tr("Enter a nonempty set name.");
    for (int row = 0; row < sets.size(); ++row)
      if (row != excludedRow && normalizedName(sets[row].toObject()["name"].toString()) == normalizedName(text))
        return tr("A set already has this name. Choose a different name.");
    return QString();
  };
  auto updateRename = [&]() {
    const int row = list.currentRow();
    const bool selected = row >= 0 && row < sets.size();
    name.setEnabled(selected); restore->setEnabled(selected); remove->setEnabled(selected);
    if (!selected) { rename->setEnabled(false); info->setText(help); return; }
    const QString current = sets[row].toObject()["name"].toString();
    const bool unchanged = name.text().trimmed() == current;
    const QString error = unchanged ? QString() : nameError(name.text(), row);
    rename->setEnabled(contextValid && !unchanged && error.isEmpty());
    info->setText(error.isEmpty() ? help : error);
  };
  auto refresh = [&](int selectedRow) {
    list.clear();
    for (const auto &value : sets) list.addItem(value.toObject()["name"].toString());
    if (!sets.isEmpty()) list.setCurrentRow(std::max(0, std::min(selectedRow, int(sets.size()) - 1)));
    else { name.clear(); updateRename(); }
  };
  auto save = [&](const QJsonArray &updated, int selectedRow) {
    QSettings writer(settingsPath(), QSettings::IniFormat);
    const QVariant previous = writer.value("channelSets");
    writer.setValue("channelSets", QJsonDocument(updated).toJson(QJsonDocument::Compact)); writer.sync();
    if (writer.status() != QSettings::NoError) {
      if (previous.isValid()) writer.setValue("channelSets", previous);
      else writer.remove("channelSets");
      info->setText(tr("The channel sets could not be saved. Check that your preferences folder is writable."));
      return false;
    }
    sets = updated; refresh(selectedRow); return true;
  };
  QObject::connect(&list, &QListWidget::currentRowChanged, &dialog, [&](int row) {
    name.setText(row >= 0 && row < sets.size() ? sets[row].toObject()["name"].toString() : QString());
    updateRename();
  });
  QObject::connect(&name, &QLineEdit::textChanged, &dialog, updateRename);
  auto renameSelected = [&]() {
    const int row = list.currentRow();
    if (!contextValid || row < 0 || row >= sets.size()) return;
    const QString requested = name.text().trimmed();
    QJsonObject entry = sets[row].toObject();
    if (requested == entry["name"].toString()) return;
    const QString error = nameError(requested, row);
    if (!error.isEmpty()) { info->setText(error); return; }
    entry["name"] = requested;
    QJsonArray updated = sets; updated[row] = entry;
    if (save(updated, row)) { name.setFocus(); name.selectAll(); }
  };
  QObject::connect(rename, &QPushButton::clicked, &dialog, renameSelected);
  QObject::connect(&name, &QLineEdit::returnPressed, &dialog, renameSelected);
  auto *editShortcut = new QShortcut(QKeySequence(Qt::Key_F2), &dialog);
  editShortcut->setContext(Qt::WidgetWithChildrenShortcut);
  QObject::connect(editShortcut, &QShortcut::activated, &dialog, [&]() {
    if (name.isEnabled()) { name.setFocus(); name.selectAll(); }
  });
  QObject::connect(capture, &QPushButton::clicked, &dialog, [&]() {
    QInputDialog prompt(&dialog); prompt.setObjectName("FunctionChannelSetCaptureName");
    prompt.setWindowTitle(tr("Channel Set")); prompt.setLabelText(tr("Name"));
    prompt.setInputMode(QInputDialog::TextInput);
    QObject::connect(&dialog, &QDialog::finished, &prompt, &QDialog::reject);
    if (prompt.exec() != QDialog::Accepted || !contextValid) return;
    const QString requested = prompt.textValue().trimmed();
    const QString error = nameError(requested, -1);
    if (!error.isEmpty()) { info->setText(error); return; }
    QJsonArray ids;
    std::vector<std::pair<TDoubleParamP, std::string>> newIdentities;
    std::map<TDoubleParam *, QString> preparedIdentities;
    for (auto *channel : tree->channels()) if (channel->isActive()) {
      auto *curve = channel->getParam();
      QString id = QString::fromStdString(curve->getWorkspaceId());
      if (id.isEmpty()) {
        auto existing = preparedIdentities.find(curve);
        if (existing != preparedIdentities.end()) id = existing->second;
        else {
          id = QUuid::createUuid().toString(QUuid::WithoutBraces);
          preparedIdentities.emplace(curve, id);
          newIdentities.emplace_back(curve, id.toStdString());
        }
      }
      ids.append(id);
    }
    if (ids.isEmpty()) { info->setText(tr("Show some curves before saving a set.")); return; }
    QJsonArray updated = sets; updated.append(QJsonObject{{"name", requested}, {"ids", ids}});
    if (save(updated, int(updated.size()) - 1)) {
      for (const auto &entry : newIdentities) entry.first->setWorkspaceId(entry.second);
      if (!newIdentities.empty() && viewer->getSceneHandle()) viewer->getSceneHandle()->setDirtyFlag(true);
    }
  });
  QObject::connect(restore, &QPushButton::clicked, &dialog, [&]() {
    const int row = list.currentRow(); if (!contextValid || row < 0) return;
    QSet<QString> ids; for (auto value : sets[row].toObject()["ids"].toArray()) ids.insert(value.toString());
    QMap<QString, int> occurrences; for (auto *channel : tree->channels()) ++occurrences[QString::fromStdString(channel->getParam()->getWorkspaceId())];
    for (auto id : ids) if (occurrences[id] > 1) { info->setText(tr("Duplicate channel identity; the set was not applied.")); return; }
    int found = 0;
    viewer->getSelection()->selectNone();
    for (auto *channel : tree->channels()) {
      const bool included = ids.contains(QString::fromStdString(channel->getParam()->getWorkspaceId()));
      channel->setIsActive(included); if (included) ++found;
    }
    info->setText(tr("Showing %1 channels. %2 saved channels are unavailable in this scene.").arg(found).arg(ids.size() - found));
  });
  QObject::connect(remove, &QPushButton::clicked, &dialog, [&]() {
    const int row = list.currentRow(); if (!contextValid || row < 0) return;
    QJsonArray updated = sets; updated.removeAt(row); save(updated, row);
  });
  QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  QWidget::setTabOrder(&list, &name); QWidget::setTabOrder(&name, rename);
  QWidget::setTabOrder(rename, restore); QWidget::setTabOrder(restore, capture);
  QWidget::setTabOrder(capture, remove); QWidget::setTabOrder(remove, buttons.button(QDialogButtonBox::Close));
  refresh(0); list.setFocus(); dialog.exec();
}

void showDependencies(FunctionViewer *viewer, FunctionTreeView *tree) {
  auto *target = viewer->getCurrentChannelCurve(); if (!target) return;
  QDialog dialog(viewer); dialog.setObjectName("FunctionDependenciesDialog"); dialog.setWindowTitle(tr("Channel Dependencies")); QVBoxLayout layout(&dialog);
  bool contextValid = true;
  if (viewer->getSceneHandle()) QObject::connect(viewer->getSceneHandle(), &TSceneHandle::sceneSwitched, &dialog, [&]() { contextValid = false; dialog.reject(); });
  if (viewer->getXsheetHandle()) QObject::connect(viewer->getXsheetHandle(), &TXsheetHandle::xsheetSwitched, &dialog, [&]() { contextValid = false; dialog.reject(); });
  QObject::connect(viewer, &FunctionViewer::workflowContextChanged, &dialog, [&]() { contextValid = false; dialog.reject(); });
  QObject::connect(tree->model(), &QAbstractItemModel::modelAboutToBeReset, &dialog, [&]() { contextValid = false; dialog.reject(); });
  QObject::connect(tree->model(), &QAbstractItemModel::layoutAboutToBeChanged, &dialog, [&]() { contextValid = false; dialog.reject(); });
  QLabel help(tr("Resolved native and SeExpr channel bindings. Double-click a channel to show it. Formula errors are listed with their frame."));
  help.setWordWrap(true); layout.addWidget(&help); QListWidget list; list.setObjectName("FunctionDependenciesList"); layout.addWidget(&list);
  auto all = tree->channels(); QSet<TDoubleParam *> sources;
  for (int k = 0; k < target->getKeyframeCount(); ++k) {
    auto key = target->getKeyframe(k);
    if (key.m_type != TDoubleKeyframe::Expression && key.m_type != TDoubleKeyframe::SimilarShape) continue;
    TExpression expression; expression.setGrammar(target->getGrammar()); expression.setOwnerParameter(target); expression.setText(key.m_expressionText);
    if (!expression.isValid()) { list.addItem(tr("Frame %1: %2").arg(key.m_frame + 1).arg(QString::fromStdString(expression.getError()))); continue; }
    QSet<int> columns; QSet<TDoubleParam *> params; referenceParams(expression, columns, params); sources.unite(params);
  }
  for (int c = 0; c < all.size(); ++c) {
    auto *channel = all[c];
    QString role;
    if (sources.contains(channel->getParam())) role += tr("Source: ");
    if (channel->getParam() != target && dependsOn(channel->getParam(), target)) role += tr("Dependent: ");
    if (role.isEmpty()) continue;
    auto *item = new QListWidgetItem(role + channel->getLongName(), &list); item->setData(Qt::UserRole, c);
    sources.remove(channel->getParam());
  }
  if (!sources.isEmpty()) list.addItem(tr("%1 resolved source channels are outside this editor's visible model.").arg(sources.size()));
  if (!list.count()) list.addItem(tr("No expression dependencies."));
  QObject::connect(&list, &QListWidget::itemDoubleClicked, &dialog, [&](QListWidgetItem *item) {
    if (!contextValid || !item->data(Qt::UserRole).isValid()) return;
    auto *channel = all[item->data(Qt::UserRole).toInt()]; channel->setIsActive(true); channel->setIsCurrent(true);
    tree->scrollTo(channel->createIndex()); dialog.accept();
  });
  QDialogButtonBox buttons(QDialogButtonBox::Close); layout.addWidget(&buttons);
  QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); dialog.exec();
}

class CurveThumbnail final : public QWidget {
  TDoubleParamP m_curve;
public:
  CurveThumbnail(QWidget *parent) : QWidget(parent) { setMinimumSize(260, 110); }
  void setCurve(const TDoubleParamP &curve) { m_curve = curve; update(); }
  void paintEvent(QPaintEvent *) override {
    QPainter p(this); p.fillRect(rect(), palette().base());
    if (!m_curve || m_curve->getKeyframeCount() < 1) return;
    double first = m_curve->keyframeIndexToFrame(0);
    double last = m_curve->keyframeIndexToFrame(m_curve->getKeyframeCount() - 1);
    QVector<double> values; double low = 1.e100, high = -low;
    for (int i = 0; i <= 128; ++i) {
      double value = m_curve->getValue(first + (last - first) * i / 128.);
      values.append(value);
      if (std::isfinite(value)) { low = std::min(low, value); high = std::max(high, value); }
    }
    double span = high > low ? high - low : 1;
    QPainterPath path; bool start = true;
    for (int i = 0; i < values.size(); ++i) {
      if (!std::isfinite(values[i])) { start = true; continue; }
      QPointF point(10 + (width() - 20) * i / 128., height() - 10 - (height() - 20) * (values[i] - low) / span);
      if (start) path.moveTo(point); else path.lineTo(point); start = false;
    }
    p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(palette().highlight().color(), 2)); p.drawPath(path);
  }
};

void showPresets(FunctionViewer *viewer, FunctionPanel *graph) {
  auto savedComparison = graph->comparisonState();
  auto *target = viewer->getCurrentChannelCurve(); if (!target) return;
  TDoubleParamP keepTarget(target);
  QDialog dialog(viewer); dialog.setObjectName("FunctionMotionPresets"); dialog.setWindowTitle(tr("Motion Curve Presets")); QVBoxLayout layout(&dialog);
  bool contextValid = true;
  if (viewer->getSceneHandle()) QObject::connect(viewer->getSceneHandle(), &TSceneHandle::sceneSwitched, &dialog, [&]() { contextValid = false; dialog.reject(); });
  if (viewer->getXsheetHandle()) QObject::connect(viewer->getXsheetHandle(), &TXsheetHandle::xsheetSwitched, &dialog, [&]() { contextValid = false; dialog.reject(); });
  QObject::connect(viewer, &FunctionViewer::workflowContextChanged, &dialog, [&]() { contextValid = false; dialog.reject(); });
  QObject::connect(graph->getModel(), &QAbstractItemModel::modelAboutToBeReset, &dialog, [&]() { contextValid = false; dialog.reject(); });
  QString folder = (ToonzFolder::getMyModuleDir() + TFilePath("function-presets")).getQString();
  QDir directory(folder);
  QSettings settings(settingsPath(), QSettings::IniFormat);
  QStringList favorites = settings.value("presetFavorites").toStringList();
  QLineEdit search; search.setObjectName("FunctionPresetSearch"); search.setPlaceholderText(tr("Search motion presets")); search.setClearButtonEnabled(true); layout.addWidget(&search);
  QListWidget list; list.setObjectName("FunctionPresetList"); layout.addWidget(&list); CurveThumbnail thumbnail(&dialog); layout.addWidget(&thumbnail);
  QCheckBox favorite(tr("Favorite")); favorite.setObjectName("FunctionPresetFavorite"); layout.addWidget(&favorite);
  QFormLayout form;
  auto *start = number(&dialog, 1, 1), *duration = number(&dialog, 24, .000001);
  auto *amplitude = number(&dialog, 1), *offset = number(&dialog, 0);
  start->setObjectName("FunctionPresetFirstFrame"); duration->setObjectName("FunctionPresetDuration");
  amplitude->setObjectName("FunctionPresetAmplitude"); offset->setObjectName("FunctionPresetOffset");
  form.addRow(tr("First frame"), start); form.addRow(tr("Duration (frames)"), duration);
  form.addRow(tr("Amplitude multiplier"), amplitude); form.addRow(tr("Value offset (display units)"), offset);
  layout.addLayout(&form);
  QLabel status(tr("Applying replaces this channel's keys as one undo step. Formula/file presets must be baked first; foreign references are never imported implicitly."));
  status.setObjectName("FunctionPresetStatus");
  status.setWordWrap(true); layout.addWidget(&status);
  QDialogButtonBox buttons(QDialogButtonBox::Apply | QDialogButtonBox::Close);
  auto *save = buttons.addButton(tr("Save current curve…"), QDialogButtonBox::ActionRole);
  auto *importButton = buttons.addButton(tr("Import existing curve…"), QDialogButtonBox::ActionRole);
  importButton->setObjectName("FunctionPresetImport"); save->setObjectName("FunctionPresetSave");
  auto *previewButton = buttons.addButton(tr("Preview"), QDialogButtonBox::ActionRole);
  previewButton->setObjectName("FunctionPresetPreview"); layout.addWidget(&buttons);
  TDoubleParamP preset;
  auto readPreset = [&](const QString &path) {
    if (QFileInfo(path).size() > 2 * 1024 * 1024) throw std::runtime_error("Preset is larger than 2 MiB");
    TIStream input{TFilePath(path)};
    TDoubleParamP loaded;
    if (QFileInfo(path).suffix().compare("curve", Qt::CaseInsensitive) == 0) {
      // Native Save Curve stores raw TDoubleParam data without a measure.
      // Match native Load Curve: retain the destination measure/default, then
      // validate the candidate before any preview, library write or mutation.
      loaded = new TDoubleParam(target->getDefaultValue());
      loaded->setMeasureName(target->getMeasureName());
      loaded->loadData(input);
      loaded->setDefaultValue(target->getDefaultValue());
    } else {
      loaded = new TDoubleParam;
      std::string tag;
      if (!input.matchTag(tag) || tag != "functionPreset") throw std::runtime_error("Invalid curve preset");
      std::string measure; input.getTagParam("measure", measure); loaded->setMeasureName(measure);
      loaded->loadData(input); input.closeChild();
    }
    loaded->setWorkspaceId(std::string());
    return loaded;
  };
  auto publishPreset = [&](const QString &name, const TDoubleParamP &value) {
    if (name.isEmpty() || name.contains('/') || name.contains('\\') || name.contains(':') || name.contains("..") || name.size() > 80 || name.contains('*') || name.contains('?') || name.contains('"') || name.contains('<') || name.contains('>') || name.contains('|') || name.endsWith('.')) { status.setText(tr("Use a short file name without path separators.")); return false; }
    if (!QDir().mkpath(folder)) { status.setText(tr("Cannot create the preset folder.")); return false; }
    const QString path = directory.filePath(name + ".fcurve");
    if (QFileInfo::exists(path)) { status.setText(tr("A preset with this name already exists. Choose another name.")); return false; }
    const QString temporary = path + "." + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".tmp";
    try {
      {
        TOStream output{TFilePath(temporary)};
        output.openChild("functionPreset", {{"measure", value->getMeasureName()}});
        TDoubleParamP copy = new TDoubleParam(*value); copy->saveData(output); output.closeChild();
        if (!output.checkStatus()) throw std::runtime_error("Preset write failed");
      }
      if (!QFile::rename(temporary, path)) throw std::runtime_error("Preset publication failed");
      return true;
    } catch (...) { QFile::remove(temporary); status.setText(tr("Could not save the preset; existing presets were preserved.")); return false; }
  };
  auto refresh = [&]() {
    list.clear();
    QStringList files = directory.entryList({"*.fcurve"}, QDir::Files, QDir::Name);
    std::stable_sort(files.begin(), files.end(), [&](const QString &a, const QString &b) { return favorites.contains(a) > favorites.contains(b); });
    for (const QString &file : files) {
      auto *item = new QListWidgetItem((favorites.contains(file) ? QString::fromUtf8("★ ") : QString()) + QFileInfo(file).completeBaseName(), &list);
      item->setData(Qt::UserRole, file); item->setHidden(!QFileInfo(file).completeBaseName().contains(search.text(), Qt::CaseInsensitive));
    }
  };
  QObject::connect(&list, &QListWidget::currentItemChanged, &dialog, [&](QListWidgetItem *item) {
    preset = TDoubleParamP(); thumbnail.setCurve(preset); graph->setComparison({}); if (!item) return;
    QString file = item->data(Qt::UserRole).toString(); QSignalBlocker block(&favorite); favorite.setChecked(favorites.contains(file));
    try {
      TDoubleParamP loaded = readPreset(directory.filePath(file));
      // Use the same numeric-only validation as Apply before evaluating previews.
      auto validation = Ops::planPreset(target, loaded.getPointer(), 0, 24, 1, 0);
      if (!validation.valid()) { status.setText(validation.error); return; }
      preset = loaded; thumbnail.setCurve(preset);
      int count = preset->getKeyframeCount();
      if (count > 1) duration->setValue(preset->keyframeIndexToFrame(count - 1) - preset->keyframeIndexToFrame(0));
      status.setText(tr("Preset loaded. Preview to compare; Apply replaces the current channel's keys."));
    } catch (...) { status.setText(tr("The preset could not be read. The current curve was not changed.")); }
  });
  QObject::connect(&search, &QLineEdit::textChanged, &dialog, [&](const QString &text) {
    for (int row = 0; row < list.count(); ++row) {
      auto *item = list.item(row);
      item->setHidden(!QFileInfo(item->data(Qt::UserRole).toString()).completeBaseName().contains(text, Qt::CaseInsensitive));
    }
    if (list.currentItem() && list.currentItem()->isHidden()) list.setCurrentRow(-1);
  });
  QObject::connect(&favorite, &QCheckBox::toggled, &dialog, [&](bool enabled) {
    if (!list.currentItem()) return; QString file = list.currentItem()->data(Qt::UserRole).toString();
    favorites.removeAll(file); if (enabled) favorites.append(file);
    settings.setValue("presetFavorites", favorites); settings.sync();
  });
  QObject::connect(save, &QPushButton::clicked, &dialog, [&]() {
    QInputDialog prompt(&dialog); prompt.setObjectName("FunctionPresetSaveName");
    prompt.setWindowTitle(tr("Save Motion Preset")); prompt.setLabelText(tr("Name"));
    prompt.setInputMode(QInputDialog::TextInput);
    QObject::connect(&dialog, &QDialog::finished, &prompt, &QDialog::reject);
    if (prompt.exec() != QDialog::Accepted || !contextValid) return;
    const QString name = prompt.textValue().trimmed();
    if (name.isEmpty()) return;
    auto validation = Ops::planPreset(target, target, 0, 24, 1, 0);
    if (!validation.valid()) { status.setText(validation.error); return; }
    if (publishPreset(name, keepTarget)) { refresh(); status.setText(tr("Preset saved.")); }
  });
  QObject::connect(importButton, &QPushButton::clicked, &dialog, [&]() {
    QFileDialog file(&dialog, tr("Import Existing Motion Curve"));
    file.setObjectName("FunctionPresetImportFile");
    file.setOption(QFileDialog::DontUseNativeDialog);
    file.setFileMode(QFileDialog::ExistingFile);
    file.setNameFilter(tr("Motion curves (*.curve *.fcurve)"));
    QObject::connect(&dialog, &QDialog::finished, &file, &QDialog::reject);
    if (file.exec() != QDialog::Accepted || !contextValid || file.selectedFiles().isEmpty()) return;
    const QString path = file.selectedFiles().front();
    try {
      const TDoubleParamP loaded = readPreset(path);
      const auto validation = Ops::planPreset(target, loaded.getPointer(), 0, 24, 1, 0);
      if (!validation.valid()) { status.setText(validation.error); return; }
      const QString interpretation = QFileInfo(path).suffix().compare("curve", Qt::CaseInsensitive) == 0
          ? tr("Native .curve files do not store a measure. This curve will use the current channel's %1 measure. Preset name:").arg(QString::fromStdString(target->getMeasureName()).isEmpty() ? tr("unitless") : QString::fromStdString(target->getMeasureName()))
          : tr("Preset name:");
      QInputDialog prompt(&dialog); prompt.setObjectName("FunctionPresetImportName");
      prompt.setWindowTitle(tr("Import Motion Preset")); prompt.setLabelText(interpretation);
      prompt.setInputMode(QInputDialog::TextInput); prompt.setTextValue(QFileInfo(path).completeBaseName());
      QObject::connect(&dialog, &QDialog::finished, &prompt, &QDialog::reject);
      if (prompt.exec() != QDialog::Accepted || !contextValid) return;
      const QString name = prompt.textValue().trimmed();
      if (publishPreset(name, loaded)) {
        // An imported preset becomes current explicitly, so reveal it even
        // when the previous search would hide its name.
        search.clear(); refresh();
        for (int row = 0; row < list.count(); ++row)
          if (list.item(row)->data(Qt::UserRole).toString() == name + ".fcurve") { list.setCurrentRow(row); break; }
        status.setText(tr("Imported numeric curve. Preview and Apply to adapt it; the current channel is unchanged."));
      }
    } catch (...) { status.setText(tr("The curve could not be imported. The library and current channel were not changed.")); }
  });
  auto makePlan = [&]() {
    if (!contextValid) { Ops::Plan plan; plan.cancelled = true; return plan; }
    return Ops::planPreset(target, preset.getPointer(), start->value() - 1, duration->value(), amplitude->value(), offset->value());
  };
  QObject::connect(previewButton, &QPushButton::clicked, &dialog, [&]() {
    auto plan = makePlan(); preview(graph, plan); status.setText(plan.valid() ? tr("Dashed curve is the proposed result.") : plan.error);
  });
  QObject::connect(buttons.button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [&]() {
    auto plan = makePlan(); QString error;
    if (!Ops::apply(plan, tr("Apply Motion Preset"), error)) { status.setText(error); return; }
    if (plan.changes.empty()) { status.setText(tr("No keys changed.")); graph->setComparison({}); return; }
    viewer->getSelection()->selectNone(); viewer->emitCurveChanged(); graph->setComparison({}); graph->refreshNormalization(); graph->update();
    status.setText(tr("Preset applied as one undo step."));
  });
  QObject::connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  refresh(); dialog.exec(); restoreOverlay(viewer, graph, savedComparison, contextValid);
}
} // namespace

QWidget *createFunctionWorkflowControls(FunctionViewer *viewer, FunctionTreeView *tree, FunctionPanel *graph) {
  auto *widget = new QWidget(viewer); auto *layout = new QVBoxLayout(widget); layout->setContentsMargins(0, 0, 0, 0);
  auto *search = new QLineEdit(widget); search->setObjectName("FunctionChannelSearch");
  search->setPlaceholderText(tr("Search channels, objects or references")); search->setClearButtonEnabled(true);
  search->setToolTip(tr("Filters the channel browser only. Active curves and selected keys remain visible in the graph and spreadsheet."));
  layout->addWidget(search); QObject::connect(search, &QLineEdit::textChanged, tree, &FunctionTreeView::setSearchText);
  auto *row = new QHBoxLayout; layout->addLayout(row);
  auto button = [&](const QString &text) { auto *b = new QToolButton(widget); b->setText(text); b->setMinimumHeight(26); row->addWidget(b); return b; };
  auto *edit = button(tr("Edit Keys…")); edit->setObjectName("FunctionEditKeys");
  QObject::connect(edit, &QToolButton::clicked, widget, [=]() { showEdit(viewer, graph); });
  auto *fit = button(tr("Fit Selected")); QObject::connect(fit, &QToolButton::clicked, graph, &FunctionPanel::fitSelectedPoints);
  auto *more = button(tr("Tools")); more->setPopupMode(QToolButton::InstantPopup); auto *menu = new QMenu(more); more->setMenu(menu);
  menu->addAction(tr("Channel Sets…"), widget, [=]() { showSets(viewer, tree); })->setObjectName("FunctionChannelSetsAction");
  menu->addAction(tr("Motion Presets…"), widget, [=]() { showPresets(viewer, graph); })->setObjectName("FunctionMotionPresetsAction");
  menu->addAction(tr("Dependencies…"), widget, [=]() { showDependencies(viewer, tree); })->setObjectName("FunctionDependenciesAction");
  auto *normalize = menu->addAction(tr("Normalize curve display")); normalize->setCheckable(true);
  QObject::connect(normalize, &QAction::toggled, graph, &FunctionPanel::setNormalized);
  menu->addAction(tr("Fit all visible curves"), graph, &FunctionPanel::fitCurve);
  auto soloState = std::make_shared<std::vector<std::pair<TDoubleParamP, bool>>>();
  menu->addAction(tr("Solo current / restore visible curves"), widget, [=]() {
    auto channels = tree->channels();
    viewer->getSelection()->selectNone();
    if (soloState->empty()) {
      auto *target = viewer->getCurrentChannelCurve(); if (!target) return;
      for (auto *channel : channels) { soloState->emplace_back(channel->getParam(), channel->isActive()); channel->setIsActive(channel->getParam() == target); }
    } else {
      for (auto *channel : channels) for (const auto &saved : *soloState) if (saved.first.getPointer() == channel->getParam()) channel->setIsActive(saved.second);
      soloState->clear();
    }
    graph->update();
  });
  menu->addAction(tr("Lock / unlock selected channels"), widget, [=]() {
    auto targets = curves(selected(viewer->getSelection()));
    if (targets.empty() && viewer->getCurrentChannelCurve()) targets.push_back(viewer->getCurrentChannelCurve());
    bool lock = false; for (const auto &curve : targets) if (!curve->isEditorLocked()) lock = true;
    for (const auto &curve : targets) curve->setEditorLocked(lock);
    graph->update(); tree->update(); viewer->onCurveSelected(viewer->getCurrentChannelCurve());
  });
  auto *interpolationButton = button(tr("Interpolation")); interpolationButton->setObjectName("FunctionInterpolation");
  interpolationButton->setPopupMode(QToolButton::InstantPopup);
  auto *interpolation = new QMenu(interpolationButton); interpolationButton->setMenu(interpolation);
  const std::pair<const char *, TDoubleKeyframe::Type> modes[] = {
    {"Constant", TDoubleKeyframe::Constant}, {"Linear", TDoubleKeyframe::Linear},
    {"Speed In / Speed Out", TDoubleKeyframe::SpeedInOut}, {"Ease In / Ease Out", TDoubleKeyframe::EaseInOut},
    {"Ease In / Ease Out %", TDoubleKeyframe::EaseInOutPercentage}, {"Exponential", TDoubleKeyframe::Exponential}};
  for (auto mode : modes) interpolation->addAction(tr(mode.first), widget, [=]() { viewer->getSelection()->setSegmentType(mode.second); })->setData(int(mode.second));
  for (bool link : {true, false}) interpolation->addAction(link ? tr("Link selected handles") : tr("Unlink selected handles"), widget, [=]() {
    auto selection = selected(viewer->getSelection()); if (viewer->getSelection()->hasLockedCurves()) return;
    TUndoManager::manager()->beginBlock();
    for (const auto &entry : selection) for (int k : entry.indices) { KeyframeSetter setter(entry.curve.getPointer(), k); if (link) setter.linkHandles(); else setter.unlinkHandles(); }
    TUndoManager::manager()->endBlock(); graph->update();
  })->setObjectName(link ? "FunctionLinkHandles" : "FunctionUnlinkHandles");
  interpolation->addAction(tr("Reset selected handles"), widget, [=]() {
    auto selection = selected(viewer->getSelection()); if (viewer->getSelection()->hasLockedCurves()) return;
    TUndoManager::manager()->beginBlock();
    for (const auto &entry : selection) for (int k : entry.indices) {
      KeyframeSetter setter(entry.curve.getPointer(), k);
      if (setter.isSpeedInOut(k - 1)) setter.setSpeedIn(TPointD(-5, 0));
      else if (setter.isEaseInOut(k - 1)) setter.setEaseIn(5);
      if (setter.isSpeedInOut(k)) setter.setSpeedOut(TPointD(5, 0));
      else if (setter.isEaseInOut(k)) setter.setEaseOut(5);
    }
    TUndoManager::manager()->endBlock(); graph->update();
  })->setObjectName("FunctionResetHandles");
  auto snapshots = std::make_shared<std::vector<std::pair<TDoubleParamP, TDoubleParamP>>>();
  auto capturedDisplay = std::make_shared<FunctionPanel::ComparisonState>();
  auto captureGeneration = std::make_shared<unsigned long long>(0);
  auto *comparisonInfo = new QLabel(widget); comparisonInfo->setObjectName("FunctionComparisonInfo");
  comparisonInfo->setWordWrap(true); comparisonInfo->hide(); layout->addWidget(comparisonInfo);
  auto clearContext = [=]() { ++*captureGeneration; snapshots->clear(); *capturedDisplay = FunctionPanel::ComparisonState(); comparisonInfo->clear(); comparisonInfo->hide(); soloState->clear(); graph->setComparison({}); widget->setProperty("comparisonVisible", false); };
  QObject::connect(viewer, &FunctionViewer::workflowContextChanged, widget, clearContext);
  QObject::connect(tree->model(), &QAbstractItemModel::modelAboutToBeReset, widget, clearContext);
  QObject::connect(tree->model(), &QAbstractItemModel::layoutChanged, widget, [=]() {
    QSet<TDoubleParam *> present; for (auto *channel : tree->channels()) present.insert(channel->getParam());
    auto missing = [&](const std::pair<TDoubleParamP, TDoubleParamP> &entry) { return !present.contains(entry.first.getPointer()); };
    const auto previousCount = snapshots->size();
    snapshots->erase(std::remove_if(snapshots->begin(), snapshots->end(), missing), snapshots->end());
    if (snapshots->size() != previousCount) ++*captureGeneration;
    capturedDisplay->curves.erase(std::remove_if(capturedDisplay->curves.begin(), capturedDisplay->curves.end(), missing), capturedDisplay->curves.end());
    capturedDisplay->frozen.erase(std::remove_if(capturedDisplay->frozen.begin(), capturedDisplay->frozen.end(), [&](const FunctionPanel::FrozenComparisonCurve &entry) { return !present.contains(entry.owner.getPointer()); }), capturedDisplay->frozen.end());
    soloState->erase(std::remove_if(soloState->begin(), soloState->end(), [&](const std::pair<TDoubleParamP, bool> &entry) { return !present.contains(entry.first.getPointer()); }), soloState->end());
    if (widget->property("comparisonVisible").toBool()) graph->setComparisonState(*capturedDisplay);
  });
  menu->addAction(tr("Capture comparison curves"), widget, [=]() {
    const auto previous = graph->comparisonState();
    std::vector<std::pair<TDoubleParamP, TDoubleParamP>> proposed;
    for (auto *channel : tree->channels()) if (channel->isActive()) proposed.emplace_back(channel->getParam(), new TDoubleParam(*channel->getParam()));
    graph->setComparison(proposed);
    QString error;
    if (!graph->freezeComparison(error)) { graph->setComparisonState(previous); QMessageBox::warning(viewer, tr("Capture Curves"), error); return; }
    ++*captureGeneration; *snapshots = std::move(proposed); *capturedDisplay = graph->comparisonState(); widget->setProperty("comparisonVisible", true);
    comparisonInfo->setText(tr("Comparison captured at frames %1–%2 with %3 sample intervals (%4 frames/sample). Display between samples is approximate. Recapture to extend the range or increase detail.")
        .arg(capturedDisplay->first + 1, 0, 'g', 8).arg(capturedDisplay->last + 1, 0, 'g', 8)
        .arg(capturedDisplay->intervals).arg((capturedDisplay->last - capturedDisplay->first) / capturedDisplay->intervals, 0, 'g', 6));
    comparisonInfo->show();
  })->setObjectName("FunctionCaptureComparison");
  menu->addAction(tr("Show / hide comparison"), widget, [=]() {
    bool visible = widget->property("comparisonVisible").toBool(); widget->setProperty("comparisonVisible", !visible);
    if (visible) graph->setComparison({}); else graph->setComparisonState(*capturedDisplay);
    comparisonInfo->setVisible(!visible && !capturedDisplay->curves.empty());
  })->setObjectName("FunctionToggleComparison");
  menu->addAction(tr("Restore captured curves…"), widget, [=]() {
    if (snapshots->empty()) return;
    const auto generation = *captureGeneration;
    if (QMessageBox::question(viewer, tr("Restore Curves"), tr("Restore all captured channels? Edits since capture will be replaced as one undo step.")) != QMessageBox::Yes) return;
    if (*captureGeneration != generation || snapshots->empty()) return;
    QSet<TDoubleParam *> present; for (auto *channel : tree->channels()) present.insert(channel->getParam());
    for (const auto &snapshot : *snapshots) if (!present.contains(snapshot.first.getPointer())) { QMessageBox::warning(viewer, tr("Restore Curves"), tr("A captured channel no longer belongs to this scene. Capture a new comparison.")); return; }
    Ops::Plan plan = Ops::planRestore(*snapshots);
    QString error; if (!Ops::apply(plan, tr("Restore Comparison Curves"), error)) QMessageBox::warning(viewer, tr("Restore Curves"), error);
    else if (plan.changes.empty()) return;
    else { viewer->getSelection()->selectNone(); viewer->emitCurveChanged(); graph->refreshNormalization(); graph->update(); }
  })->setObjectName("FunctionRestoreComparison");
  QSettings settings(settingsPath(), QSettings::IniFormat);
  auto snap = std::make_shared<int>(settings.value("snapFrames", 1).toInt());
  auto *snapKeys = menu->addAction(tr("Snap near other keys")); snapKeys->setCheckable(true); snapKeys->setChecked(settings.value("snapKeys", false).toBool());
  graph->setSnap(*snap, snapKeys->isChecked());
  auto *snapMarkers = menu->addAction(tr("Snap near scene frame markers")); snapMarkers->setCheckable(true);
  snapMarkers->setChecked(settings.value("snapMarkers", false).toBool()); graph->setSnapMarkers(snapMarkers->isChecked());
  QObject::connect(snapMarkers, &QAction::toggled, widget, [=](bool enabled) { graph->setSnapMarkers(enabled); QSettings s(settingsPath(), QSettings::IniFormat); s.setValue("snapMarkers", enabled); });
  QObject::connect(snapKeys, &QAction::toggled, widget, [=](bool enabled) { graph->setSnap(*snap, enabled); QSettings s(settingsPath(), QSettings::IniFormat); s.setValue("snapKeys", enabled); });
  menu->addAction(tr("Set frame snap increment…"), widget, [=]() {
    bool ok; int n = QInputDialog::getInt(viewer, tr("Frame Snap"), tr("Frames (Alt temporarily bypasses grid/key snapping)"), *snap, 1, 1000, 1, &ok);
    if (ok) { *snap = n; graph->setSnap(n, snapKeys->isChecked()); QSettings s(settingsPath(), QSettings::IniFormat); s.setValue("snapFrames", n); }
  });
  graph->setHitRadius(settings.value("hitRadius", 10).toInt());
  menu->addAction(tr("Set handle hit size…"), widget, [=]() {
    bool ok; QSettings s(settingsPath(), QSettings::IniFormat); int radius = QInputDialog::getInt(viewer, tr("Graph Handles"), tr("Hit radius (logical pixels)"), s.value("hitRadius", 10).toInt(), 6, 24, 1, &ok);
    if (ok) { graph->setHitRadius(radius); s.setValue("hitRadius", radius); }
  });
  graph->setValueNudge(settings.value("valueNudge", 1.).toDouble());
  menu->addAction(tr("Set keyboard value increment…"), widget, [=]() {
    bool ok; QSettings s(settingsPath(), QSettings::IniFormat); double value = QInputDialog::getDouble(viewer, tr("Keyboard Nudges"), tr("Display units (arrows; Shift multiplies by 10)"), s.value("valueNudge", 1.).toDouble(), .000001, 1.e6, 6, &ok);
    if (ok) { graph->setValueNudge(value); s.setValue("valueNudge", value); }
  });
  menu->addAction(tr("Graph editing help"), widget, [=]() { QMessageBox::information(viewer, tr("Function Editor"), tr("Arrows nudge selected keys; Shift uses ten increments. Shift-drag constrains one axis. Alt-drag bypasses grid/key/marker snapping for ordinary moves. Alt stretches a contiguous selection of three or more keys; Shift+Alt keeps a constrained move and bypasses snapping. Search filters the browser, keeping active curves visible. Locked channels remain readable; editor writes are rejected. Dashed comparison and normalized display never change animation values.")); });
  return widget;
}
