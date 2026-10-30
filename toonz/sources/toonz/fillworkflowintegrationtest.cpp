#include "fillworkflowintegrationtest.h"
#include "tapp.h"
#include "filmstrip.h"
#include "filmstripcommand.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "tools/strokeselection.h"
#include "toonzqt/selectioncommandids.h"
#include "toonzqt/menubarcommand.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/morphhints.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txsheethandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "tvectorimage.h"
#include "tregion.h"
#include "tstroke.h"
#include "tpalette.h"
#include "tproperty.h"
#include "tundo.h"
#include "tlevel_io.h"
#include <QApplication>
#include <QDialog>
#include <QComboBox>
#include <QKeyEvent>
#include <QPushButton>
#include <QTimer>
#include <QTableWidget>
#include <QLabel>
#include <QTableView>
#include <QTemporaryDir>
#include <QAction>
#include <stdexcept>
#include <memory>

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}

void setBool(TTool *tool, const char *name, bool value) {
  auto *property = dynamic_cast<TBoolProperty *>(tool->getProperties(0)->getProperty(std::string(name)));
  require(property, "fill workflow tool property missing");
  property->setValue(value);
  tool->onPropertyChanged(name);
}
bool sameGeometry(const TVectorImageP &a, const TVectorImageP &b) {
  if (a->getStrokeCount() != b->getStrokeCount()) return false;
  for (int i = 0; i < int(a->getStrokeCount()); ++i) {
    auto *x = a->getStroke(i), *y = b->getStroke(i);
    if (x->getControlPointCount() != y->getControlPointCount()) return false;
    for (int j = 0; j < x->getControlPointCount(); ++j)
      if (x->getControlPoint(j) != y->getControlPoint(j)) return false;
  }
  return true;
}
}

// Runs in the real application's native UI fixture, with its tool, level,
// scene, viewer and undo stack. This exercises the transaction, not a mock.
void runVectorFillNativeChecks() {
  auto *app = TApp::instance();
  auto *level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL, L"FillReviewFixture")->getSimpleLevel();
  TPaletteP palette = new TPalette;
  level->setPalette(palette.getPointer());
  TVectorImageP image = new TVectorImage;
  image->setPalette(palette.getPointer());
  auto *stroke = new TStroke(std::vector<TThickPoint>{
      {-20,-20,1}, {0,-20,1}, {20,-20,1}, {20,0,1}, {20,20,1},
      {0,20,1}, {-20,20,1}, {-20,0,1}, {-20,-20,1}});
  stroke->setStyle(1); stroke->setSelfLoop(true); image->addStroke(stroke);
  image->findRegions();
  require(image->getRegion(TPointD(0,0)), "fill fixture has no region");
  image->getRegion(TPointD(0,0))->setStyle(1);
  level->setFrame(TFrameId(1), image);
  app->getCurrentXsheet()->getXsheet()->setCell(0, 0, TXshCell(level, TFrameId(1)));
  app->getCurrentColumn()->setColumnIndex(0);
  app->getCurrentFrame()->setFrame(0);
  app->getCurrentLevel()->setLevel(level);
  app->getCurrentTool()->setTool("T_ControlPointEditor");
  QCoreApplication::processEvents();
  auto *tool = app->getCurrentTool()->getTool();
  setBool(tool, "Auto Select Drawing", false);
  setBool(tool, "Review Fills", true);
  auto *undo = TUndoManager::manager();
  undo->reset();
  TVectorImageP original = image->clone();
  TMouseEvent mouse;
  mouse.m_button = Qt::LeftButton; mouse.m_buttons = Qt::LeftButton;
  auto pick = [&]() {
    tool->mouseMove(TPointD(20,20), mouse);
    tool->leftButtonDown(TPointD(20,20), mouse);
    tool->leftButtonUp(TPointD(20,20), mouse);
    tool->leftButtonDown(TPointD(20,20), mouse);
  };
  auto edit = [&](bool accept, bool switchTool = false) {
    bool sawDialog = false, cleanDuringReview = false;
    app->getCurrentScene()->setDirtyFlag(false);
    pick();
    tool->leftButtonDrag(TPointD(25,25), mouse);
    QTimer response;
    response.setSingleShot(true);
    QObject::connect(&response, &QTimer::timeout, [&]() {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      sawDialog = dialog && dialog->objectName() == "vectorFillReview";
      cleanDuringReview = !app->getCurrentScene()->getDirtyFlag() &&
                          undo->getHistoryCount() == 0;
      if (sawDialog) {
        if (switchTool) {
          app->getCurrentTool()->setTool("T_Selection");
          if (dialog->isVisible()) dialog->reject();
          return;
        }
        auto *button = dialog->findChild<QPushButton *>(accept ? "vectorFillApply" : "vectorFillRestore");
        if (button) button->click(); else dialog->reject();
      } else if (dialog) dialog->reject();
    });
    response.start(0);
    tool->leftButtonUp(TPointD(25,25), mouse);
    require(sawDialog, "native control-point drag did not open fill review");
    require(cleanDuringReview, "review dirtied scene or recorded undo before Apply");
  };
  edit(false);
  require(sameGeometry(image, original) && image->getRegion(TPointD(0,0))->getStyle() == 1,
          "Restore Original failed to recover geometry and fills");
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "cancel changed history or dirty state");
  edit(true);
  require(undo->getHistoryCount() == 1 && !sameGeometry(image, original),
          "Apply did not record one changed drawing");
  TVectorImageP proposed = image->clone();
  undo->undo();
  require(sameGeometry(image, original) && image->getRegion(TPointD(0,0))->getStyle() == 1,
          "control-point undo lost geometry or fills");
  undo->redo();
  require(sameGeometry(image, proposed) && image->getRegion(TPointD(0,0))->getStyle() == 1,
          "control-point redo lost geometry or fills");
  undo->undo(); undo->reset();
  app->getCurrentScene()->setDirtyFlag(false);
  pick(); tool->leftButtonDrag(TPointD(25,25), mouse);
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  require(tool->keyDown(&escape), "Escape did not cancel pending control-point drag");
  require(sameGeometry(image, original) && undo->getHistoryCount() == 0 &&
              !app->getCurrentScene()->getDirtyFlag(), "interrupted drag was not restored");
  edit(false, true);
  require(sameGeometry(image, original) && undo->getHistoryCount() == 0 &&
              !app->getCurrentScene()->getDirtyFlag(), "tool switch during control-point review was not restored");

  // Free Deform and Envelope use the same staged multi-drawing transaction.
  // Exercise both through the actual Selection tool's corner handle.
  TVectorImageP second = original->clone();
  level->setFrame(TFrameId(2), second);
  app->getCurrentTool()->setTool("T_Selection");
  QCoreApplication::processEvents();
  tool = app->getCurrentTool()->getTool();
  auto *mode = dynamic_cast<TEnumProperty *>(tool->getProperties(0)->getProperty(std::string("Mode:")));
  require(mode, "native selection mode missing");
  mode->setValue(L"Whole Level"); tool->onPropertyChanged("Mode:");
  setBool(tool, "Review Fills", true);
  auto deform = [&](bool envelope, bool accept, bool switchTool = false) {
    setBool(tool, "Envelope", envelope);
    bool sawDialog = false, clean = false, stagedBoth = false;
    app->getCurrentScene()->setDirtyFlag(false);
    TMouseEvent cornerEvent = mouse;
    cornerEvent.m_modifiersMask = TMouseEvent::CTRL_KEY;
    const TPointD corner = image->getBBox().getP11();
    const TPointD moved = corner + TPointD(8, 5);
    tool->mouseMove(corner, cornerEvent);
    tool->leftButtonDown(corner, cornerEvent);
    tool->leftButtonDrag(moved, cornerEvent);
    QTimer response;
    response.setSingleShot(true);
    QObject::connect(&response, &QTimer::timeout, [&]() {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      sawDialog = dialog && dialog->objectName() == "vectorFillReview";
      if (!sawDialog) { if (dialog) dialog->reject(); return; }
      auto *frames = dialog->findChild<QComboBox *>("vectorFillFrame");
      stagedBoth = frames && frames->count() == 2 &&
                   !sameGeometry(image, original) && !sameGeometry(second, original);
      if (frames) frames->setCurrentIndex(1);
      clean = !app->getCurrentScene()->getDirtyFlag() && undo->getHistoryCount() == 0;
      if (switchTool) {
        app->getCurrentTool()->setTool("T_ControlPointEditor");
        if (dialog->isVisible()) dialog->reject();
        return;
      }
      auto *button = dialog->findChild<QPushButton *>(accept ? "vectorFillApply" : "vectorFillRestore");
      if (button) button->click(); else dialog->reject();
    });
    response.start(0);
    tool->leftButtonUp(moved, cornerEvent);
    require(sawDialog && stagedBoth, "deformation did not review both staged drawings");
    require(clean, "multi-drawing review dirtied history before Apply");
  };
  for (bool envelope : {false, true}) {
    deform(envelope, false);
    require(sameGeometry(image, original) && sameGeometry(second, original) &&
                undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
            "multi-drawing deformation cancel did not restore all drawings");
    require(image->getRegion(TPointD(0,0))->getStyle() == 1 &&
                second->getRegion(TPointD(0,0))->getStyle() == 1,
            "multi-drawing cancellation lost fills");
    deform(envelope, true);
    require(undo->getHistoryCount() == 1, "deformation did not create one undo transaction");
    TVectorImageP changedFirst = image->clone(), changedSecond = second->clone();
    undo->undo();
    require(sameGeometry(image, original) && sameGeometry(second, original),
            "multi-drawing deformation undo failed");
    undo->redo();
    require(sameGeometry(image, changedFirst) && sameGeometry(second, changedSecond) &&
                image->getRegion(TPointD(0,0))->getStyle() == 1 &&
                second->getRegion(TPointD(0,0))->getStyle() == 1,
            "multi-drawing deformation redo failed");
    undo->undo(); undo->reset();
  }
  deform(false, false, true);
  require(sameGeometry(image, original) && sameGeometry(second, original) &&
              undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "tool switch during multi-drawing review was not restored");
}

void runVectorMorphNativeChecks() {
  auto *app = TApp::instance();
  auto *level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL, L"MorphReviewFixture")->getSimpleLevel();
  TPaletteP palette = new TPalette;
  level->setPalette(palette.getPointer());
  auto make = [&](const std::vector<double> &offsets) {
    TVectorImageP image = new TVectorImage;
    image->setPalette(palette.getPointer());
    for (double x : offsets) {
      auto *stroke = new TStroke(std::vector<TThickPoint>{
          {x,0,2}, {x+5,10,2}, {x+10,0,2}});
      stroke->setStyle(1); image->addStroke(stroke);
    }
    return image;
  };
  TVectorImageP first = make({0,30,60}), last = make({10,50}), blank = make({});
  level->setFrame(TFrameId(1), first);
  level->setFrame(TFrameId(2), blank);
  level->setFrame(TFrameId(3), last);
  level->setFrame(TFrameId(4), first->clone());
  QJsonObject pairHint{{"kind", "pair"}, {"firstFrame", MorphHints::frame(TFrameId(1))},
      {"lastFrame", MorphHints::frame(TFrameId(3))},
      {"first", MorphHints::anchor(first,2)}, {"last", MorphHints::anchor(last,0)}};
  QJsonObject zoneHint{{"kind", "zone"}, {"firstFrame", MorphHints::frame(TFrameId(1))},
      {"lastFrame", MorphHints::frame(TFrameId(3))},
      {"first", MorphHints::anchor(first,0)}, {"last", MorphHints::anchor(last,0)},
      {"firstMembers", QJsonArray{MorphHints::anchor(first,0),MorphHints::anchor(first,2)}},
      {"lastMembers", QJsonArray{MorphHints::anchor(last,0)}},
      {"firstRect", MorphHints::rect(first->getBBox())}, {"lastRect", MorphHints::rect(last->getBBox())}};
  level->setMorphHints(MorphHints::encode({pairHint,zoneHint}));
  require(StrokeSelection::getInbetweenStrokePairs(level,TFrameId(1),TFrameId(3)) ==
              TInbetween::StrokeCorrespondence{{2,0}}, "saved native stroke pairing did not resolve");
  require(StrokeSelection::getInbetweenStrokePairs(level,TFrameId(4),TFrameId(3)).empty(),
          "copied drawing inherited another drawing's saved pairing");
  require(StrokeSelection::getInbetweenZoneHints(level,TFrameId(1),TFrameId(3)).size() == 1,
          "saved native zone did not resolve");
  std::unique_ptr<TStroke> removed(new TStroke(*first->getStroke(2)));
  first->removeStroke(2,false);
  require(StrokeSelection::getInbetweenStrokePairs(level,TFrameId(1),TFrameId(3)).empty() &&
              StrokeSelection::getInbetweenZoneHints(level,TFrameId(1),TFrameId(3)).empty(),
          "deleted member left a stale pair or zone active");
  first->addStroke(removed.release());
  require(StrokeSelection::getInbetweenStrokePairs(level,TFrameId(3),TFrameId(1)) ==
              TInbetween::StrokeCorrespondence{{0,2}}, "reverse saved pairing did not follow drawings");
  app->getCurrentXsheet()->getXsheet()->setCell(0, 0, TXshCell(level, TFrameId(1)));
  app->getCurrentLevel()->setLevel(level);
  app->getCurrentFrame()->setFrame(0);
  auto *undo = TUndoManager::manager(); undo->reset();
  app->getCurrentScene()->setDirtyFlag(false);
  const TInbetween::StrokeCorrespondence pairs{{2,0}};
  InbetweenDialog dialog(nullptr);
  dialog.setStrokePairCount(1); dialog.setZoneHintCount(0);
  dialog.setAutoMatch(false); dialog.setUnmatchedPolicy(TInbetween::HoldUnmatched);
  dialog.setPreviewSource(first, last, pairs, {});
  bool reviewed = false, immutable = false, provenance = false;
  QTimer check, timeout;
  check.setInterval(30); timeout.setSingleShot(true);
  QObject::connect(&check, &QTimer::timeout, [&]() {
    auto *table = dialog.findChild<QTableWidget *>("morphCorrespondence");
    auto *status = dialog.findChild<QLabel *>("morphReviewStatus");
    if (!table || table->rowCount() != 3 || !status || !table->property("previewReady").toBool()) return;
    reviewed = true;
    bool manual = false, disappearing = false;
    for (int i = 0; i < table->rowCount(); ++i) {
      const int source = table->item(i,0)->data(Qt::UserRole).toInt();
      const int target = table->item(i,1)->data(Qt::UserRole).toInt();
      if (source == 2 && target == 0)
        manual = table->item(i,2)->data(Qt::UserRole).toInt() == TInbetween::ManualMatch;
      if (target < 0) disappearing = true;
    }
    provenance = manual && disappearing;
    table->selectRow(0); // Native source-highlight path.
    immutable = !app->getCurrentScene()->getDirtyFlag() && undo->getHistoryCount() == 0 &&
                TVectorImageP(level->getFrame(TFrameId(2), false))->getStrokeCount() == 0;
    dialog.reject();
  });
  QObject::connect(&timeout, &QTimer::timeout, &dialog, &QDialog::reject);
  check.start(); timeout.start(5000); dialog.exec(); check.stop(); timeout.stop();
  require(reviewed && provenance, "native morph correspondence review did not resolve manual/unmatched rows");
  require(immutable && !app->getCurrentScene()->getDirtyFlag(), "morph preview edited the document");
  FilmstripCmd::InbetweenOptions options;
  options.m_unmatchedPolicy = dialog.unmatchedPolicy();
  TInbetween::MatchOptions matchOptions;
  matchOptions.m_unmatchedPolicy = TInbetween::HoldUnmatched;
  TInbetween expected(first, last, pairs, matchOptions);
  TVectorImageP expectedMiddle = expected.tween(.5);
  FilmstripCmd::inbetween(level, TFrameId(1), TFrameId(3), FilmstripCmd::II_Linear, pairs, options);
  require(undo->getHistoryCount() == 1 &&
              sameGeometry(level->getFrame(TFrameId(2), false), expectedMiddle),
          "native morph command differs from reviewed resolver or undo transaction");
  require(TVectorImageP(level->getFrame(TFrameId(1), false)).getPointer() == first.getPointer() &&
              TVectorImageP(level->getFrame(TFrameId(3), false)).getPointer() == last.getPointer(),
          "native morph command replaced key drawings");
  undo->undo();
  require(TVectorImageP(level->getFrame(TFrameId(2), false))->getStrokeCount() == 0,
          "native morph undo did not restore intermediate drawing");
  undo->redo();
  require(sameGeometry(level->getFrame(TFrameId(2), false), expectedMiddle),
          "native morph redo does not retain unmatched policy");
  undo->reset();

  // Lettered drawing IDs must bound both generation and redo exactly.
  const TFrameId a(10,"A"), b(10,"B"), c(10,"C"), d(10,"D"), e(10,"E");
  TVectorImageP suffixFirst = first->clone(), suffixLast = last->clone(), outside = first->clone();
  level->setFrame(a,suffixFirst); level->setFrame(b,make({})); level->setFrame(c,make({}));
  level->setFrame(d,suffixLast); level->setFrame(e,outside);
  FilmstripCmd::inbetween(level,a,d,FilmstripCmd::II_Linear,pairs,options);
  require(undo->getHistoryCount() == 1, "lettered morph did not create one transaction");
  undo->undo(); undo->redo();
  require(TVectorImageP(level->getFrame(d,false)) == suffixLast &&
              TVectorImageP(level->getFrame(e,false)) == outside,
          "lettered morph redo changed an endpoint or outside drawing");
  undo->reset();

  // Each endpoint is legal, but their union exceeds the native knot limit.
  // Failure must leave the level and undo stack untouched.
  StrokeOpacity::Profile denseFirst{{0.,1.}}, denseLast{{0.,1.}};
  const int half = int(StrokeOpacity::MaxKnots / 2);
  for (int i = 1; i < half; ++i) {
    denseFirst.emplace_back(double(i) / half, .5);
    denseLast.emplace_back((i - .5) / half, .75);
  }
  denseFirst.emplace_back(1.,1.); denseLast.emplace_back(1.,1.);
  // Add one distinct interior knot on each side so the union is too large.
  denseFirst.insert(denseFirst.begin()+1,TPointD(.125 / half,.5));
  denseLast.insert(denseLast.begin()+1,TPointD(.25 / half,.75));
  require(first->getStroke(2)->setOpacityProfile(denseFirst) &&
              last->getStroke(0)->setOpacityProfile(denseLast), "dense morph fixture invalid");
  const TVectorImageP previousMiddle = level->getFrame(TFrameId(2),false);
  app->getCurrentScene()->setDirtyFlag(false);
  bool rejected = false;
  try { FilmstripCmd::inbetween(level,TFrameId(1),TFrameId(3),FilmstripCmd::II_Linear,pairs,options); }
  catch (...) { rejected = true; }
  require(rejected && TVectorImageP(level->getFrame(TFrameId(2),false)) == previousMiddle &&
              undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "failed morph changed artwork, history or dirty state");
  first->getStroke(2)->setOpacityProfile({}); last->getStroke(0)->setOpacityProfile({});
}

void runVectorOpacityProfileNativeChecks() {
  auto *app = TApp::instance();
  auto *level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL, L"OpacityEnvelopeFixture")->getSimpleLevel();
  TPaletteP palette = new TPalette;
  level->setPalette(palette.getPointer());
  TVectorImageP image = new TVectorImage; image->setPalette(palette.getPointer());
  const StrokeOpacity::Profile firstProfile{{0.,.2},{1.,.8}};
  const StrokeOpacity::Profile secondProfile{{0.,1.},{1.,.4}};
  for (int i = 0; i < 2; ++i) {
    auto *stroke = new TStroke(std::vector<TThickPoint>{
        {0.,i*20.,2.},{25.,i*20.+10.,2.},{50.,i*20.,2.}});
    stroke->setStyle(1); stroke->setOpacity(i ? .8 : .3);
    stroke->setOpacityProfile(i ? secondProfile : firstProfile); image->addStroke(stroke);
  }
  level->setFrame(TFrameId(1), image);
  app->getCurrentXsheet()->getXsheet()->setCell(0,0,TXshCell(level,TFrameId(1)));
  app->getCurrentFrame()->setFrame(0); app->getCurrentColumn()->setColumnIndex(0);
  app->getCurrentLevel()->setLevel(level); app->getCurrentTool()->setTool("T_Selection");
  QCoreApplication::processEvents();
  auto *tool = app->getCurrentTool()->getTool();
  auto *mode = dynamic_cast<TEnumProperty *>(tool->getProperties(0)->getProperty(std::string("Mode:")));
  require(mode, "opacity editor selection mode unavailable");
  mode->setValue(L"Standard"); tool->onPropertyChanged("Mode:");
  auto *selection = dynamic_cast<StrokeSelection *>(tool->getSelection());
  require(selection, "opacity editor native selection unavailable");
  selection->selectNone(); selection->select(0,true); selection->select(1,true); selection->makeCurrent();
  auto *action = CommandManager::instance()->getAction(MI_EditStrokeOpacityProfile);
  require(action && action->isEnabled(), "opacity envelope command not registered and enabled");
  auto *undo = TUndoManager::manager(); undo->reset();
  auto interact = [&](bool apply) {
    bool saw = false, mixedGuard = false, clean = false, validStep = false, recoveredEndpoint = false;
    app->getCurrentScene()->setDirtyFlag(false);
    QTimer response; response.setSingleShot(true);
    QObject::connect(&response,&QTimer::timeout,[&]() {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      saw = dialog && dialog->objectName() == "strokeOpacityEditor";
      if (!saw) { if (dialog) dialog->reject(); return; }
      auto *applyButton = dialog->findChild<QPushButton *>("strokeOpacityApply");
      auto *fade = dialog->findChild<QPushButton *>("strokeOpacityFadeOut");
      auto *add = dialog->findChild<QPushButton *>("strokeOpacityAdd");
      auto *step = dialog->findChild<QPushButton *>("strokeOpacityStep");
      auto *remove = dialog->findChild<QPushButton *>("strokeOpacityRemove");
      auto *table = dialog->findChild<QTableView *>("strokeOpacityKnots");
      if (!applyButton || !fade || !add || !step || !remove || !table) { dialog->reject(); return; }
      mixedGuard = !applyButton->isEnabled();
      fade->click(); table->selectRow(1); remove->click();
      recoveredEndpoint = table->model()->rowCount() == 1 && !applyButton->isEnabled();
      add->click();
      recoveredEndpoint &= table->model()->rowCount() == 2 && applyButton->isEnabled();
      fade->click(); table->selectRow(0); add->click();
      table->selectRow(1); step->click();
      auto *model = table->model();
      validStep = model->rowCount() == 4 &&
          model->setData(model->index(2,1), .25, Qt::EditRole) && applyButton->isEnabled();
      clean = !app->getCurrentScene()->getDirtyFlag() && undo->getHistoryCount() == 0 &&
          image->getStroke(0)->getOpacityProfile() == firstProfile &&
          image->getStroke(1)->getOpacityProfile() == secondProfile;
      if (apply && validStep) applyButton->click(); else dialog->reject();
    });
    response.start(0); action->trigger();
    require(saw && mixedGuard && validStep && recoveredEndpoint,
            "native opacity editor failed mixed-profile, endpoint recovery or exact-step workflow");
    require(clean, "opacity editor preview changed document/history");
  };
  interact(false);
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag() &&
      image->getStroke(0)->getOpacityProfile() == firstProfile &&
      image->getStroke(1)->getOpacityProfile() == secondProfile, "opacity editor Cancel changed artwork");
  interact(true);
  const StrokeOpacity::Profile expected{{0.,1.},{.5,.5},{.5,.25},{1.,0.}};
  require(undo->getHistoryCount() == 1 && image->getStroke(0)->getOpacityProfile() == expected &&
              image->getStroke(1)->getOpacityProfile() == expected,
          "opacity editor did not apply one profile transaction to both selected strokes");
  require(image->getStroke(0)->getOpacity() == .3 && image->getStroke(1)->getOpacity() == .8,
          "opacity envelope edit replaced per-stroke overall opacity");
  undo->undo();
  require(image->getStroke(0)->getOpacityProfile() == firstProfile &&
              image->getStroke(1)->getOpacityProfile() == secondProfile, "opacity profile undo lost mixed originals");
  undo->redo();
  require(image->getStroke(0)->getOpacityProfile() == expected &&
              image->getStroke(1)->getOpacityProfile() == expected, "opacity profile redo lost step knots");
  QTemporaryDir dir; require(dir.isValid(), "opacity workflow reload directory");
  const TFilePath path(dir.filePath("edited.pli"));
  { TLevelWriterP writer(path); writer->getFrameWriter(TFrameId(1))->save(image); }
  TLevelReaderP reader(path); reader->loadInfo();
  TVectorImageP reopened = reader->getFrameReader(TFrameId(1))->load();
  require(reopened && reopened->getStroke(0)->getOpacityProfile() == expected &&
              reopened->getStroke(1)->getOpacityProfile() == expected,
          "native opacity editor result lost exact step knots on PLI reopen");
  undo->reset();
}
