// Distribute to Layers, Distribute to Frames and Convert Selection to Symbol:
// commands of a selection of vector strokes in the viewer that move the
// strokes, by group, into new columns, into new drawings of the same column, or
// into a new symbol. See doc/animate_distribute.md.
//
// The commands only create ordinary Toonz Vector levels, columns and
// sub-xsheet levels, so nothing new is saved. Each one is a single undo step.

#include "distributecommand.h"
#include "distributeutil.h"

// Tnz6 includes
#include "tapp.h"
#include "menubarcommandids.h"
#include "subscenecommand.h"
#include "historytypes.h"

// TnzTools includes
#include "tools/strokeselection.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"

// TnzQt includes
#include "toonzqt/dvdialog.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/tselectionhandle.h"
#include "tmsgcore.h"

// TnzLib includes
#include "toonz/toonzscene.h"
#include "toonz/levelset.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevel.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

// TnzCore includes
#include "tundo.h"
#include "tvectorimage.h"
#include "tpalette.h"

// Qt includes
#include <QMutexLocker>
#include <QObject>
#include <QString>
#include <QTimer>

// STD includes
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

//*****************************************************************************
//    Local namespace
//*****************************************************************************

namespace {

typedef DistributeUtil::Part Part;

TXsheet *currentXsheet() {
  return TApp::instance()->getCurrentXsheet()->getXsheet();
}

//-----------------------------------------------------------------------------

void notifyXsheetEdited() {
  TApp *app = TApp::instance();
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentScene()->notifyCastChange();
  app->getCurrentScene()->setDirtyFlag(true);
  app->getCurrentObject()->notifyObjectIdSwitched();
}

//=============================================================================
// The drawing to distribute
//-----------------------------------------------------------------------------

struct Target {
  StrokeSelection *m_selection = nullptr;
  TVectorImageP m_image;
  TXshSimpleLevel *m_level = nullptr;
  TFrameId m_fid;
  int m_row = -1, m_col = -1;
};

//! Returns true, and sets \e target, if the current selection is a non-empty
//! selection of strokes in the drawing of the current cell of a Toonz Vector
//! level exposed in a column of the xsheet.
bool getTarget(Target &target) {
  TApp *app                  = TApp::instance();
  StrokeSelection *selection = dynamic_cast<StrokeSelection *>(
      app->getCurrentSelection()->getSelection());
  if (!selection || selection->isEmpty()) return false;
  TVectorImageP image = selection->getImage();
  if (!image) return false;

  // The columns do not exist while editing a level in the level strip
  TFrameHandle *frame = app->getCurrentFrame();
  if (frame->isEditingLevel() || frame->isPlaying()) return false;
  if (app->getCurrentObject()->isSpline()) return false;

  TTool *tool = app->getCurrentTool()->getTool();
  if (!tool || !tool->isEnabled()) return false;

  TXshCell cell          = TTool::getImageCell();
  TXshSimpleLevel *level = cell.getSimpleLevel();
  if (!level || level->getType() != PLI_XSHLEVEL) return false;

  // The selection must be the one of the drawing the tool is editing
  if (TVectorImageP(TTool::getImage(false)) != image) return false;
  if (app->getCurrentLevel()->getSimpleLevel() != level ||
      tool->getCurrentFid() != cell.getFrameId())
    return false;

  int col = app->getCurrentColumn()->getColumnIndex();
  int row = frame->getFrame();
  if (col < 0 || row < 0) return false;

  target.m_selection = selection;
  target.m_image     = image;
  target.m_level     = level;
  target.m_fid       = cell.getFrameId();
  target.m_row       = row;
  target.m_col       = col;
  return true;
}

//-----------------------------------------------------------------------------

//! As getTarget(), but also tells the user what is wrong with the selection or
//! with the drawing.
bool getEditableTarget(Target &target) {
  if (!getTarget(target)) return false;
  if (!target.m_selection->isEditable()) {
    DVGui::error(
        QObject::tr("The selection cannot be changed. It is not editable."));
    return false;
  }
  // The cells of a column with a time remap are not the rows it shows, and the
  // new columns would not follow the remap
  TXshColumn *column           = currentXsheet()->getColumn(target.m_col);
  TXshLevelColumn *levelColumn = column ? column->getLevelColumn() : nullptr;
  if (levelColumn && levelColumn->isTimeRemapActive()) {
    DVGui::error(QObject::tr(
        "The column has a time remap. Turn the time remap off before "
        "distributing its drawing."));
    return false;
  }
  return true;
}

//-----------------------------------------------------------------------------

//! Splits the selected strokes into the parts to distribute: the strokes of a
//! group, or a single stroke. When a group is entered, its groups are the
//! parts.
std::vector<Part> getParts(const Target &target) {
  const TVectorImageP &image = target.m_image;
  const int depth            = image->isInsideGroup();
  const int strokeCount      = (int)image->getStrokeCount();

  std::vector<int> indices;
  for (int index : target.m_selection->getSelection())
    if (0 <= index && index < strokeCount) indices.push_back(index);

  return DistributeUtil::partitionStrokes(
      indices, [&](int i) { return image->getGroupDepth(i) > depth; },
      [&](int a, int b) { return image->getCommonGroupDepth(a, b) > depth; });
}

//-----------------------------------------------------------------------------

//! Returns the rows of the column where the drawing is exposed.
std::vector<int> getExposureRows(const Target &target) {
  std::vector<int> rows;
  TXsheet *xsh = currentXsheet();
  int r0 = 0, r1 = -1;
  if (xsh->getCellRange(target.m_col, r0, r1) == 0) return rows;
  const TXshCell drawing(target.m_level, target.m_fid);
  for (int r = r0; r <= r1; ++r)
    if (xsh->getCell(r, target.m_col) == drawing) rows.push_back(r);
  return rows;
}

//-----------------------------------------------------------------------------

//! Deletes the selected strokes from the drawing, but the ones in \e keep, with
//! the undo of the Delete command. Returns false, and leaves the selection as
//! it was, if nothing was deleted.
bool deleteSelectedStrokes(const Target &target, const std::set<int> &keep) {
  StrokeSelection::IndexesContainer &selected =
      target.m_selection->getSelection();
  const StrokeSelection::IndexesContainer original = selected;
  for (int index : keep) selected.erase(index);

  int expected = (int)target.m_image->getStrokeCount() - (int)selected.size();
  target.m_selection->deleteStrokes();
  if ((int)target.m_image->getStrokeCount() == expected) return true;

  selected = original;
  return false;
}

//-----------------------------------------------------------------------------

//! Builds a drawing with copies of the strokes of \e piece, an image made by
//! TVectorImage::splitImage(). The style ids stay the same, so \e palette must
//! be a copy of the palette of the image that \e piece comes from.
TVectorImageP makeImage(const TVectorImageP &piece, TPalette *palette) {
  TVectorImageP image = new TVectorImage;
  image->setPalette(palette);

  std::set<int> usedStyles;
  piece->getUsedStyles(usedStyles);
  std::map<int, int> styleTable;
  styleTable[0] = 0;
  for (int style : usedStyles) styleTable[style] = style;

  image->mergeImage(piece, TAffine(), styleTable, false);
  return image;
}

//=============================================================================
// New columns
//-----------------------------------------------------------------------------

struct NewColumn {
  TXshSimpleLevelP m_level;  //!< The new level, with a single drawing.
  TVectorImageP m_image;     //!< The drawing, kept to redo.
};

//! The look of a column that is not part of its stage object.
struct ColumnLook {
  bool m_camstandVisible = true, m_previewVisible = true;
  bool m_lightTableExcluded = false;
  UCHAR m_opacity           = 255;
  int m_colorFilterId       = 0;

  void read(const TXshColumn *column) {
    if (!column) return;
    m_camstandVisible    = column->isCamstandVisible();
    m_previewVisible     = column->isPreviewVisible();
    m_lightTableExcluded = column->isLightTableExcluded();
    m_opacity            = column->getOpacity();
    m_colorFilterId      = column->getColorFilterId();
  }

  void apply(TXshColumn *column) const {
    if (!column) return;
    column->setCamstandVisible(m_camstandVisible);
    column->setPreviewVisible(m_previewVisible);
    column->setLightTableExcluded(m_lightTableExcluded);
    column->setOpacity(m_opacity);
    column->setColorFilterId(m_colorFilterId);
  }
};

//! Places the stage object values and the look of the source column on a
//! column, and gives it the parent of the source column.
void copyPlacement(TXsheet *xsh, int sourceCol, int col,
                   const TStageObjectParams *params, const ColumnLook &look) {
  TStageObject *object = xsh->getStageObject(TStageObjectId::ColumnId(col));
  object->assignParams(params);
  // the name of the source is not copied: the column is named after its level
  object->setName(std::string());
  object->setParent(
      xsh->getStageObjectParent(TStageObjectId::ColumnId(sourceCol)));
  look.apply(xsh->getColumn(col));
}

//-----------------------------------------------------------------------------

//! Creates a Toonz Vector level named after the level of the target, with a
//! copy of its palette and a drawing with the strokes of \e piece.
bool createLevel(const Target &target, const TVectorImageP &piece,
                 NewColumn &newColumn) {
  ToonzScene *scene   = TApp::instance()->getCurrentScene()->getScene();
  TLevelSet *levelSet = scene->getLevelSet();

  std::wstring name = DistributeUtil::numberedName(
      target.m_level->getName(),
      [levelSet](const std::wstring &n) { return levelSet->hasLevel(n); });

  TXshLevel *level    = scene->createNewLevel(PLI_XSHLEVEL, name);
  TXshSimpleLevel *sl = level ? level->getSimpleLevel() : nullptr;
  if (!sl) return false;
  newColumn.m_level = sl;

  // the strokes keep their styles: the palette of the new level is a copy
  if (TPalette *palette = target.m_level->getPalette()) {
    TPalette *copy = palette->clone();
    copy->setDirtyFlag(true);
    sl->setPalette(copy);
  }

  newColumn.m_image = makeImage(piece, sl->getPalette());
  sl->setFrame(TFrameId(1), TImageP(newColumn.m_image.getPointer()));
  return true;
}

//-----------------------------------------------------------------------------

//! Removes from the scene the levels created by createLevel().
void discardLevels(const std::vector<NewColumn> &columns) {
  TApp *app           = TApp::instance();
  TLevelSet *levelSet = app->getCurrentScene()->getScene()->getLevelSet();
  for (const NewColumn &newColumn : columns) {
    newColumn.m_level->eraseFrame(TFrameId(1));
    levelSet->removeLevel(newColumn.m_level.getPointer());
  }
  app->getCurrentScene()->notifyCastChange();
}

//-----------------------------------------------------------------------------

//! Creates a level for each piece. Returns false, and removes the levels made
//! so far, if one cannot be created.
bool createLevels(const Target &target,
                  const std::vector<TVectorImageP> &pieces,
                  std::vector<NewColumn> &columns) {
  for (const TVectorImageP &piece : pieces) {
    NewColumn newColumn;
    bool ok = createLevel(target, piece, newColumn);
    if (newColumn.m_level) columns.push_back(newColumn);
    if (!ok) {
      discardLevels(columns);
      columns.clear();
      return false;
    }
  }
  return true;
}

//-----------------------------------------------------------------------------

//! Copies of the strokes of the parts, in the same order.
std::vector<TVectorImageP> copyParts(const Target &target,
                                     const std::vector<Part> &parts) {
  std::vector<TVectorImageP> pieces;
  QMutexLocker lock(target.m_image->getMutex());
  for (const Part &part : parts)
    pieces.push_back(target.m_image->splitImage(part, false));
  return pieces;
}

//=============================================================================
// NewColumnsUndo
//-----------------------------------------------------------------------------

//! Inserts new columns, one for each of the levels, right after the source
//! column, exposing the drawing of the level in the same rows. Undoing it
//! removes the columns and the levels.
class NewColumnsUndo final : public TUndo {
  int m_sourceCol;
  std::vector<NewColumn> m_columns;
  std::vector<int> m_rows;
  std::shared_ptr<TStageObjectParams> m_placement;  //!< null: default values
  ColumnLook m_look;
  QString m_history;

public:
  NewColumnsUndo(int sourceCol, const std::vector<NewColumn> &columns,
                 const std::vector<int> &rows,
                 const std::shared_ptr<TStageObjectParams> &placement,
                 const ColumnLook &look, const QString &history)
      : m_sourceCol(sourceCol)
      , m_columns(columns)
      , m_rows(rows)
      , m_placement(placement)
      , m_look(look)
      , m_history(history) {}

  void undo() const override {
    TXsheet *xsh = currentXsheet();
    TLevelSet *levelSet =
        TApp::instance()->getCurrentScene()->getScene()->getLevelSet();
    for (int i = (int)m_columns.size() - 1; i >= 0; --i) {
      xsh->removeColumn(m_sourceCol + 1 + i);
      m_columns[i].m_level->eraseFrame(TFrameId(1));
      levelSet->removeLevel(m_columns[i].m_level.getPointer());
    }
    xsh->updateFrameCount();
    notifyXsheetEdited();
  }

  void redo() const override {
    TXsheet *xsh = currentXsheet();
    TLevelSet *levelSet =
        TApp::instance()->getCurrentScene()->getScene()->getLevelSet();
    for (const NewColumn &newColumn : m_columns) {
      levelSet->insertLevel(newColumn.m_level.getPointer());
      newColumn.m_level->setFrame(TFrameId(1),
                                  TImageP(newColumn.m_image.getPointer()));
    }
    for (int i = 0; i < (int)m_columns.size(); ++i) {
      int col = m_sourceCol + 1 + i;
      xsh->insertColumn(col);
      if (m_placement)
        copyPlacement(xsh, m_sourceCol, col, m_placement.get(), m_look);
      for (int row : m_rows)
        xsh->setCell(row, col,
                     TXshCell(m_columns[i].m_level.getPointer(), TFrameId(1)));
    }
    xsh->updateFrameCount();
    notifyXsheetEdited();
  }

  int getSize() const override {
    TINT64 size = sizeof(*this) + m_rows.size() * sizeof(int);
    for (const NewColumn &newColumn : m_columns)
      size += getImageMemorySize(newColumn.m_image.getPointer());
    return clampSize(size);
  }

  QString getHistoryString() override { return m_history; }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

//=============================================================================
// NewFramesUndo
//-----------------------------------------------------------------------------

//! Adds new drawings to a level and exposes them in the rows from \e row of
//! the column, moving down the cells below.
class NewFramesUndo final : public TUndo {
  TXshSimpleLevelP m_level;
  int m_row, m_col;
  std::vector<TFrameId> m_fids;
  std::vector<TVectorImageP> m_images;

public:
  NewFramesUndo(TXshSimpleLevel *level, int row, int col,
                const std::vector<TFrameId> &fids,
                const std::vector<TVectorImageP> &images)
      : m_level(level)
      , m_row(row)
      , m_col(col)
      , m_fids(fids)
      , m_images(images) {}

  void undo() const override {
    TXsheet *xsh = currentXsheet();
    xsh->removeCells(m_row, m_col, (int)m_fids.size());
    for (const TFrameId &fid : m_fids) m_level->eraseFrame(fid);
    xsh->updateFrameCount();
    TApp::instance()->getCurrentLevel()->notifyLevelChange();
    notifyXsheetEdited();
  }

  void redo() const override {
    TXsheet *xsh = currentXsheet();
    for (int i = 0; i < (int)m_fids.size(); ++i) {
      m_level->setFrame(m_fids[i], TImageP(m_images[i].getPointer()));
      m_level->touchFrame(m_fids[i]);
    }
    xsh->insertCells(m_row, m_col, (int)m_fids.size());
    for (int i = 0; i < (int)m_fids.size(); ++i)
      xsh->setCell(m_row + i, m_col, TXshCell(m_level.getPointer(), m_fids[i]));
    xsh->updateFrameCount();
    TApp::instance()->getCurrentLevel()->notifyLevelChange();
    notifyXsheetEdited();
  }

  int getSize() const override {
    TINT64 size = sizeof(*this) + m_fids.size() * sizeof(TFrameId);
    for (const TVectorImageP &image : m_images)
      size += getImageMemorySize(image.getPointer());
    return clampSize(size);
  }

  QString getHistoryString() override {
    return QObject::tr("Distribute to Frames");
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

//=============================================================================
// Symbol
//-----------------------------------------------------------------------------

//! Places the symbol instance made by the collapse where the source column is:
//! the instance gets its stage object values, parent and look. The drawings
//! inside the symbol are not moved, so the symbol shows what the strokes
//! showed. Undoing it needs nothing: the undo of the collapse removes the
//! column.
class SymbolPlacementUndo final : public TUndo {
  int m_sourceCol, m_col;
  std::shared_ptr<TStageObjectParams> m_placement;
  ColumnLook m_look;

public:
  SymbolPlacementUndo(int sourceCol, int col,
                      const std::shared_ptr<TStageObjectParams> &placement,
                      const ColumnLook &look)
      : m_sourceCol(sourceCol)
      , m_col(col)
      , m_placement(placement)
      , m_look(look) {}

  void undo() const override {}

  void redo() const override {
    copyPlacement(currentXsheet(), m_sourceCol, m_col, m_placement.get(),
                  m_look);
    notifyXsheetEdited();
  }

  int getSize() const override { return sizeof(*this); }

  QString getHistoryString() override {
    return QObject::tr("Convert Selection to Symbol");
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

//=============================================================================
// CastEntryUndo
//-----------------------------------------------------------------------------

//! Takes a level, created by a command that does not remove it when undone,
//! out of the scene cast when the command is undone.
class CastEntryUndo final : public TUndo {
  TXshLevelP m_level;

public:
  CastEntryUndo(TXshLevel *level) : m_level(level) {}

  void undo() const override {
    TApp *app = TApp::instance();
    app->getCurrentScene()->getScene()->getLevelSet()->removeLevel(
        m_level.getPointer());
    app->getCurrentScene()->notifyCastChange();
  }

  void redo() const override {
    TApp *app = TApp::instance();
    app->getCurrentScene()->getScene()->getLevelSet()->insertLevel(
        m_level.getPointer());
    app->getCurrentScene()->notifyCastChange();
  }

  int getSize() const override { return sizeof(*this); }

  QString getHistoryString() override {
    return QObject::tr("Convert Selection to Symbol");
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

//=============================================================================
// Enabling
//-----------------------------------------------------------------------------

void updateEnabledCommands() {
  Target target;
  bool enabled = getTarget(target) && target.m_selection->isEditable();
  CommandManager *commandManager = CommandManager::instance();
  commandManager->enable(MI_DistributeToLayers, enabled);
  commandManager->enable(MI_DistributeToFrames, enabled);
  commandManager->enable(MI_ConvertSelectionToSymbol, enabled);
}

//=============================================================================
// Commands
//-----------------------------------------------------------------------------

void distributeToLayers() {
  Target target;
  if (!getEditableTarget(target)) return;

  std::vector<Part> parts = getParts(target);
  std::vector<int> rows   = getExposureRows(target);
  if (parts.empty() || rows.empty()) return;

  // The copies of the strokes go to the new levels before the strokes are
  // deleted
  std::vector<NewColumn> columns;
  if (!createLevels(target, copyParts(target, parts), columns)) {
    DVGui::error(QObject::tr("The new levels cannot be created."));
    return;
  }

  // The new columns look and are placed like the column of the drawing
  TXsheet *xsh = currentXsheet();
  std::shared_ptr<TStageObjectParams> placement(
      xsh->getStageObject(TStageObjectId::ColumnId(target.m_col))->getParams());
  ColumnLook look;
  look.read(xsh->getColumn(target.m_col));

  NewColumnsUndo *undo =
      new NewColumnsUndo(target.m_col, columns, rows, placement, look,
                         QObject::tr("Distribute to Layers"));

  TUndoManager *undoManager = TUndoManager::manager();
  undoManager->beginBlock();
  if (!deleteSelectedStrokes(target, std::set<int>())) {
    undoManager->endBlock();
    delete undo;
    discardLevels(columns);
    DVGui::error(QObject::tr("The selection cannot be distributed."));
    return;
  }
  undo->redo();
  undoManager->add(undo);
  undoManager->endBlock();

  updateEnabledCommands();
}

//-----------------------------------------------------------------------------

void distributeToFrames() {
  Target target;
  if (!getEditableTarget(target)) return;

  std::vector<Part> parts = getParts(target);
  if (parts.empty()) return;
  if (parts.size() < 2) {
    DVGui::info(QObject::tr(
        "Select at least two groups or strokes to distribute them to "
        "frames."));
    return;
  }

  // The first part stays in the drawing, the others get a new drawing each, in
  // the rows that follow
  std::set<int> usedNumbers;
  for (const TFrameId &fid : target.m_level->getFids())
    usedNumbers.insert(fid.getNumber());
  const int newCount       = (int)parts.size() - 1;
  std::vector<int> numbers = DistributeUtil::freeFrameNumbers(
      usedNumbers, target.m_fid.getNumber(), newCount);

  std::vector<Part> movedParts(parts.begin() + 1, parts.end());
  std::vector<TVectorImageP> pieces = copyParts(target, movedParts);

  std::vector<TFrameId> fids;
  std::vector<TVectorImageP> images;
  for (int i = 0; i < newCount; ++i) {
    fids.push_back(TFrameId(numbers[i]));
    images.push_back(makeImage(pieces[i], target.m_level->getPalette()));
  }

  NewFramesUndo *undo = new NewFramesUndo(target.m_level, target.m_row + 1,
                                          target.m_col, fids, images);

  TUndoManager *undoManager = TUndoManager::manager();
  undoManager->beginBlock();
  if (!deleteSelectedStrokes(target,
                             std::set<int>(parts[0].begin(), parts[0].end()))) {
    undoManager->endBlock();
    delete undo;
    DVGui::error(QObject::tr("The selection cannot be distributed."));
    return;
  }
  undo->redo();
  undoManager->add(undo);
  undoManager->endBlock();

  updateEnabledCommands();
}

//-----------------------------------------------------------------------------

void convertSelectionToSymbol() {
  Target target;
  if (!getEditableTarget(target)) return;

  // The only question, asked before anything is changed
  bool ok = false;
  QString symbolName =
      DVGui::getText(QObject::tr("Convert Selection to Symbol"),
                     QObject::tr("Symbol Name:"), QObject::tr("Symbol"), &ok)
          .trimmed();
  if (!ok) return;
  if (symbolName.isEmpty()) symbolName = QObject::tr("Symbol");

  // The dialog may have changed things
  if (!getEditableTarget(target)) return;

  std::vector<int> rows = getExposureRows(target);
  if (rows.empty()) return;

  // All the strokes go to a single new level, in a new column
  std::vector<int> indices(target.m_selection->getSelection().begin(),
                           target.m_selection->getSelection().end());
  std::vector<TVectorImageP> pieces(
      1, copyParts(target, std::vector<Part>(1, indices))[0]);
  std::vector<NewColumn> columns;
  if (!createLevels(target, pieces, columns)) {
    DVGui::error(QObject::tr("The new level cannot be created."));
    return;
  }

  // The column is collapsed with default values, so the symbol holds the
  // strokes as drawn; the instance of the symbol takes the place of the column
  TXsheet *xsh = currentXsheet();
  std::shared_ptr<TStageObjectParams> placement(
      xsh->getStageObject(TStageObjectId::ColumnId(target.m_col))->getParams());
  ColumnLook look;
  look.read(xsh->getColumn(target.m_col));

  const int newCol     = target.m_col + 1;
  NewColumnsUndo *undo = new NewColumnsUndo(
      target.m_col, columns, rows, std::shared_ptr<TStageObjectParams>(),
      ColumnLook(), QObject::tr("Convert Selection to Symbol"));

  TUndoManager *undoManager = TUndoManager::manager();
  undoManager->beginBlock();
  if (!deleteSelectedStrokes(target, std::set<int>())) {
    undoManager->endBlock();
    delete undo;
    discardLevels(columns);
    DVGui::error(QObject::tr("The selection cannot be converted."));
    return;
  }
  undo->redo();
  undoManager->add(undo);

  std::set<int> collapsed;
  collapsed.insert(newCol);
  SubsceneCmd::collapse(collapsed, symbolName.toStdWString());

  if (TXshChildLevel *symbol = xsh->getCell(rows[0], newCol).getChildLevel()) {
    SymbolPlacementUndo *placementUndo =
        new SymbolPlacementUndo(target.m_col, newCol, placement, look);
    placementUndo->redo();
    undoManager->add(placementUndo);
    undoManager->add(new CastEntryUndo(symbol));
  }
  undoManager->endBlock();

  updateEnabledCommands();
}

//=============================================================================
// Menu items
//-----------------------------------------------------------------------------

class DistributeToLayersCommand final : public MenuItemHandler {
public:
  DistributeToLayersCommand() : MenuItemHandler(MI_DistributeToLayers) {}
  void execute() override { distributeToLayers(); }
} distributeToLayersCommand;

//-----------------------------------------------------------------------------

class DistributeToFramesCommand final : public MenuItemHandler {
public:
  DistributeToFramesCommand() : MenuItemHandler(MI_DistributeToFrames) {}
  void execute() override { distributeToFrames(); }
} distributeToFramesCommand;

//-----------------------------------------------------------------------------

class ConvertSelectionToSymbolCommand final : public MenuItemHandler {
public:
  ConvertSelectionToSymbolCommand()
      : MenuItemHandler(MI_ConvertSelectionToSymbol) {}
  void execute() override { convertSelectionToSymbol(); }
} convertSelectionToSymbolCommand;

}  // namespace

//*****************************************************************************
//    Global functions
//*****************************************************************************

void initDistributeCommands(QObject *context) {
  TApp *app = TApp::instance();

  // The state is refreshed once the handles have settled
  static bool updatePending = false;
  const auto scheduleUpdate = [context]() {
    if (updatePending) return;
    updatePending = true;
    QTimer::singleShot(0, context, []() {
      updatePending = false;
      updateEnabledCommands();
    });
  };

  QObject::connect(app->getCurrentSelection(),
                   &TSelectionHandle::selectionSwitched, context,
                   scheduleUpdate);
  QObject::connect(app->getCurrentSelection(),
                   &TSelectionHandle::selectionChanged, context,
                   scheduleUpdate);
  QObject::connect(app->getCurrentTool(), &ToolHandle::toolSwitched, context,
                   scheduleUpdate);
  QObject::connect(app->getCurrentTool(), &ToolHandle::toolChanged, context,
                   scheduleUpdate);
  QObject::connect(app->getCurrentFrame(), &TFrameHandle::frameSwitched,
                   context, scheduleUpdate);
  QObject::connect(app->getCurrentFrame(), &TFrameHandle::frameTypeChanged,
                   context, scheduleUpdate);
  QObject::connect(app->getCurrentColumn(), &TColumnHandle::columnIndexSwitched,
                   context, scheduleUpdate);
  QObject::connect(app->getCurrentLevel(), &TXshLevelHandle::xshLevelSwitched,
                   context, scheduleUpdate);
  QObject::connect(app->getCurrentLevel(), &TXshLevelHandle::xshLevelChanged,
                   context, scheduleUpdate);
  QObject::connect(app->getCurrentXsheet(), &TXsheetHandle::xsheetChanged,
                   context, scheduleUpdate);
  QObject::connect(app->getCurrentXsheet(), &TXsheetHandle::xsheetSwitched,
                   context, scheduleUpdate);

  updateEnabledCommands();
}
