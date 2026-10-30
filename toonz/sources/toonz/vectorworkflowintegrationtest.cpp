#include "vectorworkflowintegrationtest.h"
#include "tapp.h"
#include "menubarcommandids.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "tools/strokeselection.h"
#include "tools/gradienthandles.h"
#include "tools/widthprofilemanager.h"
#include "filmstripselection.h"
#include "tproperty.h"
#include "tenv.h"
#include <QComboBox>
#include <QPushButton>
#include "toonzqt/menubarcommand.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "tvectorimage.h"
#include "tregion.h"
#include "tstroke.h"
#include "tpalette.h"
#include "tcolorstyles.h"
#include "tundo.h"
#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QTimer>
#include <QMessageBox>
#include <QAbstractButton>
#include <cmath>
#include <array>
#include <stdexcept>

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
void settle() { QCoreApplication::processEvents(); }
QDoubleSpinBox *opacityField() {
  for (QWidget *widget : QApplication::allWidgets())
    if (widget->objectName() == "selectionOpacity" && widget->isVisible())
      return qobject_cast<QDoubleSpinBox *>(widget);
  return nullptr;
}
TVectorImageP drawing(TPalette *palette, int style) {
  TVectorImageP image = new TVectorImage;
  image->setPalette(palette);
  std::vector<TThickPoint> rectangle = {
      {-20,-20,1}, {0,-20,1}, {20,-20,1}, {20,0,1}, {20,20,1},
      {0,20,1}, {-20,20,1}, {-20,0,1}, {-20,-20,1}};
  auto *stroke = new TStroke(rectangle);
  stroke->setSelfLoop(true); stroke->setStyle(1); stroke->setOpacity(.327);
  image->addStroke(stroke);
  std::vector<TThickPoint> line = {{-20,30,1}, {0,30,1}, {20,30,1}};
  auto *other = new TStroke(line); other->setOpacity(.7); other->setStyle(1);
  image->addStroke(other);
  image->findRegions();
  require(image->getRegion(TPointD(0,0)), "vector fixture region missing");
  image->getRegion(TPointD(0,0))->setStyle(style);
  return image;
}

void nestedGroupChecks(TFilmstripSelection &frames) {
  auto *app = TApp::instance();
  auto *level = app->getCurrentScene()->getScene()->createNewLevel(
      PLI_XSHLEVEL,L"NestedVectorWorkflowFixture")->getSimpleLevel();
  TPaletteP palette = new TPalette;
  level->setPalette(palette.getPointer());
  struct NestedDrawing {
    TVectorImageP before;
    std::array<int,5> ids;
  };
  auto make = [&](double offset, double peak) {
    NestedDrawing result;
    result.before = new TVectorImage;
    result.before->setPalette(palette.getPointer());
    const double x[] = {-70.,-45.,-10.,20.,60.};
    for (int i = 0; i < 5; ++i) {
      auto *stroke = new TStroke(std::vector<TThickPoint>{
          {x[i]+offset,i*15.,1.},{x[i]+offset+5.,i*15.,peak+i},
          {x[i]+offset+10.,i*15.,1.}});
      stroke->setStyle(1); result.ids[i] = stroke->getId();
      result.before->addStroke(stroke);
    }
    // Two individual strokes inside a nested group, plus a sibling in the
    // parent group and two independent strokes outside the parent.
    result.before->group(2,2);
    result.before->group(1,3);
    const int member = result.before->getStrokeIndexById(result.ids[2]);
    require(result.before->enterGroup(member) && result.before->enterGroup(member),
            "nested group fixture could not enter both ancestors");
    require(result.before->isInsideGroup() == 2, "nested group fixture depth");
    return result;
  };
  std::array<NestedDrawing,3> source{{make(0.,3.),make(15.,5.),make(30.,7.)}};
  for (int i = 0; i < 3; ++i) {
    level->setFrame(TFrameId(i+1),source[i].before);
    source[i].before = source[i].before->cloneWithGroupContext();
  }
  auto *xsheet = app->getCurrentXsheet()->getXsheet();
  xsheet->setCell(0,0,TXshCell(level,TFrameId(1)));
  xsheet->setCell(1,0,TXshCell(level,TFrameId(2)));
  xsheet->setCell(2,0,TXshCell(level,TFrameId(1))); // repeated exposure
  xsheet->setCell(3,0,TXshCell(level,TFrameId(3)));
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentColumn()->setColumnIndex(0);
  app->getCurrentFrame()->setFrame(0);
  app->getCurrentLevel()->setLevel(level);
  app->getCurrentTool()->setTool("T_Selection"); settle();
  auto *tool = app->getCurrentTool()->getTool();
  auto *mode = dynamic_cast<TEnumProperty *>(tool->getProperties(0)->getProperty("Mode:"));
  require(mode,"nested workflow Selection mode missing");
  frames.select(TFrameId(1)); frames.select(TFrameId(2));
  mode->setValue(L"Selected Frames"); tool->onPropertyChanged("Mode:"); settle();
  auto *undo = TUndoManager::manager(); undo->reset();
  auto checkContext = [&](int drawing, const TVectorImageP &image) {
    require(image && image->isInsideGroup() == 2,"snapshot lost nested entered group");
    const auto &reference = source[drawing];
    for (int i = 0; i < 5; ++i) {
      const int index = image->getStrokeIndexById(reference.ids[i]);
      const int original = reference.before->getStrokeIndexById(reference.ids[i]);
      require(index >= 0 && image->isEnteredGroupStroke(index) == (i == 2 || i == 3),
              "snapshot entered the wrong nested group");
      require(image->getGroupDepth(index) == reference.before->getGroupDepth(original),
              "snapshot lost nested group hierarchy");
      if (i != 2 && i != 3)
        require(*image->getStroke(index) == *reference.before->getStroke(original),
                "nested operation changed a sibling or outside group");
    }
  };
  auto sameGeometry = [](const TVectorImageP &image, const TVectorImageP &reference) {
    if (image->getStrokeCount() != reference->getStrokeCount()) return false;
    for (UINT i = 0; i < reference->getStrokeCount(); ++i) {
      const auto *stroke = reference->getStroke(i);
      const int index = image->getStrokeIndexById(stroke->getId());
      if (index < 0 || !(*image->getStroke(index) == *stroke)) return false;
    }
    return true;
  };
  auto *uniform = CommandManager::instance()->getAction(MI_WidthProfileUniform);
  require(uniform && uniform->isEnabled(),"nested width action disabled");
  uniform->trigger(); settle();
  require(undo->getHistoryCount() == 1,"nested selected-drawing width was not one undo");
  for (int cycle = 0; cycle < 2; ++cycle) {
    for (int i = 0; i < 3; ++i) {
      TVectorImageP image = level->getFrame(TFrameId(i+1),false);
      checkContext(i,image);
      for (int member : {2,3}) {
        const auto *stroke = image->getStroke(image->getStrokeIndexById(source[i].ids[member]));
        const double expected = i < 2 ? 3.+i*2.+member : 1.;
        require(stroke->getControlPoint(0).thick == expected,
                "nested width scope missed active group or changed unselected drawing");
      }
    }
    undo->undo(); settle();
    for (int i = 0; i < 3; ++i) {
      TVectorImageP image = level->getFrame(TFrameId(i+1),false);
      checkContext(i,image);
      require(sameGeometry(image,source[i].before),"nested width undo lost original drawing");
    }
    undo->redo(); settle();
  }
  undo->undo(); settle(); undo->reset();

  tool->setAlignMethod(VectorAlignment::SELECT_AREA);
  auto *align = CommandManager::instance()->getAction(MI_AlignLeft);
  require(align && align->isEnabled(),"nested alignment action disabled");
  align->trigger(); settle();
  require(undo->getHistoryCount() == 1,"nested alignment did not create one undo");
  for (int cycle = 0; cycle < 2; ++cycle) {
    for (int i = 0; i < 3; ++i) {
      TVectorImageP image = level->getFrame(TFrameId(i+1),false);
      checkContext(i,image);
      if (i < 2) {
        const auto *a = image->getStroke(image->getStrokeIndexById(source[i].ids[2]));
        const auto *b = image->getStroke(image->getStrokeIndexById(source[i].ids[3]));
        require(std::abs(a->getBBox().x0-b->getBBox().x0) < 1e-9,
                "alignment treated the entered group as one outer object");
      } else require(sameGeometry(image,source[i].before),"alignment changed unselected drawing");
    }
    undo->undo(); settle();
    for (int i = 0; i < 3; ++i) {
      TVectorImageP image = level->getFrame(TFrameId(i+1),false);
      checkContext(i,image);
      require(sameGeometry(image,source[i].before),"nested alignment undo lost geometry");
    }
    undo->redo(); settle();
  }
  undo->reset();
  mode->setValue(L"Standard"); tool->onPropertyChanged("Mode:");
  frames.selectNone();
  app->getCurrentScene()->setDirtyFlag(false);
}
}

void runVectorWorkflowNativeChecks() {
  auto *app = TApp::instance();
  auto *scene = app->getCurrentScene()->getScene();
  auto *level = scene->createNewLevel(PLI_XSHLEVEL, L"VectorWorkflowFixture")->getSimpleLevel();
  require(level, "native vector fixture level creation");
  TPaletteP palette = new TPalette;
  const int gradientId = palette->addStyle(TColorStyle::create(GradientHandles::LinearGradientTagId));
  palette->getPage(0)->addStyle(gradientId);
  level->setPalette(palette.getPointer());
  TVectorImageP first = drawing(palette.getPointer(), gradientId);
  TVectorImageP second = drawing(palette.getPointer(), gradientId);
  level->setFrame(TFrameId(1), first); level->setFrame(TFrameId(2), second);
  auto *xsheet = app->getCurrentXsheet()->getXsheet();
  xsheet->setCell(0, 0, TXshCell(level, TFrameId(1)));
  xsheet->setCell(1, 0, TXshCell(level, TFrameId(2)));
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentColumn()->setColumnIndex(0);
  app->getCurrentFrame()->setFrame(0);
  app->getCurrentTool()->setTool("T_Selection");
  settle();
  auto *tool = app->getCurrentTool()->getTool();
  auto *selection = dynamic_cast<StrokeSelection *>(tool->getSelection());
  require(selection, "native selection tool has no stroke selection");
  selection->selectNone(); selection->select(0, true); selection->makeCurrent();
  app->getCurrentTool()->notifyToolChanged(); settle();
  auto *field = opacityField();
  require(field && std::abs(field->value() - 32.7) < 1e-6, "single-stroke opacity readback");
  auto *undo = TUndoManager::manager();
  undo->reset(); app->getCurrentScene()->setDirtyFlag(false);
  selection->select(1, true); app->getCurrentTool()->notifyToolChanged(); settle();
  require(field->value() == -1 && field->specialValueText() == "Mixed", "mixed opacity readback");
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag() &&
              first->getStroke(0)->getOpacity() == .327 && first->getStroke(1)->getOpacity() == .7,
          "readback mutated drawing or undo");
  field->setValue(45.0);
  require(QMetaObject::invokeMethod(field, "editingFinished", Qt::DirectConnection), "native opacity edit signal");
  require(undo->getHistoryCount() == 1 && first->getStroke(0)->getOpacity() == .45 &&
              first->getStroke(1)->getOpacity() == .45, "native opacity apply");
  undo->undo();
  require(first->getStroke(0)->getOpacity() == .327 && first->getStroke(1)->getOpacity() == .7,
          "native opacity undo");
  undo->redo();
  require(first->getStroke(0)->getOpacity() == .45, "native opacity redo");

  // Invoke the real gradient picker and registered command twice. One region
  // in each drawing shares the style: no local-sharing heuristic can pass this.
  app->getCurrentTool()->setTool("T_GradientTransform"); settle();
  tool = app->getCurrentTool()->getTool();
  tool->rightButtonDown(TPointD(0,0), TMouseEvent());
  auto *unique = CommandManager::instance()->getAction(MI_MakeUniqueStyle);
  require(unique && unique->isEnabled(), "native unique-style action unavailable");
  undo->reset();
  unique->trigger(); settle();
  const int firstCopy = first->getRegion(TPointD(0,0))->getStyle();
  require(firstCopy != gradientId && second->getRegion(TPointD(0,0))->getStyle() == gradientId &&
              undo->getHistoryCount() == 1, "cross-drawing unique-style command");
  unique->trigger(); settle();
  const int secondCopy = first->getRegion(TPointD(0,0))->getStyle();
  require(secondCopy != firstCopy && undo->getHistoryCount() == 2, "repeat unique-style command");
  undo->undo(); require(first->getRegion(TPointD(0,0))->getStyle() == firstCopy, "repeat unique undo");
  undo->undo(); require(first->getRegion(TPointD(0,0))->getStyle() == gradientId, "unique undo");
  undo->redo(); require(first->getRegion(TPointD(0,0))->getStyle() == firstCopy, "unique redo");
  require(second->getRegion(TPointD(0,0))->getStyle() == gradientId, "other drawing changed");
  undo->reset();
  // The real animation-choice dialog must preserve the source keys and make
  // one independent animated gradient, with one undo for palette and region.
  first->getRegion(TPointD(0,0))->setStyle(gradientId);
  palette->getStyle(gradientId)->setMainColor(TPixel32::Red);
  palette->setKeyframe(gradientId, 0);
  palette->getStyle(gradientId)->setMainColor(TPixel32::Blue);
  palette->setKeyframe(gradientId, 10);
  palette->setFrame(5);
  tool->rightButtonDown(TPointD(0,0), TMouseEvent());
  QTimer::singleShot(0, [] {
    for (QWidget *widget : QApplication::topLevelWidgets())
      if (auto *question = qobject_cast<QMessageBox *>(widget))
        for (auto *button : question->buttons())
          if (button->text() == "Copy Animation") { button->click(); return; }
  });
  unique->trigger(); settle();
  const int animatedCopy = first->getRegion(TPointD(0,0))->getStyle();
  require(animatedCopy != gradientId && palette->getKeyframeCount(animatedCopy) == 2 &&
          palette->getKeyframeCount(gradientId) == 2 && undo->getHistoryCount() == 1,
          "native animated unique gradient did not copy keys atomically");
  for (int frame : {0, 3, 5, 10}) {
    palette->setFrame(frame);
    require(palette->getStyle(animatedCopy)->getMainColor() == palette->getStyle(gradientId)->getMainColor(),
            "native copied gradient animation differs");
  }
  undo->undo();
  require(first->getRegion(TPointD(0,0))->getStyle() == gradientId &&
          palette->getKeyframeCount(gradientId) == 2, "native animated unique undo affected source");
  undo->redo();
  require(first->getRegion(TPointD(0,0))->getStyle() == animatedCopy &&
          palette->getKeyframeCount(animatedCopy) == 2, "native animated unique redo lost keys");
  undo->reset();

  // Select two unique drawing IDs through the same Filmstrip selection API
  // that the native UI uses. A third drawing and repeated exposure stay intact.
  TVectorImageP third = drawing(palette.getPointer(), gradientId);
  level->setFrame(TFrameId(3), third);
  xsheet->setCell(2,0,TXshCell(level,TFrameId(1)));
  auto varyWidths = [](const TVectorImageP &image, double peak) {
    auto *stroke = image->getStroke(1);
    for (int i = 0; i < 3; ++i) {
      auto point = stroke->getControlPoint(i); point.thick = i == 1 ? peak : 1.;
      stroke->setControlPoint(i, point);
    }
    stroke->invalidate();
  };
  varyWidths(first, 3.); varyWidths(second, 5.); varyWidths(third, 7.);
  const TStroke originalFirst(*first->getStroke(1)), originalSecond(*second->getStroke(1));
  app->getCurrentTool()->setTool("T_Selection"); settle();
  tool = app->getCurrentTool()->getTool();
  TFilmstripSelection frames;
  frames.select(TFrameId(1)); frames.select(TFrameId(2));
  auto *mode = dynamic_cast<TEnumProperty *>(tool->getProperties(0)->getProperty("Mode:"));
  require(mode, "native Selection mode property missing");
  mode->setValue(L"Selected Frames"); tool->onPropertyChanged("Mode:"); settle();
  auto *uniform = CommandManager::instance()->getAction(MI_WidthProfileUniform);
  require(uniform && uniform->isEnabled(), "selected-drawing width action disabled");
  uniform->trigger(); settle();
  TVectorImageP afterFirst = level->getFrame(TFrameId(1), false);
  TVectorImageP afterSecond = level->getFrame(TFrameId(2), false);
  require(afterFirst->getStroke(1)->getControlPoint(0).thick == 3. &&
          afterSecond->getStroke(1)->getControlPoint(0).thick == 5. &&
          third->getStroke(1)->getControlPoint(0).thick == 1. &&
          undo->getHistoryCount() == 1, "selected-drawing width scope or one-step undo changed");
  require(afterFirst->getStroke(1)->getOpacity() == .45 &&
          afterSecond->getStroke(1)->getOpacity() == .7,
          "width profile changed stroke opacity");
  undo->undo(); settle();
  afterFirst = level->getFrame(TFrameId(1), false); afterSecond = level->getFrame(TFrameId(2), false);
  require(*afterFirst->getStroke(1) == originalFirst && *afterSecond->getStroke(1) == originalSecond,
          "selected-drawing width undo did not restore pressure");
  undo->reset();
  QString error;
  auto taper = WidthProfile::builtIn(WidthProfile::TAPER_BOTH);
  require(WidthProfile::writeLibrary((TEnv::getConfigDir() + "width_profiles.txt").getQString(), {taper}, error),
          "native profile fixture library setup failed");
  auto *saved = CommandManager::instance()->getAction(MI_ApplySavedWidthProfile);
  require(saved && saved->isEnabled(), "visual profile command disabled");
  auto configureManager = [](double strength, bool accept) {
    QTimer::singleShot(0, [strength, accept] {
      for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() != "widthProfileManager") continue;
        auto *dialog = qobject_cast<QDialog *>(widget);
        if (!accept) { dialog->reject(); return; }
        dialog->findChild<QComboBox *>("widthProfileBlendMode")->setCurrentIndex(1);
        dialog->findChild<QDoubleSpinBox *>("widthProfileStrength")->setValue(strength);
        for (auto *button : dialog->findChildren<QPushButton *>())
          if (button->text() == "Save and Apply") {
            if (button->isEnabled()) button->click(); else dialog->reject();
            return;
          }
      }
    });
  };
  app->getCurrentScene()->setDirtyFlag(false);
  configureManager(50., false); saved->trigger(); settle();
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "canceling visual width manager dirtied artwork");
  configureManager(0., true); saved->trigger(); settle();
  require(undo->getHistoryCount() == 0, "zero-strength width profile created undo");
  configureManager(50., true); saved->trigger(); settle();
  afterFirst = level->getFrame(TFrameId(1), false); afterSecond = level->getFrame(TFrameId(2), false);
  const double endpoint = WidthProfile::evaluate(taper, 0.);
  require(std::abs(afterFirst->getStroke(1)->getControlPoint(0).thick - (.5 + .5 * endpoint)) < 1e-9 &&
          std::abs(afterSecond->getStroke(1)->getControlPoint(0).thick - (.5 + .5 * endpoint)) < 1e-9 &&
          undo->getHistoryCount() == 1, "native width-pressure blend differs from preview contract");
  undo->undo(); settle(); undo->reset();
  tool->setAlignMethod(VectorAlignment::SELECT_AREA);
  auto *align = CommandManager::instance()->getAction(MI_AlignLeft);
  require(align && align->isEnabled(), "selected-drawing alignment action disabled");
  afterFirst = level->getFrame(TFrameId(1), false); afterSecond = level->getFrame(TFrameId(2), false);
  const TStroke beforeAlignFirst(*afterFirst->getStroke(1)), beforeAlignSecond(*afterSecond->getStroke(1));
  align->trigger(); settle();
  require(undo->getHistoryCount() == 1, "selected-drawing alignment was not a single undo");
  require(std::abs(afterFirst->getStroke(0)->getBBox().x0 - afterFirst->getStroke(1)->getBBox().x0) < 1e-9 &&
          std::abs(afterSecond->getStroke(0)->getBBox().x0 - afterSecond->getStroke(1)->getBBox().x0) < 1e-9,
          "alignment did not apply independently to both selected drawings");
  require(third->getStroke(1)->getControlPoint(0).thick == 1., "alignment touched unselected drawing");
  undo->undo(); settle();
  require(*afterFirst->getStroke(1) == beforeAlignFirst && *afterSecond->getStroke(1) == beforeAlignSecond,
          "multidrawing alignment undo lost geometry");
  undo->redo(); settle(); undo->reset();
  mode->setValue(L"Standard"); tool->onPropertyChanged("Mode:");
  frames.selectNone();
  app->getCurrentScene()->setDirtyFlag(false);
  nestedGroupChecks(frames);
}
