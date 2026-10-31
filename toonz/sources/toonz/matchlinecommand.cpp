

// TnzCore includes
#include "toonzqt/uitranslations.h"
#include "tsystem.h"
#include "tundo.h"
#include "tpalette.h"

// TnzLib includes
#include "toonz/txsheet.h"
#include "toonz/toonzscene.h"
#include "toonz/txshcell.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/levelset.h"
#include "toonz/hook.h"
#include "toonz/levelproperties.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/tstageobject.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"

// Tnz6 includes
#include "tapp.h"
#include "cellselection.h"
#include "columnselection.h"
#include "keyframeselection.h"
#include "filmstripselection.h"
#include "menubarcommandids.h"
#include "columncommand.h"

// Qt includes
#include <QCoreApplication>
#include <QPushButton>
#include <QMainWindow>

#include "matchline.h"

//*****************************************************************************
//    MergeCmappedDialog  implementation
//*****************************************************************************

void MergeCmappedDialog::accept() {
  m_levelPath = TFilePath(QString(m_saveInFileFld->getPath() + "\\" +
                                  m_fileNameFld->text() + ".tlv")
                              .toStdString());
  TFilePath fp =
      TApp::instance()->getCurrentScene()->getScene()->decodeFilePath(
          m_levelPath);

  if (TSystem::doesExistFileOrLevel(fp)) {
    if (DVGui::MsgBox(
            UiTranslations::text(tr("Level %1 already exists! Are you sure you want to overwrite "
               "it?"))
                .arg(QString::fromStdWString(m_levelPath.getWideString())),
            tr("Ok"), tr("Cancel")) != 1)
      return;
    else {
      TSystem::removeFileOrLevel(fp);
      TSystem::removeFileOrLevel(fp.withType("tpl"));
    }
  }

  Dialog::accept();
}

//------------------------------------------------------------------------------

MergeCmappedDialog::MergeCmappedDialog(TFilePath &levelPath)
    : Dialog(TApp::instance()->getMainWindow(), true, true, "Merge Tlv")
    , m_levelPath(levelPath) {
  bool ret = true;

  QString path =
      QString::fromStdWString(m_levelPath.getParentDir().getWideString());
  QString name = QString::fromStdString(m_levelPath.getName());

  setWindowTitle(tr(" Merge Tlv Levels"));
  m_saveInFileFld = new DVGui::FileField(0, path);
  ret             = ret && connect(m_saveInFileFld, SIGNAL(pathChanged()), this,
                       SLOT(onPathChanged()));
  addWidget(tr("Save in:"), m_saveInFileFld);

  m_fileNameFld = new DVGui::LineEdit(name + "_merged");
  m_fileNameFld->setMaximumHeight(DVGui::WidgetHeight);
  ret = ret && connect(m_fileNameFld, SIGNAL(editingFinished()),
                       SLOT(onNameChanged()));
  addWidget(tr("File Name:"), m_fileNameFld);

  QPushButton *okBtn = new QPushButton(tr("Apply"), this);
  okBtn->setDefault(true);
  QPushButton *cancelBtn = new QPushButton(tr("Cancel"), this);
  connect(okBtn, SIGNAL(clicked()), this, SLOT(accept()));
  connect(cancelBtn, SIGNAL(clicked()), this, SLOT(reject()));

  addButtonBarWidget(okBtn, cancelBtn);
}

//*****************************************************************************
//    MergeColumns  command
//*****************************************************************************

static bool isVectorColumn(const std::set<int> &columns) {
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  std::set<int>::const_iterator column = columns.begin();
  int start, end;
  xsh->getCellRange(*column, start, end);

  if (start > end) return false;
  // a cell at "start" must be occupied
  TXshCell cell = xsh->getCell(start, *column);
  return cell.m_level->getType() == PLI_XSHLEVEL;
}

class MergeColumnsCommand final : public MenuItemHandler {
public:
  MergeColumnsCommand() : MenuItemHandler(MI_MergeColumns) {}

  void execute() override {
    TColumnSelection *selection =
        dynamic_cast<TColumnSelection *>(TSelection::getCurrent());

    std::set<int> indices =
        selection ? selection->getIndices() : std::set<int>();

    if (indices.empty()) {
      DVGui::warning(QObject::tr(
          "It is not possible to execute the merge column command because "
          "no column was selected."));
      return;
    }

    if (indices.size() == 1) {
      DVGui::warning(QObject::tr(
          "It is not possible to execute the merge column command because "
          "only one columns is selected."));
      return;
    }

    bool groupLevels = true;
    if (isVectorColumn(indices)) {
      int opt = DVGui::MsgBox(QObject::tr("Group strokes by vector levels?"),
                              QObject::tr("Yes"), QObject::tr("No"),
                              QObject::tr("Cancel"));
      if (opt == 0 || opt == 3)
        return;
      else {
        groupLevels = (opt == 1);
      };
    }

    mergeColumns(indices, groupLevels);
    TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
  }

} MergeColumnsCommand;

//*****************************************************************************
//    ApplyMatchlines  command
//*****************************************************************************

class ApplyMatchlinesCommand final : public MenuItemHandler {
public:
  ApplyMatchlinesCommand() : MenuItemHandler(MI_ApplyMatchLines) {}

  void execute() override {
    TColumnSelection *selection =
        dynamic_cast<TColumnSelection *>(TSelection::getCurrent());
    if (!selection) {
      DVGui::warning(QObject::tr(
          "It is not possible to apply the match lines because no column "
          "was selected."));
      return;
    }

    std::set<int> indices = selection->getIndices();

    if (indices.size() != 2) {
      DVGui::warning(QObject::tr(
          "It is not possible to apply the match lines because two columns "
          "have to be selected."));
      return;
    }

    std::set<int>::iterator it = indices.begin();
    int i = *it++, j = *it;

    doMatchlines(i, j, -1, -1);
    TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
  }

} ApplyMatchlinesCommand;

//--------------------------------------------

namespace {

bool checkColumnValidity(int column) {
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  int start, end;

  xsh->getCellRange(column, start, end);

  if (start > end) return false;
  std::vector<TXshCell> cell(end - start + 1);

  xsh->getCells(start, column, cell.size(), &(cell[0]));

  TXshSimpleLevel *level = 0;

  for (int i = 0; i < (int)cell.size(); i++) {
    if (cell[i].isEmpty()) continue;
    if (!level) level = cell[i].getSimpleLevel();

    if (cell[i].getSimpleLevel()->getType() != TZP_XSHLEVEL) {
      DVGui::warning(QObject::tr(
          "Match lines can be applied to Toonz raster levels only."));
      return false;
    }
    if (level != cell[i].getSimpleLevel()) {
      DVGui::warning(
          QObject::tr("It is not possible to merge tlv columns containing more "
                      "than one level"));
      return false;
    }
  }

  if (!level) return false;

  if (!level->getPalette()) {
    DVGui::warning(
        QObject::tr("The level you are using has not a valid palette."));
    return false;
  }
  return true;
}

//---------------------------------------------------------------------------------------------------------

namespace {
//-----------------------------------------------------------------------------

class CloneLevelNoSaveUndo final : public TUndo {
  TXsheetP m_xsh;
  std::map<TXshSimpleLevel *, TXshLevelP> m_createdLevels;
  std::map<int, TXshColumnP> m_insertedColumns;
  std::map<int, TStageObjectP> m_insertedStageObjects;
  std::map<std::pair<int, int>, TXshCell> m_cells;
  std::map<int, std::string> m_oldColumnNames, m_newColumnNames;

  void restoreCell(int row, int col, const TXshCell &cell) const {
    // Column locks are UI state and may change outside the undo history.
    // Restore history through the normal xsheet path while retaining the lock.
    TXshColumnP column = m_xsh->getColumn(col);
    const bool locked = column && column->isLocked();
    struct RestoreLock {
      TXshColumnP column;
      bool locked;
      ~RestoreLock() { if (column) column->lock(locked); }
    } restore{column, locked};
    if (locked) column->lock(false);
    m_xsh->setCell(row, col, cell);
  }

  void notify() const {
    TApp *app = TApp::instance();
    app->getCurrentXsheet()->notifyXsheetChanged();
    app->getCurrentScene()->setDirtyFlag(true);
    app->getCurrentScene()->notifyCastChange();
  }

public:
  CloneLevelNoSaveUndo(
      TXsheet *xsh,
      const TCellSelection::Range &range,
      const std::map<TXshSimpleLevel *, TXshLevelP> &createdLevels,
      const std::set<int> &insertedColumnIndices,
      const std::map<int, std::string> &oldColumnNames)
      : m_xsh(xsh), m_createdLevels(createdLevels), m_oldColumnNames(oldColumnNames) {
    for (int col : insertedColumnIndices) {
      m_insertedColumns.emplace(col, xsh->getColumn(col));
      m_insertedStageObjects.emplace(
          col, xsh->getStageObject(TStageObjectId::ColumnId(col)));
    }
    for (int col = range.m_c1 + 1; col <= range.m_c1 + range.getColCount(); ++col) {
      if (xsh->getColumn(col))
        m_newColumnNames[col] = xsh->getStageObject(TStageObjectId::ColumnId(col))->getName();
      for (int row = range.m_r0; row <= range.m_r1; ++row) {
        const TXshCell &cell = xsh->getCell(row, col);
        if (!cell.isEmpty()) m_cells.emplace(std::make_pair(row, col), cell);
      }
    }
  }

  void undo() const override {
    // Reused empty columns may already contain stage keys or column settings.
    // Clear only our exposures; remove only columns inserted by this command.
    for (const auto &entry : m_cells)
      if (!m_insertedColumns.count(entry.first.second))
        restoreCell(entry.first.first, entry.first.second, TXshCell());
    for (auto it = m_insertedColumns.rbegin(); it != m_insertedColumns.rend(); ++it)
      m_xsh->removeColumn(it->first);
    for (const auto &entry : m_oldColumnNames)
      m_xsh->getStageObject(TStageObjectId::ColumnId(entry.first))->setName(entry.second);
    for (const auto &entry : m_createdLevels)
      m_xsh->getScene()->getLevelSet()->removeLevel(entry.second.getPointer());
    notify();
  }
  void redo() const override {
    // Replay the original objects and exposures, not a fresh clone of whatever
    // happens to be in the source columns now. Subsequent undos refer to these
    // same levels and keep their authoring metadata and native drawing data.
    for (const auto &entry : m_createdLevels)
      m_xsh->getScene()->getLevelSet()->insertLevel(entry.second.getPointer());
    for (const auto &entry : m_insertedColumns)
      m_xsh->insertColumn(entry.first, entry.second.getPointer(),
                         m_insertedStageObjects.at(entry.first).getPointer());
    for (const auto &entry : m_cells)
      restoreCell(entry.first.first, entry.first.second, entry.second);
    for (const auto &entry : m_newColumnNames)
      m_xsh->getStageObject(TStageObjectId::ColumnId(entry.first))->setName(entry.second);
    notify();
  }

  int getSize() const override {
    return sizeof *this + (sizeof(TXshLevelP) + sizeof(TXshSimpleLevel *)) *
                              m_createdLevels.size() +
           sizeof(decltype(m_cells)::value_type) * m_cells.size() +
           sizeof(decltype(m_insertedColumns)::value_type) * m_insertedColumns.size() +
           sizeof(decltype(m_insertedStageObjects)::value_type) * m_insertedStageObjects.size();
  }
};

//-----------------------------------------------------------------------------
}  // namespace
//-----------------------------------------------------------------------------

bool doCloneLevelNoSave(const TCellSelection::Range &range,
                        const QString &newLevelName) {
  if (range.isEmpty() || range.m_r0 < 0 || range.m_c0 < 0) return false;
  TApp *app         = TApp::instance();
  TXsheet *xsh      = app->getCurrentXsheet()->getXsheet();
  ToonzScene *scene = app->getCurrentScene()->getScene();

  // Model insertion offsets before touching columns or the Cast. Missing
  // destinations are distinct from existing empty columns with authoring state.
  int inserted = 0;
  std::map<int, int> reusedColumns;
  for (int col = range.m_c1 + 1; col <= range.m_c1 + range.getColCount(); ++col) {
    const int originalCol = col - inserted;
    if (!xsh->isColumnEmpty(originalCol)) { ++inserted; continue; }
    TXshColumn *column = xsh->getColumn(originalCol);
    if (column && column->isLocked()) return false;
    if (column) reusedColumns[col] = originalCol;
  }

  struct PreparedLevel {
    std::map<TFrameId, TImageP> frames;
    TPaletteP palette;
  };
  std::map<TXshSimpleLevel *, PreparedLevel> prepared;
  std::map<std::pair<int, int>, TXshCell> exposures;
  std::map<int, TXshColumn::ColumnType> destinationTypes;
  // Load and clone every requested drawing first. A missing/corrupt later
  // drawing must not leave earlier clones in the Cast or columns half shifted.
  try {
    for (int col = range.m_c0; col <= range.m_c1; ++col) {
      for (int row = range.m_r0; row <= range.m_r1; ++row) {
        const TXshCell &cell = xsh->getCell(row, col);
        TXshSimpleLevel *source = cell.getSimpleLevel();
        if (!source || source->getPath().isUneditable()) continue;
        const int destination = col + range.getColCount();
        const auto type = TXshColumn::toColumnType(source->getType());
        const auto typed = destinationTypes.emplace(destination, type);
        if (!typed.second && typed.first->second != type) return false;
        const auto reused = reusedColumns.find(destination);
        if (reused != reusedColumns.end() &&
            xsh->getColumn(reused->second)->getColumnType() != type) return false;
        auto &plan = prepared[source];
        if (!plan.frames.count(cell.getFrameId())) {
          TImageP image = cell.getImage(false);
          if (!image) return false;
          TImageP copy = image->cloneImage();
          if (!copy) return false;
          plan.frames.emplace(cell.getFrameId(), copy);
        }
        exposures.emplace(std::make_pair(row, destination), cell);
      }
    }
    for (auto &entry : prepared) {
      const int type = entry.first->getType();
      if (type == TZP_XSHLEVEL || type == PLI_XSHLEVEL) {
        if (!entry.first->getPalette()) return false;
        entry.second.palette = entry.first->getPalette()->clone();
      }
    }
  } catch (...) {
    return false;
  }
  if (exposures.empty()) return false;

  std::map<int, std::string> oldColumnNames;
  for (const auto &entry : reusedColumns)
    oldColumnNames[entry.first] =
        xsh->getStageObject(TStageObjectId::ColumnId(entry.second))->getName();

  std::map<TXshSimpleLevel *, TXshLevelP> createdLevels;
  try {
    for (const auto &entry : prepared) {
      TXshSimpleLevel *source = entry.first;
      TXshLevelP level = scene->createNewLevel(source->getType(), newLevelName.toStdWString());
      createdLevels.emplace(source, level);
      TXshSimpleLevel *clone = level->getSimpleLevel();
      clone->clonePropertiesFrom(source);
      *clone->getHookSet() = *source->getHookSet();
      if (entry.second.palette) clone->setPalette(entry.second.palette.getPointer());
      for (const auto &frame : entry.second.frames) clone->setFrame(frame.first, frame.second);
      clone->copyAuthoringMetadataFrom(source);
      clone->getProperties()->setDirtyFlag(true);
    }
  } catch (...) {
    for (const auto &entry : createdLevels)
      scene->getLevelSet()->removeLevel(entry.second.getPointer());
    return false;
  }

  std::set<int> insertedColumnIndices;
  for (int col = range.m_c1 + 1; col <= range.m_c1 + range.getColCount(); ++col) {
    if (xsh->isColumnEmpty(col)) continue;
    const auto type = destinationTypes.find(col);
    xsh->insertColumn(col, type != destinationTypes.end() ? type->second : TXshColumn::eLevelType);
    insertedColumnIndices.insert(col);
  }
  for (const auto &entry : exposures) {
    const int row = entry.first.first, col = entry.first.second;
    if (!xsh->getColumn(col)) {
      const int previousCount = xsh->getColumnCount();
      xsh->insertColumn(col, destinationTypes.at(col));
      // insertColumn fills any missing intermediate columns as well.
      for (int added = previousCount; added < xsh->getColumnCount(); ++added)
        insertedColumnIndices.insert(added);
    }
    xsh->setCell(row, col, TXshCell(createdLevels.at(entry.second.getSimpleLevel()),
                                   entry.second.getFrameId()));
  }
  TUndoManager::manager()->add(new CloneLevelNoSaveUndo(
      xsh, range, createdLevels, insertedColumnIndices, oldColumnNames));
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentScene()->setDirtyFlag(true);
  app->getCurrentScene()->notifyCastChange();
  return true;
}

bool cloneColumn(const TCellSelection::Range &cells,
                 const TFilePath &newLevelPath) {
  std::set<std::pair<int, int>> positions;
  for (int i = cells.m_r0; i <= cells.m_r1; i++)
    positions.insert(std::pair<int, int>(i, cells.m_c0));

  TKeyframeSelection ks(positions);
  ks.copyKeyframes();
  if (!MatchlineCommands::cloneLevels(
          cells, QString::fromStdString(newLevelPath.getName() + "_"))) return false;
  ColumnCmd::deleteColumn(cells.m_c0);
  ks.pasteKeyframes();
  return true;
}

}  // namespace

bool MatchlineCommands::cloneLevels(const TCellSelection::Range &range,
                                    const QString &newLevelName) {
  return doCloneLevelNoSave(range, newLevelName);
}

class MergeCmappedCommand final : public MenuItemHandler {
public:
  MergeCmappedCommand() : MenuItemHandler(MI_MergeCmapped) {}
  void execute() override {
    TColumnSelection *selection =
        dynamic_cast<TColumnSelection *>(TSelection::getCurrent());
    if (!selection) {
      DVGui::warning(QObject::tr(
          "It is not possible to merge tlv columns because no column was "
          "selected."));
      return;
    }

    std::set<int> indices = selection->getIndices();

    if (indices.size() < 2) {
      DVGui::warning(QObject::tr(
          "It is not possible to merge tlv columns because at least two "
          "columns have to be selected."));
      return;
    }

    std::set<int>::iterator it = indices.begin();
    int destColumn             = *it;

    TCellSelection::Range cells;
    cells.m_c0 = cells.m_c1 = destColumn;
    TXshColumn *column =
        TApp::instance()->getCurrentXsheet()->getXsheet()->getColumn(
            destColumn);
    column->getRange(cells.m_r0, cells.m_r1);

    // column->getLevelColumn()

    TFilePath newLevelPath;
    TXshCell c = TApp::instance()->getCurrentXsheet()->getXsheet()->getCell(
        cells.m_r0, destColumn);
    if (!c.isEmpty() && c.getSimpleLevel())
      newLevelPath = c.getSimpleLevel()->getPath();

    if (MergeCmappedDialog(newLevelPath).exec() != QDialog::Accepted) return;

    it = indices.begin();
    for (; it != indices.end(); ++it)
      if (!checkColumnValidity(*it)) return;

    DVGui::ProgressDialog progress(QObject::tr("Merging Tlv Levels..."),
                                   QString(), 0, indices.size() - 1,
                                   TApp::instance()->getMainWindow());
    progress.setWindowModality(Qt::WindowModal);
    progress.setWindowTitle(QObject::tr("Merging Tlv Levels..."));
    progress.setValue(0);
    progress.show();

    QCoreApplication::instance()->processEvents();

    TUndoManager::manager()->beginBlock();

    if (!cloneColumn(cells, newLevelPath)) {
      TUndoManager::manager()->endBlock();
      DVGui::warning(QObject::tr("No levels were cloned. Check for missing drawings or locked or incompatible destination columns."));
      return;
    }

    TFilePath tmpPath = newLevelPath.withName(
        QString::fromStdString(newLevelPath.getName() + "_tmp").toStdWString());

    it = indices.begin();
    ++it;
    for (int count = 0; it != indices.end();) {
      int index = *it;
      it++;
      mergeCmapped(destColumn, index - count,
                   it == indices.end() ? newLevelPath.getQString()
                                       : tmpPath.getQString(),
                   false);
      ColumnCmd::deleteColumn(index - count);
      progress.setValue(++count);
      QCoreApplication::instance()->processEvents();
    }
    TUndoManager::manager()->endBlock();
    TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
  }

} MergeCmappedCommand;

//-----------------------------------------------------------------------------
namespace {
void doDeleteCommand(bool isMatchline) {
  TRect r;

  TCellSelection *sel =
      dynamic_cast<TCellSelection *>(TSelection::getCurrent());
  if (!sel) {
    TFilmstripSelection *filmstripSelection =
        dynamic_cast<TFilmstripSelection *>(TSelection::getCurrent());
    TColumnSelection *columnSelection =
        dynamic_cast<TColumnSelection *>(TSelection::getCurrent());
    std::set<int> indices;
    std::set<TFrameId> fids;
    if (filmstripSelection &&
        (fids = filmstripSelection->getSelectedFids()).size() > 0) {
      TXshSimpleLevel *sl =
          TApp::instance()->getCurrentLevel()->getSimpleLevel();
      if (isMatchline)
        deleteMatchlines(sl, fids);
      else
        deleteInk(sl, fids);
      TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
      return;
    } else if (!columnSelection ||
               (indices = columnSelection->getIndices()).size() != 1) {
      DVGui::warning(
          QObject::tr("It is not possible to delete lines because no column, "
                      "cell or level strip frame was selected."));
      return;
    }
    int from, to;
    int columnIndex = *indices.begin();
    TXsheet *xsh    = TApp::instance()->getCurrentXsheet()->getXsheet();
    if (!xsh->getCellRange(*indices.begin(), from, to)) {
      DVGui::warning(QObject::tr("The selected column is empty."));
      return;
    }
    r.y0 = from;
    r.y1 = to;
    r.x0 = r.x1 = columnIndex;
  } else
    sel->getSelectedCells(r.y0, r.x0, r.y1, r.x1);

  if (r.x0 != r.x1) {
    DVGui::warning(QObject::tr("Selected cells must be in the same column."));
    return;
  }

  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();

  int i;

  for (i = r.y0; i <= r.y1; i++) {
    TXshCell cell = xsh->getCell(i, r.x0);
    if (cell.isEmpty()) {
      DVGui::warning(
          QObject::tr("It is not possible to delete lines because no column, "
                      "cell or level strip frame was selected."));
      return;
    }
    if (cell.m_level->getType() != TZP_XSHLEVEL) {
      DVGui::warning(QObject::tr(
          "Match lines can be deleted from Toonz raster levels only"));
      return;
    }
  }

  std::set<TFrameId> fids;
  for (i = r.y0; i <= r.y1; i++) {
    const TXshCell &cell = xsh->getCell(i, r.x0);
    fids.insert(cell.getFrameId());
  }

  TXshSimpleLevel *sl = xsh->getCell(r.y0, r.x0).getSimpleLevel();
  assert(sl);

  if (isMatchline)
    deleteMatchlines(sl, fids);
  else
    deleteInk(sl, fids);

  TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
}

}  // namespace

//-----------------------------------------------------------------------------

class DeleteInkCommand final : public MenuItemHandler {
public:
  DeleteInkCommand() : MenuItemHandler(MI_DeleteInk) {}
  void execute() override { doDeleteCommand(false); }

} DeleteInkCommand;

//-----------------------------------------------------------------------------

class DeleteMatchlinesCommand final : public MenuItemHandler {
public:
  DeleteMatchlinesCommand() : MenuItemHandler(MI_DeleteMatchLines) {}
  void execute() override { doDeleteCommand(true); }

} DeleteMatchlinesCommand;
