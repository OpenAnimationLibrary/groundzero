

#include "toonzqt/uitranslations.h"
#include "tools/strokeselection.h"
#include "vectorselectiontool.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tcamera.h"
#include <algorithm>
#include <cmath>

// TnzTools includes
#include "tools/imagegrouping.h"
#include "tools/toolhandle.h"
#include "tools/tool.h"
#include "tools/toolutils.h"
#include "tools/artlayer.h"
#include "tools/strokeopacityeditor.h"

// TnzQt includes
#include "toonzqt/selectioncommandids.h"
#include "toonzqt/imageutils.h"
#include "toonzqt/tselectionhandle.h"
#include "toonzqt/strokesdata.h"
#include "toonzqt/rasterimagedata.h"
#include "toonzqt/dvdialog.h"

// TnzLib includes
#include "toonz/tpalettehandle.h"
#include "toonz/palettecontroller.h"
#include "toonz/tobjecthandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"

#include "toonz/tcenterlinevectorizer.h"
#include "toonz/stage.h"
#include "toonz/tstageobject.h"
#include "toonz/toonzscene.h"
#include "toonz/sceneproperties.h"
#include "toonz/tframehandle.h"
#include "toonz/contouroutline.h"
#include "toonz/customvectorbrushexporter.h"
#include "toonz/toonzfolders.h"
#include "toonz/txshlevel.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/morphhints.h"

// TnzCore includes
#include "tthreadmessage.h"
#include "tundo.h"
#include "tstroke.h"
#include "tsystem.h"
#include "tvectorimage.h"
#include "tcolorstyles.h"
#include "tpalette.h"
#include "tregion.h"

// Qt includes
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QMutexLocker>
#include <limits>
#include <map>

//=============================================================================
namespace {

// Warn about unusually large brushes without preventing their use.
constexpr TINT64 RecommendedCustomVectorBrushBytes = 32 * 1024;

QString normalizeBrushName(QString name) {
  name = name.trimmed();
  if (name.endsWith(".pli", Qt::CaseInsensitive)) {
    name.chop(4);
    name = name.trimmed();
  }
  return name;
}

QString validateBrushName(const QString &name) {
  if (name.isEmpty()) return QObject::tr("Please enter a brush name.");
  if (name == "." || name == "..")
    return QObject::tr("Please enter a valid brush name.");
  if (name.endsWith('.') || name.endsWith(' '))
    return QObject::tr("A brush name cannot end with a dot or a space.");

  const QString invalidCharacters = "\\/:*?\"<>|";
  for (const QChar character : name) {
    if (character.unicode() < 32 || invalidCharacters.contains(character))
      return QObject::tr(
          "A brush name cannot contain \\ / : * ? \" < > | or control "
          "characters.");
  }

  const QString deviceName = name.section('.', 0, 0).toUpper();
  if (deviceName == "CON" || deviceName == "PRN" || deviceName == "AUX" ||
      deviceName == "NUL" ||
      ((deviceName.startsWith("COM") || deviceName.startsWith("LPT")) &&
       deviceName.size() == 4 && deviceName.at(3) >= '1' &&
       deviceName.at(3) <= '9'))
    return QObject::tr("That brush name is reserved by the operating system.");

  if (!TFilePath(name + ".pli").getFrame().isNoFrame())
    return QObject::tr("A brush name cannot end with a numbered frame suffix.");

  return QString();
}

QString suggestedBrushName() {
  TXshLevel *level = TTool::getApplication()->getCurrentLevel()->getLevel();
  QString name = level ? QString::fromStdWString(level->getName()) : QString();

  QString sanitized;
  const QString invalidCharacters = "\\/:*?\"<>|";
  sanitized.reserve(name.size());
  for (const QChar character : name) {
    sanitized.append(character.unicode() < 32 ||
                             invalidCharacters.contains(character)
                         ? QChar('_')
                         : character);
  }
  sanitized = sanitized.trimmed();
  while (sanitized.endsWith('.')) sanitized.chop(1);
  if (sanitized.isEmpty()) sanitized = QStringLiteral("custom");
  return sanitized + QStringLiteral("_brush");
}

void vectorizeToonzImageData(const TVectorImageP &image,
                             const ToonzImageData *tiData,
                             std::set<int> &indexes, TPalette *palette,
                             const VectorizerConfiguration &config) {
  if (!tiData) return;
  QApplication::setOverrideCursor(Qt::WaitCursor);
  TRasterP ras;
  std::vector<TRectD> rects;
  std::vector<TStroke> strokes;
  std::vector<TStroke> originalStrokes;
  TAffine affine;
  double dpiX, dpiY;
  tiData->getData(ras, dpiX, dpiY, rects, strokes, originalStrokes, affine,
                  image->getPalette());
  TRasterCM32P rasCM = ras;
  TToonzImageP ti(rasCM, rasCM->getBounds());
  VectorizerCore vc;
  TVectorImageP vi = vc.vectorize(ti, config, palette);
  assert(vi);
  vi->setPalette(palette);

  TScale sc(dpiX / Stage::inch, dpiY / Stage::inch);
  int i;
  TRectD selectionBounds;
  for (i = 0; i < (int)rects.size(); i++) selectionBounds += rects[i];
  for (i = 0; i < (int)strokes.size(); i++)
    selectionBounds += strokes[i].getBBox();
  TTranslation tr(selectionBounds.getP00());

  for (i = 0; i < (int)vi->getStrokeCount(); i++) {
    TStroke *stroke = vi->getStroke(i);
    stroke->transform(sc.inv() * affine * tr, true);
  }

  UINT oldImageSize = image->getStrokeCount();
  image->mergeImage(vi, TAffine());
  UINT newImageSize = image->getStrokeCount();
  indexes.clear();
  for (UINT sI = oldImageSize; sI < newImageSize; sI++) indexes.insert(sI);
  QApplication::restoreOverrideCursor();
}

//-----------------------------------------------------------------------------

void copyStrokesWithoutUndo(TVectorImageP image, std::set<int> &indexes) {
  QClipboard *clipboard = QApplication::clipboard();
  StrokesData *data     = new StrokesData();
  data->setImage(image, indexes);
  data->setClipboardFormats();
  clipboard->setMimeData(data, QClipboard::Clipboard);
}

//-----------------------------------------------------------------------------

bool pasteStrokesWithoutUndo(TVectorImageP image, std::set<int> &outIndexes,
                             TSceneHandle *sceneHandle, bool insert = true) {
  QMutexLocker lock(image->getMutex());
  QClipboard *clipboard = QApplication::clipboard();
  const StrokesData *stData =
      dynamic_cast<const StrokesData *>(clipboard->mimeData());
  std::unique_ptr<StrokesData> transferredData;
  if (!stData) {
    transferredData.reset(StrokesData::fromClipboard(clipboard->mimeData()));
    stData = transferredData.get();
  }
  const ToonzImageData *tiData =
      dynamic_cast<const ToonzImageData *>(clipboard->mimeData());
  const FullColorImageData *fciData =
      dynamic_cast<const FullColorImageData *>(clipboard->mimeData());
  std::set<int> indexes = outIndexes;
  if (stData)
    stData->getImage(image, indexes, insert);
  else if (tiData) {
    ToonzScene *scene = sceneHandle->getScene();
    assert(scene);

    const VectorizerParameters *vParams =
        scene->getProperties()->getVectorizerParameters();
    assert(vParams);

    std::unique_ptr<VectorizerConfiguration> config(
        vParams->getCurrentConfiguration(0.0));
    vectorizeToonzImageData(image, tiData, indexes, image->getPalette(),
                            *config);
  } else if (fciData) {
    DVGui::error(QObject::tr(
        "The copied selection cannot be pasted in the current drawing."));
    return false;
  } else
    return false;

  StrokeSelection *selection = dynamic_cast<StrokeSelection *>(
      TTool::getApplication()->getCurrentSelection()->getSelection());
  if (selection) selection->notifyView();
  outIndexes = indexes;  // outIndexes is a reference  to current  selection, so
                         // the notifyImageChanged could reset it!
  return true;
}

//-----------------------------------------------------------------------------

void deleteStrokesWithoutUndo(TVectorImageP image, std::set<int> &indexes) {
  QMutexLocker lock(image->getMutex());
  std::vector<int> indexesV(indexes.begin(), indexes.end());
  TRectD bbox;
  UINT i = 0;
  for (; i < indexesV.size(); i++)
    bbox += image->getStroke(indexesV[i])->getBBox();

  std::vector<TFilledRegionInf> regions;
  ImageUtils::getFillingInformationOverlappingArea(image, regions, bbox);

  TVectorImageP other = image->splitImage(indexesV, true);

  indexes.clear();

  TTool::getApplication()->getCurrentTool()->getTool()->notifyImageChanged();
  StrokeSelection *selection = dynamic_cast<StrokeSelection *>(
      TTool::getApplication()->getCurrentSelection()->getSelection());
  if (selection) selection->notifyView();
}

//-----------------------------------------------------------------------------

void cutStrokesWithoutUndo(TVectorImageP image, std::set<int> &indexes) {
  copyStrokesWithoutUndo(image, indexes);
  deleteStrokesWithoutUndo(image, indexes);
}

//=============================================================================
// CopyStrokesUndo
//-----------------------------------------------------------------------------

class CopyStrokesUndo final : public TUndo {
  QMimeData *m_oldData;
  QMimeData *m_newData;

public:
  CopyStrokesUndo(QMimeData *oldData, QMimeData *newData)
      : m_oldData(oldData), m_newData(newData) {}

  void undo() const override {
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setMimeData(cloneData(m_oldData), QClipboard::Clipboard);
  }

  void redo() const override {
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setMimeData(cloneData(m_newData), QClipboard::Clipboard);
  }

  int getSize() const override { return sizeof(*this); }
};

//=============================================================================
// PasteStrokesUndo
//-----------------------------------------------------------------------------

class PasteStrokesUndo final : public ToolUtils::TToolUndo {
  std::set<int> m_indexes;
  QMimeData *m_oldData;
  TSceneHandle *m_sceneHandle;

public:
  PasteStrokesUndo(TXshSimpleLevel *level, const TFrameId &frameId,
                   std::set<int> &indexes, TPaletteP oldPalette,
                   TSceneHandle *sceneHandle, bool createdFrame,
                   bool createdLevel)
      : TToolUndo(level, frameId, createdFrame, createdLevel, oldPalette)
      , m_indexes(indexes)
      , m_sceneHandle(sceneHandle) {
    QClipboard *clipboard = QApplication::clipboard();
    m_oldData             = cloneData(clipboard->mimeData());
  }

  ~PasteStrokesUndo() { delete m_oldData; }

  void undo() const override {
    TVectorImageP image = m_level->getFrame(m_frameId, true);

    // Se la selezione corrente e' la stroke selection devo svuotarla,
    // altrimenti puo' rimanere selezionato uno stroke che non esiste piu'.
    StrokeSelection *selection = dynamic_cast<StrokeSelection *>(
        TTool::getApplication()->getCurrentSelection()->getSelection());
    if (selection) selection->selectNone();

    std::set<int> indexes = m_indexes;
    deleteStrokesWithoutUndo(image, indexes);

    removeLevelAndFrameIfNeeded();

    TTool::getApplication()->getCurrentXsheet()->notifyXsheetChanged();
    notifyImageChanged();
  }

  void redo() const override {
    insertLevelAndFrameIfNeeded();

    TVectorImageP image   = m_level->getFrame(m_frameId, true);
    std::set<int> indexes = m_indexes;

    QClipboard *clipboard = QApplication::clipboard();
    QMimeData *data       = cloneData(clipboard->mimeData());

    clipboard->setMimeData(cloneData(m_oldData), QClipboard::Clipboard);

    pasteStrokesWithoutUndo(image, indexes, m_sceneHandle);
    TTool::getApplication()->getCurrentTool()->getTool()->notifyImageChanged();

    clipboard->setMimeData(data, QClipboard::Clipboard);

    TTool::getApplication()->getCurrentXsheet()->notifyXsheetChanged();
    notifyImageChanged();
  }

  int getSize() const override { return sizeof(*this); }
};

//--------------------------------------------------------------------

class RemoveEndpointsUndo final : public TUndo {
  TXshSimpleLevelP m_level;
  TFrameId m_frameId;
  std::vector<UndoVectorData> m_strokes;

public:
  RemoveEndpointsUndo(TXshSimpleLevel *level, const TFrameId &frameId,
                      std::vector<UndoVectorData> strokes)
      : m_level(level)
      , m_frameId(frameId)
      , m_strokes(strokes)

  {}

  ~RemoveEndpointsUndo() {
    int i;
    for (i = 0; i < (int)m_strokes.size(); i++) delete m_strokes[i].oldStroke;
  }

  void undo() const override {
    TVectorImageP vi = m_level->getFrame(m_frameId, true);
    int i;
    for (i = 0; i < (int)m_strokes.size(); i++) {
      TStroke *newS = new TStroke(*(m_strokes[i].oldStroke));
      newS->setId(m_strokes[i].oldStroke->getId());
      vi->restoreEndpoints(m_strokes[i].index, newS, m_strokes[i].offset);
    }
    StrokeSelection *selection = dynamic_cast<StrokeSelection *>(
        TTool::getApplication()->getCurrentSelection()->getSelection());
    if (selection) selection->selectNone();

    TTool::getApplication()->getCurrentTool()->getTool()->notifyImageChanged();
  }

  void redo() const override {
    int i;
    TVectorImageP vi = m_level->getFrame(m_frameId, true);
    for (i = 0; i < (int)m_strokes.size(); i++) {
      TStroke *s = vi->removeEndpoints(m_strokes[i].index);
      delete s;
      // assert(s==m_strokes[i].second);
    }
    TTool::getApplication()->getCurrentTool()->getTool()->notifyImageChanged();
  }

  int getSize() const override { return sizeof(*this); }
};
//=============================================================================
// DeleteFramesUndo
//-----------------------------------------------------------------------------

class DeleteStrokesUndo : public TUndo {
protected:
  TXshSimpleLevelP m_level;
  TFrameId m_frameId;
  std::set<int> m_indexes;
  QMimeData *m_data;
  TSceneHandle *m_sceneHandle;

public:
  DeleteStrokesUndo(TXshSimpleLevel *level, const TFrameId &frameId,
                    std::set<int> indexes, QMimeData *data,
                    TSceneHandle *sceneHandle)
      : m_level(level)
      , m_frameId(frameId)
      , m_indexes(indexes)
      , m_data(data)
      , m_sceneHandle(sceneHandle) {}

  ~DeleteStrokesUndo() { delete m_data; }

  void undo() const override {
    QClipboard *clipboard = QApplication::clipboard();
    QMimeData *oldData    = cloneData(clipboard->mimeData());

    clipboard->setMimeData(cloneData(m_data), QClipboard::Clipboard);
    std::set<int> indexes = m_indexes;
    TVectorImageP image   = m_level->getFrame(m_frameId, true);
    pasteStrokesWithoutUndo(image, indexes, m_sceneHandle, false);
    TTool::getApplication()->getCurrentTool()->getTool()->notifyImageChanged();

    clipboard->setMimeData(oldData, QClipboard::Clipboard);
  }

  void redo() const override {
    TVectorImageP image   = m_level->getFrame(m_frameId, true);
    std::set<int> indexes = m_indexes;
    deleteStrokesWithoutUndo(image, indexes);
  }

  int getSize() const override { return sizeof(*this); }
};

class OrderStrokesUndo : public TUndo {
protected:
  TXshSimpleLevelP m_level;
  TFrameId m_frameId;
  std::vector<int> m_oldIndexes;
  std::vector<int> m_newIndexes;

public:
  OrderStrokesUndo(TXshSimpleLevelP level, TFrameId frameId,
                   std::vector<int> oldIndexes, std::vector<int> newIndexes)
      : m_level(level)
      , m_frameId(frameId)
      , m_oldIndexes(oldIndexes)
      , m_newIndexes(newIndexes) {}

  ~OrderStrokesUndo() {}

  QString getHistoryString() {
    return UiTranslations::text(QObject::tr(
               "Sort Vector Strokes With Palette Order Level : %1 Frame : %2"))
        .arg(QString::fromStdWString(m_level->getName()))
        .arg(m_frameId.getNumber());
  }

  void undo() const override {
    TVectorImageP vi = m_level->getFrame(m_frameId, true);
    if (!vi) return;
    vi->reOrderStrokes(m_newIndexes, m_oldIndexes);
    TTool::getApplication()->getCurrentTool()->getTool()->notifyImageChanged();
  }

  void redo() const override {
    TVectorImageP vi = m_level->getFrame(m_frameId, true);
    if (!vi) return;
    vi->reOrderStrokes(m_oldIndexes, m_newIndexes);
    TTool::getApplication()->getCurrentTool()->getTool()->notifyImageChanged();
  }

  int getSize() const override { return sizeof(*this); }
};

//=============================================================================
// ConvertToOutlineShapesUndo
//-----------------------------------------------------------------------------

// Full image/palette snapshots preserve the original groups and every fill;
// conversion can introduce several editable boundary strokes per brush stroke.
class ConvertToOutlineShapesUndo final : public TUndo {
  TXshSimpleLevelP m_level;
  TFrameId m_frameId;
  TVectorImageP m_before, m_after;
  TPaletteP m_beforePalette, m_afterPalette;

  void restore(const TVectorImageP &snapshot, const TPaletteP &palette) const {
    // Keep the level palette identity shared by all its drawings. Only the
    // changed drawing is replaced; alpha styles and palette history restore
    // atomically with its native group/fill snapshot.
    const int paletteFrame = m_level->getPalette()->getFrame();
    m_level->getPalette()->assign(palette.getPointer());
    m_level->getPalette()->setFrame(paletteFrame);
    m_level->getPalette()->setDirtyFlag(true);
    TVectorImageP image = snapshot->clone();
    image->setPalette(m_level->getPalette());
    m_level->setFrame(m_frameId, image);
    TTool::Application *app = TTool::getApplication();
    TSelection *selection = app->getCurrentSelection()->getSelection();
    if (selection) selection->selectNone();
    app->getPaletteController()->getCurrentLevelPalette()->notifyPaletteChanged();
    app->getCurrentXsheet()->notifyXsheetChanged();
    app->getCurrentTool()->getTool()->notifyImageChanged(m_frameId, m_level.getPointer());
  }

public:
  ConvertToOutlineShapesUndo(TXshSimpleLevel *level, const TFrameId &frameId,
                             const TVectorImageP &before,
                             const TVectorImageP &after)
      : m_level(level), m_frameId(frameId), m_before(before->clone()),
        m_after(after->clone()), m_beforePalette(before->getPalette()->clone()),
        m_afterPalette(after->getPalette()->clone()) {
    m_before->setPalette(m_beforePalette.getPointer());
    m_after->setPalette(m_afterPalette.getPointer());
  }

  void undo() const override { restore(m_before, m_beforePalette); }
  void redo() const override { restore(m_after, m_afterPalette); }

  int getSize() const override {
    int bytes = sizeof(*this);
    for (const auto &image : {m_before, m_after})
      for (UINT i = 0; i < image->getStrokeCount(); ++i)
        bytes += sizeof(TStroke) + image->getStroke(i)->getControlPointCount() * sizeof(TThickPoint);
    return bytes;
  }
  QString getHistoryString() override {
    return QObject::tr("Convert Brush Strokes to Outline Shapes");
  }
};

//=============================================================================
// CutStrokesUndo
//-----------------------------------------------------------------------------

class CutStrokesUndo final : public DeleteStrokesUndo {
public:
  CutStrokesUndo(TXshSimpleLevel *level, const TFrameId &frameId,
                 std::set<int> indexes, QMimeData *data,
                 TSceneHandle *sceneHandle)
      : DeleteStrokesUndo(level, frameId, indexes, data, sceneHandle) {}

  ~CutStrokesUndo() {}

  void redo() const override {
    TVectorImageP image   = m_level->getFrame(m_frameId, true);
    std::set<int> indexes = m_indexes;
    cutStrokesWithoutUndo(image, indexes);
  }
};

// With palette Order
void sortStylesIds(QVector<int> &styles, TPaletteP palette) {
  if (!palette) return;
  QVector<int> tmpStyles = styles;
  styles.clear();
  for (int p = 0; p < palette->getPageCount(); p++) {
    TPalette::Page *page = palette->getPage(p);
    for (int s = 0; s < page->getStyleCount(); s++) {
      int tmpId = page->getStyleId(s);
      if (tmpStyles.indexOf(tmpId, 0) != -1) {
        styles.append(tmpId);
      }
      if (tmpStyles.count() == styles.count()) return;
    }
  }
  assert(false);
}

}  // namespace

//=============================================================================
//
// StrokeSelection ctor/dtor
//
//-----------------------------------------------------------------------------

StrokeSelection::StrokeSelection()
    : m_groupCommand(new TGroupCommand())
    , m_sceneHandle()
    , m_updateSelectionBBox(false) {
  m_groupCommand->setSelection(this);
}

//-----------------------------------------------------------------------------

StrokeSelection::~StrokeSelection() {}

//-----------------------------------------------------------------------------

StrokeSelection::StrokeSelection(const StrokeSelection &other)
    : m_vi(other.m_vi)
    , m_indexes(other.m_indexes)
    , m_selectionOrder(other.m_selectionOrder)
    , m_groupCommand(new TGroupCommand())
    , m_sceneHandle(other.m_sceneHandle)
    , m_updateSelectionBBox(other.m_updateSelectionBBox) {
  m_groupCommand->setSelection(this);
}

//-----------------------------------------------------------------------------

StrokeSelection &StrokeSelection::operator=(const StrokeSelection &other) {
  m_vi                  = other.m_vi;
  m_indexes             = other.m_indexes;
  m_selectionOrder      = other.m_selectionOrder;
  m_sceneHandle         = other.m_sceneHandle;
  m_updateSelectionBBox = other.m_updateSelectionBBox;

  return *this;
}

//-----------------------------------------------------------------------------

void StrokeSelection::select(int index, bool on) {
  if (m_selectionOrder.size() != m_indexes.size())
    m_selectionOrder = selectionOrder();
  if (on) {
    if (m_indexes.insert(index).second) m_selectionOrder.push_back(index);
  } else {
    m_indexes.erase(index);
    m_selectionOrder.erase(
        std::remove(m_selectionOrder.begin(), m_selectionOrder.end(), index),
        m_selectionOrder.end());
  }
}

void StrokeSelection::toggle(int index) { select(index, !isSelected(index)); }

//=============================================================================
//
// removeEndpoints
//
//-----------------------------------------------------------------------------

void StrokeSelection::removeEndpoints() {
  if (!m_vi) return;
  if (m_indexes.empty()) return;

  if (!isEditable()) {
    DVGui::error(
        QObject::tr("The selection cannot be updated. It is not editable."));
    return;
  }

  std::vector<UndoVectorData> undoData;

  m_vi->findRegions();
  for (auto const &e : m_indexes) {
    double offset;
    TStroke *s = m_vi->removeEndpoints(e, &offset);
    UndoVectorData undoStroke;

    undoStroke.index   = e;
    undoStroke.oldStroke = s;
    undoStroke.offset    = offset;

    if (s) undoData.push_back(undoStroke);
  }
  TTool *tool = TTool::getApplication()->getCurrentTool()->getTool();
  TXshSimpleLevel *level =
      TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  if (!undoData.empty())
    TUndoManager::manager()->add(
        new RemoveEndpointsUndo(level, tool->getCurrentFid(), undoData));

  m_updateSelectionBBox = true;
  tool->notifyImageChanged();
  m_updateSelectionBBox = false;
}

//=============================================================================
//
// sortWithPaletteOrder
//
//-----------------------------------------------------------------------------

void StrokeSelection::sortWithPaletteOrder() {
  if (!m_vi) return;
  if (m_indexes.empty()) return;

  if (!isEditable()) {
    DVGui::error(
        QObject::tr("The selection cannot be updated. It is not editable."));
    return;
  }
  
  TPalette *palette = m_vi->getPalette();
  TTool *tool       = TTool::getApplication()->getCurrentTool()->getTool();

  QHash<int, int> originalIndexToStyle;
  std::set<int> styles;
  std::vector<int> oldOrder;
  for (auto index : m_indexes) {
    TStroke *stroke = m_vi->getStroke(index);
    if (m_vi->isStrokeGrouped(index)) continue; 
    oldOrder.push_back(index);
    int styleId     = stroke->getStyle();
    styles.insert(stroke->getStyle());
    originalIndexToStyle.insert(index, styleId);
  }
  if (originalIndexToStyle.empty()) return;
  // Sort Styles
  QVector<int> styleOrder = QVector<int>(styles.begin(), styles.end());
  sortStylesIds(styleOrder, palette);
  // Build map from style to it's order
  QHash<int, int> styleIndexMap;
  for (int i = 0; i < styleOrder.size(); ++i) {
    styleIndexMap[styleOrder[i]] = i;
  }
  // Sort indexes by palette order
  std::vector<int> newOrder = oldOrder;
  std::stable_sort(newOrder.begin(), newOrder.end(), [&](int a, int b) {
    return styleIndexMap.value(originalIndexToStyle[a], -1) >
           styleIndexMap.value(originalIndexToStyle[b], -1);
  });
  // Remove don't need indexes
  std::vector<int> oldIndexes, newIndexes;
  for (int i = 0; i < oldOrder.size(); i++) {
    if (oldOrder[i] != newOrder[i]) {
      oldIndexes.push_back(newOrder[i]);
      newIndexes.push_back(oldOrder[i]);
    }
  }
  // Do reorder
  if (oldIndexes.empty()) return;
  m_vi->reOrderStrokes(oldIndexes, newIndexes);

  TXshSimpleLevel *level =
          TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  TUndoManager::manager()->add(new OrderStrokesUndo(
      level, tool->getCurrentFid(), oldIndexes, newIndexes));

  m_updateSelectionBBox = true;
  tool->notifyImageChanged();
  m_updateSelectionBBox = false;
}

//=============================================================================
//
// convertToOutlineShapes
//
//-----------------------------------------------------------------------------

void StrokeSelection::convertToOutlineShapes() {
  if (!m_vi) return;
  if (m_indexes.empty()) return;

  if (!isEditable()) {
    DVGui::error(
        QObject::tr("The selection cannot be updated. It is not editable."));
    return;
  }

  TTool *tool = TTool::getApplication()->getCurrentTool()->getTool();
  TXshSimpleLevel *level =
      TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  if (!tool || !level) return;

  TVectorImageP converted;
  try {
    converted = ContourOutline::convertImage(
        m_vi, std::vector<int>(m_indexes.begin(), m_indexes.end()));
  } catch (const std::exception &error) {
    DVGui::warning(QString::fromUtf8(error.what()));
    return;
  }
  if (!converted) {
    DVGui::warning(QObject::tr(
        "No selected stroke has thickness to convert to outline shapes."));
    return;
  }
  auto *undo = new ConvertToOutlineShapesUndo(
      level, tool->getCurrentFid(), m_vi, converted);
  undo->redo();
  TUndoManager::manager()->add(undo);
}

namespace {
class StrokeOpacityProfileUndo final : public ToolUtils::TToolUndo {
  std::vector<int> m_indices;
  std::vector<StrokeOpacity::Profile> m_before;
  StrokeOpacity::Profile m_after;
  void restore(bool after) const {
    TVectorImageP image = m_level->getFrame(m_frameId, true);
    if (!image) return;
    for (size_t i = 0; i < m_indices.size(); ++i) {
      if (m_indices[i] < 0 || m_indices[i] >= int(image->getStrokeCount())) return;
    }
    for (size_t i = 0; i < m_indices.size(); ++i)
      image->getStroke(m_indices[i])->setOpacityProfile(after ? m_after : m_before[i]);
    m_level->setDirtyFlag(true);
    TTool::getApplication()->getCurrentScene()->setDirtyFlag(true);
    notifyImageChanged();
  }
public:
  StrokeOpacityProfileUndo(TXshSimpleLevel *level, const TFrameId &fid,
                           const TVectorImageP &image, const std::set<int> &selection,
                           const StrokeOpacity::Profile &after)
      : TToolUndo(level, fid), m_indices(selection.begin(), selection.end()), m_after(after) {
    for (int index : m_indices) m_before.push_back(image->getStroke(index)->getOpacityProfile());
  }
  void undo() const override { restore(false); }
  void redo() const override { restore(true); }
  int getSize() const override {
    size_t knots = m_after.size();
    for (const auto &profile : m_before) knots += profile.size();
    const size_t size = sizeof(*this) + knots * sizeof(TPointD) + m_indices.size()*sizeof(int);
    return int(std::min<size_t>(size, std::numeric_limits<int>::max()));
  }
  QString getToolName() override { return QObject::tr("Edit Stroke Opacity Envelope"); }
};
}  // namespace

void StrokeSelection::editOpacityProfile() {
  if (!m_vi || m_indexes.empty() || !isEditable()) return;
  auto *app = TTool::getApplication();
  TXshSimpleLevelP level = app->getCurrentLevel()->getSimpleLevel();
  auto *tool = app->getCurrentTool()->getTool();
  if (!level || !tool) return;
  const TFrameId fid = tool->getCurrentFid();
  const TVectorImageP image = m_vi;
  const std::set<int> selected = m_indexes;
  std::map<int,int> selectedIds;
  for (int index : selected) {
    if (index < 0 || index >= int(image->getStrokeCount())) return;
    selectedIds.emplace(index,image->getStroke(index)->getId());
  }
  StrokeOpacityEditor::Dialog editor(image, selected, QApplication::activeWindow());
  if (editor.exec() != QDialog::Accepted) return;
  // Context-switch signals reject the dialog; also revalidate its immutable
  // selection before applying in case an external editor replaced the image.
  if (app->getCurrentLevel()->getSimpleLevel() != level.getPointer() ||
      tool->getCurrentFid() != fid || m_vi != image || m_indexes != selected ||
      !isEditable() || !StrokeOpacity::valid(editor.profile())) return;
  bool changed = false;
  for (int index : selected) {
    if (index >= int(image->getStrokeCount()) ||
        image->getStroke(index)->getId() != selectedIds[index]) return;
    changed |= image->getStroke(index)->getOpacityProfile() != editor.profile();
  }
  if (!changed) return;
  auto *undo = new StrokeOpacityProfileUndo(level.getPointer(), fid, image, selected, editor.profile());
  undo->redo();
  TUndoManager::manager()->add(undo);
}

//=============================================================================
//
// Inbetween stroke pairs
//
//-----------------------------------------------------------------------------

namespace {
struct MarkedMorphHint {
  TXshSimpleLevelP level;
  TFrameId fid;
  QJsonArray anchors;
  TRectD bounds;
  bool zone = false;
};
MarkedMorphHint &markedMorphHint() {
  static MarkedMorphHint mark;
  return mark;
}

class MorphHintsUndo final : public TUndo {
  TXshSimpleLevelP m_level;
  std::string m_before, m_after;
  void restore(const std::string &value) const {
    m_level->setMorphHints(value);
    TTool::getApplication()->getCurrentScene()->setDirtyFlag(true);
    TTool::getApplication()->getCurrentLevel()->notifyLevelChange();
  }
public:
  MorphHintsUndo(TXshSimpleLevel *level, const std::string &after)
      : m_level(level), m_before(level->getMorphHints()), m_after(after) {}
  void undo() const override { restore(m_before); }
  void redo() const override { restore(m_after); }
  int getSize() const override {
    return sizeof(*this) + int(m_before.size() + m_after.size());
  }
  QString getHistoryString() override { return QObject::tr("Edit Inbetween Hints"); }
};

bool storeMorphHints(TXshSimpleLevel *level, const QJsonArray &records) {
  const std::string value = MorphHints::encode(records);
  if (records.size() > MorphHints::MaxRecords || value.size() > MorphHints::MaxBytes) {
    DVGui::warning(QObject::tr("Too many Inbetween hints. Clear unused hints first."));
    return false;
  }
  if (level->getMorphHints() == value) return true;
  auto undo = new MorphHintsUndo(level, value);
  undo->redo();
  TUndoManager::manager()->add(undo);
  return true;
}

void markMorphHint(const TVectorImageP &image, const std::vector<int> &order,
                   bool zone) {
  if (!image || order.empty()) return;
  TTool *tool = TTool::getApplication()->getCurrentTool()->getTool();
  TXshSimpleLevel *level = TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  if (!tool || !level) return;
  const TFrameId fid = tool->getCurrentFid();
  QJsonArray anchors;
  TRectD bounds;
  for (int index : order) {
    if (index < 0 || index >= int(image->getStrokeCount())) return;
    anchors.append(MorphHints::anchor(image, index));
    bounds += image->getStroke(index)->getBBox();
  }
  auto &mark = markedMorphHint();
  if (!mark.level || mark.level.getPointer() != level || mark.fid == fid || mark.zone != zone) {
    mark.level = level;
    mark.fid = fid;
    mark.anchors = anchors;
    mark.bounds = bounds;
    mark.zone = zone;
    DVGui::info(zone ? QObject::tr("Zone marked. Select the matching zone in another drawing of this level and add the hint again.")
                    : QObject::tr("Strokes marked. Select matching strokes in another drawing of this level, in the same order, and pair again."));
    return;
  }
  if (!zone && mark.anchors.size() != anchors.size()) {
    DVGui::warning(QObject::tr("Select the same number of strokes as in the marked drawing."));
    return;
  }
  QJsonArray records;
  if (!MorphHints::decode(level->getMorphHints(), records)) {
    DVGui::warning(QObject::tr("This level contains unsupported or damaged Inbetween hints. Clear them explicitly before replacing them."));
    return;
  }
  TVectorImageP markedImage = level->getFrame(mark.fid, false);
  for (const auto &anchor : mark.anchors)
    if (MorphHints::resolve(anchor.toObject(), markedImage) < 0) {
      DVGui::warning(QObject::tr("The marked strokes changed or are ambiguous. Mark them again."));
      mark = MarkedMorphHint();
      return;
    }
  for (const auto &anchor : anchors)
    if (MorphHints::resolve(anchor.toObject(), image) < 0) {
      DVGui::warning(QObject::tr("The matching strokes are ambiguous. Select distinct strokes before pairing."));
      return;
    }
  const int count = zone ? 1 : anchors.size();
  for (int i = 0; i < count; ++i) {
    QJsonObject record{{"kind", zone ? "zone" : "pair"},
                       {"firstFrame", MorphHints::frame(mark.fid)},
                       {"lastFrame", MorphHints::frame(fid)},
                       {"first", mark.anchors[i]}, {"last", anchors[i]}};
    if (zone) {
      record["firstRect"] = MorphHints::rect(mark.bounds);
      record["lastRect"] = MorphHints::rect(bounds);
      record["firstMembers"] = mark.anchors;
      record["lastMembers"] = anchors;
    }
    // Re-pairing a stroke replaces its previous choice for these drawings.
    QJsonArray kept;
    for (const auto &old : records) {
      const auto o = old.toObject();
      const bool forward = MorphHints::frame(o["firstFrame"].toObject()) == mark.fid &&
                           MorphHints::frame(o["lastFrame"].toObject()) == fid;
      const bool reverse = MorphHints::frame(o["firstFrame"].toObject()) == fid &&
                           MorphHints::frame(o["lastFrame"].toObject()) == mark.fid;
      const bool conflicts = o["kind"] == record["kind"] &&
          ((forward && (o["first"] == record["first"] || o["last"] == record["last"])) ||
           (reverse && (o["last"] == record["first"] || o["first"] == record["last"])));
      if (!conflicts) kept.append(old);
    }
    records = kept;
    records.append(record);
  }
  if (storeMorphHints(level, records)) {
    mark = MarkedMorphHint();
    DVGui::info(QObject::tr("Inbetween hints recorded. Save the scene to retain them."));
  }
}

QJsonArray resolvedHintRecords(TXshSimpleLevel *level, const TFrameId &first,
                               const TFrameId &last) {
  QJsonArray records, result;
  if (!level || !MorphHints::decode(level->getMorphHints(), records)) return result;
  for (const auto &value : records) {
    auto record = value.toObject();
    const auto a = MorphHints::frame(record["firstFrame"].toObject());
    const auto b = MorphHints::frame(record["lastFrame"].toObject());
    if (a == first && b == last) result.append(record);
    else if (a == last && b == first) {
      const auto anchor = record["first"].toObject();
      record["first"] = record["last"];
      record["last"] = anchor;
      const auto rect = record["firstRect"].toArray();
      record["firstRect"] = record["lastRect"];
      record["lastRect"] = rect;
      if (record.contains("firstMembers") && record.contains("lastMembers")) {
        const auto members = record["firstMembers"].toArray();
        record["firstMembers"] = record["lastMembers"];
        record["lastMembers"] = members;
      }
      result.append(record);
    }
  }
  return result;
}
} // namespace

void StrokeSelection::pairStrokesForInbetween() {
  markMorphHint(m_vi, selectionOrder(), false);
}
void StrokeSelection::addInbetweenZoneHint() {
  markMorphHint(m_vi, selectionOrder(), true);
}
void StrokeSelection::clearInbetweenStrokePairs() {
  markedMorphHint() = MarkedMorphHint();
  auto level = TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  if (level) storeMorphHints(level, {});
}
bool StrokeSelection::hasInbetweenStrokePairs() {
  auto level = TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
  return level && (!level->getMorphHints().empty() ||
                  markedMorphHint().level.getPointer() == level);
}
std::vector<std::pair<int, int>> StrokeSelection::getInbetweenStrokePairs(
    TXshSimpleLevel *level, const TFrameId &first, const TFrameId &last) {
  std::vector<std::pair<int, int>> result;
  if (!level) return result;
  TVectorImageP firstImage = level->getFrame(first, false), lastImage = level->getFrame(last, false);
  for (const auto &value : resolvedHintRecords(level, first, last)) {
    const auto record = value.toObject();
    if (record["kind"].toString() != "pair") continue;
    int a = MorphHints::resolve(record["first"].toObject(), firstImage);
    int b = MorphHints::resolve(record["last"].toObject(), lastImage);
    if (a >= 0 && b >= 0) result.emplace_back(a, b);
  }
  return result;
}
std::vector<std::pair<TRectD, TRectD>> StrokeSelection::getInbetweenZoneHints(
    TXshSimpleLevel *level, const TFrameId &first, const TFrameId &last) {
  std::vector<std::pair<TRectD, TRectD>> result;
  if (!level) return result;
  TVectorImageP firstImage = level->getFrame(first, false), lastImage = level->getFrame(last, false);
  for (const auto &value : resolvedHintRecords(level, first, last)) {
    const auto record = value.toObject();
    if (record["kind"].toString() != "zone") continue;
    if (MorphHints::resolve(record["first"].toObject(), firstImage) < 0 ||
        MorphHints::resolve(record["last"].toObject(), lastImage) < 0) continue;
    bool valid = true;
    for (const auto &anchor : record["firstMembers"].toArray())
      if (MorphHints::resolve(anchor.toObject(), firstImage) < 0) valid = false;
    for (const auto &anchor : record["lastMembers"].toArray())
      if (MorphHints::resolve(anchor.toObject(), lastImage) < 0) valid = false;
    if (!valid) continue;
    result.emplace_back(MorphHints::rect(record["firstRect"].toArray()),
                        MorphHints::rect(record["lastRect"].toArray()));
  }
  return result;
}

//=============================================================================
//
// selectAll
//
//-----------------------------------------------------------------------------

void StrokeSelection::selectAll() {
  if (!m_vi) return;

  int sCount = int(m_vi->getStrokeCount());

  ArtLayer::Filter filter(*m_vi);
  for (int s = 0; s < sCount; ++s) {
    if (filter.isEditable(s)) m_indexes.insert(s);
  }

  StrokeSelection *selection = dynamic_cast<StrokeSelection *>(
      TTool::getApplication()->getCurrentSelection()->getSelection());
  if (selection) selection->notifyView();
  TTool::getApplication()
      ->getCurrentTool()
      ->notifyToolChanged();  // Refreshes toolbar values
}

//=============================================================================
//
// deleteStrokes
//
//-----------------------------------------------------------------------------

void StrokeSelection::deleteStrokes() {
  if (!m_vi) return;
  if (m_indexes.empty()) return;
  TTool *tool = TTool::getApplication()->getCurrentTool()->getTool();
  if (!tool) return;

  if (!isEditable()) {
    DVGui::error(
        QObject::tr("The selection cannot be deleted. It is not editable."));
    return;
  }

  bool isSpline = tool->getApplication()->getCurrentObject()->isSpline();
  TUndo *undo;
  if (isSpline)
    undo = new ToolUtils::UndoPath(
        tool->getXsheet()->getStageObject(tool->getObjectId())->getSpline());

  StrokesData *data = new StrokesData();
  data->setImage(m_vi, m_indexes);
  std::set<int> oldIndexes = m_indexes;
  deleteStrokesWithoutUndo(m_vi, m_indexes);

  if (!isSpline) {
    TXshSimpleLevel *level =
        TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
    TUndoManager::manager()->add(new DeleteStrokesUndo(
        level, tool->getCurrentFid(), oldIndexes, data, m_sceneHandle));
  } else {
    assert(undo);
    if (undo) TUndoManager::manager()->add(undo);
  }
}

//=============================================================================
//
// StrokeSelection::copy()
//
//-----------------------------------------------------------------------------

void StrokeSelection::copy() {
  if (m_indexes.empty()) return;
  QClipboard *clipboard = QApplication::clipboard();
  QMimeData *oldData    = cloneData(clipboard->mimeData());
  copyStrokesWithoutUndo(m_vi, m_indexes);
  QMimeData *newData = cloneData(clipboard->mimeData());

  // TUndoManager::manager()->add(new CopyStrokesUndo(oldData, newData));
}

//=============================================================================
//
// StrokeSelection::saveAsCustomVectorBrush()
//
//-----------------------------------------------------------------------------

void StrokeSelection::saveAsCustomVectorBrush() {
  saveSelectedVectors(QApplication::keyboardModifiers() & Qt::ShiftModifier);
}

//-----------------------------------------------------------------------------

void StrokeSelection::saveVectorSelectionAs() { saveSelectedVectors(true); }

//-----------------------------------------------------------------------------

void StrokeSelection::saveSelectedVectors(bool chooseDestination) {
  if (!m_vi || m_indexes.empty()) return;

  std::vector<int> indexes(m_indexes.begin(), m_indexes.end());
  for (int index : indexes) {
    if (index < 0 || index >= static_cast<int>(m_vi->getStrokeCount())) {
      DVGui::error(QObject::tr(
          "The vector selection is no longer valid. Please select it again."));
      return;
    }
  }

  TVectorImageP selectedImage;
  {
    QMutexLocker lock(m_vi->getMutex());
    selectedImage = m_vi->splitImage(indexes, false);
  }
  if (!selectedImage || selectedImage->getStrokeCount() == 0) return;

  QWidget *parent = QApplication::activeWindow();
  const TFilePath brushFolder =
      ToonzFolder::getLibraryFolder() + "vector brushes";
  TFilePath destination;

  if (chooseDestination) {
    TFilePath initialFolder = brushFolder;
    TXshLevel *level = TTool::getApplication()->getCurrentLevel()->getLevel();
    if (level && !level->getPath().isEmpty()) {
      TFilePath levelPath = level->getPath();
      if (m_sceneHandle && m_sceneHandle->getScene())
        levelPath = m_sceneHandle->getScene()->decodeFilePath(levelPath);
      if (!levelPath.isEmpty()) initialFolder = levelPath.getParentDir();
    }

    const TFilePath initialPath =
        initialFolder + TFilePath(suggestedBrushName() + ".pli");
    QString fileName = QFileDialog::getSaveFileName(
        parent, QObject::tr("Save Vector Selection As"),
        initialPath.getQString(), QObject::tr("OpenToonz Vector Level (*.pli)"),
        nullptr, QFileDialog::DontConfirmOverwrite);
    if (fileName.isEmpty()) return;

    destination = TFilePath(fileName);
    if (QString::fromStdString(destination.getType())
            .compare("pli", Qt::CaseInsensitive) != 0)
      destination = TFilePath(destination.getWideString() + L".pli");
  } else {
    QString proposedName = suggestedBrushName();
    QString brushName;
    while (brushName.isEmpty()) {
      bool accepted = false;
      QString input = QInputDialog::getText(
          parent, QObject::tr("Save as Custom Vector Brush"),
          QObject::tr("Brush name:"), QLineEdit::Normal, proposedName,
          &accepted);
      if (!accepted) return;

      input               = normalizeBrushName(input);
      const QString error = validateBrushName(input);
      if (!error.isEmpty()) {
        DVGui::warning(error);
        proposedName = input;
        continue;
      }
      brushName = input;
    }
    destination = brushFolder + TFilePath(brushName + ".pli");
  }

  bool appendToDestination = false;
  if (TFileStatus(destination).doesExist()) {
    const QString question =
        chooseDestination
            ? UiTranslations::text(QObject::tr("The file '%1' already exists."))
                  .arg(destination.getQString())
            : UiTranslations::text(QObject::tr("The custom vector brush '%1' already exists."))
                  .arg(QString::fromStdWString(destination.getWideName()));
    const int choice =
        DVGui::MsgBox(question, QObject::tr("Overwrite"), QObject::tr("Append"),
                      QObject::tr("Cancel"), 2, parent);
    if (choice != 1 && choice != 2) return;
    appendToDestination = choice == 2;
  }

  const TFilePath destinationFolder = destination.getParentDir();
  const bool installsCustomBrush    = destinationFolder == brushFolder;

  TTool *tool = TTool::getApplication()->getCurrentTool()->getTool();
  const TFrameId sourceFrameId = tool ? tool->getCurrentFid() : TFrameId(1);

  CustomVectorBrushExporter::Source source;
  source.preferredFirstFrame = sourceFrameId;
  source.frames.emplace_back(sourceFrameId, selectedImage);

  CustomVectorBrushExporter::PreparedFile prepared;
  QString errorMessage;
  if (!CustomVectorBrushExporter::prepare(source, destinationFolder, prepared,
                                          errorMessage)) {
    DVGui::error(errorMessage);
    return;
  }

  if (installsCustomBrush &&
      prepared.serializedBytes > RecommendedCustomVectorBrushBytes) {
    const TINT64 sizeInKilobytes = (prepared.serializedBytes + 1023) / 1024;
    const QString question =
        UiTranslations::text(QObject::tr(
            "This custom vector brush is %1 KB, above the recommended size "
            "of 32 KB. Large vector brushes may reduce drawing and rendering "
            "performance. Continue?"))
            .arg(sizeInKilobytes);
    if (DVGui::MsgBox(question, QObject::tr("Continue"), QObject::tr("Cancel"),
                      1, parent) != 1) {
      CustomVectorBrushExporter::discard(prepared);
      return;
    }
  }

  if (appendToDestination && !CustomVectorBrushExporter::prepareAppend(
                                 source, destination, prepared, errorMessage)) {
    CustomVectorBrushExporter::discard(prepared);
    DVGui::error(errorMessage);
    return;
  }

  if (!CustomVectorBrushExporter::commit(prepared, destination, errorMessage)) {
    CustomVectorBrushExporter::discard(prepared);
    DVGui::error(errorMessage);
    return;
  }

  FolderListenerManager::instance()->notifyFolderChanged(destinationFolder);
}

//=============================================================================
//
// StrokeSelection::paste()
//
//-----------------------------------------------------------------------------

void StrokeSelection::paste() {
  TTool *tool = TTool::getApplication()->getCurrentTool()->getTool();
  if (!tool) return;
  if (!isEditable()) {
    DVGui::error(
        QObject::tr("The selection cannot be pasted. It is not editable."));
    return;
  }

  if (TTool::getApplication()->getCurrentObject()->isSpline()) {
    const StrokesData *stData = dynamic_cast<const StrokesData *>(
        QApplication::clipboard()->mimeData());
    std::unique_ptr<StrokesData> transferredData;
    if (!stData) {
      transferredData.reset(
          StrokesData::fromClipboard(QApplication::clipboard()->mimeData()));
      stData = transferredData.get();
    }
    if (!stData) return;
    TVectorImageP splineImg = tool->getImage(true);
    TVectorImageP img       = stData->m_image;
    if (!splineImg || !img) return;

    QMutexLocker lock(splineImg->getMutex());
    TUndo *undo = new ToolUtils::UndoPath(
        tool->getXsheet()->getStageObject(tool->getObjectId())->getSpline());
    while (splineImg->getStrokeCount() > 0) splineImg->deleteStroke(0);

    TStroke *stroke = img->getStroke(0);
    splineImg->addStroke(new TStroke(*stroke), false);
    TUndoManager::manager()->add(undo);
    tool->notifyImageChanged();
    tool->invalidate();
    return;
  }

  TVectorImageP tarImg = tool->touchImage();
  if (!tarImg) return;
  TPaletteP palette       = tarImg->getPalette();
  TPaletteP oldPalette    = new TPalette();
  if (palette) oldPalette = palette->clone();
  bool isPaste = pasteStrokesWithoutUndo(tarImg, m_indexes, m_sceneHandle);
  if (isPaste) {
    TXshSimpleLevel *level =
        TTool::getApplication()->getCurrentLevel()->getSimpleLevel();
    TUndoManager::manager()->add(new PasteStrokesUndo(
        level, tool->getCurrentFid(), m_indexes, oldPalette, m_sceneHandle,
        tool->m_isFrameCreated, tool->m_isLevelCreated));
    m_updateSelectionBBox = isPaste;
  }
  tool->notifyImageChanged();
  tool->getApplication()
      ->getPaletteController()
      ->getCurrentLevelPalette()
      ->notifyPaletteChanged();
  m_updateSelectionBBox = false;
  tool->invalidate();
}

//=============================================================================
//
// StrokeSelection::cut()
//
//-----------------------------------------------------------------------------

void StrokeSelection::cut() {
  if (m_indexes.empty()) return;
  TTool *tool = TTool::getApplication()->getCurrentTool()->getTool();
  if (!tool) return;

  if (!isEditable()) {
    DVGui::error(
        QObject::tr("The selection cannot be deleted. It is not editable."));
    return;
  }

  bool isSpline = tool->getApplication()->getCurrentObject()->isSpline();
  TUndo *undo;
  if (isSpline)
    undo = new ToolUtils::UndoPath(
        tool->getXsheet()->getStageObject(tool->getObjectId())->getSpline());

  StrokesData *data = new StrokesData();
  data->setImage(m_vi, m_indexes);
  std::set<int> oldIndexes = m_indexes;
  cutStrokesWithoutUndo(m_vi, m_indexes);
  if (!isSpline) {
    TXshSimpleLevel *level =
        tool->getApplication()->getCurrentLevel()->getSimpleLevel();
    TUndoManager::manager()->add(new CutStrokesUndo(
        level, tool->getCurrentFid(), oldIndexes, data, m_sceneHandle));
  } else {
    assert(undo);
    if (undo) TUndoManager::manager()->add(undo);
  }
}

//=============================================================================
//
// enableCommands
//
//-----------------------------------------------------------------------------

void StrokeSelection::enableCommands() {
  enableCommand(this, MI_Clear, &StrokeSelection::deleteStrokes);
  enableCommand(this, MI_Cut, &StrokeSelection::cut);
  enableCommand(this, MI_Copy, &StrokeSelection::copy);
  enableCommand(this, MI_Paste, &StrokeSelection::paste);
  enableCommand(this, MI_SaveAsCustomVectorBrush,
                &StrokeSelection::saveAsCustomVectorBrush);

  enableCommand(m_groupCommand.get(), MI_Group, &TGroupCommand::group);
  enableCommand(m_groupCommand.get(), MI_Ungroup, &TGroupCommand::ungroup);
  enableCommand(m_groupCommand.get(), MI_BringToFront, &TGroupCommand::front);
  enableCommand(m_groupCommand.get(), MI_BringForward, &TGroupCommand::forward);
  enableCommand(m_groupCommand.get(), MI_SendBack, &TGroupCommand::back);
  enableCommand(m_groupCommand.get(), MI_SendBackward,
                &TGroupCommand::backward);
  enableCommand(m_groupCommand.get(), MI_EnterGroup,
                &TGroupCommand::enterGroup);
  enableCommand(m_groupCommand.get(), MI_ExitGroup, &TGroupCommand::exitGroup);

  enableCommand(this, MI_RemoveEndpoints, &StrokeSelection::removeEndpoints);
  enableCommand(this, MI_SortWithPaletteOrder, &StrokeSelection::sortWithPaletteOrder);
  enableCommand(this, MI_ConvertToOutlineShapes,
                &StrokeSelection::convertToOutlineShapes);
  enableWidthProfileCommands();
  enableCommand(this, MI_EditStrokeOpacityProfile,
                &StrokeSelection::editOpacityProfile);
  enableCommand(this, MI_PairInbetweenStrokes,
                &StrokeSelection::pairStrokesForInbetween);
  enableCommand(this, MI_AddInbetweenZoneHint,
                &StrokeSelection::addInbetweenZoneHint);
  enableCommand(this, MI_ClearInbetweenStrokePairs,
                &StrokeSelection::clearInbetweenStrokePairs);
  enableCommand(this, MI_SelectAll, &StrokeSelection::selectAll);
  enableCommand(this, MI_AlignLeft, &StrokeSelection::alignStrokes,
                VectorAlignment::ALIGN_LEFT);
  enableCommand(this, MI_AlignRight, &StrokeSelection::alignStrokes,
                VectorAlignment::ALIGN_RIGHT);
  enableCommand(this, MI_AlignTop, &StrokeSelection::alignStrokes,
                VectorAlignment::ALIGN_TOP);
  enableCommand(this, MI_AlignBottom, &StrokeSelection::alignStrokes,
                VectorAlignment::ALIGN_BOTTOM);
  enableCommand(this, MI_AlignCenterHorizontal, &StrokeSelection::alignStrokes,
                VectorAlignment::ALIGN_CENTER_H);
  enableCommand(this, MI_AlignCenterVertical, &StrokeSelection::alignStrokes,
                VectorAlignment::ALIGN_CENTER_V);
  enableCommand(this, MI_DistributeHorizontal, &StrokeSelection::alignStrokes,
                VectorAlignment::DISTRIBUTE_H);
  enableCommand(this, MI_DistributeVertical, &StrokeSelection::alignStrokes,
                VectorAlignment::DISTRIBUTE_V);
}

//===================================================================
namespace {
//===================================================================

class UndoSetStrokeStyle final : public TUndo {
  TVectorImageP m_image;
  std::vector<int> m_strokeIndexes;
  std::vector<int> m_oldStyles;
  int m_newStyle;

public:
  UndoSetStrokeStyle(TVectorImageP image, int newStyle)
      : m_image(image), m_newStyle(newStyle) {}

  void addStroke(TStroke *stroke) {
    m_strokeIndexes.push_back(m_image->getStrokeIndex(stroke));
    m_oldStyles.push_back(stroke->getStyle());
  }

  void undo() const override {
    UINT size = m_strokeIndexes.size();
    assert(size == m_oldStyles.size());

    for (UINT i = 0; i != size; i++) {
      int index = m_strokeIndexes[i];
      if (index != -1 && index < (int)m_image->getStrokeCount())
        m_image->getStroke(index)->setStyle(m_oldStyles[i]);
    }

    TTool::getApplication()->getCurrentTool()->getTool()->notifyImageChanged();
  }

  void redo() const override {
    UINT size = m_strokeIndexes.size();
    assert(size == m_oldStyles.size());

    for (UINT i = 0; i != size; i++) {
      int index = m_strokeIndexes[i];
      if (index != -1 && index < (int)m_image->getStrokeCount())
        m_image->getStroke(index)->setStyle(m_newStyle);
    }

    TTool::getApplication()->getCurrentTool()->getTool()->notifyImageChanged();
  }

  int getSize() const override {
    return sizeof(*this) +
           m_strokeIndexes.capacity() * sizeof(m_strokeIndexes[0]) +
           m_oldStyles.capacity() * sizeof(m_oldStyles[0]);
  }
};

}  // namespace

//=============================================================================
//
// changeColorStyle
//
//-----------------------------------------------------------------------------

void StrokeSelection::changeColorStyle(int styleIndex) {
  TTool *tool = TTool::getApplication()->getCurrentTool()->getTool();
  if (!tool) return;
  TVectorImageP img(tool->getImage(true));
  if (!img) return;
  TPalette *palette = img->getPalette();
  TColorStyle *cs   = palette->getStyle(styleIndex);
  if (!cs->isStrokeStyle()) return;
  if (m_indexes.empty()) return;

  UndoSetStrokeStyle *undo = new UndoSetStrokeStyle(img, styleIndex);
  std::set<int>::iterator it;
  for (it = m_indexes.begin(); it != m_indexes.end(); ++it) {
    int index = *it;
    assert(0 <= index && index < (int)img->getStrokeCount());
    TStroke *stroke = img->getStroke(index);
    undo->addStroke(stroke);
    stroke->setStyle(styleIndex);
  }

  tool->notifyImageChanged();
  TUndoManager::manager()->add(undo);
}

//-----------------------------------------------------------------------------

bool StrokeSelection::isEditable() {
  TTool::Application *app = TTool::getApplication();
  TXshSimpleLevel *level  = app->getCurrentLevel()->getSimpleLevel();

  TFrameHandle *frame = app->getCurrentFrame();
  bool filmstrip      = frame->isEditingLevel();

  if (level) {
    if (level->isReadOnly()) return false;

    TFrameId frameId = app->getCurrentTool()->getTool()->getCurrentFid();
    if (level->isFrameReadOnly(frameId)) return false;
  }

  if (!filmstrip) {
    int colIndex = app->getCurrentTool()->getTool()->getColumnIndex();
    int rowIndex = frame->getFrame();
    if (app->getCurrentTool()->getTool()->isColumnLocked(colIndex))
      return false;

    TXsheet *xsh      = app->getCurrentXsheet()->getXsheet();
    TStageObject *obj = xsh->getStageObject(TStageObjectId::ColumnId(colIndex));
    // Test for Mesh-deformed levels
    const TStageObjectId &parentId = obj->getParent();
    if (parentId.isColumn() && obj->getParentHandle()[0] != 'H') {
      TXshSimpleLevel *parentSl =
          xsh->getCell(rowIndex, parentId.getIndex()).getSimpleLevel();
      if (parentSl && parentSl->getType() == MESH_XSHLEVEL) return false;
    }
  }

  return true;
}

// Adapted from manongjohn's Tahoma2D PR #1275. A selected group is one
// alignment object, including every member below the currently entered group.
namespace {
struct AlignmentObject {
  std::vector<int> strokes;
  TRectD box;
};

std::vector<AlignmentObject> alignmentObjects(const TVectorImageP &vi,
                                              const std::vector<int> &order) {
  std::vector<AlignmentObject> objects;
  std::set<int> included;
  int depth = vi->isInsideGroup();
  for (int index : order) {
    if (index < 0 || index >= (int)vi->getStrokeCount() ||
        included.count(index) || !vi->isEnteredGroupStroke(index))
      continue;
    AlignmentObject object;
    object.strokes.push_back(index);
    object.box = vi->getStroke(index)->getBBox();
    included.insert(index);
    if (vi->getGroupDepth(index) > depth) {
      for (int i = 0; i < (int)vi->getStrokeCount(); ++i) {
        if (included.count(i) || !vi->isEnteredGroupStroke(i) ||
            vi->getCommonGroupDepth(index, i) <= depth)
          continue;
        object.strokes.push_back(i);
        object.box += vi->getStroke(i)->getBBox();
        included.insert(i);
      }
    }
    objects.push_back(object);
  }
  return objects;
}

class AlignStrokesUndo final : public ToolUtils::TToolUndo {
  std::vector<int> m_indexes;
  std::vector<std::unique_ptr<TStroke>> m_before, m_after;
  std::vector<TFilledRegionInf> m_beforeRegions, m_afterRegions;

  void apply(bool forward) const {
    TVectorImageP vi = m_level->getFrame(m_frameId, true);
    if (!vi) return;
    QMutexLocker lock(vi->getMutex());
    const auto &target = forward ? m_after : m_before;
    std::vector<std::unique_ptr<TStroke>> previous;
    std::vector<TStroke *> previousPtrs;
    for (size_t i = 0; i < m_indexes.size(); ++i) {
      int index = m_indexes[i];
      if (index < 0 || index >= (int)vi->getStrokeCount()) return;
      TStroke *stroke = vi->getStroke(index);
      if (stroke->getControlPointCount() != target[i]->getControlPointCount())
        return;
      previous.emplace_back(new TStroke(*stroke));
      previousPtrs.push_back(previous.back().get());
    }
    vi->findRegions();
    for (size_t i = 0; i < m_indexes.size(); ++i) {
      TStroke *stroke = vi->getStroke(m_indexes[i]);
      for (int j = 0; j < stroke->getControlPointCount(); ++j)
        stroke->setControlPoint(j, target[i]->getControlPoint(j));
    }
    vi->notifyChangedStrokes(m_indexes, previousPtrs);
    ImageUtils::assignFillingInformation(
        *vi, forward ? m_afterRegions : m_beforeRegions);
    lock.unlock();
    m_level->touchFrame(m_frameId);
    notifyImageChanged();
    TTool::Application *app = TTool::getApplication();
    TTool *tool             = app->getCurrentTool()->getTool();
    if (tool && TVectorImageP(tool->getImage(false)) == vi) {
      StrokeSelection *selection = dynamic_cast<StrokeSelection *>(
          app->getCurrentSelection()->getSelection());
      if (selection)
        selection->notifyAlignmentChanged();
      else
        tool->notifyImageChanged();
    }
    app->getCurrentXsheet()->notifyXsheetChanged();
  }

public:
  AlignStrokesUndo(TXshSimpleLevel *level, const TFrameId &fid,
                   const TVectorImageP &vi, const std::vector<int> &indexes)
      : TToolUndo(level, fid), m_indexes(indexes) {
    for (int index : indexes)
      m_before.emplace_back(new TStroke(*vi->getStroke(index)));
    ImageUtils::getFillingInformationOverlappingArea(vi, m_beforeRegions,
                                                     vi->getBBox());
  }
  void captureAfter(const TVectorImageP &vi) {
    for (int index : m_indexes)
      m_after.emplace_back(new TStroke(*vi->getStroke(index)));
    ImageUtils::getFillingInformationOverlappingArea(vi, m_afterRegions,
                                                     vi->getBBox());
  }
  void undo() const override { apply(false); }
  void redo() const override { apply(true); }
  int getSize() const override {
    int size = sizeof(*this);
    for (const auto &stroke : m_before)
      size += 2 * (sizeof(TStroke) +
                   stroke->getControlPointCount() * sizeof(TThickPoint));
    return size + (m_beforeRegions.size() + m_afterRegions.size()) *
                      sizeof(TFilledRegionInf);
  }
  QString getHistoryString() override {
    return QObject::tr("Align and Distribute Vector Strokes");
  }
};
}  // namespace

std::vector<int> StrokeSelection::selectionOrder() const {
  std::vector<int> order;
  std::set<int> remaining = m_indexes;
  for (int index : m_selectionOrder)
    if (remaining.erase(index)) order.push_back(index);
  // Existing callers can populate the set directly (paste, select all, etc.).
  order.insert(order.end(), remaining.begin(), remaining.end());
  return order;
}

void StrokeSelection::notifyAlignmentChanged() {
  TTool *tool = TTool::getApplication()->getCurrentTool()->getTool();
  if (!tool) return;
  m_updateSelectionBBox = true;
  tool->notifyImageChanged();
  m_updateSelectionBBox = false;
}

void StrokeSelection::alignStrokes(VectorAlignment::Type type) {
  using namespace VectorAlignment;
  TTool::Application *app = TTool::getApplication();
  TTool *tool = app->getCurrentTool()->getTool();
  TXshSimpleLevel *level = app->getCurrentLevel()->getSimpleLevel();
  auto *selectionTool = dynamic_cast<VectorSelectionTool *>(tool);
  if (!m_vi || !selectionTool || !tool->isEnabled() || !level ||
      app->getCurrentObject()->isSpline() || tool->isDragging()) return;
  if (TVectorImageP(tool->getImage(false)) != m_vi) return;
  if (!isEditable()) {
    DVGui::error(QObject::tr("The selection is not editable."));
    return;
  }
  const bool acrossFrames = selectionTool->isLevelType() ||
                            selectionTool->isSelectedFramesType();
  if (!acrossFrames && isEmpty()) return;
  std::vector<TFrameId> frames;
  if (acrossFrames) {
    level->getFids(frames);
    if (selectionTool->isSelectedFramesType()) {
      const auto &selected = selectionTool->getTargetFrames();
      frames.erase(std::remove_if(frames.begin(), frames.end(),
          [&](const TFrameId &fid) { return !selected.count(fid); }), frames.end());
    }
  } else frames.push_back(tool->getCurrentFid());

  // The camera reference is the active scene time for every selected drawing.
  // Other reference methods are evaluated independently inside each drawing;
  // first/last refer to stacking order outside the active manual selection.
  Method method = tool->getAlignMethod();
  TRectD camera;
  if (method == CAMERA_AREA) {
    ToonzScene *scene = app->getCurrentScene()->getScene();
    if (!scene || !scene->getCurrentCamera()) return;
    camera = scene->getCurrentCamera()->getStageRect();
    if (!app->getCurrentFrame()->isEditingLevel()) {
      int col = app->getCurrentColumn()->getColumnIndex();
      TXsheet *xsh = tool->getXsheet();
      if (!xsh || col < 0) return;
      int frame = app->getCurrentFrame()->getFrameIndex();
      TAffine levelAff = xsh->getPlacement(TStageObjectId::ColumnId(col), frame);
      if (std::abs(levelAff.det()) < 1e-12) return;
      TAffine cameraAff = xsh->getPlacement(
          TStageObjectId::CameraId(xsh->getCameraColumnIndex()), frame);
      camera = (levelAff.inv() * cameraAff) * camera;
    }
  }
  // Prepare all results before changing any drawing. A locked drawing rejects
  // the operation as a whole, and repeated exposures are processed once.
  std::vector<std::unique_ptr<AlignStrokesUndo>> edits;
  for (const TFrameId &fid : frames) {
    if (level->isFrameReadOnly(fid)) {
      DVGui::warning(QObject::tr("A selected drawing is read-only. No drawings were aligned."));
      return;
    }
    TVectorImageP image = level->getFrame(fid, false);
    if (!image) continue;
    QMutexLocker lock(image->getMutex());
    TVectorImageP proposed = image->cloneWithGroupContext();
    auto order = acrossFrames
        ? getSelectedStrokes(*proposed, selectionTool->levelSelection())
        : selectionOrder();
    ArtLayer::Filter filter(*proposed);
    order.erase(std::remove_if(order.begin(), order.end(),
        [&](int index) { return !filter.isEditable(index); }), order.end());
    auto objects = alignmentObjects(proposed, order);
    // A group is indivisible. Never move a hidden or locked art role merely
    // because another member of its group is selected.
    objects.erase(std::remove_if(objects.begin(), objects.end(),
        [&](const AlignmentObject &object) {
          return std::any_of(object.strokes.begin(), object.strokes.end(),
              [&](int index) { return !filter.isEditable(index); });
        }), objects.end());
    std::vector<TRectD> boxes;
    for (const auto &object : objects) boxes.push_back(object.box);
    auto deltas = offsets(boxes, type, method, camera);
    std::vector<int> indexes;
    for (size_t i = 0; i < objects.size(); ++i)
      if (deltas[i] != TPointD())
        indexes.insert(indexes.end(), objects[i].strokes.begin(), objects[i].strokes.end());
    if (indexes.empty()) continue;
    proposed->findRegions();
    auto undo = std::make_unique<AlignStrokesUndo>(level, fid, proposed, indexes);
    std::vector<std::unique_ptr<TStroke>> previous;
    std::vector<TStroke *> previousPtrs;
    for (size_t i = 0; i < objects.size(); ++i) {
      if (deltas[i] == TPointD()) continue;
      for (int index : objects[i].strokes) {
        TStroke *stroke = proposed->getStroke(index);
        previous.emplace_back(new TStroke(*stroke));
        previousPtrs.push_back(previous.back().get());
        stroke->transform(TTranslation(deltas[i]));
      }
    }
    proposed->notifyChangedStrokes(indexes, previousPtrs);
    undo->captureAfter(proposed);
    edits.push_back(std::move(undo));
  }
  if (edits.empty()) return;
  TUndoManager::manager()->beginBlock();
  for (auto &undo : edits) {
    undo->redo();
    TUndoManager::manager()->add(undo.release());
  }
  TUndoManager::manager()->endBlock();
  notifyAlignmentChanged();
  app->getCurrentXsheet()->notifyXsheetChanged();
}
