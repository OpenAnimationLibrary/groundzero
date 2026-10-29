// Whole-rig breakdown and ease commands for the Xsheet and the Timeline.
// See doc/rig_breakdown_ease.md.

#include "rigeasecommand.h"

// Tnz6 includes
#include "menubarcommandids.h"
#include "cellselection.h"
#include "columnselection.h"
#include "tapp.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"
#include "toonzqt/tselectionhandle.h"

// TnzLib includes
#include "toonz/doubleparamcmd.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"

// TnzBase includes
#include "tdoubleparam.h"
#include "tdoublekeyframe.h"

// TnzCore includes
#include "tundo.h"

// Qt includes
#include <QAction>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QMainWindow>
#include <QTimer>

#include <algorithm>
#include <set>
#include <vector>

namespace {

//-----------------------------------------------------------------------------

// Refreshes the Xsheet / Timeline when the whole command is done, undone or
// redone. It is the last entry of the command's undo block, so on undo it runs
// before the curves are restored: the refresh is repeated once the undo is
// over.
class RigEaseNotifyUndo final : public TUndo {
public:
  static void notify() {
    TApp *app = TApp::instance();
    app->getCurrentScene()->setDirtyFlag(true);
    TXsheetHandle *xshHandle = app->getCurrentXsheet();
    xshHandle->notifyXsheetChanged();
    QTimer::singleShot(0, xshHandle,
                       [xshHandle]() { xshHandle->notifyXsheetChanged(); });
  }
  void undo() const override { notify(); }
  void redo() const override { notify(); }
  int getSize() const override { return sizeof(*this); }
};

//-----------------------------------------------------------------------------

// The animatable channels of a stage object, as in the Function Editor.
const TStageObject::Channel cChannels[] = {
    TStageObject::T_Angle,  TStageObject::T_X,      TStageObject::T_Y,
    TStageObject::T_Z,      TStageObject::T_SO,     TStageObject::T_ScaleX,
    TStageObject::T_ScaleY, TStageObject::T_Scale,  TStageObject::T_ShearX,
    TStageObject::T_ShearY, TStageObject::T_RotX,   TStageObject::T_RotY,
    TStageObject::T_Path,   TStageObject::T_FocusZ, TStageObject::T_Aperture};

//-----------------------------------------------------------------------------

// Finds the animated channels the commands work on: those of the stage
// objects of the selected columns (cell or column selection, else the current
// column) and, if "Include Child Objects" is on, of the objects attached to
// them, directly or not. rowRange is the range of the selected rows when a
// cell selection spans more than one row, else the current frame.
bool getTargetCurves(std::vector<TDoubleParam *> &curves, int &frame,
                     std::pair<int, int> &rowRange) {
  TApp *app = TApp::instance();
  // the frame is a row of the xsheet only when editing the scene
  if (!app->getCurrentFrame()->isEditingScene()) return false;
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  if (!xsh) return false;

  frame    = app->getCurrentFrame()->getFrame();
  rowRange = std::make_pair(frame, frame);

  std::set<int> columns;
  TSelection *selection = app->getCurrentSelection()->getSelection();
  if (TCellSelection *cellSelection =
          dynamic_cast<TCellSelection *>(selection)) {
    if (!cellSelection->isEmpty()) {
      int r0, c0, r1, c1;
      cellSelection->getSelectedCells(r0, c0, r1, c1);
      for (int c = std::min(c0, c1); c <= std::max(c0, c1); c++)
        columns.insert(c);
      if (r0 != r1)
        rowRange = std::make_pair(std::min(r0, r1), std::max(r0, r1));
    }
  } else if (TColumnSelection *columnSelection =
                 dynamic_cast<TColumnSelection *>(selection)) {
    if (!columnSelection->isEmpty()) columns = columnSelection->getIndices();
  }
  if (columns.empty())
    columns.insert(app->getCurrentColumn()->getColumnIndex());

  // the camera column (-1) has no column stage object
  std::set<TStageObjectId> roots;
  for (int c : columns)
    if (c >= 0) roots.insert(TStageObjectId::ColumnId(c));
  if (roots.empty()) return false;

  TStageObjectTree *tree           = xsh->getStageObjectTree();
  std::set<TStageObjectId> targets = roots;
  QAction *includeChildren =
      CommandManager::instance()->getAction(MI_RigIncludeChildren);
  if (includeChildren && includeChildren->isChecked()) {
    int count = tree->getStageObjectCount();
    for (int i = 0; i < count; i++) {
      TStageObject *object = tree->getStageObject(i);
      // climb the parents; the step limit guards against a loop in the tree
      TStageObjectId parentId = object->getParent();
      for (int step = 0; step < count && parentId != TStageObjectId::NoneId;
           step++) {
        if (roots.count(parentId)) {
          targets.insert(object->getId());
          break;
        }
        TStageObject *parent = tree->getStageObject(parentId, false);
        if (!parent) break;
        parentId = parent->getParent();
      }
    }
  }

  for (const TStageObjectId &id : targets) {
    TStageObject *object = tree->getStageObject(id, false);
    if (!object) continue;
    for (TStageObject::Channel channel : cChannels) {
      TDoubleParam *curve = object->getParam(channel);
      if (curve && curve->hasKeyframes()) curves.push_back(curve);
    }
  }
  return !curves.empty();
}

//-----------------------------------------------------------------------------

// One undo step: the keys are made by the same helper as the Function
// Spreadsheet's Set Breakdown Key.
void insertRigBreakdown(double favourPercent) {
  std::vector<TDoubleParam *> curves;
  std::pair<int, int> rowRange;
  int frame;
  if (!getTargetCurves(curves, frame, rowRange)) return;

  int changed           = 0;
  TUndoManager *manager = TUndoManager::manager();
  manager->beginBlock();
  for (TDoubleParam *curve : curves)
    if (KeyframeSetter::insertBreakdown(curve, frame, favourPercent / 100.0))
      changed++;
  if (changed > 0) {
    RigEaseNotifyUndo::notify();
    manager->add(new RigEaseNotifyUndo());
  }
  manager->endBlock();
}

//-----------------------------------------------------------------------------

// Segments of the types below keep their interpolation: a hold, an
// expression, a file or a similar shape is not an animation curve to ease.
bool canBeEased(TDoubleKeyframe::Type type) {
  return type != TDoubleKeyframe::Constant &&
         type != TDoubleKeyframe::Expression && type != TDoubleKeyframe::File &&
         type != TDoubleKeyframe::SimilarShape;
}

// One undo step: the segments are set by the same helper as the Function
// Spreadsheet's Ease Presets.
void applyRigEase(double easeOut, double easeIn) {
  std::vector<TDoubleParam *> curves;
  std::pair<int, int> rowRange;
  int frame;
  if (!getTargetCurves(curves, frame, rowRange)) return;

  int changed           = 0;
  TUndoManager *manager = TUndoManager::manager();
  manager->beginBlock();
  for (TDoubleParam *curve : curves) {
    int keyCount = curve->getKeyframeCount();
    // The segments overlapping the rows: [first key, next key) meets
    // [row0, row1]. On the last key of a curve, the segment before it.
    std::vector<int> segments;
    for (int k = 0; k + 1 < keyCount; k++) {
      double f0 = curve->keyframeIndexToFrame(k);
      double f1 = curve->keyframeIndexToFrame(k + 1);
      bool hit  = f0 <= rowRange.second && f1 > rowRange.first;
      if (!hit && k + 2 == keyCount && rowRange.first == rowRange.second &&
          f1 == rowRange.first)
        hit = true;
      if (hit && canBeEased(curve->getKeyframe(k).m_type))
        segments.push_back(k);
    }
    for (int k : segments) {
      KeyframeSetter::setSegmentEase(curve, k, easeOut, easeIn);
      changed++;
    }
  }
  if (changed > 0) {
    RigEaseNotifyUndo::notify();
    manager->add(new RigEaseNotifyUndo());
  }
  manager->endBlock();
}

//-----------------------------------------------------------------------------

QWidget *dialogParent() { return TApp::instance()->getMainWindow(); }

// Asks for a breakdown percentage. Returns false if canceled.
bool getCustomBreakdown(double &favour) {
  bool ok      = false;
  double value = QInputDialog::getDouble(
      dialogParent(),
      QCoreApplication::translate("RigEaseCommand", "Custom Rig Breakdown"),
      QCoreApplication::translate("RigEaseCommand",
                                  "Position Between Keys (0% keeps the "
                                  "previous key, 100% reaches the next):"),
      favour, -100.0, 200.0, 1, &ok);
  if (ok) favour = value;
  return ok;
}

// Asks for Ease Out and Ease In percentages. The fields keep their sum at
// 100% or less. Returns false if canceled.
bool getCustomEase(double &easeOut, double &easeIn) {
  QDialog dialog(dialogParent());
  dialog.setWindowTitle(
      QCoreApplication::translate("RigEaseCommand", "Custom Rig Ease"));

  QDoubleSpinBox *easeOutFld = new QDoubleSpinBox(&dialog);
  QDoubleSpinBox *easeInFld  = new QDoubleSpinBox(&dialog);
  for (QDoubleSpinBox *field : {easeOutFld, easeInFld}) {
    field->setRange(0.0, 100.0);
    field->setDecimals(1);
    field->setSuffix("%");
  }
  easeOutFld->setValue(easeOut);
  easeInFld->setMaximum(100.0 - easeOutFld->value());
  easeInFld->setValue(easeIn);
  easeOutFld->setMaximum(100.0 - easeInFld->value());
  QObject::connect(
      easeOutFld, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      easeInFld,
      [easeInFld](double value) { easeInFld->setMaximum(100.0 - value); });
  QObject::connect(
      easeInFld, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
      easeOutFld,
      [easeOutFld](double value) { easeOutFld->setMaximum(100.0 - value); });

  QDialogButtonBox *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  QObject::connect(buttons, SIGNAL(accepted()), &dialog, SLOT(accept()));
  QObject::connect(buttons, SIGNAL(rejected()), &dialog, SLOT(reject()));

  QFormLayout *layout = new QFormLayout(&dialog);
  layout->addRow(QCoreApplication::translate("RigEaseCommand", "Ease Out:"),
                 easeOutFld);
  layout->addRow(QCoreApplication::translate("RigEaseCommand", "Ease In:"),
                 easeInFld);
  layout->addRow(buttons);

  if (dialog.exec() != QDialog::Accepted) return false;
  easeOut = easeOutFld->value();
  easeIn  = easeInFld->value();
  return true;
}

//-----------------------------------------------------------------------------

class RigBreakdownCommand final : public MenuItemHandler {
  const RigBreakdownPreset *m_preset;  // null for the custom one

public:
  RigBreakdownCommand(CommandId id, const RigBreakdownPreset *preset)
      : MenuItemHandler(id), m_preset(preset) {}

  void execute() override {
    if (m_preset) {
      insertRigBreakdown(m_preset->favour);
      return;
    }
    // remember the last value for the session
    static double favour = 50.0;
    if (getCustomBreakdown(favour)) insertRigBreakdown(favour);
  }
};

class RigEaseCommand final : public MenuItemHandler {
  const RigEasePreset *m_preset;  // null for the custom one

public:
  RigEaseCommand(CommandId id, const RigEasePreset *preset)
      : MenuItemHandler(id), m_preset(preset) {}

  void execute() override {
    if (m_preset) {
      applyRigEase(m_preset->easeOut, m_preset->easeIn);
      return;
    }
    // remember the last values for the session
    static double easeOut = 100.0 / 3, easeIn = 100.0 / 3;
    if (getCustomEase(easeOut, easeIn)) applyRigEase(easeOut, easeIn);
  }
};

// Creates the command handlers; they register themselves by command id.
struct RigEaseCommandsRegistrar {
  RigEaseCommandsRegistrar() {
    for (int i = 0; i < cRigBreakdownPresetCount; i++)
      new RigBreakdownCommand(cRigBreakdownPresets[i].id,
                              &cRigBreakdownPresets[i]);
    new RigBreakdownCommand(MI_RigBreakdownCustom, nullptr);
    for (int i = 0; i < cRigEasePresetCount; i++)
      new RigEaseCommand(cRigEasePresets[i].id, &cRigEasePresets[i]);
    new RigEaseCommand(MI_RigEaseCustom, nullptr);
  }
} rigEaseCommandsRegistrar;

}  // namespace
