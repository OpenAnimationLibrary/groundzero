#include "strokeopacityeditintegrationtest.h"
#include "tapp.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "tools/strokeselection.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/selectioncommandids.h"
#include "toonzqt/tselectionhandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/levelproperties.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "tvectorimage.h"
#include "tstroke.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tlevel_io.h"
#include "tlevel.h"
#include "tproperty.h"
#include "tundo.h"
#include <QApplication>
#include <QDialog>
#include <QAction>
#include <QEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
bool approximatelyEqual(double a, double b) { return std::abs(a - b) < 1e-7; }
TStroke *line(double from, double to, double y = 0.) {
  auto *stroke = new TStroke(std::vector<TThickPoint>{
      {from,y,2.}, {(from+to)*.5,y,2.}, {to,y,2.}});
  stroke->setStyle(1);
  return stroke;
}
TVectorImageP imageWith(TStroke *stroke, TPalette *palette) {
  TVectorImageP image = new TVectorImage;
  image->setPalette(palette);
  image->addStroke(stroke);
  return image;
}
bool sameDrawing(const TVectorImageP &a, const TVectorImageP &b, bool compareIds = true) {
  if (a->getStrokeCount() != b->getStrokeCount()) return false;
  for (int i = 0; i < a->getStrokeCount(); ++i) {
    const TStroke *x = a->getStroke(i), *y = b->getStroke(i);
    if ((compareIds && x->getId() != y->getId()) || x->getStyle() != y->getStyle() ||
        x->isSelfLoop() != y->isSelfLoop() || x->getOpacity() != y->getOpacity() ||
        x->getOpacityProfile() != y->getOpacityProfile() ||
        x->getControlPointCount() != y->getControlPointCount()) return false;
    for (int p = 0; p < x->getControlPointCount(); ++p)
      if (x->getControlPoint(p) != y->getControlPoint(p)) return false;
  }
  return true;
}
StrokeOpacity::Profile denseProfile(bool concentrated = false) {
  StrokeOpacity::Profile profile;
  for (size_t i = 0; i < StrokeOpacity::MaxKnots; ++i)
    profile.emplace_back(i == 0 ? 0. : i + 1 == StrokeOpacity::MaxKnots ? 1. :
        concentrated ? .2 + .6 * double(i - 1) / double(StrokeOpacity::MaxKnots - 3) :
        double(i) / double(StrokeOpacity::MaxKnots - 1), (i & 1) ? .8 : .2);
  return profile;
}
TStroke *closedStroke() {
  auto *stroke = new TStroke(std::vector<TThickPoint>{
      {0,0,2}, {50,0,2}, {100,0,2}, {100,50,2}, {100,100,2},
      {50,100,2}, {0,100,2}, {0,50,2}, {0,0,2}});
  stroke->setStyle(1);
  stroke->setSelfLoop();
  return stroke;
}
void nativeImageEdits(TPalette *palette) {
  auto *source = line(0, 100);
  source->setOpacity(.5);
  source->setOpacityProfile({{0.,0.},{1.,1.}});
  TVectorImageP split = imageWith(source, palette);
  split->splitStroke(0, {{0., .25}, {.75, 1.}});
  require(split->getStrokeCount() == 2, "native interval split stroke count");
  require(approximatelyEqual(split->getStroke(0)->getOpacityAtLength(12.5), .0625) &&
          approximatelyEqual(split->getStroke(1)->getOpacityAtLength(12.5), .4375),
          "native split restarted opacity instead of retaining original arc position");

  auto *stepped = line(0, 100);
  stepped->setOpacityProfile({{0.,.1},{0.,.4},{.5,.6},{1.,.8},{1.,.2}});
  TStroke reversed(*stepped);
  reversed.changeDirection();
  require(approximatelyEqual(reversed.getOpacityAtLength(0), .2) &&
          approximatelyEqual(reversed.getOpacityAtLength(reversed.getLength()), .1),
          "reversing an endpoint step changed visible cap opacity");
  TVectorImageP endpoints = imageWith(stepped, palette);
  endpoints->splitStroke(0, {{0.,.25},{.75,1.}});
  const auto &head = endpoints->getStroke(0)->getOpacityProfile();
  const auto &tail = endpoints->getStroke(1)->getOpacityProfile();
  require(head.size() >= 3 && head[0] == TPointD(0.,.1) && head[1] == TPointD(0.,.4) &&
          tail.size() >= 3 && tail[tail.size()-2] == TPointD(1.,.8) && tail.back() == TPointD(1.,.2) &&
          approximatelyEqual(endpoints->getStroke(0)->getOpacityAtLength(0), .1) &&
          approximatelyEqual(endpoints->getStroke(1)->getOpacityAtLength(endpoints->getStroke(1)->getLength()), .2),
          "native split lost a retained endpoint opacity step or evaluated the wrong side");

  for (bool smooth : {false, true}) for (bool reverseFirst : {false, true})
    for (bool reverseLast : {false, true}) {
      auto *a = line(reverseFirst ? 10 : 0, reverseFirst ? 0 : 10);
      auto *b = line(reverseLast ? 20 : 10, reverseLast ? 10 : 20);
      a->setOpacity(.4); b->setOpacity(.8);
      a->setOpacityProfile({{0.,.1},{1.,.9}});
      b->setOpacityProfile({{0.,.2},{1.,.7}});
      TVectorImageP joined = imageWith(a, palette); joined->addStroke(b);
      joined->joinStroke(0, 1, reverseFirst ? 0 : 2, reverseLast ? 2 : 0, smooth);
      require(joined->getStrokeCount() == 1, "native Tape join count");
      const TStroke *result = joined->getStroke(0);
      require(approximatelyEqual(result->getOpacityAtLength(0), .4 * (reverseFirst ? .9 : .1)) &&
              approximatelyEqual(result->getOpacityAtLength(result->getLength()), .8 * (reverseLast ? .2 : .7)),
              "native Tape lost a scalar or reversed envelope endpoint");
      require(approximatelyEqual(result->getOpacityAtLength(result->getLength() * .25), .2) &&
              approximatelyEqual(result->getOpacityAtLength(result->getLength() * .75), .36),
              "native Tape did not retain both opacity interiors");
    }

  // Capacity failures occur on valid drawings and must not alter live geometry,
  // IDs or the original envelope, including smoothing's endpoint adjustments.
  const auto dense = denseProfile();
  for (bool smooth : {false, true}) {
    TVectorImageP image = imageWith(line(0, 10), palette);
    image->addStroke(line(20, 30));
    image->getStroke(0)->setOpacityProfile(dense);
    image->getStroke(1)->setOpacityProfile(dense);
    TVectorImageP before = image->clone();
    bool rejected = false;
    try { image->joinStroke(0, 1, 2, 0, smooth); }
    catch (const std::length_error &) { rejected = true; }
    require(rejected && sameDrawing(image, before), "rejected native Tape join changed live drawing");
  }
  TVectorImageP closed = imageWith(closedStroke(), palette);
  closed->getStroke(0)->setOpacityProfile(dense);
  TVectorImageP before = closed->clone();
  bool rejected = false;
  try { closed->splitStroke(0, {{0., .21345}, {.21345, 1.}}); }
  catch (const std::length_error &) { rejected = true; }
  require(rejected && sameDrawing(closed, before), "rejected native closed cut changed live drawing");
}
void enumProperty(TTool *tool, const char *name, const std::wstring &value) {
  auto *property = dynamic_cast<TEnumProperty *>(tool->getProperties(0)->getProperty(name));
  require(property != nullptr, "native stroke tool property missing");
  property->setValue(value);
  tool->onPropertyChanged(name);
}
void boolProperty(TTool *tool, const char *name, bool value) {
  auto *property = dynamic_cast<TBoolProperty *>(tool->getProperties(0)->getProperty(name));
  require(property != nullptr, "native stroke tool boolean property missing");
  property->setValue(value); tool->onPropertyChanged(name);
}

void opacityReviewGuards() {
  auto *app = TApp::instance();
  auto *level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL, L"OpacityReviewGuardFixture")->getSimpleLevel();
  TPaletteP palette = new TPalette;
  level->setPalette(palette.getPointer());
  TVectorImageP image = imageWith(line(0, 100), palette.getPointer());
  image->addStroke(line(0, 100, 20));
  const StrokeOpacity::Profile original{{0., .2}, {1., .8}};
  const StrokeOpacity::Profile intervening{{0., .7}, {.5, .1}, {1., .9}};
  const StrokeOpacity::Profile proposed{{0., 1.}, {1., 0.}};
  image->getStroke(0)->setOpacityProfile(original);
  image->getStroke(1)->setOpacityProfile(original);
  level->setFrame(TFrameId(1), image);
  level->setFrame(TFrameId(2), image->clone());
  auto *xsheet = app->getCurrentXsheet()->getXsheet();
  xsheet->setCell(0, 0, TXshCell(level, TFrameId(1)));
  xsheet->setCell(1, 0, TXshCell(level, TFrameId(2)));
  app->getCurrentColumn()->setColumnIndex(0);
  app->getCurrentFrame()->setFrame(0);
  app->getCurrentLevel()->setLevel(level);
  app->getCurrentTool()->setTool("T_Selection");
  app->getCurrentXsheet()->notifyXsheetChanged();
  QCoreApplication::processEvents();
  auto *tool = app->getCurrentTool()->getTool();
  enumProperty(tool, "Mode:", L"Standard");
  tool->onImageChanged();
  auto *selection = dynamic_cast<StrokeSelection *>(tool->getSelection());
  require(selection && selection->getImage() == image,
          "opacity review guard fixture did not select its native drawing");
  auto selectSource = [&]() {
    selection->selectNone(); selection->select(0, true); selection->makeCurrent();
    app->getCurrentSelection()->notifySelectionChanged();
  };
  selectSource();
  auto *action = CommandManager::instance()->getAction(MI_EditStrokeOpacityProfile);
  require(action && action->isEnabled(), "opacity review guard command unavailable");
  auto *undo = TUndoManager::manager();
  undo->reset();

  // A queued Apply must not regain authority after a context switch returns to
  // exactly the same pointers, nor overwrite a same-image script edit.
  const char *cases[] = {"envelope", "selected geometry", "unselected geometry",
                         "palette", "frame leave/return", "selection leave/return"};
  for (int scenario = 0; scenario < 6; ++scenario) {
    selectSource();
    app->getCurrentScene()->setDirtyFlag(false);
    level->getProperties()->setDirtyFlag(false);
    bool saw = false, accepted = false, restoredContext = false, timedOut = false;
    TVectorImageP expected;
    TPixel32 expectedColor;
    bool expectedDirty = false;
    QTimer response, deadline;
    response.setSingleShot(true); deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, [&]() {
      timedOut = true;
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
    });
    QObject::connect(&response, &QTimer::timeout, [&]() {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      saw = dialog && dialog->objectName() == "strokeOpacityEditor";
      if (!saw) { if (dialog) dialog->reject(); return; }
      auto *fade = dialog->findChild<QPushButton *>("strokeOpacityFadeOut");
      auto *apply = dialog->findChild<QPushButton *>("strokeOpacityApply");
      if (!fade || !apply) { dialog->reject(); return; }
      fade->click();
      QMetaObject::invokeMethod(apply, "click", Qt::QueuedConnection);
      if (scenario == 0) image->getStroke(0)->setOpacityProfile(intervening);
      else if (scenario == 1) image->getStroke(0)->transform(TTranslation(3., 4.));
      else if (scenario == 2) image->getStroke(1)->transform(TTranslation(-2., 5.));
      else if (scenario == 3) palette->getStyle(1)->setMainColor(TPixel32(80, 160, 220));
      else if (scenario == 4) {
        app->getCurrentFrame()->setFrame(1); tool->onImageChanged();
        app->getCurrentFrame()->setFrame(0); tool->onImageChanged();
        selectSource();
      } else {
        selection->selectNone(); selection->select(1, true);
        app->getCurrentSelection()->notifySelectionChanged();
        selectSource();
      }
      expectedDirty = scenario < 4;
      if (expectedDirty) {
        level->setDirtyFlag(true);
        app->getCurrentScene()->setDirtyFlag(true);
      }
      expected = image->clone();
      expectedColor = palette->getStyle(1)->getMainColor();
      restoredContext = selection->getImage() == image &&
          selection->getSelection() == std::set<int>{0} &&
          app->getCurrentFrame()->getFrame() == 0;
      // Deliver the already queued button activation in this modal event turn,
      // after reject/return, so the test really requires the invalidation latch.
      QCoreApplication::sendPostedEvents(apply, QEvent::MetaCall);
      accepted = dialog->result() == QDialog::Accepted;
    });
    deadline.start(10000); response.start(0); action->trigger(); deadline.stop();
    if (!saw || !accepted || !restoredContext || timedOut || !expected ||
        TVectorImageP(level->getFrame(TFrameId(1), false)) != image ||
        !sameDrawing(image, expected) || palette->getStyle(1)->getMainColor() != expectedColor ||
        undo->getHistoryCount() != 0 ||
        app->getCurrentScene()->getDirtyFlag() != expectedDirty ||
        level->getProperties()->getDirtyFlag() != expectedDirty)
      throw std::runtime_error(std::string("opacity stale review failed to preserve ") + cases[scenario]);
  }

  // Cancellation and a newly opened review still work after each rejected
  // attempt; the accepted edit retains the intervening source as its undo base.
  TVectorImageP before = image->clone();
  auto finish = [&](bool apply) {
    bool saw = false;
    QTimer response; response.setSingleShot(true);
    QObject::connect(&response, &QTimer::timeout, [&]() {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      saw = dialog && dialog->objectName() == "strokeOpacityEditor";
      if (!saw) { if (dialog) dialog->reject(); return; }
      auto *fade = dialog->findChild<QPushButton *>("strokeOpacityFadeOut");
      auto *button = dialog->findChild<QPushButton *>(apply ? "strokeOpacityApply" : "strokeOpacityCancel");
      if (!fade || !button) { saw = false; dialog->reject(); return; }
      fade->click(); button->click();
    });
    response.start(0); action->trigger();
    require(saw, "opacity review could not be reopened after a stale attempt");
  };
  finish(false);
  require(sameDrawing(image, before) && undo->getHistoryCount() == 0,
          "opacity Cancel after a stale review changed source or history");
  finish(true);
  require(undo->getHistoryCount() == 1 && image->getStroke(0)->getOpacityProfile() == proposed &&
          image->getStroke(1)->getOpacityProfile() == original,
          "fresh opacity review did not apply only the reviewed selection");
  for (int repeat = 0; repeat < 3; ++repeat) {
    undo->undo(); require(sameDrawing(image, before), "opacity undo lost intervening source edits");
    undo->redo(); require(image->getStroke(0)->getOpacityProfile() == proposed,
                          "opacity redo lost the reviewed envelope");
  }
  QTemporaryDir directory;
  require(directory.isValid(), "opacity stale-review reload directory unavailable");
  const TFilePath path(directory.filePath("reviewed.pli"));
  { TLevelWriterP writer(path); writer->getFrameWriter(TFrameId(1))->save(image); }
  TLevelReaderP reader(path);
  TLevelP reopenedLevel = reader->loadInfo();
  require(reopenedLevel && reopenedLevel->getPalette(),
          "opacity review reopen did not load its native palette");
  TVectorImageP reopened = reader->getFrameReader(TFrameId(1))->load();
  require(reopened, "opacity review reopen did not load its native drawing");
  reopened->setPalette(reopenedLevel->getPalette());
  // Native PLI reconstructs runtime stroke IDs; geometry and authored opacity
  // must persist, while live undo checks above still require the original IDs.
  require(reopened && sameDrawing(image, reopened, false) &&
          reopened->getPalette()->getStyle(1)->getMainColor() == palette->getStyle(1)->getMainColor(),
          "opacity review reopen lost the accepted envelope or intervening source edits");
  undo->reset();
}
}

void runStrokeOpacityEditNativeChecks() {
  auto *app = TApp::instance();
  TPaletteP palette = new TPalette;
  nativeImageEdits(palette.getPointer());
  auto *level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL, L"StrokeOpacityEditFixture")->getSimpleLevel();
  level->setPalette(palette.getPointer());
  auto *source = line(0, 100);
  source->setOpacity(.5);
  const StrokeOpacity::Profile profile{{0.,0.},{.4,.8},{1.,1.}};
  source->setOpacityProfile(profile);
  TVectorImageP image = imageWith(source, palette.getPointer());
  level->setFrame(TFrameId(1), image);
  app->getCurrentXsheet()->getXsheet()->setCell(0, 0, TXshCell(level, TFrameId(1)));
  app->getCurrentColumn()->setColumnIndex(0);
  app->getCurrentFrame()->setFrame(0);
  app->getCurrentLevel()->setLevel(level);
  app->getCurrentXsheet()->notifyXsheetChanged();
  auto *undo = TUndoManager::manager();
  undo->reset();
  TMouseEvent mouse; mouse.m_button = Qt::LeftButton; mouse.m_buttons = Qt::LeftButton;

  app->getCurrentTool()->setTool("T_Cutter");
  QCoreApplication::processEvents();
  auto *tool = app->getCurrentTool()->getTool();
  boolProperty(tool, "Snap At Intersection", false);
  tool->leftButtonDown(TPointD(25,0), mouse);
  tool->leftButtonUp(TPointD(25,0), mouse);
  require(image->getStrokeCount() == 2 && undo->getHistoryCount() == 1,
          "actual Cutter gesture did not create one undoable split");
  require(approximatelyEqual(image->getStroke(0)->getOpacityAtLength(12.5), .125) &&
          approximatelyEqual(image->getStroke(1)->getOpacityAtLength(25.), 5. / 12.),
          "actual Cutter did not retain opacity at original positions");
  undo->undo();
  require(image->getStrokeCount() == 1 && image->getStroke(0)->getOpacityProfile() == profile,
          "Cutter undo lost original opacity envelope");
  undo->redo(); require(image->getStrokeCount() == 2, "Cutter redo lost split");
  undo->undo(); undo->reset();

  app->getCurrentTool()->setTool("T_Pump");
  QCoreApplication::processEvents();
  tool = app->getCurrentTool()->getTool();
  enumProperty(tool, "Mode:", L"Pump");
  tool->onEnter();
  const double thickness = image->getStroke(0)->getMaxThickness();
  tool->mouseMove(TPointD(50,0), mouse);
  tool->leftButtonDown(TPointD(50,0), mouse);
  tool->leftButtonDrag(TPointD(50,20), mouse);
  tool->leftButtonUp(TPointD(50,20), mouse);
  require(undo->getHistoryCount() == 1 && image->getStroke(0)->getMaxThickness() > thickness,
          "actual Pump gesture did not change width");
  require(image->getStroke(0)->getOpacityProfile() == profile && image->getStroke(0)->getOpacity() == .5,
          "Pump width edit replaced full opacity by a temporary slice");
  undo->undo(); require(image->getStroke(0)->getOpacityProfile() == profile, "Pump opacity undo");
  undo->redo(); require(image->getStroke(0)->getOpacityProfile() == profile, "Pump opacity redo");
  undo->undo();
  undo->reset();

  app->getCurrentTool()->setTool("T_Eraser");
  QCoreApplication::processEvents();
  tool = app->getCurrentTool()->getTool();
  tool->onEnter();
  enumProperty(tool, "Type:", L"Normal");
  boolProperty(tool, "Selective", false);
  boolProperty(tool, "Invert", false);
  boolProperty(tool, "Frame Range", false);
  auto *size = dynamic_cast<TDoubleProperty *>(tool->getProperties(0)->getProperty("Size:"));
  require(size, "native Eraser size property missing");
  size->setValue(10.); tool->onPropertyChanged("Size:");
  tool->leftButtonDown(TPointD(0,0), mouse);
  tool->leftButtonUp(TPointD(0,0), mouse);
  require(image->getStrokeCount() == 1 && undo->getHistoryCount() == 1,
          "actual Eraser endpoint gesture did not produce one edit");
  const double remainingStart = image->getStroke(0)->getControlPoint(0).x;
  require(remainingStart > 0. && remainingStart < 40. &&
          approximatelyEqual(image->getStroke(0)->getOpacityAtLength(0), remainingStart * .01) &&
          approximatelyEqual(image->getStroke(0)->getOpacityAtLength(image->getStroke(0)->getLength()), .5),
          "one-intersection Eraser retained the unsliced opacity envelope");
  undo->undo(); require(image->getStroke(0)->getOpacityProfile() == profile, "Eraser opacity undo");
  undo->redo(); require(image->getStroke(0)->getControlPoint(0).x == remainingStart, "Eraser redo geometry");
  undo->undo(); undo->reset();

  TVectorImageP taped = imageWith(line(0,40), palette.getPointer());
  taped->addStroke(line(60,100));
  taped->getStroke(0)->setOpacity(.2); taped->getStroke(1)->setOpacity(.8);
  taped->getStroke(0)->setOpacityProfile({{0.,0.},{1.,1.}});
  taped->getStroke(1)->setOpacityProfile({{0.,1.},{1.,0.}});
  TVectorImageP untaped = taped->clone();
  level->setFrame(TFrameId(1), taped);
  app->getCurrentTool()->setTool("T_Tape");
  tool = app->getCurrentTool()->getTool();
  enumProperty(tool, "Type", L"Normal");
  enumProperty(tool, "Mode", L"Endpoint to Endpoint");
  boolProperty(tool, "JoinStrokes", true); boolProperty(tool, "Smooth", false);
  tool->mouseMove(TPointD(40,0), mouse);
  tool->leftButtonDown(TPointD(40,0), mouse);
  tool->leftButtonDrag(TPointD(60,0), mouse);
  tool->leftButtonUp(TPointD(60,0), mouse);
  require(taped->getStrokeCount() == 1 && undo->getHistoryCount() == 1 &&
          approximatelyEqual(taped->getStroke(0)->getOpacityAtLength(20), .1) &&
          approximatelyEqual(taped->getStroke(0)->getOpacityAtLength(50), .5) &&
          approximatelyEqual(taped->getStroke(0)->getOpacityAtLength(80), .4),
          "actual Tape gesture lost source opacity or connecting-segment interpolation");
  undo->undo(); require(sameDrawing(taped, untaped), "actual Tape undo lost original strokes");
  undo->redo(); require(taped->getStrokeCount() == 1, "actual Tape redo lost join");
  undo->undo(); undo->reset();

  TVectorImageP closed = imageWith(closedStroke(), palette.getPointer());
  closed->getStroke(0)->setOpacityProfile(denseProfile());
  TVectorImageP before = closed->clone();
  level->setFrame(TFrameId(1), closed);
  app->getCurrentTool()->setTool("T_Cutter");
  tool = app->getCurrentTool()->getTool();
  app->getCurrentScene()->setDirtyFlag(false);
  bool sawWarning = false;
  QTimer warningResponse;
  warningResponse.setSingleShot(true);
  QObject::connect(&warningResponse, &QTimer::timeout, [&] {
    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
      sawWarning = true;
      dialog->accept();
    }
  });
  warningResponse.start(0);
  tool->leftButtonDown(TPointD(21.345,0), mouse);
  require(sawWarning && sameDrawing(closed, before) && undo->getHistoryCount() == 0 &&
          !app->getCurrentScene()->getDirtyFlag(),
          "rejected actual Cutter gesture changed artwork, dirty state or undo history");

  // The first stroke is fully covered, while the second would need too many
  // envelope knots after a closed-curve erase. Reject the entire brush sample.
  TVectorImageP erasing = imageWith(line(9,11), palette.getPointer());
  auto *loop = closedStroke(); loop->setOpacityProfile(denseProfile(true));
  erasing->addStroke(loop);
  before = erasing->clone();
  level->setFrame(TFrameId(1), erasing);
  app->getCurrentTool()->setTool("T_Eraser");
  tool = app->getCurrentTool()->getTool(); tool->onEnter();
  enumProperty(tool, "Type:", L"Normal");
  app->getCurrentScene()->setDirtyFlag(false);
  sawWarning = false;
  warningResponse.start(0);
  tool->leftButtonDown(TPointD(10,0), mouse);
  tool->leftButtonUp(TPointD(10,0), mouse);
  require(sawWarning && sameDrawing(erasing, before) && undo->getHistoryCount() == 0 &&
          !app->getCurrentScene()->getDirtyFlag(),
          "rejected actual Eraser sample partly deleted strokes or registered empty undo");
  undo->reset();
  app->getCurrentTool()->setTool("T_Selection");
  opacityReviewGuards();
}
