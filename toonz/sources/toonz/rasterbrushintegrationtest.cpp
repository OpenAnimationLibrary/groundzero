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
#include <algorithm>
#include <cstdlib>
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
void requireSame(const TRaster32P &expected, const TRaster32P &actual,
                 const QString &context) {
  if (same(expected, actual)) return;
  std::cerr << "raster-brush-integration: pixel mismatch "
            << context.toStdString() << " expected "
            << (expected ? expected->getLx() : -1) << 'x'
            << (expected ? expected->getLy() : -1) << " actual "
            << (actual ? actual->getLx() : -1) << 'x'
            << (actual ? actual->getLy() : -1) << std::endl;
  if (expected && actual && expected->getSize() == actual->getSize()) {
    int differences = 0, maxChannel = 0;
    for (int y = 0; y < expected->getLy(); ++y)
      for (int x = 0; x < expected->getLx(); ++x) {
        const auto a = expected->pixels(y)[x], b = actual->pixels(y)[x];
        if (a == b) continue;
        maxChannel = std::max(
            {maxChannel, std::abs(int(a.r) - b.r), std::abs(int(a.g) - b.g),
             std::abs(int(a.b) - b.b), std::abs(int(a.m) - b.m)});
        if (++differences <= 8)
          std::cerr << "pixel " << x << ',' << y << " RGBA " << int(a.r) << ','
                    << int(a.g) << ',' << int(a.b) << ',' << int(a.m) << " -> "
                    << int(b.r) << ',' << int(b.g) << ',' << int(b.b) << ','
                    << int(b.m) << std::endl;
      }
    std::cerr << "different pixels " << differences << " max channel "
              << maxChannel << std::endl;
  }
  throw std::runtime_error(context.toStdString());
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
    checkpoint("qualifying exact premultiplied PNG persistence");
    {
      // Exercise every valid 8-bit alpha/channel pair through real file I/O.
      // Two cycles also expose cumulative rounding drift.
      TRaster32P values(256, 256);
      for (int alpha = 0; alpha < 256; ++alpha)
        for (int channel = 0; channel < 256; ++channel) {
          const int c                    = std::min(channel, alpha);
          values->pixels(alpha)[channel] = TPixel32(c, alpha - c, c / 2, alpha);
        }
      const TRaster32P originalValues = values->clone();
      const TFilePath path(
          QDir::current().filePath("premultiplied-roundtrip.png"));
      for (int cycle = 0; cycle < 2; ++cycle) {
        TImageWriter::save(path, TRasterP(values));
        TRasterP decoded;
        require(TImageReader::load(path, decoded),
                "cannot read PNG channel fixture");
        values = TRaster32P(decoded);
        requireSame(originalValues, values,
                    "PNG premultiplied channel round trip");
      }
    }
    checkpoint("qualifying source wet stroke transactions and persistence");
    for (const QString &model :
         {"legacy-smear", "legacy-dulling", "new-smear", "new-dulling"})
      for (const QString &variant :
           {QString(), QString("wetPressure"), QString("radiusPressure")}) {
        const QString label =
            model + "-" + (variant.isEmpty() ? QString("fixed") : variant);
        const QString sourcePath =
            QDir::current().filePath("original-source-" + label + ".kpp");
        QFile sourceFile(sourcePath);
        const QByteArray sourceBytes =
            RasterBrushTest::hardPixelKpp(variant, model);
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
        require(
            fixture.preset.profile ==
                    "krita-5.3.4.1-" + model.toStdString() + "-candidate-1" &&
                !fixture.provenance["sourceReferenceVerified"].toBool(),
            "KPP worker lost strict candidate identity or qualification");
        if (variant == "radiusPressure")
          require(fixture.preset.smudgeRadius ==
                          (model.endsWith("dulling") ? 1 : 0) &&
                      fixture.preset.pickupControl.mappings.size() == 1 &&
                      fixture.preset.colorRateControl.mappings.size() == 1,
                  "combined source radius and pressure mappings were lost");
        fixture.preset.name =
            "Original source transaction " + label.toStdString();
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
            QDir::current().filePath("source-" + label + ".png"));
        TImageWriter::save(output, TRasterP(painted));
        TRasterP reopened;
        require(TImageReader::load(output, reopened),
                "source wet PNG could not reopen");
        requireSame(
            painted, TRaster32P(reopened),
            "source wet save/reopen changed committed pixels: " + model);
        require(undo->undo() && same(baseline, raster(level, 1)),
                "source wet second undo failed");
        undo->reset();
        draw(tool, true);
        require(
            same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
            "cancelled source wet stroke left pixels or undo history");
        draw(tool, false);
        require(
            same(painted, raster(level, 1)),
            "source wet state leaked across completed or cancelled strokes");
        require(undo->undo() && same(baseline, raster(level, 1)),
                "repeated source wet undo failed");
        undo->reset();
      }
    checkpoint("qualifying combined portable resources and native dynamics");
    for (bool wet : {false, true}) {
      RasterBrush::Record fixture;
      auto mask   = std::make_shared<RasterBrush::Image>();
      mask->width = mask->height = 4;
      for (int i = 0; i < 16; ++i)
        mask->pixels.push_back({0, 0, 0, (i + i / 4) % 2 ? .25 : 1});
      auto &p = fixture.preset;
      p.name =
          wet ? "Original combined native wet" : "Original combined native dry";
      p.tip.shape = RasterBrush::Shape::Bitmap;
      p.tip.images.push_back(mask);
      p.texture.image = mask;
      p.texture.scale = 2;
      p.texture.depth = .7;
      p.size.value    = 24;
      p.size.mappings.push_back(
          {RasterBrush::Sensor::Pressure, {{0, .2}, {1, 1}}, 1});
      p.aspect.value = .6;
      p.angle.value  = 90;
      p.angle.mappings.push_back(
          {RasterBrush::Sensor::Rotation, {{0, 0}, {1, 1}}, 1});
      p.opacity.value = .8;
      p.opacity.mappings.push_back(
          {RasterBrush::Sensor::TiltX, {{0, .3}, {1, 1}}, 1});
      p.flow.value          = .3;
      p.backgroundMix.value = .2;
      p.wash                = !wet;
      if (wet) {
        p.wet = RasterBrush::Wet::Dulling;
        p.pickupControl.mappings.push_back(
            {RasterBrush::Sensor::Pressure, {{0, .2}, {1, 1}}, 1});
      } else {
        p.secondary.enabled       = true;
        p.secondary.tip.shape     = RasterBrush::Shape::Square;
        p.secondary.size.value    = .75;
        p.secondary.opacity.value = .7;
        p.blend                   = RasterBrush::Blend::Multiply;
      }
      // Export after editing, reload with all resources embedded, then install
      // through the same portable record path used by the brush selector.
      const QString exported = QDir::current().filePath(
          wet ? "combined-wet.gzbrush" : "combined-dry.gzbrush");
      require(RasterBrush::save(exported, fixture, importError),
              "combined preset export failed");
      fixture = RasterBrush::load(exported, importError);
      require(importError.isEmpty() && fixture.preset.tip.images.size() == 1 &&
                  fixture.preset.texture.image &&
                  fixture.preset.secondary.enabled == !wet,
              "combined preset lost embedded resources");
      const QString folder =
          ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
      const QString installed =
          RasterBrush::install(folder, fixture, importError);
      require(!installed.isEmpty() && importError.isEmpty(),
              "combined preset install failed");
      tool->onActivate();
      property->setValue(
          QDir(folder).relativeFilePath(installed).toStdWString());
      require(tool->onPropertyChanged("Native Brush:"),
              "combined preset selection failed");
      const TRaster32P baseline = raster(level, 1)->clone();
      draw(tool, false, .8, .8, 1.5707963267948966);
      const TRaster32P high = raster(level, 1)->clone();
      require(!same(baseline, high) && undo->getHistoryCount() == 1 &&
                  undo->undo() && same(baseline, raster(level, 1)) &&
                  undo->redo() && same(high, raster(level, 1)),
              "combined preset paint/undo/redo transaction failed");
      const TFilePath output(QDir::current().filePath(
          wet ? "combined-wet.png" : "combined-dry.png"));
      TImageWriter::save(output, TRasterP(high));
      TRasterP reopened;
      require(TImageReader::load(output, reopened),
              "combined preset painted image did not reopen");
      requireSame(high, TRaster32P(reopened), "combined preset PNG round trip");
      require(undo->undo() && same(baseline, raster(level, 1)),
              "combined preset restore failed");
      undo->reset();
      draw(tool, true, .8, .8, 1.5707963267948966);
      require(same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
              "combined preset cancellation left pixels or history");
      draw(tool, false, .2, -.8, .4);
      require(!same(high, raster(level, 1)),
              "combined preset ignored changed tablet input");
      require(undo->undo() && same(baseline, raster(level, 1)),
              "combined preset low-input undo failed");
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
