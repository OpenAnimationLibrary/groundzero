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
#include <functional>
#include <stdexcept>
#include <vector>

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
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
void checkPaint(const TVectorImageP &image) {
  const auto pixels = render(image);
  require(pixel(pixels,-12,0).r > 240 && pixel(pixels,12,0).r > 240,
          "clipped Brush did not paint both sides of the ring");
  require(pixel(pixels,0,0).m == 0, "clipped Brush filled the existing hole");
  require(pixel(pixels,25,0).m == 0, "clipped Brush escaped its mask");
  require(pixel(pixels,0,12).b > 240 && pixel(pixels,0,12).r < 10,
          "clipped Brush changed an unrelated existing fill");
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
  mouseStroke(tool,{-15,0},{30,0});
  require(undo->getHistoryCount() == 1 && image(0)->getStrokeCount() == 4,
          "Paint Inside gesture did not create one clipped two-part object");
  checkPaint(image(0));
  undo->undo(); require(image(0)->getStrokeCount() == 2, "Paint Inside undo lost its source drawing");
  undo->redo(); checkPaint(image(0)); undo->undo(); undo->reset();

  // Capture through the real Selection -> Brush transition, not a test-only
  // mask setter. Both boundaries are selected; the unfilled inner one stays a hole.
  auto capture = [&] {
    app->getCurrentTool()->setTool("T_Selection");
    auto *selection = dynamic_cast<StrokeSelection *>(app->getCurrentTool()->getTool()->getSelection());
    require(selection, "native paint selection is unavailable");
    selection->selectNone(); selection->select(0,true); selection->select(1,true);
    selection->makeCurrent();
    app->getCurrentTool()->setTool("T_Brush");
    tool = app->getCurrentTool()->getTool();
    enumValue(tool,"Paint:",4);
  };
  capture(); mouseStroke(tool,{-15,0},{30,0}); checkPaint(image(0));
  require(undo->getHistoryCount() == 1, "Paint Selection gesture made multiple undo records");
  undo->undo(); undo->reset();
  capture();
  auto point = image(0)->getStroke(0)->getControlPoint(0);
  point.x -= 2; image(0)->getStroke(0)->setControlPoint(0,point);
  app->getCurrentScene()->setDirtyFlag(false);
  acknowledgedWarning([&] { require(!tool->preLeftButtonDown(), "stale selection mask was accepted"); });
  require(image(0)->getStrokeCount() == 2 && undo->getHistoryCount() == 0 &&
              !app->getCurrentScene()->getDirtyFlag(), "stale-mask refusal changed the drawing");

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
  enumValue(tool,"Range:",1);
  mouseStroke(tool,{-15,0},{30,0}); row(2);
  acknowledgedWarning([&] { require(!tool->preLeftButtonDown(), "range inferred an unrelated selection"); });
  enumValue(tool,"Selection Mask:",1);
  mouseStroke(tool,{-15,0},{30,0});
  require(undo->getHistoryCount() == 1, "selection range did not form one undo step");
  for (int i = 0; i < 3; ++i) checkPaint(image(i));
  undo->undo();
  for (int i = 0; i < 3; ++i)
    require(image(i)->getStrokeCount() == 2, "range undo restored the exposure row instead of drawing ID");
  undo->redo(); for (int i = 0; i < 3; ++i) checkPaint(image(i));
  undo->undo(); undo->reset();

  putDrawings(3,1); row(0); enumValue(tool,"Paint:",3);
  mouseStroke(tool,{-15,0},{30,0}); row(2);
  app->getCurrentScene()->setDirtyFlag(false);
  acknowledgedWarning([&] { mouseStroke(tool,{-15,0},{30,0}); });
  for (int i = 0; i < 3; ++i)
    require(image(i)->getStrokeCount() == 2, "invalid Paint Inside range was only partially canceled");
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "invalid Paint Inside range changed undo or scene state");

  // Fill-only palette styles are accepted by the real input handler.
  putDrawings(3); row(0); enumValue(tool,"Range:",0);
  app->getPaletteController()->getCurrentLevelPalette()->setStyleIndex(gradient);
  mouseStroke(tool,{-15,0},{30,0});
  require(image(0)->getStrokeCount() == 4 && undo->getHistoryCount() == 1,
          "fill-capable gradient was silently rejected by Brush input");
  undo->undo(); undo->reset();

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
  mouseStroke(tool,{-15,0},{30,0});
  require(undo->getHistoryCount() == 1 && image(2)->getStrokeCount() == 4 &&
              image(0)->getStrokeCount() == 3 && image(4)->getStrokeCount() == 3,
          "Guided painting duplicated the current stroke or repainted guide endpoints");
  for (int i : {1,2,3}) checkPaint(image(i));
  for (int repeat = 0; repeat < 2; ++repeat) {
    undo->undo();
    for (int i : {1,2,3}) require(image(i)->getStrokeCount() == 2,
                                "Guided undo changed the wrong drawing");
    require(image(0)->getStrokeCount() == 3 && image(4)->getStrokeCount() == 3,
            "Guided undo removed an existing guide");
    undo->redo(); for (int i : {1,2,3}) checkPaint(image(i));
  }
  undo->undo(); undo->reset();
  // Exposure order can run opposite to drawing-number order. Endpoint flags
  // must follow the caller's guides when the interpolation range is sorted.
  for (int i = 0; i < 5; ++i)
    app->getCurrentXsheet()->getXsheet()->setCell(i,0,TXshCell(level,fids[4-i]));
  row(2); mouseStroke(tool,{-15,0},{30,0});
  require(image(0)->getStrokeCount() == 3 && image(4)->getStrokeCount() == 3 &&
              image(2)->getStrokeCount() == 4 && undo->getHistoryCount() == 1,
          "reversed guide drawing numbers changed endpoint inclusion");
  for (int i : {1,2,3}) checkPaint(image(i));
  undo->undo(); undo->reset();
  for (int i = 0; i < 5; ++i)
    app->getCurrentXsheet()->getXsheet()->setCell(i,0,TXshCell(level,fids[i]));
  row(2);
  // A bad front guide must not leave a successfully generated back half.
  image(4)->getStroke(2)->setOpacity(.5);
  acknowledgedWarning([&] { mouseStroke(tool,{-15,0},{30,0}); });
  require(image(1)->getStrokeCount() == 2 && image(3)->getStrokeCount() == 2 &&
              image(2)->getStrokeCount() == 4 && undo->getHistoryCount() == 1,
          "unsupported guide appearance generated a partial range or a Normal fallback");
  undo->undo(); undo->reset();
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
  size->setValue({0.,0.}); tool->onPropertyChanged("Size");
  acknowledgedWarning([&] { require(!tool->preLeftButtonDown(), "zero-width paint fell back to Normal"); });
  require(image(2)->getStrokeCount() == 2 && undo->getHistoryCount() == 0,
          "unsupported Brush width changed a drawing");
  size->setValue({10.,10.}); tool->onPropertyChanged("Size");
  undo->reset();
}
