// Production level-column checks with two exposures of the same drawing.
// The pixels use CPU raster checks, but the linked renderer's shader manager
// creates an offscreen context and therefore needs a GUI application.
#include "toonz/activechecks.h"
#include "toonz/levelproperties.h"
#include "toonz/sceneproperties.h"
#include "toonz/scenefx.h"
#include "toonz/tcamera.h"
#include "toonz/tcolumnfx.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "tcolorstyles.h"
#include "tconvert.h"
#include "tenv.h"
#include "texception.h"
#include "tpalette.h"
#include "trasterfx.h"
#include "trenderer.h"
#include "tthread.h"
#include "ttoonzimage.h"

#include <QGuiApplication>
#include <QDir>
#include <QObject>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>
#include <string>

DV_IMPORT_API void initStdFx();

namespace {

const TDimension imageSize(16, 12);

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

struct RenderScope {
  TRenderer renderer;
  unsigned long id;
  RenderScope() : renderer(1), id(TRenderer::buildRenderId()) {
    renderer.enablePrecomputing(false);
    renderer.install(id);
    renderer.declareRenderStart(id);
    renderer.declareFrameStart(0);
  }
  ~RenderScope() {
    renderer.declareFrameEnd(0);
    renderer.declareRenderEnd(id);
    renderer.uninstall();
  }
};

struct Fixture {
  ToonzScene scene;
  TXshSimpleLevel *level;
  TToonzImageP drawing;
  TRasterCM32P original;
  TLevelColumnFx *columns[2];

  explicit Fixture(const QString &directory) {
    TProjectManager::instance()->initializeScene(&scene);
    scene.setScenePath(TFilePath(QDir(directory).filePath("checks.tnz")));
    TXshLevel *created = scene.createNewLevel(TZP_XSHLEVEL, L"Checks");
    level              = created ? created->getSimpleLevel() : nullptr;
    require(level != nullptr, "cannot create the test level");
    level->setPath(TFilePath(QDir(directory).filePath("checks.tlv")), true);
    level->setPalette(new TPalette);
    level->getPalette()->getStyle(1)->setMainColor(TPixel32(30, 60, 90));
    level->getProperties()->setImageRes(imageSize);

    TRasterCM32P raster(imageSize);
    raster->fill(TPixelCM32(0, 0, 255));
    // A closed outline straddling both axes of the tile grid.
    for (int x = 3; x <= 12; ++x) {
      raster->pixels(2)[x] = TPixelCM32(1, 0, 0);
      raster->pixels(9)[x] = TPixelCM32(1, 0, 0);
    }
    for (int y = 2; y <= 9; ++y) {
      raster->pixels(y)[3]  = TPixelCM32(1, 0, 0);
      raster->pixels(y)[12] = TPixelCM32(1, 0, 0);
    }
    drawing = TToonzImageP(raster, TRect(1, 1, 14, 10));
    drawing->setPalette(level->getPalette());
    drawing->setDpi(72, 72);
    original = raster->clone();
    level->setFrame(TFrameId(1), drawing);

    for (int c = 0; c < 2; ++c) {
      scene.getXsheet()->setCell(0, c, TXshCell(created, TFrameId(1)));
      columns[c] =
          scene.getXsheet()->getColumn(c)->getLevelColumn()->getLevelColumnFx();
      require(columns[c] && columns[c]->getIdentifier() != 0,
              "test column has no renderer identity");
    }
    require(columns[0]->getIdentifier() != columns[1]->getIdentifier(),
            "test columns share their renderer identity");
    scene.getCurrentCamera()->setRes(imageSize);
    scene.getCurrentCamera()->setSize(TDimensionD(16.0 / 72.0, 12.0 / 72.0));
    scene.getProperties()->setBgColor(TPixel32(210, 220, 230, 0));
  }

  TRenderSettings settings(int mask) {
    TRenderSettings result;
    result.m_activeChecks.m_enabled         = true;
    result.m_activeChecks.m_checks          = mask;
    result.m_activeChecks.m_colorIndex      = 1;
    result.m_activeChecks.m_columnPath      = {0};
    result.m_activeChecks.m_fillOnlySavebox = false;
    resolveActiveRenderChecks(&scene, result.m_activeChecks);
    require(result.m_activeChecks.m_targetFxId == columns[0]->getIdentifier(),
            "saved column path did not resolve to its renderer identity");
    return result;
  }

  void requireUnchanged() const {
    const TRasterCM32P raster = drawing->getRaster();
    for (int y = 0; y < imageSize.ly; ++y)
      for (int x = 0; x < imageSize.lx; ++x)
        require(raster->pixels(y)[x] == original->pixels(y)[x],
                "render checks modified source ink/paint pixels");
    require(level->getPalette()->getStyle(1)->getAverageColor() ==
                TPixel32(30, 60, 90),
            "render checks modified the source palette");
    require(drawing->getSavebox() == TRect(1, 1, 14, 10),
            "render checks modified the source savebox");
  }
};

TRaster32P renderColumn(TLevelColumnFx *column, const TRenderSettings &settings,
                        const TRect &sourceRect) {
  TRaster32P raster(sourceRect.getSize());
  raster->clear();
  TTile tile(raster, TPointD(sourceRect.x0 - imageSize.lx / 2,
                             sourceRect.y0 - imageSize.ly / 2));
  // Skip the final cache so the tiled comparison cannot reuse an already
  // rendered whole-frame check image. The shared source cache still runs.
  column->doCompute(tile, 0, settings);
  return raster;
}

void testIsolationAndSnapshot(Fixture &fixture) {
  RenderScope render;
  TRenderSettings settings = fixture.settings(TRenderCheckSettings::Ink);
  const TRenderSettings normal;
  const std::string originalAlias = fixture.columns[0]->getAlias(0, normal);
  require(fixture.columns[1]->getAlias(0, normal) == originalAlias,
          "same-source setup did not share the original alias");
  require(fixture.columns[0]->getAlias(0, settings) !=
              fixture.columns[1]->getAlias(0, settings),
          "checked and ordinary columns shared the rendered cache alias");
  require(fixture.columns[1]->getAlias(0, settings) == originalAlias,
          "checks changed the other column's cache alias");

  TFxPair roots;
  roots.m_frameA = fixture.columns[0];
  TRenderer::RenderData submitted(0, settings, roots);
  settings.m_activeChecks.m_inkColor   = TPixel32::Green;
  settings.m_activeChecks.m_targetFxId = fixture.columns[1]->getIdentifier();
  const TRect whole(imageSize);
  TRaster32P checked =
      renderColumn(fixture.columns[0], submitted.m_info, whole);
  TRaster32P ordinary =
      renderColumn(fixture.columns[1], submitted.m_info, whole);
  require(checked->pixels(4)[3] == TPixel32::Red,
          "submitted job followed later changes to the check settings");
  require(ordinary->pixels(4)[3] == TPixel32(30, 60, 90),
          "source checks leaked to another exposure of the level");
  require(checked->pixels(5)[7] == TPixel32::Transparent,
          "Ink Check filled an empty area");

  TRenderSettings disabled          = submitted.m_info;
  disabled.m_activeChecks.m_enabled = false;
  require(fixture.columns[0]->getAlias(0, disabled) == originalAlias,
          "disabled checks changed the original cache alias");
  TRaster32P restored = renderColumn(fixture.columns[0], disabled, whole);
  require(restored->pixels(4)[3] == TPixel32(30, 60, 90),
          "disabled checks changed ordinary source rendering");
  fixture.requireUnchanged();
}

void testFillAcrossTiles(Fixture &fixture, bool fillOnlySavebox) {
  RenderScope render;
  TRenderSettings settings = fixture.settings(TRenderCheckSettings::Fill);
  settings.m_activeChecks.m_fillOnlySavebox = fillOnlySavebox;
  TRaster32P tiled(imageSize);
  tiled->clear();
  for (int y = 0; y < imageSize.ly; y += 6) {
    for (int x = 0; x < imageSize.lx; x += 8) {
      TRaster32P part =
          renderColumn(fixture.columns[0], settings, TRect(x, y, x + 7, y + 5));
      for (int py = 0; py < part->getLy(); ++py)
        for (int px = 0; px < part->getLx(); ++px)
          tiled->pixels(y + py)[x + px] = part->pixels(py)[px];
    }
  }
  require(tiled->pixels(5)[7] == TPixel32(128, 128, 128) &&
              tiled->pixels(6)[8] == TPixel32(128, 128, 128),
          "Fill Check lost an enclosed area spanning render tiles");
  require(tiled->pixels(0)[0] == TPixel32::Transparent,
          "Fill Check made the source background opaque");
  TRaster32P whole =
      renderColumn(fixture.columns[0], settings, TRect(imageSize));
  for (int y = 0; y < imageSize.ly; ++y)
    for (int x = 0; x < imageSize.lx; ++x)
      require(whole->pixels(y)[x] == tiled->pixels(y)[x],
              "Fill Check changed across a render tile boundary");
  fixture.requireUnchanged();
}

void testBlackBackground(Fixture &fixture) {
  RenderScope render;
  const TPixel32 originalBackground =
      fixture.scene.getProperties()->getBgColor();
  TRenderSettings settings = fixture.settings(TRenderCheckSettings::BlackBg);
  TRasterFxP graph =
      buildSceneFx(&fixture.scene, 0.0, 1, false, &settings.m_activeChecks);
  require(bool(graph), "cannot build the scene render graph");
  TTile output;
  graph->allocateAndCompute(output, TPointD(-8, -6), imageSize,
                            TRaster32P(1, 1), 0, settings);
  TRaster32P raster = output.getRaster();
  require(raster && raster->pixels(0)[0] == TPixel32::Black,
          "Black BG was not included in the rendered scene");
  require(fixture.scene.getProperties()->getBgColor() == originalBackground,
          "Black BG changed the scene's stored background");
  fixture.requireUnchanged();
}

void testAnimatedPaletteWithAntialias(Fixture &fixture) {
  RenderScope render;
  TPalette *palette     = fixture.level->getPalette();
  const int unusedStyle = palette->addStyle(TPixel32::Blue);
  palette->setKeyframe(1, 0);
  palette->getStyle(1)->setMainColor(TPixel32(180, 30, 150));
  palette->setKeyframe(1, 10);
  fixture.level->getProperties()->setDoAntialias(10);

  // The reference is rendered while the palette is already at the output
  // frame. The checked job must match it even if the Viewer is elsewhere.
  palette->setFrame(10);
  palette->setFrame(0);
  TRaster32P reference =
      renderColumn(fixture.columns[0], TRenderSettings(), TRect(imageSize));
  palette->setFrame(10);
  TRenderSettings settings = fixture.settings(TRenderCheckSettings::Ink);
  settings.m_activeChecks.m_colorIndex = unusedStyle;
  TRaster32P checked =
      renderColumn(fixture.columns[0], settings, TRect(imageSize));
  for (int y = 0; y < imageSize.ly; ++y)
    for (int x = 0; x < imageSize.lx; ++x)
      require(checked->pixels(y)[x] == reference->pixels(y)[x],
              "checked level antialiasing used the Viewer's palette frame");
  require(palette->getFrame() == 10 &&
              palette->getStyle(1)->getAverageColor() == TPixel32(180, 30, 150),
          "render checks changed the live animated palette frame");
  palette->setFrame(0);
  fixture.level->getProperties()->setDoAntialias(0);
  fixture.requireUnchanged();
}

}  // namespace

int main(int argc, char *argv[]) {
  QGuiApplication application(argc, argv);
  QObject mainScope(&application);
  mainScope.setObjectName("mainScope");
  try {
    TThread::init();
    QTemporaryDir directory;
    require(directory.isValid(), "cannot create test directory");
    const QString stuff = QDir(directory.path()).filePath("stuff");
    QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default"));
    TEnv::setStuffDir(TFilePath(stuff));
    initStdFx();
    Fixture fixture(directory.path());
    std::cout << "active checks: column isolation" << std::endl;
    testIsolationAndSnapshot(fixture);
    std::cout << "active checks: fill full image" << std::endl;
    testFillAcrossTiles(fixture, false);
    std::cout << "active checks: fill savebox" << std::endl;
    testFillAcrossTiles(fixture, true);
    std::cout << "active checks: black background" << std::endl;
    testBlackBackground(fixture);
    std::cout << "active checks: animated palette" << std::endl;
    testAnimatedPaletteWithAntialias(fixture);
  } catch (const TException &error) {
    std::cerr << "activecheckscolumn_test: " << ::to_string(error.getMessage())
              << std::endl;
    return 1;
  } catch (const std::exception &error) {
    std::cerr << "activecheckscolumn_test: " << error.what() << std::endl;
    return 1;
  }
  std::cout << "ok: render check column isolation, tiled Fill and background"
            << std::endl;
  return 0;
}
