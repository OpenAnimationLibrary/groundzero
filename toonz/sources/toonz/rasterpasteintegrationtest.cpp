#include "rasterpasteintegrationtest.h"

#include "cellselection.h"
#include "menubarcommandids.h"
#include "nativeuiintegrationtest.h"
#include "tapp.h"
#include "tcolorstyles.h"
#include "timagecache.h"
#include "tpalette.h"
#include "tsimplecolorstyles.h"
#include "trasterimage.h"
#include "ttoonzimage.h"
#include "tundo.h"
#include "tools/rasterselection.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "toonz/levelset.h"
#include "toonz/preferences.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/dvmimedata.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/rasterimagedata.h"
#include "toonzqt/strokesdata.h"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QClipboard>
#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QTimer>

#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
constexpr int canvasSize = 16;
constexpr double dpi     = 96.0;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void resetScene() {
  QString error;
  if (!resetNativeUiIntegrationScene(&error))
    throw std::runtime_error(error.toStdString());
}

class SavedPasteState final {
  std::unique_ptr<QMimeData> m_clipboard;
  QString m_tool;
  int m_levelType;
  double m_width, m_height, m_dpi;
  bool m_cameraSize;

public:
  SavedPasteState()
      : m_tool(TApp::instance()->getCurrentTool()->getRequestedToolName())
      , m_levelType(Preferences::instance()->getDefLevelType())
      , m_width(Preferences::instance()->getDefLevelWidth())
      , m_height(Preferences::instance()->getDefLevelHeight())
      , m_dpi(Preferences::instance()->getDefLevelDpi())
      , m_cameraSize(
            Preferences::instance()->isNewLevelSizeToCameraSizeEnabled()) {
    const auto* source = QApplication::clipboard()->mimeData();
    const auto* native = dynamic_cast<const DvMimeData*>(source);
    m_clipboard.reset(native ? native->clone() : new QMimeData);
    if (!source) return;
    for (const auto& format : source->formats())
      m_clipboard->setData(format, source->data(format));
    if (source->hasImage()) m_clipboard->setImageData(source->imageData());
  }

  ~SavedPasteState() {
    auto* preferences = Preferences::instance();
    preferences->setValue(DefLevelType, m_levelType, false);
    preferences->setValue(DefLevelWidth, m_width, false);
    preferences->setValue(DefLevelHeight, m_height, false);
    preferences->setValue(DefLevelDpi, m_dpi, false);
    preferences->setValue(newLevelSizeToCameraSizeEnabled, m_cameraSize, false);
    TApp::instance()->getCurrentTool()->setTool(m_tool);
    QApplication::clipboard()->setMimeData(m_clipboard.release());
  }
};

TPixel32 pixelAt(int x, int y) {
  return (x + 2 * y) % 3 == 0 ? TPixel32::Blue : TPixel32::Green;
}

TPaletteP makePalette() {
  TPaletteP palette = new TPalette;
  palette->setStyle(1, TPixel32::Blue);
  require(palette->addStyle(TPixel32::Green) == 2,
          "raster paste fixture palette did not have two initial styles");
  return palette;
}

TRasterCM32P makeIndexedRaster(bool painted) {
  TRasterCM32P raster(canvasSize, canvasSize);
  raster->fill(TPixelCM32());
  if (painted)
    for (int y = 0; y < canvasSize; ++y)
      for (int x = 0; x < canvasSize; ++x)
        raster->pixels(y)[x] =
            TPixelCM32(pixelAt(x, y) == TPixel32::Blue ? 1 : 2, 0, 0);
  return raster;
}

TRasterP makeRaster(int levelType, bool painted) {
  if (levelType == TZP_XSHLEVEL) return makeIndexedRaster(painted);
  TRaster32P raster(canvasSize, canvasSize);
  raster->clear();
  if (painted)
    for (int y = 0; y < canvasSize; ++y)
      for (int x = 0; x < canvasSize; ++x) raster->pixels(y)[x] = pixelAt(x, y);
  return raster;
}

std::unique_ptr<RasterImageData> rasterPayload(int levelType,
                                               const TPaletteP& palette) {
  std::unique_ptr<RasterImageData> data;
  if (levelType == TZP_XSHLEVEL)
    data.reset(new ToonzImageData);
  else
    data.reset(new FullColorImageData);
  data->setData(makeRaster(levelType, true), palette, dpi, dpi,
                TDimension(canvasSize, canvasSize),
                {TRectD(-canvasSize / 2, -canvasSize / 2, canvasSize / 2,
                        canvasSize / 2)},
                {}, {}, TAffine());
  return data;
}

void setNativeClipboard(int levelType, const TPaletteP& palette) {
  auto data = rasterPayload(levelType, palette);
  QImage preview(canvasSize, canvasSize, QImage::Format_ARGB32);
  preview.fill(Qt::red);
  data->setImageData(preview);
  QApplication::clipboard()->setMimeData(data.release());
  require(!QApplication::clipboard()->image().isNull(),
          "native raster clipboard fixture is missing its external preview");
}

TXshSimpleLevel* makeLevel(int levelType, const TPaletteP& palette) {
  auto* app   = TApp::instance();
  auto* level = app->getCurrentScene()
                    ->getScene()
                    ->createNewLevel(levelType, L"RasterPasteFixture",
                                     TDimension(canvasSize, canvasSize), dpi)
                    ->getSimpleLevel();
  level->setPalette(palette.getPointer());
  TImageP image;
  if (levelType == PLI_XSHLEVEL) {
    image = TVectorImageP(new TVectorImage);
  } else if (levelType == TZP_XSHLEVEL) {
    TRasterCM32P raster = makeRaster(levelType, false);
    TToonzImageP drawing(raster, raster->getBounds());
    drawing->setDpi(dpi, dpi);
    image = drawing;
  } else {
    TRasterImageP drawing(makeRaster(levelType, false));
    drawing->setDpi(dpi, dpi);
    image = drawing;
  }
  image->setPalette(palette.getPointer());
  level->setFrame(TFrameId(1), image);
  app->getCurrentXsheet()->getXsheet()->setCell(0, 0,
                                                TXshCell(level, TFrameId(1)));
  app->getCurrentXsheet()->notifyXsheetChanged();
  return level;
}

void checkIndexedPixel(const TToonzImageP& image, int x, int y, bool painted) {
  const auto pixel = image->getRaster()->pixels(y)[x];
  if (!painted) {
    require(pixel == TPixelCM32(), "raster paste undo left indexed pixels");
    return;
  }
  require(pixel.getTone() == 0 && pixel.getPaint() == 0,
          "native indexed paste changed ink coverage");
  auto* palette = image->getPalette();
  require(palette && pixel.getInk() < palette->getStyleCount(),
          "native indexed paste contains an invalid palette index");
  require(
      palette->getStyle(pixel.getInk())->getMainColor() == pixelAt(x, y),
      "native indexed paste used preview pixels or changed their positions");
}

void checkRasterPixels(const TImageP& image, bool painted) {
  TToonzImageP indexed    = image;
  TRasterImageP fullColor = image;
  for (int y = 0; y < canvasSize; ++y) {
    for (int x = 0; x < canvasSize; ++x) {
      if (indexed) {
        checkIndexedPixel(indexed, x, y, painted);
      } else {
        TRaster32P pixels = fullColor->getRaster();
        require(pixels && pixels->pixels(y)[x] ==
                              (painted ? pixelAt(x, y) : TPixel32(0, 0, 0, 0)),
                "native fullcolor paste used preview pixels or changed their "
                "positions");
      }
    }
  }
}

void checkPixels(const TXshCell& cell, int levelType, bool painted) {
  require(!cell.isEmpty() && cell.getSimpleLevel()->getType() == levelType,
          "native raster paste changed the destination level type");
  TImageP image           = cell.getImage(false);
  TToonzImageP indexed    = image;
  TRasterImageP fullColor = image;
  TRasterP raster;
  double dpiX = 0.0, dpiY = 0.0;
  if (levelType == TZP_XSHLEVEL) {
    require(bool(indexed), "native indexed paste lost its indexed image");
    raster = indexed->getRaster();
    indexed->getDpi(dpiX, dpiY);
  } else {
    require(bool(fullColor), "native fullcolor paste lost its raster image");
    raster = fullColor->getRaster();
    fullColor->getDpi(dpiX, dpiY);
  }
  require(raster && raster->getSize() == TDimension(canvasSize, canvasSize) &&
              dpiX == dpi && dpiY == dpi,
          "native raster paste changed canvas size or native DPI");
  checkRasterPixels(image, painted);
}

bool cancelExternalDialog(QDialog* dialog) {
  const QString prompt = QObject::tr(
      "Pasting external image from clipboard.\n\nWhat do you want to do?");
  bool expected = false;
  for (auto* label : dialog->findChildren<QLabel*>())
    if (label->text() == prompt) expected = true;
  if (!expected) return false;
  for (auto* button : dialog->findChildren<QPushButton*>()) {
    if (button->text() == QObject::tr("Cancel")) {
      button->click();
      return true;
    }
  }
  return false;
}

// The real Paste action is synchronous. This scoped timer handles only dialogs
// opened by that call, and closes unexpected dialogs before reporting failure.
void pasteCell(int row, int col, bool external, bool floating = false) {
  auto* app   = TApp::instance();
  auto* sheet = app->getCurrentXsheet()->getXsheet();
  app->getCurrentFrame()->setFrame(row);
  app->getCurrentColumn()->setColumnIndex(col);
  app->getCurrentColumn()->setColumn(sheet->getColumn(col));
  app->getCurrentLevel()->setLevel(
      sheet->getCell(row, col).m_level.getPointer());
  app->getCurrentTool()->setTool(floating ? "T_Selection" : "T_Brush");
  TCellSelection selection;
  struct ReleaseSelection {
    TSelection* selection;
    ~ReleaseSelection() { selection->makeNotCurrent(); }
  } release{&selection};
  selection.selectCell(row, col);
  selection.makeCurrent();
  RasterSelection* rasterSelection = nullptr;
  if (floating) {
    rasterSelection = dynamic_cast<RasterSelection*>(
        app->getCurrentTool()->getTool()->getSelection());
    require(rasterSelection, "Raster Selection tool is unavailable");
    rasterSelection->makeCurrent();
    release.selection = rasterSelection;
  }
  auto* action = CommandManager::instance()->getAction(MI_Paste);
  require(action && action->isEnabled(), "Xsheet Paste action is unavailable");
  int visited   = 0;
  bool canceled = false;
  QTimer dialogs;
  QObject::connect(&dialogs, &QTimer::timeout, &dialogs, [&] {
    auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
    if (!dialog) return;
    ++visited;
    if (external && cancelExternalDialog(dialog)) {
      canceled = true;
      return;
    }
    dialog->reject();
  });
  dialogs.start(10);
  action->trigger();
  dialogs.stop();
  require(external ? visited == 1 && canceled : visited == 0,
          "native raster opened a dialog or external image lost its chooser");
  if (floating) {
    require(rasterSelection->isFloating(),
            "Selection paste did not create a floating selection");
    rasterSelection->selectNone();
  }
}

TPaletteP setStrokeClipboard() {
  TPaletteP palette   = makePalette();
  TVectorImageP image = new TVectorImage;
  image->setPalette(palette.getPointer());
  auto* stroke =
      new TStroke(std::vector<TThickPoint>{{-3, -2, 1}, {0, 2, 1}, {3, -1, 1}});
  stroke->setStyle(1);
  image->addStroke(stroke);
  QApplication::clipboard()->setMimeData(new StrokesData(image.getPointer()));
  return palette;
}

int paintedPixels(const TImageP& image) {
  TToonzImageP indexed       = image;
  TRasterImageP fullColor    = image;
  TRasterCM32P indexedRaster = indexed ? indexed->getRaster() : TRasterCM32P();
  TRaster32P colorRaster     = fullColor ? fullColor->getRaster() : TRasterP();
  require(indexedRaster || colorRaster, "Converted paste has no raster");
  TRasterP raster =
      indexedRaster ? TRasterP(indexedRaster) : TRasterP(colorRaster);
  require(raster->getSize() == TDimension(canvasSize, canvasSize),
          "Converted paste changed the destination dimensions");
  int painted = 0;
  for (int y = 0; y < canvasSize; ++y)
    for (int x = 0; x < canvasSize; ++x)
      if (indexedRaster ? !(indexedRaster->pixels(y)[x] == TPixelCM32())
                        : colorRaster->pixels(y)[x].m != 0)
        ++painted;
  return painted;
}

QByteArray rasterBytes(const TImageP& image) {
  TToonzImageP indexed    = image;
  TRasterImageP fullColor = image;
  require(indexed || fullColor, "Converted raster redo lost its image");
  TRasterP raster =
      indexed ? TRasterP(indexed->getRaster()) : fullColor->getRaster();
  raster->lock();
  struct UnlockRaster {
    TRasterP raster;
    ~UnlockRaster() { raster->unlock(); }
  } unlock{raster};
  QByteArray bytes;
  for (int row = 0; row < raster->getLy(); ++row)
    bytes.append(reinterpret_cast<const char*>(raster->getRawData() +
                                               row * raster->getWrap() *
                                                   raster->getPixelSize()),
                 raster->getLx() * raster->getPixelSize());
  return bytes;
}

void undoConvertedRaster(const TXshCell& cell) {
  auto* undo        = TUndoManager::manager();
  const int painted = paintedPixels(cell.getImage(false));
  require(painted > 0, "Converted raster paste produced no pixels");
  const QByteArray expected = rasterBytes(cell.getImage(false));
  const int entries         = undo->getCurrentHistoryIndex();
  require(entries > 0, "Converted raster paste produced no undo history");
  while (undo->getCurrentHistoryIndex() > 0)
    require(undo->undo(), "Converted raster paste could not be undone");
  checkPixels(cell, cell.getSimpleLevel()->getType(), false);
  while (undo->getCurrentHistoryIndex() < entries)
    require(undo->redo(), "Converted raster paste could not be redone");
  require(rasterBytes(cell.getImage(false)) == expected,
          "Converted raster redo changed pixels or palette indices");
  while (undo->getCurrentHistoryIndex() > 0)
    require(undo->undo(), "Converted raster cleanup could not undo");
  checkPixels(cell, cell.getSimpleLevel()->getType(), false);
  undo->reset();
}

std::set<std::string> oldFloatingCacheKeys() {
  std::ostringstream inventory;
  TImageCache::instance()->dump(inventory);
  std::istringstream lines(inventory.str());
  std::set<std::string> result;
  std::string key;
  while (std::getline(lines, key))
    if (key.find("UndoPasteFloatingSelection_oldFloating_") == 0)
      result.insert(key);
  return result;
}

void checkStrokeConversionOwnership(int levelType, bool floating) {
  resetScene();
  auto* level = makeLevel(levelType, makePalette());
  const TXshCell cell(level, TFrameId(1));
  const TPaletteP sourcePalette = setStrokeClipboard();
  const int references          = sourcePalette->getRefCount();
  for (int cycle = 0; cycle < 4; ++cycle) {
    const auto before = oldFloatingCacheKeys();
    pasteCell(0, 0, false, floating);
    auto retained = oldFloatingCacheKeys();
    for (const auto& key : before) retained.erase(key);
    require(retained.size() == (floating ? 1u : 0u),
            "Floating paste did not expose its owned undo image");
    undoConvertedRaster(cell);
    for (const auto& key : retained)
      require(!TImageCache::instance()->isCached(key),
              "Cleared floating paste history retained its original image");
    require(oldFloatingCacheKeys() == before,
            "Floating paste history cleanup changed unrelated undo images");
    require(sourcePalette->getRefCount() == references,
            "Cleared vector paste retained a converted raster payload");
  }
}

void checkRasterToVectorCycles() {
  resetScene();
  auto* level = makeLevel(PLI_XSHLEVEL, makePalette());
  const TXshCell cell(level, TFrameId(1));
  auto* undo = TUndoManager::manager();
  setNativeClipboard(TZP_XSHLEVEL, level->getPalette());
  for (int cycle = 0; cycle < 4; ++cycle) {
    pasteCell(0, 0, false);
    TVectorImageP image = cell.getImage(false);
    require(image && image->getStrokeCount() > 0,
            "Indexed paste did not convert to vector strokes");
    const int strokes = image->getStrokeCount();
    require(undo->undo() && image->getStrokeCount() == 0,
            "Indexed-to-vector paste did not undo");
    require(undo->redo() && image->getStrokeCount() == strokes,
            "Indexed-to-vector paste did not redo");
    require(undo->undo(), "Indexed-to-vector cleanup did not undo");
    undo->reset();
  }
}

void checkExternalRasterCycles() {
  resetScene();
  auto* level = makeLevel(OVL_XSHLEVEL, makePalette());
  QImage image(canvasSize, canvasSize, QImage::Format_ARGB32);
  image.fill(Qt::red);
  QApplication::clipboard()->setImage(image);
  for (int cycle = 0; cycle < 4; ++cycle) {
    pasteCell(0, 0, false);
    undoConvertedRaster(TXshCell(level, TFrameId(1)));
  }
}

void checkNullTargetPaletteCopies(int levelType) {
  auto data = rasterPayload(levelType, makePalette());
  for (int cycle = 0; cycle < 8; ++cycle) {
    TRasterP raster;
    double dpiX = 0, dpiY = 0;
    std::vector<TRectD> rectangles;
    std::vector<TStroke> strokes, original;
    TAffine affine;
    data->getData(raster, dpiX, dpiY, rectangles, strokes, original, affine,
                  nullptr);
    require(raster && raster->getSize() == TDimension(canvasSize, canvasSize) &&
                dpiX == dpi && dpiY == dpi && rectangles.size() == 1,
            "Palette-free raster copy lost its pixels or geometry");
    if (levelType == OVL_XSHLEVEL) {
      TRaster32P pixels = raster;
      require(pixels && pixels->pixels(0)[0] == pixelAt(0, 0) &&
                  pixels->pixels(0)[1] == pixelAt(1, 0),
              "Palette-free fullcolor copy changed source pixels");
    } else {
      TRasterCM32P pixels = raster;
      require(pixels && pixels->pixels(0)[0].getTone() == 0 &&
                  pixels->pixels(0)[0].getInk() != 0,
              "Palette-free indexed copy changed source coverage");
    }
  }
}

class ThrowingCloneStyle final : public TSolidColorStyle {
public:
  ThrowingCloneStyle() : TSolidColorStyle(TPixel32::Blue) {}
  TColorStyle* clone() const override {
    throw std::runtime_error("Fixture palette clone failure");
  }
};

void checkNullPaletteFailure(int levelType) {
  TPaletteP source = makePalette();
  source->setStyle(1, new ThrowingCloneStyle);
  auto data            = rasterPayload(levelType, source);
  const int references = source->getRefCount();
  for (int cycle = 0; cycle < 4; ++cycle) {
    TRasterP raster;
    double dpiX = 0, dpiY = 0;
    std::vector<TRectD> rectangles;
    std::vector<TStroke> strokes, original;
    TAffine affine;
    bool rejected = false;
    try {
      data->getData(raster, dpiX, dpiY, rectangles, strokes, original, affine,
                    nullptr);
    } catch (const std::runtime_error& error) {
      rejected = std::string(error.what()) == "Fixture palette clone failure";
    }
    require(rejected && source->getRefCount() == references &&
                source->getStyle(1)->getMainColor() == TPixel32::Blue,
            "Failed palette merge destroyed or retained its source palette");
  }
}

void checkPasteCase(TXshSimpleLevel* level, int row, int col) {
  const int levelType = level->getType();
  auto* sheet         = TApp::instance()->getCurrentXsheet()->getXsheet();
  auto* undo          = TUndoManager::manager();
  const bool empty    = sheet->getCell(row, col).isEmpty();
  std::cout << "raster-paste-native: type=" << levelType << " row=" << row
            << " column=" << col << std::endl;
  undo->reset();
  setNativeClipboard(levelType, level->getPalette());
  pasteCell(row, col, false);
  const TXshCell pasted = sheet->getCell(row, col);
  checkPixels(pasted, levelType, true);
  require(col != 0 || pasted.getSimpleLevel() == level,
          "native raster paste replaced an existing target level");
  require(undo->getCurrentHistoryIndex() == 1 && undo->getHistoryCount() == 1,
          "native raster paste did not produce one undo step");
  undo->undo();
  if (empty) {
    require(sheet->getCell(row, col).isEmpty() &&
                !pasted.getSimpleLevel()->isFid(pasted.getFrameId()),
            "native raster paste undo retained a new frame or exposure");
  } else {
    checkPixels(sheet->getCell(row, col), levelType, false);
  }
  undo->redo();
  require(sheet->getCell(row, col) == pasted,
          "native raster paste redo changed the level or drawing identity");
  checkPixels(sheet->getCell(row, col), levelType, true);
  undo->undo();
  undo->reset();
}

void checkExternalCancel(TXshSimpleLevel* level) {
  auto* app               = TApp::instance();
  auto* scene             = app->getCurrentScene()->getScene();
  auto* sheet             = app->getCurrentXsheet()->getXsheet();
  auto* undo              = TUndoManager::manager();
  const int levels        = scene->getLevelSet()->getLevelCount();
  const int styles        = level->getPalette()->getStyleCount();
  const TXshCell original = sheet->getCell(0, 0);
  setNativeClipboard(level->getType(), level->getPalette());
  pasteCell(0, 0, false);
  checkPixels(original, level->getType(), true);
  require(undo->getHistoryCount() == 1,
          "external Cancel fixture did not establish native paste history");
  undo->undo();
  checkPixels(original, level->getType(), false);
  const int historyCount = undo->getHistoryCount();
  const int historyIndex = undo->getCurrentHistoryIndex();
  require(historyIndex == 0, "external Cancel fixture did not establish redo");
  QImage image(canvasSize, canvasSize, QImage::Format_ARGB32);
  image.fill(Qt::red);
  QApplication::clipboard()->setImage(image);
  pasteCell(0, 0, true);
  require(sheet->getCell(0, 0) == original && sheet->isColumnEmpty(1) &&
              scene->getLevelSet()->getLevelCount() == levels &&
              level->getPalette()->getStyleCount() == styles &&
              app->getCurrentColumn()->getColumnIndex() == 0,
          "external image Cancel mutated the scene, palette or target");
  require(undo->getHistoryCount() == historyCount,
          "external image Cancel discarded the native paste redo entry");
  require(undo->getCurrentHistoryIndex() == historyIndex,
          "external image Cancel moved the undo history cursor");
  checkPixels(original, level->getType(), false);
  undo->redo();
  require(sheet->getCell(0, 0) == original,
          "redo after external image Cancel changed drawing identity");
  checkPixels(original, level->getType(), true);
  undo->undo();
  checkPixels(original, level->getType(), false);
  undo->reset();
}
void checkNativePasteTargets() {
  resetScene();
  auto* preferences = Preferences::instance();
  preferences->setValue(DefLevelType, PLI_XSHLEVEL, false);
  // Empty-column paste currently uses the new-level preferences. Keep that
  // policy explicit while testing native payload dispatch independently.
  preferences->setValue(newLevelSizeToCameraSizeEnabled, false, false);
  preferences->setValue(DefLevelWidth, canvasSize / dpi, false);
  preferences->setValue(DefLevelHeight, canvasSize / dpi, false);
  preferences->setValue(DefLevelDpi, dpi, false);
  auto* indexed = makeLevel(TZP_XSHLEVEL, makePalette());
  for (const auto& cell : {TPoint(0, 0), TPoint(0, 1), TPoint(1, 0)})
    checkPasteCase(indexed, cell.y, cell.x);
  checkExternalCancel(indexed);
  resetScene();
  auto* fullColor = makeLevel(OVL_XSHLEVEL, makePalette());
  for (const auto& cell : {TPoint(0, 0), TPoint(0, 1), TPoint(1, 0)})
    checkPasteCase(fullColor, cell.y, cell.x);
  for (int type : {TZP_XSHLEVEL, OVL_XSHLEVEL}) {
    for (bool floating : {false, true})
      checkStrokeConversionOwnership(type, floating);
    checkNullTargetPaletteCopies(type);
    checkNullPaletteFailure(type);
  }
  checkRasterToVectorCycles();
  checkExternalRasterCycles();
}
}  // namespace

bool runRasterPasteIntegrationTest(QString* error) {
  if (error) error->clear();
  SavedPasteState saved;
  QString failure;
  try {
    checkNativePasteTargets();
  } catch (const std::exception& exception) {
    failure = QString::fromUtf8(exception.what());
  } catch (...) {
    failure = "Unknown exception during native raster paste regression";
  }
  QString cleanup;
  if (!resetNativeUiIntegrationScene(&cleanup))
    failure += " Fixture cleanup: " + cleanup;
  if (error) *error = failure;
  if (!failure.isEmpty()) return false;
  std::cout
      << "raster-paste-native: PASS native payload precedence, six targets, "
         "conversion ownership, undo/redo and external Cancel"
      << std::endl;
  return true;
}
