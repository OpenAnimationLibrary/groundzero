#include "rasterbrushintegrationtest.h"
#include "../toonzlib/tests/rasterbrushfixture.h"
#include "tapp.h"
#include "sceneviewer.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tpalettehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/preferences.h"
#include "toonz/rasterbrushimport.h"
#include "toonz/toonzfolders.h"
#include "tpalette.h"
#include "tproperty.h"
#include "trasterimage.h"
#include "tundo.h"
#include "timage_io.h"
#include <QMainWindow>
#include <QCoreApplication>
#include <QEventLoop>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void settle() {
  QCoreApplication::sendPostedEvents();
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
}
bool same(const TRaster32P &a, const TRaster32P &b) {
  if (!a || !b || a->getSize() != b->getSize()) return false;
  for (int y = 0; y < a->getLy(); ++y)
    for (int x = 0; x < a->getLx(); ++x)
      if (a->pixels(y)[x] != b->pixels(y)[x]) return false;
  return true;
}
TRaster32P raster(TXshSimpleLevel *level, int frame) {
  TRasterImageP image(level->getFrame(TFrameId(frame), true));
  return image ? TRaster32P(image->getRaster()) : TRaster32P();
}
void checkpoint(const char *stage) {
  std::cerr << "raster-brush-integration: " << stage << std::endl;
}
void tap(TTool *tool) {
  require(tool->preLeftButtonDown(), "brush refused tap");
  TMouseEvent event;
  event.m_button   = Qt::LeftButton;
  event.m_buttons  = Qt::LeftButton;
  event.m_isTablet = true;
  event.m_pressure = 1;
  tool->leftButtonDown(TPointD(0, 0), event);
  event.m_buttons = Qt::NoButton;
  tool->leftButtonUp(TPointD(0, 0), event);
  settle();
}
void draw(TTool *tool, bool cancel, double pressure = .8, double tiltX = .2,
          double rotation = .4) {
  require(tool->preLeftButtonDown(), "brush refused prepared raster");
  TMouseEvent event;
  event.m_button   = Qt::LeftButton;
  event.m_buttons  = Qt::LeftButton;
  event.m_isTablet = true;
  event.m_pressure = pressure;
  event.m_tilt     = TPointD(tiltX, -.3);
  event.m_hasTilt = event.m_hasRotation = true;
  event.m_rotation                      = rotation;
  tool->leftButtonDown(TPointD(-30, 0), event);
  for (int i = 1; i <= 20; ++i)
    tool->leftButtonDrag(TPointD(-30 + 3 * i, 0), event);
  if (cancel) {
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    require(tool->keyDown(&escape), "native stroke did not handle Escape");
  }
  event.m_buttons = Qt::NoButton;
  tool->leftButtonUp(TPointD(30, 0), event);
  settle();
}
}  // namespace

int runRasterBrushIntegrationTest(QMainWindow *window) {
  try {
    checkpoint("importing through the packaged application worker");
    RasterBrush::Record original;
    QString importError;
    QString package = QDir::current().filePath("original.gzbrush");
    require(RasterBrush::save(package, original, importError),
            "cannot write original native brush fixture");
    auto imported = RasterBrush::importBrushProcess(
        QCoreApplication::applicationFilePath(), package, importError);
    require(importError.isEmpty() && imported.complete &&
                imported.records.size() == 1,
            "packaged application brush import worker failed");
    checkpoint("creating disposable full-color level");
    auto app   = TApp::instance();
    auto scene = app->getCurrentScene()->getScene();
    require(scene && window, "native application scene is missing");
    Preferences::instance()->setValue(EnableAutocreation, true);
    auto level = scene->createNewLevel(OVL_XSHLEVEL, L"RasterBrushFixture")
                     ->getSimpleLevel();
    require(level, "cannot create raster level");
    level->setPath(TFilePath(QDir::current().filePath("brush..png")));
    level->getProperties()->setImageRes(TDimension(128, 128));
    level->getProperties()->setDpi(TPointD(72, 72));
    level->getProperties()->setImageDpi(TPointD(72, 72));
    auto palette = new TPalette;
    level->setPalette(palette);
    TRaster32P initial(128, 128);
    initial->fill(TPixel32(255, 255, 255, 255));
    TRasterImageP image(new TRasterImage(initial));
    image->setDpi(72, 72);
    level->setFrame(TFrameId(1), image);
    app->getCurrentLevel()->setLevel(level);
    app->getCurrentFrame()->setFid(TFrameId(1));
    app->getCurrentPalette()->setPalette(palette, 1);
    app->getCurrentTool()->setTool("T_Brush");
    settle();
    TTool *tool = TTool::getTool("T_Brush", TTool::RasterImage);
    require(tool, "full-color brush tool unavailable");
    SceneViewer *viewer = nullptr;
    for (auto candidate : window->findChildren<SceneViewer *>())
      if (candidate->isVisible()) {
        viewer = candidate;
        break;
      }
    require(viewer, "native scene viewer unavailable");
    tool->setViewer(viewer);
    tool->onActivate();
    auto property = dynamic_cast<TEnumProperty *>(
        tool->getProperties(0)->getProperty("Native Brush:"));
    require(property && property->isValue(L"<round>"),
            "native brush selector missing");
    property->setValue(L"<round>");
    require(tool->onPropertyChanged("Native Brush:"),
            "native brush selection failed");
    auto undo = TUndoManager::manager();
    undo->reset();
    checkpoint("qualifying source initialization without paint or undo");
    {
      RasterBrush::Record fixture;
      fixture.preset.name         = "Original source initializer";
      fixture.preset.profile      = "krita-5.3.4.1-legacy-dulling-candidate-1";
      fixture.preset.wet          = RasterBrush::Wet::Dulling;
      fixture.preset.smudgeRadius = 0;
      const QString folder =
          ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
      QString path = RasterBrush::install(folder, fixture, importError);
      require(!path.isEmpty() && importError.isEmpty(),
              "cannot install source initializer");
      tool->onActivate();
      property->setValue(QDir(folder).relativeFilePath(path).toStdWString());
      require(tool->onPropertyChanged("Native Brush:"),
              "cannot select source initializer");
      TRaster32P baseline = raster(level, 1)->clone();
      tap(tool);
      require(same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
              "source initialization painted or created an undo entry");
      app->getCurrentFrame()->setFid(TFrameId(4));
      settle();
      tap(tool);
      require(
          !level->getFrame(TFrameId(4), false) && undo->getHistoryCount() == 0,
          "unpainted source initialization retained a created frame");
      app->getCurrentFrame()->setFid(TFrameId(1));
      settle();
    }
    checkpoint("qualifying source wet stroke transactions and persistence");
    for (const QString &model :
         {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"}) {
      const QString sourcePath =
          QDir::current().filePath("original-source-" + model + ".kpp");
      QFile sourceFile(sourcePath);
      const QByteArray sourceBytes = RasterBrushTest::hardPixelKpp({}, model);
      require(sourceFile.open(QIODevice::WriteOnly) &&
                  sourceFile.write(sourceBytes) == sourceBytes.size(),
              "cannot write original KPP transaction fixture");
      sourceFile.close();
      const auto sourceImport = RasterBrush::importBrushProcess(
          QCoreApplication::applicationFilePath(), sourcePath, importError);
      require(importError.isEmpty() && sourceImport.complete &&
                  sourceImport.records.size() == 1,
              "packaged application KPP import failed");
      auto fixture = sourceImport.records.front();
      require(fixture.preset.profile ==
                      "krita-5.3.4.1-" + model.toStdString() + "-candidate-1" &&
                  !fixture.provenance["sourceReferenceVerified"].toBool(),
              "KPP worker lost strict candidate identity or qualification");
      fixture.preset.name =
          "Original source transaction " + model.toStdString();
      const QString folder =
          ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
      const QString path = RasterBrush::install(folder, fixture, importError);
      require(!path.isEmpty() && importError.isEmpty(),
              "cannot install source transaction fixture");
      tool->onActivate();
      property->setValue(QDir(folder).relativeFilePath(path).toStdWString());
      require(tool->onPropertyChanged("Native Brush:"),
              "cannot select source transaction fixture");
      const TRaster32P baseline = raster(level, 1)->clone();
      draw(tool, false);
      const TRaster32P painted = raster(level, 1)->clone();
      require(!same(baseline, painted) && undo->getHistoryCount() == 1,
              "source wet stroke did not create one painted transaction");
      require(undo->undo() && same(baseline, raster(level, 1)),
              "source wet undo did not restore original pixels");
      require(undo->redo() && same(painted, raster(level, 1)),
              "source wet redo did not restore exact pixels");
      const TFilePath output(
          QDir::current().filePath("source-" + model + ".png"));
      TImageWriter::save(output, TRasterP(painted));
      TRasterP reopened;
      require(TImageReader::load(output, reopened) &&
                  same(painted, TRaster32P(reopened)),
              "source wet save/reopen changed committed pixels");
      require(undo->undo() && same(baseline, raster(level, 1)),
              "source wet second undo failed");
      undo->reset();
      draw(tool, true);
      require(same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
              "cancelled source wet stroke left pixels or undo history");
      draw(tool, false);
      require(same(painted, raster(level, 1)),
              "source wet state leaked across completed or cancelled strokes");
      require(undo->undo() && same(baseline, raster(level, 1)),
              "repeated source wet undo failed");
      undo->reset();
    }
    checkpoint("qualifying pressure, tilt and rotation through tool input");
    for (auto sensor :
         {RasterBrush::Sensor::Pressure, RasterBrush::Sensor::TiltX,
          RasterBrush::Sensor::Rotation}) {
      RasterBrush::Record fixture;
      fixture.preset.name          = "Original input fixture";
      fixture.preset.opacity.value = .7;
      fixture.preset.opacity.mappings.push_back({sensor, {{0, 0}, {1, 1}}, 1});
      const QString folder =
          ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
      QString path = RasterBrush::install(folder, fixture, importError);
      require(!path.isEmpty() && importError.isEmpty(),
              "cannot install original sensor fixture");
      tool->onActivate();
      property->setValue(QDir(folder).relativeFilePath(path).toStdWString());
      require(tool->onPropertyChanged("Native Brush:"),
              "cannot select installed sensor fixture");
      TRaster32P baseline = raster(level, 1)->clone();
      draw(tool, false, .2, -.8, 1.5707963267948966);
      TRaster32P low = raster(level, 1)->clone();
      require(undo->undo() && same(baseline, raster(level, 1)),
              "sensor stroke undo did not restore baseline");
      draw(tool, false, .8, .8, 3.141592653589793);
      TRaster32P high = raster(level, 1)->clone();
      require(!same(low, high), "tablet sensor did not affect actual pixels");
      require(undo->undo() && same(baseline, raster(level, 1)),
              "second sensor stroke undo did not restore baseline");
      undo->reset();
    }
    property->setValue(L"<round>");
    require(tool->onPropertyChanged("Native Brush:"),
            "cannot restore native round brush");
    TRaster32P before = raster(level, 1)->clone();
    checkpoint("drawing through actual brush tool and undo manager");
    draw(tool, false);
    TRaster32P after = raster(level, 1)->clone();
    require(!same(before, after), "native tool did not paint");
    require(undo->getHistoryCount() == 1,
            "stroke was not one undo transaction");
    require(undo->undo(), "native undo unavailable");
    require(same(before, raster(level, 1)), "actual undo changed prior pixels");
    require(undo->redo(), "native redo unavailable");
    require(same(after, raster(level, 1)),
            "actual redo changed painted pixels");
    checkpoint("cancelling existing-frame stroke");
    int history = undo->getHistoryCount();
    draw(tool, true);
    require(same(after, raster(level, 1)), "cancel failed to restore raster");
    require(undo->getHistoryCount() == history, "cancel created an undo entry");
    checkpoint("creating and undoing a new frame");
    app->getCurrentFrame()->setFid(TFrameId(2));
    settle();
    draw(tool, false);
    require(bool(raster(level, 2)), "new frame was not created");
    TRaster32P second = raster(level, 2)->clone();
    require(undo->undo(), "new-frame undo unavailable");
    require(!level->getFrame(TFrameId(2), false),
            "undo did not remove created frame");
    require(undo->redo(), "new-frame redo unavailable");
    require(same(second, raster(level, 2)),
            "redo did not restore created frame");
    checkpoint("cancelling newly created frame");
    app->getCurrentFrame()->setFid(TFrameId(3));
    settle();
    draw(tool, true);
    require(!level->getFrame(TFrameId(3), false),
            "cancel left a newly created frame");
    checkpoint("saving and reopening native-painted image");
    TFilePath output(QDir::current().filePath("native-painted.png"));
    TImageWriter::save(output, TRasterP(second));
    TRasterP reopened;
    require(TImageReader::load(output, reopened),
            "cannot reopen saved native-painted image");
    require(same(second, TRaster32P(reopened)),
            "saved native-painted pixels changed");
    checkpoint("saving and reopening scene and raster level");
    require(scene->getXsheet()->setCell(0, 0, TXshCell(level, TFrameId(1))) &&
                scene->getXsheet()->setCell(1, 0, TXshCell(level, TFrameId(2))),
            "cannot expose painted level for scene persistence");
    level->save();
    TFilePath scenePath(QDir::current().filePath("native-painted.tnz"));
    scene->saveCopy(scenePath);
    {
      ToonzScene restored;
      restored.loadTnzFile(scenePath);
      auto restoredLevel =
          restored.getLevelSet()->getLevel(L"RasterBrushFixture");
      require(restoredLevel && restoredLevel->getSimpleLevel(),
              "scene lost painted raster level");
      auto restoredRaster = restoredLevel->getSimpleLevel();
      restoredRaster->load();
      require(same(after, raster(restoredRaster, 1)) &&
                  same(second, raster(restoredRaster, 2)),
              "scene/level reopening changed native-painted pixels");
      require(restored.getXsheet()->getCell(1, 0).m_frameId == TFrameId(2),
              "scene reopening lost painted frame exposure");
      require(!restoredRaster->getFrame(TFrameId(3), false),
              "scene reopening resurrected cancelled frame");
    }
    checkpoint("creating, undoing and redoing a new raster level");
    Preferences::instance()->setValue(DefLevelType, OVL_XSHLEVEL);
    app->getCurrentColumn()->setColumnIndex(1);
    app->getCurrentFrame()->setFrame(0);
    app->getCurrentLevel()->setLevel(nullptr);
    app->getCurrentXsheet()->notifyXsheetChanged();
    settle();
    const int levelCount = scene->getLevelSet()->getLevelCount();
    draw(tool, false);
    auto createdCell = scene->getXsheet()->getCell(0, 1);
    require(createdCell.getSimpleLevel(), "stroke did not create raster level");
    TRasterImageP createdImage(createdCell.getImage(true));
    require(bool(createdImage), "new level has no painted raster");
    TRaster32P createdPixels = createdImage->getRaster()->clone();
    require(scene->getLevelSet()->getLevelCount() == levelCount + 1,
            "new level was not added to the scene cast");
    require(undo->undo(), "new-level undo unavailable");
    require(scene->getXsheet()->getCell(0, 1).isEmpty() &&
                scene->getLevelSet()->getLevelCount() == levelCount,
            "new-level undo left scene cells or cast entries");
    require(undo->redo(), "new-level redo unavailable");
    auto restoredCell = scene->getXsheet()->getCell(0, 1);
    TRasterImageP restoredImage(restoredCell.getImage(true));
    require(restoredImage &&
                same(createdPixels, TRaster32P(restoredImage->getRaster())) &&
                scene->getLevelSet()->getLevelCount() == levelCount + 1,
            "new-level redo lost painted pixels or scene cast entry");
    checkpoint("cancelling a newly created raster level");
    app->getCurrentColumn()->setColumnIndex(2);
    app->getCurrentFrame()->setFrame(0);
    app->getCurrentLevel()->setLevel(nullptr);
    app->getCurrentXsheet()->notifyXsheetChanged();
    settle();
    history = undo->getHistoryCount();
    draw(tool, true);
    require(scene->getXsheet()->getCell(0, 2).isEmpty() &&
                scene->getLevelSet()->getLevelCount() == levelCount + 1 &&
                undo->getHistoryCount() == history,
            "cancelled new level left scene content or undo history");
    tool->onDeactivate();
    undo->reset();
    app->getCurrentScene()->setDirtyFlag(false);
    std::cout << "raster-brush-integration: PASS" << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "raster-brush-integration: FAIL: " << error.what()
              << std::endl;
  } catch (...) {
    std::cerr << "raster-brush-integration: FAIL: unexpected native exception"
              << std::endl;
  }
  return 1;
}
