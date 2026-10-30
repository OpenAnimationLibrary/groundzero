#include "contourworkflowintegrationtest.h"
#include "tapp.h"
#include "menubarcommandids.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "tools/strokeselection.h"
#include "toonzqt/menubarcommand.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txsheethandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "tvectorimage.h"
#include "tvectorrenderdata.h"
#include "tofflinegl.h"
#include "tregion.h"
#include "tstroke.h"
#include "tpalette.h"
#include "tsimplecolorstyles.h"
#include "tundo.h"
#include <QAction>
#include <QApplication>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
TRaster32P render(const TVectorImageP &image) {
  TOfflineGL context(TDimension(256,256));
  context.makeCurrent();
  context.clear(TPixel32(0,0,0,0));
  TVectorRenderData rd(TVectorRenderData::ProductionSettings(),
      TTranslation(128,128) * TScale(4), TRect(TDimension(256,256)), image->getPalette());
  // Group-entry state belongs to the editor, not to exported artwork.
  context.draw(image->clone(), rd, true);
  TRaster32P pixels = context.getRaster()->clone();
  context.doneCurrent();
  return pixels;
}
void samePixel(const TRaster32P &before, const TRaster32P &after,
               int x, int y, const char *message) {
  const auto a = before->pixels(y * 4 + 128)[x * 4 + 128];
  const auto b = after->pixels(y * 4 + 128)[x * 4 + 128];
  require(std::abs(int(a.r) - b.r) <= 2 && std::abs(int(a.g) - b.g) <= 2 &&
              std::abs(int(a.b) - b.b) <= 2 && std::abs(int(a.m) - b.m) <= 2,
          message);
}
void compareRenders(const TRaster32P &before, const TRaster32P &after) {
  int occupied = 0, mismatched = 0;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const auto a = before->pixels(y)[x], b = after->pixels(y)[x];
      if (a.m || b.m) ++occupied;
      const int difference = std::max({std::abs(int(a.r)-b.r), std::abs(int(a.g)-b.g),
                                      std::abs(int(a.b)-b.b), std::abs(int(a.m)-b.m)});
      if (difference > 32) ++mismatched;
    }
  require(occupied > 1000, "contour render fixture produced no real artwork");
  // The fitted boundary and the brush tessellator may differ at antialiased
  // edge pixels. Interior ordering, alpha and holes are checked separately.
  require(mismatched * 20 <= occupied, "contour render differs beyond boundary tolerance");
  samePixel(before, after, 12,0, "converted ring no longer covers the lower line");
  samePixel(before, after, 12,5, "converted ring opacity changed");
  samePixel(before, after, 0,5, "closed ring hole was filled");
  samePixel(before, after, 0,-5, "existing fill was lost");
  samePixel(before, after, 0,0, "unselected line changed inside ring hole");
  samePixel(before, after, 12,8, "converted stroke covered an originally higher stroke");
  samePixel(before, after, 0,22, "unaffected group changed");
}
}

void runContourOutlineNativeChecks() {
  auto *app = TApp::instance();
  auto *level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL, L"ContourWorkflowFixture")->getSimpleLevel();
  TPaletteP palette = new TPalette;
  const int red = palette->addStyle(new TSolidColorStyle(TPixel32(255,0,0)));
  const int blue = palette->addStyle(new TSolidColorStyle(TPixel32(0,0,255)));
  palette->getPage(0)->addStyle(red); palette->getPage(0)->addStyle(blue);
  level->setPalette(palette.getPointer());
  TVectorImageP original = new TVectorImage;
  original->setPalette(palette.getPointer());
  auto *line = new TStroke(std::vector<TThickPoint>{{-25,0,2},{0,0,2},{25,0,2}});
  line->setStyle(1); original->addStroke(line);
  auto *ring = new TStroke(std::vector<TThickPoint>{
      {-12,-12,3},{0,-12,3},{12,-12,3},{12,0,3},{12,12,3},
      {0,12,3},{-12,12,3},{-12,0,3},{-12,-12,3}});
  ring->setSelfLoop(true); ring->setStyle(red); ring->setOpacity(.5);
  original->addStroke(ring);
  auto *top = new TStroke(std::vector<TThickPoint>{{-20,8,1},{0,8,1},{20,8,1}});
  top->setStyle(blue); original->addStroke(top);
  original->group(0, 3);
  original->group(0, 3); // Preserve an existing nested ancestor hierarchy.
  auto *outside = new TStroke(std::vector<TThickPoint>{{-20,22,1},{0,22,1},{20,22,1}});
  outside->setStyle(1); outside->setOpacity(.75); original->addStroke(outside);
  original->findRegions();
  require(original->getRegion(TPointD(0,-5)), "native contour fixture has no region");
  original->getRegion(TPointD(0,-5))->setStyle(blue);
  level->setFrame(TFrameId(1), original);
  app->getCurrentXsheet()->getXsheet()->setCell(0,0,TXshCell(level,TFrameId(1)));
  app->getCurrentColumn()->setColumnIndex(0);
  app->getCurrentFrame()->setFrame(0);
  app->getCurrentLevel()->setLevel(level);
  app->getCurrentTool()->setTool("T_Selection");
  QCoreApplication::processEvents();
  auto *selection = dynamic_cast<StrokeSelection *>(app->getCurrentTool()->getTool()->getSelection());
  require(selection, "native contour selection unavailable");
  // Enter the nested group to select just the top brush stroke.
  require(original->enterGroup(1) && original->enterGroup(1), "native contour fixture group entry failed");
  selection->selectNone(); selection->select(1, true); selection->makeCurrent();
  app->getCurrentTool()->notifyToolChanged();
  const auto before = render(original);
  const int originalStyleCount = palette->getStyleCount();
  auto *undo = TUndoManager::manager(); undo->reset();
  auto *action = CommandManager::instance()->getAction(MI_ConvertToOutlineShapes);
  require(action && action->isEnabled(), "native contour command disabled");
  action->trigger();
  TVectorImageP converted = level->getFrame(TFrameId(1), false);
  require(converted && undo->getHistoryCount() == 1, "contour command did not create one undo step");
  compareRenders(before, render(converted));
  const int unselected = converted->getStrokeIndexById(line->getId());
  require(unselected >= 0 && converted->getGroupDepth(unselected) == 3,
          "contour layers lost original nested group ancestors");
  undo->undo();
  TVectorImageP restored = level->getFrame(TFrameId(1), false);
  require(restored->getStrokeCount() == 4 && restored->getGroupDepth(0) == 2 &&
              restored->getGroupDepth(3) == 0 &&
              restored->getPalette()->getStyleCount() == originalStyleCount,
          "contour undo lost original groups or palette");
  compareRenders(before, render(restored));
  undo->redo();
  compareRenders(before, render(TVectorImageP(level->getFrame(TFrameId(1), false))));
  undo->reset();
}
