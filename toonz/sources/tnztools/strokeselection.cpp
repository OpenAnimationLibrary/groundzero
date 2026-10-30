

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

// The strokes are changed in place (same index, id, style and group): only
// their control points and the self loop flag differ.
class ConvertToOutlineShapesUndo final : public TUndo {
public:
  struct Item {
    int m_index;
    std::vector<TThickPoint> m_before, m_after;
    bool m_selfLoopBefore;

    Item() : m_index(-1), m_selfLoopBefore(false) {}
  };

private:
  TXshSimpleLevelP m_level;
  TFrameId m_frameId;
  std::vector<Item> m_items;
  TRectD m_area;  // where the fills are saved
  std::vector<TFilledRegionInf> m_fillBefore, m_fillAfter;

  // Sets the control points of the strokes, before or after the conversion
  void reshapeStrokes(const TVectorImageP &vi, bool after) const {
    std::vector<int> indexes;
    std::vector<TStroke *> oldStrokes;
    for (const Item &item : m_items) {
      if (item.m_index < 0 || item.m_index >= (int)vi->getStrokeCount())
        continue;
      TStroke *stroke = vi->getStroke(item.m_index);
      oldStrokes.push_back(new TStroke(*stroke));
      indexes.push_back(item.m_index);

      const std::vector<TThickPoint> &points =
          after ? item.m_after : item.m_before;
      stroke->reshape(&points[0], (int)points.size());
      stroke->setSelfLoop(after ? true : item.m_selfLoopBefore);
    }
    vi->notifyChangedStrokes(indexes, oldStrokes);
    for (TStroke *oldStroke : oldStrokes) delete oldStroke;
    vi->findRegions();
  }

  void applyFills(const TVectorImageP &vi,
                  const std::vector<TFilledRegionInf> &fills) const {
    for (const TFilledRegionInf &fill : fills) {
      TRegion *region = vi->getRegion(fill.m_regionId);
      if (region) region->setStyle(fill.m_styleId);
    }
  }

  void notify() const {
    TTool::Application *app    = TTool::getApplication();
    StrokeSelection *selection = dynamic_cast<StrokeSelection *>(
        app->getCurrentSelection()->getSelection());
    if (selection) selection->selectNone();
    app->getCurrentXsheet()->notifyXsheetChanged();
    app->getCurrentTool()->getTool()->notifyImageChanged();
  }

public:
  ConvertToOutlineShapesUndo(TXshSimpleLevel *level, const TFrameId &frameId,
                             const std::vector<Item> &items, const TRectD &area,
                             const std::vector<TFilledRegionInf> &fillBefore)
      : m_level(level)
      , m_frameId(frameId)
      , m_items(items)
      , m_area(area)
      , m_fillBefore(fillBefore) {}

  // Fills, with style, the unfilled regions whose inside lies within the
  // closed outline stroke. Each region is tested at one of its internal points
  // against a polygon sampled along the stroke: testing the regions against
  // a copy of the stroke itself (selectFill) compares points that lie on the
  // outline, where the answer depends on rounding.
  static void fillInside(const TVectorImageP &vi, const TStroke &outline,
                         int style) {
    const int samples = std::max(64, 8 * outline.getChunkCount());
    std::vector<TPointD> polygon;
    polygon.reserve(samples);
    for (int i = 0; i < samples; i++)
      polygon.push_back(outline.getPoint((double)i / samples));

    auto inside = [&polygon](const TPointD &p) {
      bool in = false;
      for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const TPointD &a = polygon[i], &b = polygon[j];
        if ((a.y > p.y) != (b.y > p.y) &&
            p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
          in = !in;
      }
      return in;
    };

    const TRectD bbox = outline.getBBox();
    std::vector<TRegion *> regions;
    for (UINT i = 0; i < vi->getRegionCount(); i++)
      regions.push_back(vi->getRegion(i));
    while (!regions.empty()) {
      TRegion *region = regions.back();
      regions.pop_back();
      if (!region || !bbox.overlaps(region->getBBox())) continue;
      for (UINT i = 0; i < region->getSubregionCount(); i++)
        regions.push_back(region->getSubregion(i));
      TPointD p;
      if (region->getStyle() == 0 && region->getInternalPoint(p) && inside(p))
        region->setStyle(style);
    }
  }

  // Does the conversion the first time. The new shapes are filled with the
  // style of their stroke, as a brush stroke has no fill of its own.
  void convert() {
    TVectorImageP vi = m_level->getFrame(m_frameId, true);
    if (!vi) return;
    QMutexLocker lock(vi->getMutex());
    reshapeStrokes(vi, true);
    for (const Item &item : m_items) {
      if (item.m_index < 0 || item.m_index >= (int)vi->getStrokeCount())
        continue;
      TStroke *stroke = vi->getStroke(item.m_index);
      fillInside(vi, *stroke, stroke->getStyle());
    }
    ImageUtils::getFillingInformationOverlappingArea(vi, m_fillAfter, m_area);
  }

  void undo() const override {
    TVectorImageP vi = m_level->getFrame(m_frameId, true);
    if (!vi) return;
    {
      QMutexLocker lock(vi->getMutex());
      reshapeStrokes(vi, false);
      applyFills(vi, m_fillBefore);
    }
    notify();
  }

  void redo() const override {
    TVectorImageP vi = m_level->getFrame(m_frameId, true);
    if (!vi) return;
    {
      QMutexLocker lock(vi->getMutex());
      reshapeStrokes(vi, true);
      applyFills(vi, m_fillAfter);
    }
    notify();
  }

  int getSize() const override {
    int points = 0;
    for (const Item &item : m_items)
      points += (int)(item.m_before.size() + item.m_after.size());
    return sizeof(*this) + points * sizeof(TThickPoint) +
           (m_fillBefore.size() + m_fillAfter.size()) *
               sizeof(TFilledRegionInf);
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

  // Compute all the outlines first: nothing is changed if none can be
  std::vector<ConvertToOutlineShapesUndo::Item> items;
  TRectD area;
  for (int index : m_indexes) {
    TStroke *stroke = m_vi->getStroke(index);

    ConvertToOutlineShapesUndo::Item item;
    if (!ContourOutline::makeOutlineControlPoints(*stroke, item.m_after))
      continue;

    item.m_index = index;
    for (int i = 0; i < stroke->getControlPointCount(); i++)
      item.m_before.push_back(stroke->getControlPoint(i));
    item.m_selfLoopBefore = stroke->isSelfLoop();
    items.push_back(item);

    area += stroke->getBBox();
    TRectD shapeBox(item.m_after[0].x, item.m_after[0].y,
                    item.m_after[0].x + 1e-3, item.m_after[0].y + 1e-3);
    for (const TThickPoint &p : item.m_after)
      shapeBox += TRectD(p.x, p.y, p.x + 1e-3, p.y + 1e-3);
    area += shapeBox;
  }

  if (items.empty()) {
    DVGui::warning(
        QObject::tr("No selected stroke can be converted. Only open strokes "
                    "with some thickness can be converted to outline "
                    "shapes."));
    return;
  }

  area = area.enlarge(1.0);
  std::vector<TFilledRegionInf> fillBefore;
  ImageUtils::getFillingInformationOverlappingArea(m_vi, fillBefore, area);

  ConvertToOutlineShapesUndo *undo = new ConvertToOutlineShapesUndo(
      level, tool->getCurrentFid(), items, area, fillBefore);
  undo->convert();
  TUndoManager::manager()->add(undo);

  m_updateSelectionBBox = true;
  tool->notifyImageChanged();
  m_updateSelectionBBox = false;
}

//=============================================================================
//
// Inbetween stroke pairs
//
//-----------------------------------------------------------------------------

namespace {

// Strokes are identified by id, which is unique in the session, so the hints
// silently stop applying when the paired strokes no longer exist.
struct InbetweenStrokePairs {
  const TVectorImage *m_markedImage = nullptr;  // only compared
  std::vector<int> m_markedIds;
  std::vector<std::pair<int, int>> m_pairs;
};

InbetweenStrokePairs &inbetweenStrokePairs() {
  static InbetweenStrokePairs pairs;
  return pairs;
}

// Zone hints for the Inbetween command. A hint is a pair of zones, one on each
// key drawing. As for the stroke pairs, the drawings are identified through the
// id of a stroke they contain (the first selected one), so a hint stops
// applying when that stroke is gone.
// The drawings are also remembered by pointer, because a key drawing
// duplicated from the other one holds the same stroke ids.
struct InbetweenZoneHints {
  struct Hint {
    int m_idA, m_idB;
    TRectD m_rectA, m_rectB;
    const TVectorImage *m_imageA, *m_imageB;  // only compared
  };

  int m_markedId                    = -1;       // -1: nothing marked yet
  const TVectorImage *m_markedImage = nullptr;  // only compared
  TRectD m_markedRect;
  std::vector<Hint> m_hints;
};

InbetweenZoneHints &inbetweenZoneHints() {
  static InbetweenZoneHints hints;
  return hints;
}

}  // namespace

//-----------------------------------------------------------------------------

void StrokeSelection::pairStrokesForInbetween() {
  if (!m_vi || m_indexes.empty()) return;

  std::vector<int> ids;
  for (int index : selectionOrder())
    ids.push_back(m_vi->getStroke(index)->getId());

  InbetweenStrokePairs &data = inbetweenStrokePairs();

  // first step: remember the strokes selected in the first key drawing
  if (data.m_markedIds.empty() || data.m_markedImage == m_vi.getPointer()) {
    data.m_markedImage = m_vi.getPointer();
    data.m_markedIds   = ids;
    DVGui::info(
        UiTranslations::text(QObject::tr("%1 stroke(s) marked for Inbetween. Select the matching "
                    "strokes in the other key drawing, in the same order, "
                    "and use Pair Strokes for Inbetween again."))
            .arg((int)ids.size()));
    return;
  }

  // second step: pair them with the strokes selected in this key drawing
  if (ids.size() != data.m_markedIds.size()) {
    DVGui::warning(
        UiTranslations::text(QObject::tr("The number of selected strokes (%1) does not match the "
                    "number of marked strokes (%2)."))
            .arg((int)ids.size())
            .arg((int)data.m_markedIds.size()));
    return;
  }

  for (int i = 0; i < (int)ids.size(); i++) {
    int id0 = data.m_markedIds[i], id1 = ids[i];
    // a new pair replaces the previous pairs of the same strokes
    data.m_pairs.erase(
        std::remove_if(data.m_pairs.begin(), data.m_pairs.end(),
                       [id0, id1](const std::pair<int, int> &pair) {
                         return pair.first == id0 || pair.second == id0 ||
                                pair.first == id1 || pair.second == id1;
                       }),
        data.m_pairs.end());
    data.m_pairs.push_back(std::make_pair(id0, id1));
  }
  data.m_markedImage = nullptr;
  data.m_markedIds.clear();

  DVGui::info(
      UiTranslations::text(QObject::tr("%1 stroke pair(s) set for Inbetween.")).arg((int)ids.size()));
}

//-----------------------------------------------------------------------------

void StrokeSelection::clearInbetweenStrokePairs() {
  InbetweenStrokePairs &data = inbetweenStrokePairs();
  data.m_markedImage         = nullptr;
  data.m_markedIds.clear();
  data.m_pairs.clear();

  InbetweenZoneHints &zones = inbetweenZoneHints();
  zones.m_markedId          = -1;
  zones.m_markedImage       = nullptr;
  zones.m_markedRect        = TRectD();
  zones.m_hints.clear();
}

//-----------------------------------------------------------------------------

bool StrokeSelection::hasInbetweenStrokePairs() {
  const InbetweenStrokePairs &data = inbetweenStrokePairs();
  const InbetweenZoneHints &zones  = inbetweenZoneHints();
  return !data.m_pairs.empty() || !data.m_markedIds.empty() ||
         !zones.m_hints.empty() || zones.m_markedId >= 0;
}

//-----------------------------------------------------------------------------

std::vector<std::pair<int, int>> StrokeSelection::getInbetweenStrokePairs(
    const TVectorImageP &firstImage, const TVectorImageP &lastImage) {
  std::vector<std::pair<int, int>> indexPairs;
  if (!firstImage || !lastImage) return indexPairs;

  for (const std::pair<int, int> &pair : inbetweenStrokePairs().m_pairs) {
    // pairs can be set from either key drawing
    int index0 = firstImage->getStrokeIndexById(pair.first);
    int index1 = lastImage->getStrokeIndexById(pair.second);
    if (index0 < 0 || index1 < 0) {
      index0 = firstImage->getStrokeIndexById(pair.second);
      index1 = lastImage->getStrokeIndexById(pair.first);
    }
    if (index0 >= 0 && index1 >= 0)
      indexPairs.push_back(std::make_pair(index0, index1));
  }
  return indexPairs;
}

//=============================================================================
//
// Inbetween zone hints
//
//-----------------------------------------------------------------------------

void StrokeSelection::addInbetweenZoneHint() {
  if (!m_vi || m_indexes.empty()) return;

  std::vector<int> order = selectionOrder();
  if (order.empty()) return;
  TRectD box;
  for (int index : order) box += m_vi->getStroke(index)->getBBox();
  int anchorId = m_vi->getStroke(order.front())->getId();

  InbetweenZoneHints &data = inbetweenZoneHints();

  // first step: remember the zone in the first key drawing (the call is
  // repeated on the same drawing: replace the zone)
  if (data.m_markedId < 0 || data.m_markedImage == m_vi.getPointer()) {
    data.m_markedId    = anchorId;
    data.m_markedImage = m_vi.getPointer();
    data.m_markedRect  = box;
    DVGui::info(QObject::tr(
        "Zone marked for Inbetween. Select the strokes of the "
        "matching zone in the other key drawing and use Add Inbetween "
        "Zone Hint from Selection again."));
    return;
  }

  // second step: the zone of this key drawing is the target
  InbetweenZoneHints::Hint hint;
  hint.m_idA    = data.m_markedId;
  hint.m_idB    = anchorId;
  hint.m_rectA  = data.m_markedRect;
  hint.m_rectB  = box;
  hint.m_imageA = data.m_markedImage;
  hint.m_imageB = m_vi.getPointer();
  data.m_hints.push_back(hint);
  data.m_markedId    = -1;
  data.m_markedImage = nullptr;
  data.m_markedRect  = TRectD();

  DVGui::info(UiTranslations::text(QObject::tr("%1 zone hint(s) set for Inbetween."))
                  .arg((int)data.m_hints.size()));
}

//-----------------------------------------------------------------------------

std::vector<std::pair<TRectD, TRectD>> StrokeSelection::getInbetweenZoneHints(
    const TVectorImageP &firstImage, const TVectorImageP &lastImage) {
  std::vector<std::pair<TRectD, TRectD>> rects;
  if (!firstImage || !lastImage) return rects;

  for (const InbetweenZoneHints::Hint &hint : inbetweenZoneHints().m_hints) {
    // hints can be set from either key drawing
    bool idsAB = firstImage->getStrokeIndexById(hint.m_idA) >= 0 &&
                 lastImage->getStrokeIndexById(hint.m_idB) >= 0;
    bool idsBA = firstImage->getStrokeIndexById(hint.m_idB) >= 0 &&
                 lastImage->getStrokeIndexById(hint.m_idA) >= 0;
    // a key drawing duplicated from the other holds the same stroke ids: the
    // drawings the hint was set on decide then
    if (idsAB && idsBA && hint.m_imageA == lastImage.getPointer() &&
        hint.m_imageB == firstImage.getPointer())
      idsAB = false;
    if (idsAB)
      rects.push_back(std::make_pair(hint.m_rectA, hint.m_rectB));
    else if (idsBA)
      rects.push_back(std::make_pair(hint.m_rectB, hint.m_rectA));
  }
  return rects;
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
  TTool *tool             = app->getCurrentTool()->getTool();
  TXshSimpleLevel *level  = app->getCurrentLevel()->getSimpleLevel();
  if (!m_vi || isEmpty() || !tool || !tool->isEnabled() || !level ||
      app->getCurrentObject()->isSpline() || tool->isDragging())
    return;
  if (TVectorImageP(tool->getImage(false)) != m_vi) return;
  // These commands operate on the displayed drawing only. Multi-frame/level
  // selection has different semantics and is deliberately left to its tool.
  VectorSelectionTool *selectionTool =
      dynamic_cast<VectorSelectionTool *>(tool);
  if (!selectionTool || selectionTool->isLevelType() ||
      selectionTool->isSelectedFramesType())
    return;
  if (!isEditable()) {
    DVGui::error(QObject::tr("The selection is not editable."));
    return;
  }
  QMutexLocker lock(m_vi->getMutex());
  auto objects = alignmentObjects(m_vi, selectionOrder());
  std::vector<TRectD> boxes;
  for (const auto &object : objects) boxes.push_back(object.box);
  Method method = tool->getAlignMethod();
  TRectD camera;
  if (method == CAMERA_AREA) {
    ToonzScene *scene = app->getCurrentScene()->getScene();
    if (!scene || !scene->getCurrentCamera()) return;
    camera = scene->getCurrentCamera()->getStageRect();
    if (!app->getCurrentFrame()->isEditingLevel()) {
      int col      = app->getCurrentColumn()->getColumnIndex();
      TXsheet *xsh = tool->getXsheet();
      if (!xsh || col < 0) return;
      int frame = app->getCurrentFrame()->getFrameIndex();
      TAffine levelAff =
          xsh->getPlacement(TStageObjectId::ColumnId(col), frame);
      if (std::abs(levelAff.det()) < 1e-12) return;
      TAffine cameraAff = xsh->getPlacement(
          TStageObjectId::CameraId(xsh->getCameraColumnIndex()), frame);
      // Align to the camera's axis-aligned bounds in drawing coordinates.
      camera = (levelAff.inv() * cameraAff) * camera;
    }
  }
  auto deltas = offsets(boxes, type, method, camera);
  std::vector<int> indexes;
  for (size_t i = 0; i < objects.size(); ++i)
    if (deltas[i] != TPointD())
      indexes.insert(indexes.end(), objects[i].strokes.begin(),
                     objects[i].strokes.end());
  if (indexes.empty()) return;
  m_vi->findRegions();
  auto undo = std::make_unique<AlignStrokesUndo>(level, tool->getCurrentFid(),
                                                 m_vi, indexes);
  std::vector<std::unique_ptr<TStroke>> previous;
  std::vector<TStroke *> previousPtrs;
  for (size_t i = 0; i < objects.size(); ++i) {
    if (deltas[i] == TPointD()) continue;
    for (int index : objects[i].strokes) {
      TStroke *stroke = m_vi->getStroke(index);
      previous.emplace_back(new TStroke(*stroke));
      previousPtrs.push_back(previous.back().get());
      stroke->transform(TTranslation(deltas[i]));
    }
  }
  m_vi->notifyChangedStrokes(indexes, previousPtrs);
  undo->captureAfter(m_vi);
  lock.unlock();
  TUndoManager::manager()->add(undo.release());
  notifyAlignmentChanged();
  app->getCurrentXsheet()->notifyXsheetChanged();
}
