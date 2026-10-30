#include "rasterbrushintegrationtest.h"
#include "tapp.h"
#include "sceneviewer.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tpalettehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/preferences.h"
#include "tpalette.h"
#include "tproperty.h"
#include "trasterimage.h"
#include "tundo.h"
#include "timage_io.h"
#include <QMainWindow>
#include <QCoreApplication>
#include <QEventLoop>
#include <QDir>
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
void draw(TTool *tool, bool cancel) {
  require(tool->preLeftButtonDown(), "brush refused prepared raster");
  TMouseEvent event;
  event.m_button   = Qt::LeftButton;
  event.m_buttons  = Qt::LeftButton;
  event.m_isTablet = true;
  event.m_pressure = .8;
  event.m_tilt     = TPointD(.2, -.3);
  event.m_hasTilt = event.m_hasRotation = true;
  event.m_rotation                      = .4;
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
