// Bake Deformers into Vector Level: applies the warp of the deformer fxs that
// a vector level column goes through to the strokes of its drawings (see
// doc/deformer_bake.md). Bake Envelope into Vector Level is the same command.

// Tnz6 includes
#include "toonzqt/uitranslations.h"
#include "tapp.h"
#include "menubarcommandids.h"
#include "cellselection.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/icongenerator.h"
#include "toonzqt/selection.h"
#include "toonzqt/tselectionhandle.h"

// TnzLib includes
#include "toonz/fxdag.h"
#include "toonz/tcolumnfx.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tdeformer.h"
#include "toonz/tdeformerstack.h"
#include "toonz/tfxhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

// TnzExt includes
#include "ext/plasticskeletondeformation.h"

// TnzBase includes
#include "tfx.h"
#include "tfxattributes.h"

// TnzCore includes
#include "tundo.h"
#include "tvectorimage.h"

// Qt includes
#include <QCheckBox>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QStringList>

#include <memory>
#include <string>
#include <vector>

//**************************************************************************
//    Local namespace stuff
//**************************************************************************

namespace {

// The longest fx chain walked, against cycles
const int c_maxChainLength = 64;

//--------------------------------------------------------------

// A drawing to bake, and the row whose deformation it takes
struct BakeTarget {
  TXshSimpleLevelP m_level;
  TFrameId m_fid;
  int m_row;
};

// A baked drawing, before and after the bake
struct BakedDrawing {
  TXshSimpleLevelP m_level;
  TFrameId m_fid;
  TVectorImageP m_before, m_after;
};

//--------------------------------------------------------------

// Returns the level column fx found going up from fx through the first input
// ports, the Source ports of the deformer fxs; null if there is none.
TLevelColumnFx *findSourceColumnFx(TFx *fx) {
  for (int depth = 0; fx && depth < c_maxChainLength; ++depth) {
    if (TLevelColumnFx *columnFx = dynamic_cast<TLevelColumnFx *>(fx))
      return columnFx;
    if (fx->getInputPortCount() == 0) return nullptr;
    fx = fx->getInputPort(0)->getFx();
  }
  return nullptr;
}

//--------------------------------------------------------------

// Fills fxs with the enabled deformer fxs (see TDeformerSource) that the
// column's drawings go through, in order from the column fx downstream.
/*
  The walk follows the chain as the colour overrides of a column are found
  (toonzlib/stagevisitor.cpp): it stops where the chain splits or is also
  rendered in the Xsheet, since the drawings are rendered more than once
  there, and at an fx that does not take the chain in its first port or that
  is not an fx of the Xsheet's dag (the Xsheet and Output nodes, the fxs in a
  macro). While the chain enters each fx through its first port, the column's
  placement is carried up past the fx (FxBuilder::makePFfromGenericFx()), so
  every fx of the chain works in the space of the column's drawings.
*/
void collectDeformerFxs(TXsheet *xsh, TFx *columnFx, std::vector<TFx *> &fxs) {
  FxDag *dag          = xsh->getFxDag();
  TFxSet *terminalFxs = dag->getTerminalFxs();
  TFxSet *internalFxs = dag->getInternalFxs();

  TFx *fx = columnFx;
  for (int depth = 0; fx && depth < c_maxChainLength; ++depth) {
    if (fx->getOutputConnectionCount() != 1 || terminalFxs->containsFx(fx))
      break;

    TFxPort *port = fx->getOutputConnection(0);
    TFx *next     = port ? port->getOwnerFx() : nullptr;
    if (!next || !internalFxs->containsFx(next) ||
        next->getInputPortCount() == 0 || next->getInputPort(0) != port)
      break;

    // A disabled fx passes its Source through
    if (next->getAttributes()->isEnabled() &&
        dynamic_cast<TDeformerSource *>(next))
      fxs.push_back(next);
    fx = next;
  }
}

//--------------------------------------------------------------

// Returns whether the column's drawings are deformed at row by the Plastic
// skeleton of a mesh parent column, as FxBuilder::addPlasticDeformerFx()
// tells: the fxs then work in the space of the mesh, on the deformed image.
bool isPlasticDeformed(TXsheet *xsh, int col, int row) {
  TStageObject *obj = xsh->getStageObject(TStageObjectId::ColumnId(col));
  TStageObjectId parentId(obj->getParent());
  if (!parentId.isColumn() || obj->getParentHandle()[0] == 'H') return false;

  if (!xsh->getStageObject(parentId)->getPlasticSkeletonDeformation())
    return false;

  TXshSimpleLevel *parentSl =
      xsh->getCell(row, parentId.getIndex()).getSimpleLevel();
  return parentSl && parentSl->getType() == MESH_XSHLEVEL;
}

//--------------------------------------------------------------

// Fills targets with the vector drawings of the column on the selected rows,
// if the cell selection includes the column, or else on the current row. A
// drawing exposed on several rows is baked once, at the first of them.
void getTargets(TXsheet *xsh, int col, int currentRow,
                std::vector<BakeTarget> &targets) {
  int r0 = currentRow, r1 = currentRow;

  TCellSelection *cellSelection = dynamic_cast<TCellSelection *>(
      TApp::instance()->getCurrentSelection()->getSelection());
  if (cellSelection && !cellSelection->isEmpty()) {
    TCellSelection::Range range = cellSelection->getSelectedCells();
    if (range.m_c0 <= col && col <= range.m_c1)
      r0 = range.m_r0, r1 = range.m_r1;
  }

  for (int r = r0; r <= r1; ++r) {
    const TXshCell &cell = xsh->getCell(r, col);
    TXshSimpleLevel *sl  = cell.getSimpleLevel();
    if (!sl || sl->getType() != PLI_XSHLEVEL) continue;

    TFrameId fid = cell.getFrameId();
    bool found   = false;
    for (const BakeTarget &target : targets)
      if (target.m_level.getPointer() == sl && target.m_fid == fid) {
        found = true;
        break;
      }
    if (found) continue;

    BakeTarget target;
    target.m_level = TXshSimpleLevelP(sl);
    target.m_fid   = fid;
    target.m_row   = r;
    targets.push_back(target);
  }
}

//==============================================================

class BakeDeformersUndo final : public TUndo {
  std::vector<BakedDrawing> m_drawings;
  std::vector<TFxP> m_fxs;  // Disabled by the bake

public:
  BakeDeformersUndo(const std::vector<BakedDrawing> &drawings,
                    const std::vector<TFxP> &fxs)
      : m_drawings(drawings), m_fxs(fxs) {}

  void undo() const override { apply(false); }
  void redo() const override { apply(true); }

  int getSize() const override {
    TINT64 size = sizeof(*this);
    for (const BakedDrawing &drawing : m_drawings)
      size += getImageMemorySize(drawing.m_before.getPointer()) +
              getImageMemorySize(drawing.m_after.getPointer());
    return clampSize(size);
  }

  QString getHistoryString() override {
    QStringList fids;
    for (const BakedDrawing &drawing : m_drawings)
      fids << QString::fromStdString(drawing.m_fid.expand());

    QString levelName;
    if (!m_drawings.empty())
      levelName = QString::fromStdWString(m_drawings[0].m_level->getName());

    return UiTranslations::text(QObject::tr("Bake Deformers into Vector Level  : %1 (%2)"))
        .arg(levelName)
        .arg(fids.join(", "));
  }

private:
  void apply(bool baked) const {
    TApp *app = TApp::instance();

    for (const BakedDrawing &drawing : m_drawings) {
      const TVectorImageP &image = baked ? drawing.m_after : drawing.m_before;
      drawing.m_level->setFrame(drawing.m_fid, TImageP(image.getPointer()));
      drawing.m_level->setDirtyFlag(true);
      IconGenerator::instance()->invalidate(drawing.m_level.getPointer(),
                                            drawing.m_fid);
    }
    app->getCurrentLevel()->notifyLevelChange();

    if (m_fxs.empty()) return;

    for (const TFxP &fx : m_fxs) fx->getAttributes()->enable(!baked);
    app->getCurrentXsheet()->notifyXsheetChanged();
    app->getCurrentFx()->notifyFxChanged();
    app->getCurrentScene()->setDirtyFlag(true);
  }
};

//==============================================================

// Asks to confirm the bake; disableFxs is the choice of disabling the baked
// fxs. Returns false if the bake is canceled.
bool confirmBake(const QStringList &fxNames, int drawingCount,
                 bool &disableFxs) {
  DVGui::Dialog dialog(TApp::instance()->getMainWindow(), true);
  dialog.setWindowTitle(QObject::tr("Bake Deformers into Vector Level"));

  QLabel *label = new QLabel(
      UiTranslations::text(QObject::tr("The deformation of %1 will be applied to the strokes of %2 "
                  "drawing(s) of the column, each with the deformation of its "
                  "frame. A drawing exposed on several of the selected frames "
                  "takes the deformation of the first one."))
          .arg(fxNames.join(", "))
          .arg(drawingCount),
      &dialog);
  label->setWordWrap(true);
  dialog.addWidget(label);

  QCheckBox *disableCheckBox = new QCheckBox(
      QObject::tr("Disable the baked fxs, so that they do not deform the "
                  "drawings again (on every frame)"),
      &dialog);
  disableCheckBox->setChecked(true);
  dialog.addWidget(disableCheckBox);

  QPushButton *bakeButton   = new QPushButton(QObject::tr("Bake"), &dialog);
  QPushButton *cancelButton = new QPushButton(QObject::tr("Cancel"), &dialog);
  bakeButton->setDefault(true);
  dialog.addButtonBarWidget(bakeButton, cancelButton);
  QObject::connect(bakeButton, SIGNAL(clicked()), &dialog, SLOT(accept()));
  QObject::connect(cancelButton, SIGNAL(clicked()), &dialog, SLOT(reject()));

  if (dialog.exec() != QDialog::Accepted) return false;

  disableFxs = disableCheckBox->isChecked();
  return true;
}

//--------------------------------------------------------------

void bakeDeformers() {
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();

  TFrameHandle *frameHandle = app->getCurrentFrame();
  if (frameHandle->getFrameType() != TFrameHandle::SceneFrame) {
    DVGui::error(QObject::tr("Go to a frame of the scene first."));
    return;
  }
  int row = frameHandle->getFrame();

  // The column: the Source column of the current fx if it is a deformer, as
  // the first envelope bake did, or else the current column
  TLevelColumnFx *columnFx = nullptr;
  TFx *currentFx           = app->getCurrentFx()->getFx();
  if (currentFx && dynamic_cast<TDeformerSource *>(currentFx))
    columnFx = findSourceColumnFx(currentFx);
  if (!columnFx) {
    int col             = app->getCurrentColumn()->getColumnIndex();
    TXshColumn *column  = col >= 0 ? xsh->getColumn(col) : nullptr;
    TXshLevelColumn *lc = column ? column->getLevelColumn() : nullptr;
    if (lc) columnFx = lc->getLevelColumnFx();
  }

  TXshLevelColumn *levelColumn = columnFx ? columnFx->getColumn() : nullptr;
  if (!levelColumn || levelColumn->getIndex() < 0 ||
      xsh->getColumn(levelColumn->getIndex()) != levelColumn) {
    DVGui::error(
        QObject::tr("Select a Toonz Vector level column, or a deformer fx "
                    "applied to one in the Schematic."));
    return;
  }
  int col = levelColumn->getIndex();

  std::vector<TFx *> fxs;
  collectDeformerFxs(xsh, columnFx, fxs);
  if (fxs.empty()) {
    DVGui::error(QObject::tr(
        "No enabled deformer fx is applied to the column. The column must "
        "reach the deformer fxs through their Source port, in a chain that "
        "does not split and is not connected to the Xsheet node on the "
        "way."));
    return;
  }

  std::vector<BakeTarget> targets;
  getTargets(xsh, col, row, targets);
  if (targets.empty()) {
    DVGui::error(QObject::tr(
        "The current frame of the column is not a Toonz Vector level "
        "drawing."));
    return;
  }

  // The deformers of each target's row, applied in the order of the chain
  std::vector<std::shared_ptr<TDeformerStack>> stacks;
  std::vector<bool> used(fxs.size(), false);
  bool deforms = false;
  for (const BakeTarget &target : targets) {
    if (isPlasticDeformed(xsh, col, target.m_row)) {
      DVGui::error(QObject::tr(
          "The column is deformed by a Plastic skeleton: the deformer fxs "
          "apply to the Plastic deformation, which cannot be baked."));
      return;
    }

    std::shared_ptr<TDeformerStack> stack(new TDeformerStack);
    for (int i = 0; i < (int)fxs.size(); ++i) {
      const TDeformerSource *source =
          dynamic_cast<const TDeformerSource *>(fxs[i]);
      std::shared_ptr<const TDeformer> deformer =
          source->makeDeformer(target.m_row, TAffine());
      if (!deformer) continue;

      stack->addLayer(deformer);
      used[i] = true;
    }

    if (stack->getLayerCount() > 0)
      deforms = true;
    else
      stack.reset();
    stacks.push_back(stack);
  }

  if (!deforms) {
    DVGui::error(QObject::tr(
        "The deformer fxs do not deform the drawings at their frames: there "
        "is nothing to bake."));
    return;
  }

  QStringList fxNames;
  for (int i = 0; i < (int)fxs.size(); ++i)
    if (used[i]) fxNames << QString::fromStdWString(fxs[i]->getName());

  int drawingCount = 0;
  for (const std::shared_ptr<TDeformerStack> &stack : stacks)
    if (stack) ++drawingCount;

  bool disableFxs = true;
  if (!confirmBake(fxNames, drawingCount, disableFxs)) return;

  std::vector<BakedDrawing> drawings;
  for (int t = 0; t < (int)targets.size(); ++t) {
    if (!stacks[t]) continue;

    const BakeTarget &target = targets[t];
    TVectorImageP before(
        target.m_level->getFullsampledFrame(target.m_fid, false));
    if (!before) continue;

    TVectorImageP after(before->clone());
    deformVectorImage(*after, *stacks[t], target.m_row);

    BakedDrawing drawing;
    drawing.m_level  = target.m_level;
    drawing.m_fid    = target.m_fid;
    drawing.m_before = before;
    drawing.m_after  = after;
    drawings.push_back(drawing);
  }

  if (drawings.empty()) {
    DVGui::error(QObject::tr("The drawings could not be loaded."));
    return;
  }

  std::vector<TFxP> disabledFxs;
  if (disableFxs)
    for (int i = 0; i < (int)fxs.size(); ++i)
      if (used[i]) disabledFxs.push_back(TFxP(fxs[i]));

  BakeDeformersUndo *undo = new BakeDeformersUndo(drawings, disabledFxs);
  undo->redo();
  TUndoManager::manager()->add(undo);
}

//==============================================================

class BakeDeformersCommand final : public MenuItemHandler {
public:
  BakeDeformersCommand(CommandId id) : MenuItemHandler(id) {}

  void execute() override { bakeDeformers(); }
};

// Bake Envelope into Vector Level, the first version of the command, is kept
// as an alias for the shortcuts and the menus that use it
BakeDeformersCommand bakeDeformersCommand(MI_BakeDeformers),
    bakeEnvelopeCommand(MI_BakeEnvelope);

}  // namespace
