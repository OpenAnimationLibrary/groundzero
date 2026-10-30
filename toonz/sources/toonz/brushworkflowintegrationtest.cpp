#include "brushworkflowintegrationtest.h"
#include "tapp.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "tools/strokeselection.h"
#include "tools/gradienthandles.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheethandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/palettecontroller.h"
#include "toonz/tpalettehandle.h"
#include "toonz/preferences.h"
#include "toonz/tonionskinmaskhandle.h"
#include "tvectorimage.h"
#include "tvectorrenderdata.h"
#include "tofflinegl.h"
#include "tregion.h"
#include "tstroke.h"
#include "tpalette.h"
#include "tsimplecolorstyles.h"
#include "tproperty.h"
#include "tundo.h"
#include <QApplication>
#include <QDialog>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
void checkpoint(const std::string &phase) {
  std::cout << "vector-brush-native: " << phase << std::endl;
}
void enumValue(TTool *tool, const char *name, int value) {
  auto *property = dynamic_cast<TEnumProperty *>(tool->getProperties(0)->getProperty(name));
  require(property, "native Brush enum option is missing");
  property->setIndex(value); tool->onPropertyChanged(name);
}
void boolValue(TTool *tool, const char *name, bool value) {
  auto *property = dynamic_cast<TBoolProperty *>(tool->getProperties(0)->getProperty(name));
  require(property, "native Brush boolean option is missing");
  property->setValue(value); tool->onPropertyChanged(name);
}
void doubleValue(TTool *tool, const char *name, double value) {
  auto *property = dynamic_cast<TDoubleProperty *>(tool->getProperties(0)->getProperty(name));
  require(property, "native Brush numeric option is missing");
  property->setValue(value); tool->onPropertyChanged(name);
}
TStroke *rectangle(double radius) {
  auto *stroke = new TStroke(std::vector<TThickPoint>{
      {-radius,-radius,0},{0,-radius,0},{radius,-radius,0},
      {radius,0,0},{radius,radius,0},{0,radius,0},
      {-radius,radius,0},{-radius,0,0},{-radius,-radius,0}});
  stroke->setStyle(0); stroke->setSelfLoop(true);
  return stroke;
}
TVectorImageP drawing(TPalette *palette, int fill) {
  TVectorImageP image = new TVectorImage;
  image->setPalette(palette);
  image->addStroke(rectangle(20)); image->addStroke(rectangle(6));
  image->group(0,2); image->findRegions();
  require(image->getRegion(TPointD(-12,0)) && image->getRegion(TPointD(0,0)),
          "native Brush fixture did not form a ring");
  image->getRegion(TPointD(-12,0))->setStyle(fill);
  image->getRegion(TPointD(0,0))->setStyle(0);
  require(image->enterGroup(0), "native Brush fixture group entry failed");
  return image;
}
TRaster32P render(const TVectorImageP &image) {
  TOfflineGL context(TDimension(256,256)); context.makeCurrent();
  context.clear(TPixel32(0,0,0,0));
  TVectorRenderData rd(TVectorRenderData::ProductionSettings(),
      TTranslation(128,128) * TScale(4), TRect(TDimension(256,256)), image->getPalette());
  context.draw(image->clone(), rd, true);
  TRaster32P result = context.getRaster()->clone();
  context.doneCurrent(); return result;
}
TPixel32 pixel(const TRaster32P &raster, int x, int y) {
  return raster->pixels(128 + y * 4)[128 + x * 4];
}
void describeRegion(std::ostream &out, TRegion *region, int depth,
                    int &remainingRegions) {
  --remainingRegions;
  TPointD point;
  const bool hasPoint = region->getInternalPoint(point);
  const TRectD box = region->getBBox();
  out << "\nregion depth=" << depth << " style=" << region->getStyle()
      << " bbox=(" << box.x0 << ',' << box.y0 << ',' << box.x1 << ',' << box.y1
      << ") internal=";
  if (hasPoint) out << '(' << point.x << ',' << point.y << ')';
  else out << "unavailable";
  out << " edges=";
  const UINT edgeCount = region->getEdgeCount();
  for (UINT i = 0; i < std::min<UINT>(edgeCount, 16); ++i) {
    const auto *edge = region->getEdge(i);
    if (!edge) { out << " [null]"; continue; }
    out << " [index=" << edge->m_index << " id="
        << (edge->m_s ? edge->m_s->getId() : -1)
        << " w=" << edge->m_w0 << ':' << edge->m_w1 << ']';
  }
  if (edgeCount > 16) out << " [" << edgeCount - 16 << " edges omitted]";
  const UINT childCount = region->getSubregionCount();
  for (UINT i = 0; i < childCount; ++i) {
    if (depth >= 8 || remainingRegions == 0) {
      out << "\n[" << childCount - i << " child regions and their descendants omitted: "
          << (depth >= 8 ? "depth limit 8" : "total region limit 32") << ']';
      break;
    }
    describeRegion(out, region->getSubregion(i), depth + 1, remainingRegions);
  }
}
std::string paintDiagnostics(const TVectorImageP &image,
                             const TRaster32P &pixels) {
  std::ostringstream out;
  out << std::setprecision(17);
  for (const auto &position : std::vector<TPointD>{{-12,0},{12,0},{0,0},{25,0},{0,12}}) {
    const auto color = pixel(pixels, int(position.x), int(position.y));
    out << "\npixel(" << position.x << ',' << position.y << ") RGBA=("
        << int(color.r) << ',' << int(color.g) << ',' << int(color.b) << ',' << int(color.m) << ')';
  }
  out << "\nentered group depth=" << image->isInsideGroup()
      << " stroke count=" << image->getStrokeCount()
      << " region count=" << image->getRegionCount();
  const int strokeCount = int(image->getStrokeCount());
  for (int i = 0; i < std::min(strokeCount, 32); ++i) {
    const auto *stroke = image->getStroke(i);
    const TRectD box = stroke->getBBox();
    out << "\nstroke " << i << " id=" << stroke->getId()
        << " style=" << stroke->getStyle() << " group depth=" << image->getGroupDepth(i)
        << " entered=" << image->isEnteredGroupStroke(i)
        << " bbox=(" << box.x0 << ',' << box.y0 << ',' << box.x1 << ',' << box.y1
        << ") CP=" << stroke->getControlPointCount()
        << " opacity=" << stroke->getOpacity();
    auto *palette = image->getPalette();
    if (palette && stroke->getStyle() >= 0 && stroke->getStyle() < palette->getStyleCount()) {
      const auto color = palette->getStyle(stroke->getStyle())->getAverageColor();
      out << " style RGBA=(" << int(color.r) << ',' << int(color.g) << ','
          << int(color.b) << ',' << int(color.m) << ')';
    }
  }
  if (strokeCount > 32) out << "\n[" << strokeCount - 32 << " strokes omitted: limit 32]";
  int remainingRegions = 32;
  for (UINT i = 0; i < image->getRegionCount(); ++i) {
    if (remainingRegions == 0) {
      out << "\n[" << image->getRegionCount() - i
          << " top-level regions and their descendants omitted: total region limit 32]";
      break;
    }
    describeRegion(out, image->getRegion(i), 0, remainingRegions);
  }
  auto *app = TApp::instance();
  auto *tool = app->getCurrentTool()->getTool();
  out << "\ncurrent style=" << app->getCurrentLevelStyleIndex()
      << " drawing=" << tool->getCurrentFid().expand();
  for (const char *name : {"Paint:","Range:","Art Layer:","Fluid Width:","Size"})
    if (auto *property = tool->getProperties(0)->getProperty(name))
      out << ' ' << name << property->getValueAsString();
  std::string detail = out.str();
  constexpr size_t maxDetailBytes = 32 * 1024;
  if (detail.size() > maxDetailBytes) {
    const std::string omission = "\n[remaining diagnostic detail omitted: 32 KiB limit]";
    detail.resize(maxDetailBytes - omission.size());
    detail += omission;
  }
  return detail;
}
void checkPaint(const TVectorImageP &image, const std::string &phase) {
  checkpoint(phase + ": checking rendered paint");
  const auto pixels = render(image);
  auto check = [&](bool ok, const char *message) {
    if (!ok) throw std::runtime_error(phase + ": " + message + paintDiagnostics(image, pixels));
  };
  check(pixel(pixels,-12,0).r > 240 && pixel(pixels,12,0).r > 240,
          "clipped Brush did not paint both sides of the ring");
  check(pixel(pixels,0,0).m == 0, "clipped Brush filled the existing hole");
  check(pixel(pixels,25,0).m == 0, "clipped Brush escaped its mask");
  check(pixel(pixels,0,12).b > 240 && pixel(pixels,0,12).r < 10,
          "clipped Brush changed an unrelated existing fill");
  check(image->isInsideGroup() == 1 && image->getGroupDepth(0) == 1 &&
            image->getGroupDepth(1) == 1 && image->getStrokeCount() == 4 &&
            image->getGroupDepth(2) == 2 && image->getGroupDepth(3) == 2 &&
            image->isEnteredGroupStroke(2) && image->isEnteredGroupStroke(3) &&
            image->sameGroup(2,3) && image->getCommonGroupDepth(0,2) == 1,
        "clipped Brush moved its paint object outside the entered group");
  checkpoint(phase + ": paint passed");
}
void mouseStroke(TTool *tool, TPointD start, TPointD end) {
  TMouseEvent event;
  event.m_button = Qt::LeftButton; event.m_buttons = Qt::LeftButton;
  tool->mouseMove(start, event);
  require(tool->preLeftButtonDown(), "native Brush rejected a supported gesture");
  event.m_pos = start; event.m_mousePos = QPointF(start.x,start.y);
  tool->leftButtonDown(start,event);
  for (int i = 1; i < 10; ++i) {
    const auto point = start + (end - start) * (double(i) / 10);
    event.m_pos = point; event.m_mousePos = QPointF(point.x,point.y);
    tool->leftButtonDrag(point,event);
  }
  event.m_pos = end; event.m_mousePos = QPointF(end.x,end.y);
  event.m_buttons = Qt::NoButton;
  tool->leftButtonUp(end,event);
}
void acknowledgedWarning(const std::function<void()> &action) {
  bool shown = false;
  QTimer response;
  // Mouse move pumps non-input events before pen-down. Keep waiting until
  // the actual warning opens instead of consuming a one-shot there.
  QObject::connect(&response, &QTimer::timeout, [&] {
    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
      shown = true; response.stop(); dialog->reject();
    }
  });
  response.start(10); action();
  require(shown, "unsupported Brush operation did not explain why it was rejected");
}
}

void runBrushWorkflowNativeChecks() {
  checkpoint("preparing ring drawings and native Brush");
  auto *app = TApp::instance();
  auto *preferences = Preferences::instance();
  const int oldGuided = preferences->getGuidedDrawingType();
  const bool oldAuto = preferences->getGuidedAutoInbetween();
  const auto oldOnion = app->getCurrentOnionSkin()->getOnionSkinMask();
  struct Restore {
    Preferences *preferences; int guided; bool automatic; OnionSkinMask onion;
    ~Restore() {
      preferences->setValue(guidedDrawingType,guided,false);
      preferences->setValue(guidedAutoInbetween,automatic,false);
      TApp::instance()->getCurrentOnionSkin()->setOnionSkinMask(onion);
    }
  } restore{preferences,oldGuided,oldAuto,oldOnion};
  preferences->setValue(guidedDrawingType,0,false);
  preferences->setValue(guidedAutoInbetween,false,false);
  auto *level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL,L"BrushWorkflowFixture")->getSimpleLevel();
  TPaletteP palette = new TPalette;
  const int red = palette->addStyle(new TSolidColorStyle(TPixel32(255,0,0)));
  const int blue = palette->addStyle(new TSolidColorStyle(TPixel32(0,0,255)));
  const int gradient = palette->addStyle(TColorStyle::create(GradientHandles::LinearGradientTagId));
  for (int style : {red,blue,gradient}) palette->getPage(0)->addStyle(style);
  level->setPalette(palette.getPointer());
  auto *undo = TUndoManager::manager();
  const TFrameId fids[] = {TFrameId(11),TFrameId(21),TFrameId(31),TFrameId(41),TFrameId(51)};
  auto putDrawings = [&](int count, int missingFill = -1) {
    for (int i = 0; i < count; ++i) {
      level->setFrame(fids[i],drawing(palette.getPointer(),i == missingFill ? 0 : blue));
      app->getCurrentXsheet()->getXsheet()->setCell(i,0,TXshCell(level,fids[i]));
    }
  };
  auto row = [&](int index) {
    app->getCurrentColumn()->setColumnIndex(0);
    app->getCurrentFrame()->setFrame(index);
    app->getCurrentLevel()->setLevel(level);
    app->getPaletteController()->getCurrentLevelPalette()->setPalette(palette.getPointer(),red);
  };
  auto image = [&](int index) { return TVectorImageP(level->getFrame(fids[index],false)); };
  putDrawings(3); row(0);
  app->getCurrentTool()->setTool("T_Brush");
  auto *tool = app->getCurrentTool()->getTool();
  require(tool && (tool->getTargetType() & TTool::Vectors), "native vector Brush is unavailable");
  enumValue(tool,"Paint:",3); enumValue(tool,"Range:",0);
  enumValue(tool,"Selection Mask:",0); enumValue(tool,"Art Layer:",0);
  for (const char *name : {"Pressure","Break","Snap","Assistants","Auto Close","Auto Group","Auto Fill"})
    boolValue(tool,name,false);
  doubleValue(tool,"Accuracy:",100); doubleValue(tool,"Smooth:",0);
  auto *size = dynamic_cast<TDoublePairProperty *>(tool->getProperties(0)->getProperty("Size"));
  require(size, "native Brush Size option missing");
  size->setValue({10.,10.}); tool->onPropertyChanged("Size");
  tool->onEnter(); undo->reset();
  require(app->getCurrentLevel()->getSimpleLevel() == level &&
              tool->getCurrentFid() == fids[0] && TVectorImageP(tool->getImage(false)) == image(0),
          "initial Paint Inside gesture targets another drawing");
  require(app->getPaletteController()->getCurrentLevelPalette()->getPalette() == palette.getPointer() &&
              app->getCurrentLevelStyleIndex() == red &&
              app->getCurrentLevelStyle() == palette->getStyle(red),
          "initial Paint Inside gesture does not use the red fixture style");
  auto *fluidWidth = tool->getProperties(0)->getProperty("Fluid Width:");
  require(fluidWidth, "native Brush Fluid Width option missing");
  checkpoint("initial Paint Inside: before gesture; Fluid Width=" + fluidWidth->getValueAsString());
  mouseStroke(tool,{-15,0},{30,0});
  checkpoint("initial Paint Inside: gesture returned");
  require(undo->getHistoryCount() == 1 && image(0)->getStrokeCount() == 4,
          "Paint Inside gesture did not create one clipped two-part object");
  checkPaint(image(0), "initial Paint Inside");
  checkpoint("Paint Inside: undo and redo");
  undo->undo(); require(image(0)->getStrokeCount() == 2, "Paint Inside undo lost its source drawing");
  undo->redo(); checkPaint(image(0), "Paint Inside redo"); undo->undo(); undo->reset();

  // A last child may be nested deeper than the group being edited, and an
  // outside object may overlap it. Paint belongs directly in the entered
  // group, above that child and below the outside object.
  checkpoint("nested Paint Inside: preparing deeper sibling and outer overlap");
  const TVectorImageP simpleRing = image(0);
  TVectorImageP nested = drawing(palette.getPointer(), blue);
  nested->exitGroup(); nested->group(0,2);
  require(nested->enterGroup(0) && nested->enterGroup(0),
          "nested Paint Inside fixture could not enter both ancestors");
  const int boundaryIds[] = {nested->getStroke(0)->getId(), nested->getStroke(1)->getId()};
  TVectorImageP sibling = new TVectorImage;
  sibling->setPalette(palette.getPointer());
  auto *under = new TStroke(std::vector<TThickPoint>{{-14,0,1},{-12,0,1},{-10,0,1}});
  under->setStyle(1); const int underId = under->getId();
  sibling->addStroke(under); sibling->group(0,1);
  nested->mergeImage(std::vector<const TVectorImage *>{sibling.getPointer()});
  nested->exitGroup(); nested->exitGroup();
  auto *over = new TStroke(std::vector<TThickPoint>{{10,0,1},{12,0,1},{14,0,1}});
  over->setStyle(blue); const int overId = over->getId(); nested->addStroke(over);
  require(nested->enterGroup(0) && nested->enterGroup(0),
          "nested Paint Inside fixture lost its entered ancestors");
  level->setFrame(fids[0], nested);
  app->getCurrentXsheet()->notifyXsheetChanged(); row(0);
  auto checkNested = [&](bool painted, const std::string &phase) {
    checkpoint(phase + ": checking ancestry, ordering and rendered paint");
    const TVectorImageP actual = image(0);
    const auto pixels = render(actual);
    auto check = [&](bool ok, const char *message) {
      if (!ok) throw std::runtime_error(phase + ": " + message + paintDiagnostics(actual,pixels));
    };
    check(actual->isInsideGroup() == 2 && actual->getStrokeCount() == (painted ? 6 : 4),
          "nested Brush changed the entered group or stroke count");
    check(actual->getStroke(0)->getId() == boundaryIds[0] &&
              actual->getStroke(1)->getId() == boundaryIds[1] &&
              actual->getGroupDepth(0) == 2 && actual->getGroupDepth(1) == 2 &&
              actual->getStrokeIndexById(underId) == 2 && actual->getGroupDepth(2) == 3,
          "nested Brush changed existing boundaries or deeper sibling");
    const int overIndex = actual->getStrokeIndexById(overId);
    check(overIndex == (painted ? 5 : 3) && actual->getGroupDepth(overIndex) == 0 &&
              !actual->isEnteredGroupStroke(overIndex),
          "nested Brush crossed an outside layer");
    if (painted)
      check(actual->getGroupDepth(3) == 3 && actual->getGroupDepth(4) == 3 &&
                actual->sameGroup(3,4) && actual->getCommonGroupDepth(2,3) == 2 &&
                actual->isEnteredGroupStroke(3) && actual->isEnteredGroupStroke(4),
            "nested Brush inherited the deeper sibling instead of the entered group");
    const auto left = pixel(pixels,-12,0), right = pixel(pixels,12,0);
    check((painted ? left.r > 240 : left.r < 10) && left.b < 10 && left.m > 240 &&
              right.b > 240 && right.r < 10,
          "nested Brush changed sibling or outer-layer overlap order");
    check(pixel(pixels,0,0).m == 0 && pixel(pixels,25,0).m == 0 &&
              pixel(pixels,0,12).b > 240 && pixel(pixels,0,12).r < 10,
          "nested Brush changed a hole, mask limit or unrelated fill");
  };
  checkNested(false, "nested Paint Inside baseline");
  mouseStroke(tool,{-15,0},{30,0});
  require(undo->getHistoryCount() == 1, "nested Paint Inside did not create one undo step");
  checkNested(true, "nested Paint Inside");
  for (int repeat = 0; repeat < 2; ++repeat) {
    undo->undo(); checkNested(false, "nested Paint Inside undo " + std::to_string(repeat));
    undo->redo(); checkNested(true, "nested Paint Inside redo " + std::to_string(repeat));
  }
  undo->undo(); undo->reset();
  level->setFrame(fids[0], simpleRing);
  app->getCurrentXsheet()->notifyXsheetChanged(); row(0);

  // Capture through the real Selection -> Brush transition, not a test-only
  // mask setter. Both boundaries are selected; the unfilled inner one stays a hole.
  auto capture = [&] {
    checkpoint("Paint Selection: capturing native Selection-to-Brush mask");
    app->getCurrentTool()->setTool("T_Selection");
    auto *selection = dynamic_cast<StrokeSelection *>(app->getCurrentTool()->getTool()->getSelection());
    require(selection, "native paint selection is unavailable");
    selection->selectNone(); selection->select(0,true); selection->select(1,true);
    selection->makeCurrent();
    app->getCurrentTool()->setTool("T_Brush");
    tool = app->getCurrentTool()->getTool();
    enumValue(tool,"Paint:",4);
    checkpoint("Paint Selection: mask capture returned");
  };
  capture(); mouseStroke(tool,{-15,0},{30,0}); checkPaint(image(0), "Paint Selection");
  require(undo->getHistoryCount() == 1, "Paint Selection gesture made multiple undo records");
  undo->undo(); undo->reset();
  checkpoint("Paint Selection: stale-mask refusal");
  capture();
  auto point = image(0)->getStroke(0)->getControlPoint(0);
  point.x -= 2; image(0)->getStroke(0)->setControlPoint(0,point);
  app->getCurrentScene()->setDirtyFlag(false);
  acknowledgedWarning([&] { require(!tool->preLeftButtonDown(), "stale selection mask was accepted"); });
  require(image(0)->getStrokeCount() == 2 && undo->getHistoryCount() == 0 &&
              !app->getCurrentScene()->getDirtyFlag(), "stale-mask refusal changed the drawing");

  checkpoint("Paint Selection: Art Layer and read-only refusals");
  putDrawings(3); row(0); capture();
  enumValue(tool,"Art Layer:",1); // The selected boundaries are Colour Art.
  acknowledgedWarning([&] { require(!tool->preLeftButtonDown(), "masked-out Art Layer remained editable"); });
  enumValue(tool,"Art Layer:",0);
  level->setIsReadOnly(true);
  require(!tool->preLeftButtonDown(), "Brush modified a read-only level");
  level->setIsReadOnly(false);
  auto *column = app->getCurrentXsheet()->getXsheet()->getColumn(0);
  column->lock(true); require(!tool->preLeftButtonDown(), "Brush modified a locked column");
  column->lock(false);

  // Range selection reuse requires an explicit user choice. Drawing IDs are
  // intentionally different from their exposure rows to exercise undo IDs.
  checkpoint("Paint Selection range: implicit-mask refusal then explicit reuse");
  enumValue(tool,"Range:",1);
  mouseStroke(tool,{-15,0},{30,0}); row(2);
  acknowledgedWarning([&] { require(!tool->preLeftButtonDown(), "range inferred an unrelated selection"); });
  enumValue(tool,"Selection Mask:",1);
  mouseStroke(tool,{-15,0},{30,0});
  require(undo->getHistoryCount() == 1, "selection range did not form one undo step");
  for (int i = 0; i < 3; ++i) checkPaint(image(i), "Paint Selection range drawing " + std::to_string(i));
  undo->undo();
  for (int i = 0; i < 3; ++i)
    require(image(i)->getStrokeCount() == 2, "range undo restored the exposure row instead of drawing ID");
  undo->redo(); for (int i = 0; i < 3; ++i) checkPaint(image(i), "Paint Selection range redo drawing " + std::to_string(i));
  undo->undo(); undo->reset();

  checkpoint("Paint Inside range: missing-fill atomic refusal");
  putDrawings(3,1); row(0); enumValue(tool,"Paint:",3);
  mouseStroke(tool,{-15,0},{30,0}); row(2);
  app->getCurrentScene()->setDirtyFlag(false);
  acknowledgedWarning([&] { mouseStroke(tool,{-15,0},{30,0}); });
  for (int i = 0; i < 3; ++i)
    require(image(i)->getStrokeCount() == 2, "invalid Paint Inside range was only partially canceled");
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "invalid Paint Inside range changed undo or scene state");

  // Fill-only palette styles are accepted by the real input handler.
  checkpoint("Paint Inside: fill-only gradient style");
  putDrawings(3); row(0); enumValue(tool,"Range:",0);
  app->getPaletteController()->getCurrentLevelPalette()->setStyleIndex(gradient);
  mouseStroke(tool,{-15,0},{30,0});
  require(image(0)->getStrokeCount() == 4 && undo->getHistoryCount() == 1,
          "fill-capable gradient was silently rejected by Brush input");
  undo->undo(); undo->reset();

  checkpoint("Guided painting: preparing explicit guide strokes");
  putDrawings(5); row(2);
  for (int i : {0,4}) {
    const double y = i == 0 ? -2. : 2.;
    auto *guide = new TStroke(std::vector<TThickPoint>{{-15,y,5},{7.5,y,5},{30,y,5}});
    guide->setStyle(red); image(i)->addStroke(guide);
  }
  require(tool->getViewer(), "Guided Drawing fixture has no native viewer");
  tool->getViewer()->setGuidedBackStroke(2); tool->getViewer()->setGuidedFrontStroke(2);
  OnionSkinMask onion; onion.enable(true); onion.setFos(0,true); onion.setFos(4,true);
  app->getCurrentOnionSkin()->setOnionSkinMask(onion);
  preferences->setValue(guidedDrawingType,1,false);
  preferences->setValue(guidedAutoInbetween,true,false);
  checkpoint("Guided painting: before native gesture");
  mouseStroke(tool,{-15,0},{30,0});
  checkpoint("Guided painting: gesture returned");
  require(undo->getHistoryCount() == 1 && image(2)->getStrokeCount() == 4 &&
              image(0)->getStrokeCount() == 3 && image(4)->getStrokeCount() == 3,
          "Guided painting duplicated the current stroke or repainted guide endpoints");
  for (int i : {1,2,3}) checkPaint(image(i), "Guided painting drawing " + std::to_string(i));
  for (int repeat = 0; repeat < 2; ++repeat) {
    undo->undo();
    for (int i : {1,2,3}) require(image(i)->getStrokeCount() == 2,
                                "Guided undo changed the wrong drawing");
    require(image(0)->getStrokeCount() == 3 && image(4)->getStrokeCount() == 3,
            "Guided undo removed an existing guide");
    undo->redo(); for (int i : {1,2,3}) checkPaint(image(i),
        "Guided painting redo " + std::to_string(repeat) + " drawing " + std::to_string(i));
  }
  undo->undo(); undo->reset();
  // Exposure order can run opposite to drawing-number order. Endpoint flags
  // must follow the caller's guides when the interpolation range is sorted.
  checkpoint("Guided painting: reversed exposure order");
  for (int i = 0; i < 5; ++i)
    app->getCurrentXsheet()->getXsheet()->setCell(i,0,TXshCell(level,fids[4-i]));
  row(2); mouseStroke(tool,{-15,0},{30,0});
  require(image(0)->getStrokeCount() == 3 && image(4)->getStrokeCount() == 3 &&
              image(2)->getStrokeCount() == 4 && undo->getHistoryCount() == 1,
          "reversed guide drawing numbers changed endpoint inclusion");
  for (int i : {1,2,3}) checkPaint(image(i), "Guided reversed exposure drawing " + std::to_string(i));
  undo->undo(); undo->reset();
  for (int i = 0; i < 5; ++i)
    app->getCurrentXsheet()->getXsheet()->setCell(i,0,TXshCell(level,fids[i]));
  row(2);
  // A bad front guide must not leave a successfully generated back half.
  checkpoint("Guided painting: unsupported opacity and foreign-level refusals");
  image(4)->getStroke(2)->setOpacity(.5);
  acknowledgedWarning([&] { mouseStroke(tool,{-15,0},{30,0}); });
  require(image(1)->getStrokeCount() == 2 && image(3)->getStrokeCount() == 2 &&
              image(2)->getStrokeCount() == 4 && undo->getHistoryCount() == 1,
          "unsupported guide appearance generated a partial range or a Normal fallback");
  undo->undo(); undo->reset();
  checkpoint("Guided painting: foreign-level guide refusal");
  image(4)->getStroke(2)->setOpacity(1.);
  auto *otherLevel = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL,L"ForeignGuideFixture")->getSimpleLevel();
  otherLevel->setPalette(palette.getPointer());
  otherLevel->setFrame(fids[4],image(4)->clone());
  app->getCurrentXsheet()->getXsheet()->setCell(4,0,TXshCell(otherLevel,fids[4]));
  row(2);
  acknowledgedWarning([&] { mouseStroke(tool,{-15,0},{30,0}); });
  require(image(1)->getStrokeCount() == 2 && image(3)->getStrokeCount() == 2 &&
              image(2)->getStrokeCount() == 4 && undo->getHistoryCount() == 1,
          "Guided painting used a drawing ID from another level");
  undo->undo(); undo->reset();
  checkpoint("Paint Inside: zero-width refusal");
  size->setValue({0.,0.}); tool->onPropertyChanged("Size");
  acknowledgedWarning([&] { require(!tool->preLeftButtonDown(), "zero-width paint fell back to Normal"); });
  require(image(2)->getStrokeCount() == 2 && undo->getHistoryCount() == 0,
          "unsupported Brush width changed a drawing");
  size->setValue({10.,10.}); tool->onPropertyChanged("Size");
  undo->reset();
  checkpoint("PASS");
}
