#include "tools/rasterselection.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "toonz/tapplication.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txsheet.h"
#include "toonz/toonzscene.h"
#include "toonz/levelset.h"
#include "toonz/levelproperties.h"
#include "toonz/palettecontroller.h"
#include "toonz/tpalettehandle.h"
#include "toonz/preferences.h"
#include "toonz/studiopalette.h"
#include "toonz/toonzfolders.h"
#include "toonzqt/tselectionhandle.h"
#include "tenv.h"
#include "timagecache.h"
#include "tundo.h"
#include "tthread.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class TestTool final : public TTool {
public:
  TestTool() : TTool("T_RasterPasteRegression") { bind(RasterImage); }
  ToolType getToolType() const override { return GenericTool; }
};

class TestApplication final : public TApplication {
public:
  TestApplication() {
    scene.setScene(new ToonzScene);
    sheet.setXsheet(scene.getScene()->getXsheet());
    frame.setFrame(0);
    column.setColumnIndex(0);
    object.setObjectId(TStageObjectId::ColumnId(0));
    TTool::setApplication(this);
    tool.onImageChanged(TImage::RASTER);
    tool.setTool("T_RasterPasteRegression");
    require(tool.getTool() == &nativeTool,
            "Test tool must use native tool machinery");
  }
  ~TestApplication() override {
    TUndoManager::manager()->reset();
    level.setLevel(nullptr);
    sheet.setXsheet(nullptr);
    scene.setScene(nullptr);
    TTool::setApplication(nullptr);
  }
  TFrameHandle* getCurrentFrame() const override { return &frame; }
  TXshLevelHandle* getCurrentLevel() const override { return &level; }
  TXsheetHandle* getCurrentXsheet() const override { return &sheet; }
  TObjectHandle* getCurrentObject() const override { return &object; }
  TColumnHandle* getCurrentColumn() const override { return &column; }
  TSceneHandle* getCurrentScene() const override { return &scene; }
  ToolHandle* getCurrentTool() const override { return &tool; }
  TSelectionHandle* getCurrentSelection() const override { return &selection; }
  TOnionSkinMaskHandle* getCurrentOnionSkin() const override { return nullptr; }
  TPaletteHandle* getCurrentPalette() const override {
    return palette.getCurrentPalette();
  }
  TFxHandle* getCurrentFx() const override { return nullptr; }
  PaletteController* getPaletteController() const override { return &palette; }
  TColorStyle* getCurrentLevelStyle() const override { return nullptr; }
  int getCurrentLevelStyleIndex() const override { return 0; }
  void setCurrentLevelStyleIndex(int, bool) override {}

  mutable TFrameHandle frame;
  mutable TXshLevelHandle level;
  mutable TXsheetHandle sheet;
  mutable TObjectHandle object;
  mutable TColumnHandle column;
  mutable TSceneHandle scene;
  mutable ToolHandle tool;
  mutable TSelectionHandle selection;
  mutable PaletteController palette;
  TestTool nativeTool;
};

void initialize(const QString& root, const char* executable) {
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     root + "/settings");
  QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                     root + "/settings");
  TEnv::setApplicationFileName(executable);
  TEnv::setRootVarName("TOONZROOT");
  TEnv::setSystemVarPrefix("TOONZ");
  TEnv::setArgPathValue("TOONZROOT", root.toStdString());
  for (const auto& entry : TEnv::getSystemPathMap()) {
    const QString path =
        QDir(root).filePath(QString::fromStdString(entry.second));
    require(QDir().mkpath(path), "Create private native profile");
    TEnv::setArgPathValue("TOONZ" + entry.first, path.toStdString());
  }
  require(QDir().mkpath(root + "/cache"), "Create private cache");
  ToonzFolder::setCacheRootFolder(TFilePath((root + "/cache").toStdWString()));
  TImageCache::instance()->setRootDir(
      TFilePath((root + "/cache").toStdWString()));
  StudioPalette::enable(false);
  TThread::init();
}

TXshSimpleLevelP makeLevel(TestApplication& app, int type) {
  static int number      = 0;
  TXshSimpleLevelP level = new TXshSimpleLevel;
  level->setType(type);
  level->setName(L"Paste" + std::to_wstring(++number));
  level->setScene(app.scene.getScene());
  level->setPath(
      TFilePath("+drawings/paste" + std::to_string(number) + ".png"));
  app.scene.getScene()->getLevelSet()->insertLevel(level.getPointer());
  TRaster32P raster(8, 8);
  raster->clear();
  TRasterImageP image = new TRasterImage(raster);
  image->setDpi(120, 120);
  level->setFrame(TFrameId(1), image);
  app.sheet.getXsheet()->setCell(0, 0,
                                 TXshCell(level.getPointer(), TFrameId(1)));
  return level;
}

void checkFloating(const RasterSelection& selection) {
  TRaster32P raster = selection.getFloatingSelection();
  require(raster && raster->getSize() == TDimension(2, 2),
          "External clipboard image must create a native floating selection");
  require(
      raster->pixels(0)[0] == TPixel32(0, 0, 255, 255) &&
          raster->pixels(0)[1] == TPixel32(255, 255, 0, 255) &&
          raster->pixels(1)[0] == TPixel32(255, 0, 0, 255) &&
          raster->pixels(1)[1] == TPixel32(0, 255, 0, 255),
      "Clipboard pixels must preserve colors and top-to-bottom orientation");
}

QImage clipboardPixels() {
  QImage image(2, 2, QImage::Format_ARGB32);
  image.setPixel(0, 0, qRgb(255, 0, 0));
  image.setPixel(1, 0, qRgb(0, 255, 0));
  image.setPixel(0, 1, qRgb(0, 0, 255));
  image.setPixel(1, 1, qRgb(255, 255, 0));
  return image;
}

void unchangedCanvas(TXshSimpleLevel* level) {
  TRasterImageP image = level->getFrame(TFrameId(1), false);
  TRaster32P raster   = image->getRaster();
  for (int y = 0; y < raster->getLy(); ++y)
    for (int x = 0; x < raster->getLx(); ++x)
      require(
          raster->pixels(y)[x] == TPixel32::Transparent,
          "Floating or rejected paste must leave destination pixels unchanged");
}

void pasteCase(TestApplication& app, int levelType, bool bindCurrentLevel) {
  const auto level = makeLevel(app, levelType);
  app.level.setLevel(bindCurrentLevel ? level.getPointer() : nullptr);
  TUndoManager::manager()->reset();
  RasterSelection selection;
  selection.setCurrentImage(level->getFrame(TFrameId(1), false),
                            TXshCell(level.getPointer(), TFrameId(1)));
  QApplication::clipboard()->setImage(clipboardPixels());
  selection.pasteSelection();
  checkFloating(selection);
  require(TUndoManager::manager()->getHistoryCount() == 1,
          "Paste must create one native undo entry");
  TUndoManager::manager()->undo();
  require(!selection.isFloating(),
          "Native Undo must remove the pasted floating selection");
  TUndoManager::manager()->redo();
  checkFloating(selection);
  unchangedCanvas(level.getPointer());
  TUndoManager::manager()->reset();
  selection.setFloatingSeletion(TRasterP());
  std::cout << "rasterpaste-test: "
            << (bindCurrentLevel
                    ? (levelType == OVL_XSHLEVEL ? "bound-level" : "scan-level")
                    : "image-only")
            << " clipboard pixels and native Undo/Redo PASS" << std::endl;
}
void emptyInputs(TestApplication& app) {
  const auto level = makeLevel(app, OVL_XSHLEVEL);
  app.level.setLevel(level.getPointer());
  RasterSelection selection;
  selection.setCurrentImage(level->getFrame(TFrameId(1), false),
                            TXshCell(level.getPointer(), TFrameId(1)));
  QApplication::clipboard()->clear();
  selection.pasteSelection();
  require(!selection.isFloating() &&
              TUndoManager::manager()->getHistoryCount() == 0,
          "Empty clipboard must not paste or create Undo history");
  Preferences::instance()->setValue(EnableAutocreation, false, false);
  app.level.setLevel(nullptr);
  app.sheet.getXsheet()->setCell(0, 0, TXshCell());
  selection.setCurrentImage(level->getFrame(TFrameId(1), false), TXshCell());
  QApplication::clipboard()->setImage(clipboardPixels());
  selection.pasteSelection();
  require(!selection.isFloating() &&
              TUndoManager::manager()->getHistoryCount() == 0,
          "Ownerless image must not paste or create Undo history");
  require(app.sheet.getXsheet()->getCell(0, 0).isEmpty(),
          "Rejected ownerless paste must not insert a native level exposure");
  unchangedCanvas(level.getPointer());
  std::cout
      << "rasterpaste-test: empty clipboard and ownerless image rejection PASS"
      << std::endl;
}

void readOnlyOwner(TestApplication& app, bool entireLevel) {
  const auto level = makeLevel(app, OVL_XSHLEVEL);
  app.level.setLevel(level.getPointer());
  TUndoManager::manager()->reset();
  RasterSelection selection;
  selection.setCurrentImage(level->getFrame(TFrameId(1), false),
                            TXshCell(level.getPointer(), TFrameId(1)));
  QApplication::clipboard()->setImage(clipboardPixels());
  selection.pasteSelection();
  checkFloating(selection);
  TUndoManager::manager()->undo();
  selection.select(TRectD(-1, -1, 1, 1));
  const TRectD bounds = selection.getSelectionBound();
  require(!bounds.isEmpty(),
          "Refused paste must preserve an existing selection");
  const int history = TUndoManager::manager()->getCurrentHistoryIndex();
  const bool dirty  = level->getDirtyFlag();
  level->setIsReadOnly(entireLevel);
  level->getProperties()->setIsStopMotion(!entireLevel);
  require(
      entireLevel ? level->isReadOnly() : level->isFrameReadOnly(TFrameId(1)),
      "Fixture must use native level or frame read-only admission");
  app.level.setLevel(nullptr);
  selection.pasteSelection();
  require(!selection.isFloating() && selection.getSelectionBound() == bounds,
          "Read-only fallback owner must preserve the selection");
  require(TUndoManager::manager()->getHistoryCount() == 1 &&
              TUndoManager::manager()->getCurrentHistoryIndex() == history,
          "Refused paste must preserve native history and its redo branch");
  require(level->getDirtyFlag() == dirty &&
              app.sheet.getXsheet()->getCell(0, 0) ==
                  TXshCell(level.getPointer(), TFrameId(1)),
          "Refused paste must preserve dirty state and the native exposure");
  unchangedCanvas(level.getPointer());
  level->setIsReadOnly(false);
  level->getProperties()->setIsStopMotion(false);
  TUndoManager::manager()->redo();
  checkFloating(selection);
  TUndoManager::manager()->reset();
  selection.setFloatingSeletion(TRasterP());
  std::cout << "rasterpaste-test: "
            << (entireLevel ? "read-only level" : "read-only frame")
            << " fallback preserves selection and native redo PASS"
            << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir root;
    require(root.isValid(), "Create private native test profile");
    qputenv("TOONZROOT", root.path().toUtf8());
    require(QDir().mkpath(root.path() + "/profiles"),
            "Create private profile path");
    qputenv("TOONZPROFILES", (root.path() + "/profiles").toUtf8());
    QApplication application(argc, argv);
    initialize(root.path(), argv[0]);
    TestApplication native;
    pasteCase(native, OVL_XSHLEVEL, true);
    pasteCase(native, TZI_XSHLEVEL, true);
    pasteCase(native, OVL_XSHLEVEL, false);
    readOnlyOwner(native, true);
    readOnlyOwner(native, false);
    emptyInputs(native);
    QApplication::clipboard()->clear();
    TImageCache::instance()->clear(true);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "rasterpaste-test: " << error.what() << '\n';
    return 1;
  }
}
