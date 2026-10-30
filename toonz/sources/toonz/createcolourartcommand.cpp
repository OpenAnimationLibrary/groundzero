// Create Colour Art from Line Art: appends to the drawings of a Toonz Vector
// level a Colour Art copy (all thicknesses 0, see ArtLayer in
// tools/artlayer.h) of each of their Line Art strokes.

// Tnz6 includes
#include "toonzqt/uitranslations.h"
#include "tapp.h"
#include "menubarcommandids.h"
#include "cellselection.h"
#include "filmstripselection.h"

// TnzTools includes
#include "tools/artlayer.h"
#include "tools/tool.h"

// TnzQt includes
#include "toonzqt/menubarcommand.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/icongenerator.h"
#include "toonzqt/tselectionhandle.h"
#include "tmsgcore.h"

// TnzLib includes
#include "toonz/tframehandle.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"

// TnzCore includes
#include "tundo.h"
#include "tvectorimage.h"

#include <map>
#include <set>

//**************************************************************************
//    Local namespace stuff
//**************************************************************************

namespace {

class CreateColourArtUndo final : public TUndo {
  TXshSimpleLevelP m_level;
  TFrameId m_fid;
  TVectorImageP m_before, m_after;

public:
  CreateColourArtUndo(const TXshSimpleLevelP &level, const TFrameId &fid,
                      const TVectorImageP &before, const TVectorImageP &after)
      : m_level(level), m_fid(fid), m_before(before), m_after(after) {}

  void undo() const override { setImage(m_before); }
  void redo() const override { setImage(m_after); }

  int getSize() const override {
    return clampSize(getImageMemorySize(m_before.getPointer()) +
                     getImageMemorySize(m_after.getPointer()));
  }

  QString getHistoryString() override {
    return UiTranslations::text(QObject::tr("Create Colour Art from Line Art  : %1 (%2)"))
        .arg(QString::fromStdWString(m_level->getName()))
        .arg(QString::fromStdString(m_fid.expand()));
  }

private:
  void setImage(const TVectorImageP &image) const {
    m_level->setFrame(m_fid, TImageP(image.getPointer()));
    m_level->setDirtyFlag(true);
    IconGenerator::instance()->invalidate(m_level.getPointer(), m_fid);
    TApp::instance()->getCurrentLevel()->notifyLevelChange();
  }
};

//==============================================================

typedef std::map<TXshSimpleLevel *, std::set<TFrameId>> FrameMap;

//! Collects the Toonz Vector drawings to process: the selected frames of the
//! film strip, the selected cells of the xsheet, or else the current drawing.
void getFrames(FrameMap &frames) {
  TApp *app             = TApp::instance();
  TSelection *selection = app->getCurrentSelection()->getSelection();

  const auto add = [&frames](TXshSimpleLevel *level, const TFrameId &fid) {
    if (level && level->getType() == PLI_XSHLEVEL && level->isFid(fid))
      frames[level].insert(fid);
  };

  if (const TFilmstripSelection *filmstrip =
          dynamic_cast<const TFilmstripSelection *>(selection)) {
    TXshSimpleLevel *level = app->getCurrentLevel()->getSimpleLevel();
    for (const TFrameId &fid : filmstrip->getSelectedFids()) add(level, fid);
  } else if (const TCellSelection *cells =
                 dynamic_cast<const TCellSelection *>(selection)) {
    int r0, c0, r1, c1;
    cells->getSelectedCells(r0, c0, r1, c1);
    TXsheet *xsheet = app->getCurrentXsheet()->getXsheet();
    for (int r = r0; r <= r1; ++r)
      for (int c = c0; c <= c1; ++c) {
        const TXshCell &cell = xsheet->getCell(r, c);
        add(cell.getSimpleLevel(), cell.getFrameId());
      }
  }

  if (frames.empty()) {
    TXshCell cell = TTool::getImageCell();
    add(cell.getSimpleLevel(), cell.getFrameId());
  }
}

//==============================================================

class CreateColourArtCommand final : public MenuItemHandler {
public:
  CreateColourArtCommand() : MenuItemHandler(MI_CreateColourArt) {}

  void execute() override {
    FrameMap frames;
    getFrames(frames);
    if (frames.empty()) {
      DVGui::error(QObject::tr(
          "Select Toonz Vector drawings, in the Xsheet or in the Level Strip, "
          "or go to one."));
      return;
    }

    int drawingCount = 0, strokeCount = 0;
    bool started = false;
    for (auto &entry : frames) {
      TXshSimpleLevelP level(entry.first);
      if (level->isReadOnly()) continue;

      for (const TFrameId &fid : entry.second) {
        if (level->isFrameReadOnly(fid)) continue;

        TVectorImageP before(level->getFullsampledFrame(fid, false));
        if (!before) continue;

        TVectorImageP after(before->clone());
        int added = ArtLayer::createColourArt(*after);
        if (!added) continue;

        // One undo step per drawing, together
        if (!started) {
          TUndoManager::manager()->beginBlock();
          started = true;
        }
        CreateColourArtUndo *undo =
            new CreateColourArtUndo(level, fid, before, after);
        undo->redo();
        TUndoManager::manager()->add(undo);

        ++drawingCount;
        strokeCount += added;
      }
    }
    if (started) TUndoManager::manager()->endBlock();

    if (!started) {
      DVGui::info(QObject::tr(
          "There is no Line Art stroke without a Colour Art copy."));
      return;
    }

    TApp::instance()->getCurrentXsheet()->notifyXsheetChanged();
    DVGui::info(UiTranslations::text(QObject::tr("%1 Colour Art strokes created in %2 drawings. "
                            "Set \"Art Layer:\" or open the Art Layer "
                            "Manager to see and edit them."))
                    .arg(strokeCount)
                    .arg(drawingCount));
  }
} createColourArtCommand;

}  // namespace
