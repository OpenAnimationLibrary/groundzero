#include "vectorworkflowintegrationtest.h"
#include "vectormodaltest.h"
#include "tapp.h"
#include "menubarcommandids.h"
#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "tools/strokeselection.h"
#include "tools/gradienthandles.h"
#include "tools/widthprofilemanager.h"
#include "tools/vectoralignmentpreview.h"
#include "tools/vectorimagesnapshot.h"
#include "toonzqt/tselectionhandle.h"
#include "iocommand.h"
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
#include "toonz/imagemanager.h"
#include "toonz/levelproperties.h"
#include "toonz/sceneresources.h"
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
#include "tlevel_io.h"
#include "tlevel.h"
#include "tstream.h"
#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QTimer>
#include <QMessageBox>
#include <QAbstractButton>
#include <QTemporaryDir>
#include <QFile>
#include <QPointer>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <cmath>
#include <array>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
void settle() { QCoreApplication::processEvents(); }
QByteArray readBytes(const QString &path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "read native profile library");
  return file.readAll();
}
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

struct AlignmentObservation {
  TVectorImageP before, after;
  TRectD bounds;
  QSize size;
  QImage pixels;
};
int pixelDifference(const QImage &a, const QImage &b) {
  require(!a.isNull() && a.size() == b.size(), "alignment preview capture size differs");
  const auto x = a.convertToFormat(QImage::Format_RGBA8888);
  const auto y = b.convertToFormat(QImage::Format_RGBA8888);
  int result = 0;
  for (int row = 0; row < x.height(); ++row)
    for (int col = 0; col < x.width() * 4; ++col)
      result = std::max(result, std::abs(int(x.constScanLine(row)[col]) -
                                       int(y.constScanLine(row)[col])));
  return result;
}
std::vector<AlignmentObservation> reviewAlignment(
    QAction *action, bool accept, int drawingCount,
    const std::function<void()> &whileOpen = {}, bool expectSourceInvalidation = false,
    bool expectOwnerCancellation = false) {
  bool visited = false, sourceWarning = false, expired = false;
  std::exception_ptr failure;
  std::vector<AlignmentObservation> result;
  static int nextReview = 0;
  const int review = ++nextReview;
  QString phase = "trigger";
  const auto trace = [&](const QString &next) {
    phase = next;
    std::cout << "vector-alignment-native: review=" << review
              << " action=" << action->text().toStdString()
              << " drawings=" << drawingCount << " accept=" << accept
              << " phase=" << phase.toStdString() << std::endl;
  };
  const auto modalDescription = [] {
    QWidget *modal = QApplication::activeModalWidget();
    if (!modal) return QString("none");
    QString labels;
    for (auto *label : modal->findChildren<QLabel *>()) {
      labels += label->text() + ' ';
      if (labels.size() > 400) break;
    }
    return QString("class=%1 name=%2 visible=%3 title=%4 text=%5")
        .arg(modal->metaObject()->className()).arg(modal->objectName().left(100))
        .arg(modal->isVisible()).arg(modal->windowTitle().left(120)).arg(labels.left(400));
  };
  QTimer inspect, deadline;
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, [&] {
    expired = true;
    std::cerr << "vector-alignment-native: review=" << review << " deadline phase="
              << phase.toStdString() << " modal=" << modalDescription().toStdString() << std::endl;
    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
  });
  QObject::connect(&inspect, &QTimer::timeout, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (visited) {
      if (!dialog || dialog->objectName() == "vectorAlignmentPreview") return;
      QString text;
      for (auto *label : dialog->findChildren<QLabel *>()) text += label->text();
      sourceWarning = text.contains("reviewed drawing or selection changed");
      trace("source warning: " + modalDescription());
      if (!expectSourceInvalidation || !sourceWarning)
        failure = std::make_exception_ptr(std::runtime_error(
            "alignment opened an unexpected dialog: " + text.toStdString()));
      dialog->reject();
      return;
    }
    if (!dialog || dialog->objectName() != "vectorAlignmentPreview") {
      failure = std::make_exception_ptr(std::runtime_error("alignment preview did not open"));
      if (dialog) dialog->reject();
      return;
    }
    visited = true;
    trace("preview received");
    try {
      auto *frames = dialog->findChild<QComboBox *>("vectorAlignmentDrawing");
      auto *before = dynamic_cast<VectorAlignmentPreview::Swatch *>(
          dialog->findChild<QWidget *>("vectorAlignmentBefore"));
      auto *after = dynamic_cast<VectorAlignmentPreview::Swatch *>(
          dialog->findChild<QWidget *>("vectorAlignmentAfter"));
      require(frames && frames->count() == drawingCount && before && after,
              "alignment preview missed a changed drawing or native swatch");
      for (int i = 0; i < drawingCount; ++i) {
        trace(QString("drawing %1 select").arg(i));
        frames->setCurrentIndex(i);
        // Let normal show/resize and drawing-change events initialize each
        // PlaneViewer before inspecting its rendered framebuffer.
        trace(QString("drawing %1 settle").arg(i));
        settle();
        trace(QString("drawing %1 capture original").arg(i));
        const QImage original = before->grabFramebuffer();
        trace(QString("drawing %1 capture proposed").arg(i));
        const QImage proposed = after->grabFramebuffer();
        require(before->bounds() == after->bounds(), "alignment preview uses independent fit scales");
        const int difference = pixelDifference(original, proposed);
        if (difference <= 2) {
          std::ostringstream detail;
          detail << "alignment preview did not visibly display changed positions: drawing="
              << frames->itemText(i).toStdString() << " delta=" << difference
              << " pixels=" << original.width() << 'x' << original.height();
          for (UINT index = 0; index < std::min<UINT>(4,before->image()->getStrokeCount()); ++index) {
            const auto *a = before->image()->getStroke(index), *b = after->image()->getStroke(index);
            detail << " stroke" << index << " x0=" << a->getBBox().x0 << ':' << b->getBBox().x0
                << " opacity=" << a->getOpacity() << " knots=" << a->getOpacityProfile().size();
          }
          throw std::runtime_error(detail.str());
        }
        result.push_back({before->image()->cloneWithGroupContext(),
                          after->image()->cloneWithGroupContext(), after->bounds(),
                          after->size(), proposed});
        trace(QString("drawing %1 comparison passed").arg(i));
      }
      trace("intervening callback");
      if (whileOpen) whileOpen();
      auto *button = dialog->findChild<QPushButton *>(accept ? "vectorAlignmentApply" :
                                                              "vectorAlignmentCancel");
      require(button && (button->isEnabled() || expectOwnerCancellation), "alignment preview action unavailable");
      trace("action click");
      if (expectOwnerCancellation) {
        bool delivered = false;
        require(!dialog->parentWidget() && !button->isEnabled(),
                "owner-cancelled alignment still has a parent or actionable Apply");
        require(QMetaObject::invokeMethod(button,[&] { delivered = true; button->click(); },Qt::QueuedConnection),
                "owner-cancelled alignment could not queue its actual Apply");
        QCoreApplication::sendPostedEvents(button,QEvent::MetaCall);
        require(delivered && dialog->result() == QDialog::Rejected,
                "queued Apply revived owner-cancelled alignment");
      } else button->click();
      trace("action click returned");
    } catch (...) {
      failure = std::current_exception();
      dialog->reject();
    }
  });
  trace("trigger");
  inspect.start(0);
  deadline.start(10000);
  action->trigger();
  inspect.stop();
  deadline.stop();
  trace("command returned");
  if (failure) std::rethrow_exception(failure);
  require(!expired, "alignment native modal workflow exceeded its 10-second deadline");
  require(visited, "alignment command bypassed explicit review");
  require(!expectSourceInvalidation || sourceWarning,
          "alignment failed to explain that a reviewed source changed");
  return result;
}
void checkAlignmentGeometry(const TVectorImageP &live, const AlignmentObservation &review) {
  require(live->getStrokeCount() == review.after->getStrokeCount(), "alignment changed stroke count");
  for (UINT i = 0; i < live->getStrokeCount(); ++i) {
    const auto *a = live->getStroke(i), *b = review.after->getStroke(i);
    require(*a == *b && a->getStyle() == b->getStyle() &&
            a->getOpacity() == b->getOpacity() && a->getOpacityProfile() == b->getOpacityProfile(),
            "alignment applied a different drawing from the preview");
  }
}
void checkAlignmentOutput(const TVectorImageP &live, const AlignmentObservation &review) {
  checkAlignmentGeometry(live, review);
  VectorAlignmentPreview::Swatch swatch(nullptr);
  swatch.resize(review.size);
  swatch.setImage(live, review.bounds);
  swatch.show(); settle();
  require(pixelDifference(swatch.grabFramebuffer(), review.pixels) <= 2,
          "applied alignment pixels differ from the reviewed native output");
}

void reviewWidthManager(QAction *action, double strength, bool accept,
                        const std::function<void()> &whileOpen = {},
                        bool expectSourceWarning = false) {
  bool visited = false, warned = false;
  std::exception_ptr failure;
  QTimer inspect;
  QObject::connect(&inspect, &QTimer::timeout, [&] {
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (visited) {
      if (!dialog || dialog->objectName() == "widthProfileManager") return;
      QString text;
      for (auto *label : dialog->findChildren<QLabel *>()) text += label->text();
      warned = text.contains("editing context changed");
      if (!expectSourceWarning || !warned)
        failure = std::make_exception_ptr(std::runtime_error(
            "width manager opened an unexpected dialog: " + text.toStdString()));
      dialog->reject();
      return;
    }
    if (!dialog || dialog->objectName() != "widthProfileManager") {
      failure = std::make_exception_ptr(std::runtime_error("width manager did not open"));
      if (dialog) dialog->reject();
      return;
    }
    visited = true;
    try {
      dialog->findChild<QComboBox *>("widthProfileBlendMode")->setCurrentIndex(1);
      dialog->findChild<QDoubleSpinBox *>("widthProfileStrength")->setValue(strength);
      QPushButton *apply = nullptr;
      for (auto *button : dialog->findChildren<QPushButton *>())
        if (button->text() == "Save and Apply") apply = button;
      require(apply && apply->isEnabled(), "width manager Apply unavailable");
      if (whileOpen) whileOpen();
      // Deliberately deliver the already-queued click even if the callback
      // rejected the dialog through a context signal, or switched away/back.
      if (accept) {
        bool delivered = false;
        require(QMetaObject::invokeMethod(apply,[&] { delivered = true; apply->click(); },Qt::QueuedConnection),
                "width manager could not queue the Apply click");
        QCoreApplication::sendPostedEvents(apply,QEvent::MetaCall);
        require(delivered && dialog->result() == (dialog->isEnabled() ? QDialog::Accepted : QDialog::Rejected),
                "width manager queued Apply did not respect review cancellation");
      } else dialog->reject();
    } catch (...) {
      failure = std::current_exception();
      dialog->reject();
    }
  });
  inspect.start(0);
  action->trigger();
  inspect.stop();
  if (failure) std::rethrow_exception(failure);
  require(visited, "width profile action bypassed its manager");
  require(warned == expectSourceWarning, "width manager source invalidation was not reported");
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
  reviewAlignment(align, true, 2); settle();
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
  palette->getStyle(gradientId)->setGlobalName(L"fixture-gradient-source");
  palette->getStyle(gradientId)->setOriginalName(L"Linked gradient");
  palette->getStyle(gradientId)->setIsEditedFlag(true);
  palette->getStyle(gradientId)->setMainColor(TPixel32::Red);
  palette->setKeyframe(gradientId, 0);
  palette->getStyle(gradientId)->setMainColor(TPixel32::Blue);
  palette->setKeyframe(gradientId, 10);
  palette->setFrame(5);
  // Establish an actually saved, clean drawing. A test that starts with a
  // modified image cannot detect the canceled dialog's old writable readback.
  QTemporaryDir gradientStorage;
  require(gradientStorage.isValid(), "gradient persistence fixture directory unavailable");
  const TFilePath gradientPath(gradientStorage.filePath("gradient-choices.pli"));
  level->setPath(gradientPath,true);
  level->setDirtyFlag(true); palette->setDirtyFlag(true); level->save();
  palette->setFrame(5); // Preview an interpolated appearance, not an endpoint.
  auto *images = ImageManager::instance();
  const auto firstId = level->getImageId(TFrameId(1)), secondId = level->getImageId(TFrameId(2));
  require(!images->isModified(firstId) && !images->isModified(secondId),
          "saved gradient fixture did not establish clean native image state");
  app->getCurrentScene()->setDirtyFlag(false);
  auto paletteBytes = [&] {
    const QString path = gradientStorage.filePath("palette-snapshot.tpl");
    { TOStream output{TFilePath(path)}; palette->saveData(output); }
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "native gradient palette snapshot unavailable");
    return file.readAll();
  };
  auto chooseUnique = [&](const QString &choice, const std::function<void()> &whileOpen = {},
                          bool ownerCancelled = false) {
    bool visited = false, expired = false;
    std::exception_ptr failure;
    QTimer drive, deadline; drive.setInterval(20); deadline.setSingleShot(true);
    QObject::connect(&drive,&QTimer::timeout,[&] {
      if (visited) return;
      auto *question = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
      if (!question) return;
      visited = true;
      try {
        // QMessageBox intentionally ignores window titles on macOS. Identify
        // this exact native question and its choices independently of chrome.
        auto *cancel = question->button(QMessageBox::Cancel);
        QAbstractButton *animation = nullptr, *appearance = nullptr;
        for (auto *button : question->buttons()) {
          if (button->text() == QObject::tr("Copy Animation") &&
              question->buttonRole(button) == QMessageBox::YesRole) animation = button;
          if (button->text() == QObject::tr("Current Appearance") &&
              question->buttonRole(button) == QMessageBox::NoRole) appearance = button;
        }
        const bool expected = question->icon() == QMessageBox::Question &&
            question->text() == QObject::tr(
                "This gradient is animated. Copy its animation or only its "
                "appearance at the current frame?") &&
            question->buttons().size() == 3 && animation && appearance && cancel &&
            question->buttonRole(cancel) == QMessageBox::RejectRole;
        if (!expected)
          throw std::runtime_error(QString(
              "Unexpected native gradient dialog: icon=%1 title='%2' text='%3' buttons=%4")
              .arg(int(question->icon())).arg(question->windowTitle().left(120))
              .arg(question->text().left(240)).arg(question->buttons().size()).toStdString());
        if (whileOpen) whileOpen();
        QAbstractButton *selected = choice == "Cancel" ? cancel :
            choice == "Copy Animation" ? animation :
            choice == "Current Appearance" ? appearance : nullptr;
        require(selected && (selected->isEnabled() || ownerCancelled), "native gradient choice unavailable");
        // Deliver outside the timer callback, including any nested UI work.
        if (ownerCancelled) {
          bool delivered = false;
          require(!question->parentWidget() && !selected->isEnabled(),
                  "owner-cancelled gradient retained an actionable choice");
          require(QMetaObject::invokeMethod(selected,[&] { delivered = true; selected->click(); },Qt::QueuedConnection),
                  "owner-cancelled gradient could not queue its actual choice");
          QCoreApplication::sendPostedEvents(selected,QEvent::MetaCall);
          require(delivered && !question->clickedButton() && question->result() == QDialog::Rejected,
                  "queued gradient choice revived an owner-cancelled operation");
        } else require(QMetaObject::invokeMethod(selected,"click",Qt::QueuedConnection),
                       "native gradient choice could not queue its actual button");
      } catch (...) { failure = std::current_exception(); question->reject(); }
    });
    QObject::connect(&deadline,&QTimer::timeout,[&] {
      expired = true;
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
    });
    drive.start(); deadline.start(5000); unique->trigger(); drive.stop(); deadline.stop();
    if (failure) std::rethrow_exception(failure);
    require(visited && !expired,"native gradient choice workflow did not complete");
  };
  tool->rightButtonDown(TPointD(0,0), TMouseEvent());
  const auto unchangedPalette = paletteBytes();
  const int unchangedFrame = palette->getFrame();
  const bool cachedFirst = images->isCached(firstId), cachedSecond = images->isCached(secondId);
  const bool paletteDirty = palette->getDirtyFlag(), levelDirty = level->getProperties()->getDirtyFlag();
  for (int repeat = 0; repeat < 4; ++repeat) {
    QPointer<QWidget> previous = QApplication::activeWindow();
    std::unique_ptr<QWidget> owner;
    if (repeat >= 2) {
      owner.reset(new QWidget);
      VectorModalTest::activateOwner(*owner, "gradient");
      chooseUnique(repeat == 2 ? "Copy Animation" : "Current Appearance", [&] {
        require(VectorModalTest::hasReviewOwner(QApplication::activeModalWidget(), owner.get()),
                "gradient review did not capture its actual native owner");
        if (repeat == 2) { owner->hide(); owner->show(); } else owner.reset();
      },true);
    } else chooseUnique("Cancel");
    settle();
    require(!images->isModified(firstId) && !images->isModified(secondId) &&
                images->isCached(firstId) == cachedFirst && images->isCached(secondId) == cachedSecond &&
                TVectorImageP(level->getFrame(TFrameId(1),false)) == first &&
                TVectorImageP(level->getFrame(TFrameId(2),false)) == second &&
                first->getRegion(TPointD(0,0))->getStyle() == gradientId &&
                second->getRegion(TPointD(0,0))->getStyle() == gradientId &&
                palette->getFrame() == unchangedFrame && paletteBytes() == unchangedPalette &&
                palette->getDirtyFlag() == paletteDirty && level->getProperties()->getDirtyFlag() == levelDirty &&
                undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
            "canceling animated Make Unique changed image/cache/palette/frame/history state");
    owner.reset(); if (previous) QApplication::setActiveWindow(previous);
  }
  const TPaletteP originalAnimation = palette->clone();
  const std::unique_ptr<TColorStyle> currentAppearance(palette->getStyle(gradientId)->clone());
  auto checkCopy = [&](TPalette *candidate, int id, bool animated) {
    require(candidate->getKeyframeCount(gradientId) == 2 &&
                candidate->getKeyframe(gradientId,0) == 0 && candidate->getKeyframe(gradientId,1) == 10 &&
                candidate->getKeyframeCount(id) == (animated ? 2 : 0) &&
                candidate->getStyle(id)->getGlobalName().empty() &&
                candidate->getStyle(id)->getOriginalName().empty(),
            "unique gradient lost exact keys or retained a shared style link");
    if (animated) require(candidate->getKeyframe(id,0) == 0 && candidate->getKeyframe(id,1) == 10,
                          "copied gradient changed native keyframe positions");
    TPaletteP sampled = candidate->clone(), reference = originalAnimation->clone();
    for (int frame = 0; frame <= 10; ++frame) {
      sampled->setFrame(frame); reference->setFrame(frame);
      require(*sampled->getStyle(gradientId) == *reference->getStyle(gradientId),
              "Make Unique changed source gradient animation");
      std::unique_ptr<TColorStyle> expected((animated ? reference->getStyle(gradientId)
                                                    : currentAppearance.get())->clone());
      expected->setName(sampled->getStyle(id)->getName());
      expected->setGlobalName(L""); expected->setOriginalName(L""); expected->setIsEditedFlag(false);
      require(*sampled->getStyle(id) == *expected,
              "unique gradient did not retain the chosen animation/current appearance");
    }
  };
  auto reopenCopy = [&](int id, bool animated) {
    const int frame = palette->getFrame();
    level->save();
    palette->setFrame(frame);
    TLevelReaderP reader{gradientPath};
    const TLevelP saved = reader->loadInfo();
    require(saved && saved->getPalette(),"saved unique gradient palette unavailable");
    checkCopy(saved->getPalette(),id,animated);
    const TVectorImageP a = reader->getFrameReader(TFrameId(1))->load();
    const TVectorImageP b = reader->getFrameReader(TFrameId(2))->load();
    require(a && b && a->getRegion(TPointD(0,0))->getStyle() == id &&
                b->getRegion(TPointD(0,0))->getStyle() == gradientId,
            "unique gradient reload changed current/shared drawing ownership");
  };
  chooseUnique("Current Appearance"); settle();
  const int appearanceCopy = first->getRegion(TPointD(0,0))->getStyle();
  require(appearanceCopy != gradientId && undo->getHistoryCount() == 1 &&
              second->getRegion(TPointD(0,0))->getStyle() == gradientId,
          "current-appearance gradient copy did not retain other drawing identity");
  checkCopy(palette.getPointer(),appearanceCopy,false); reopenCopy(appearanceCopy,false);
  undo->undo();
  require(first->getRegion(TPointD(0,0))->getStyle() == gradientId,
          "current-appearance gradient undo lost original region style");
  undo->redo(); checkCopy(palette.getPointer(),appearanceCopy,false);
  undo->undo(); undo->reset();
  tool->rightButtonDown(TPointD(0,0), TMouseEvent());
  int queuedStyle = -1;
  chooseUnique("Copy Animation", [&] {
    // A queued same-palette edit changes both the append ID and insertion
    // position while the question is open. The accepted copy must re-resolve.
    queuedStyle = palette->addStyle(TPixel32(12,34,56));
    auto *page = palette->getStylePage(gradientId);
    page->insertStyle(page->search(gradientId),queuedStyle);
  }); settle();
  const int animatedCopy = first->getRegion(TPointD(0,0))->getStyle();
  require(animatedCopy != gradientId && palette->getKeyframeCount(animatedCopy) == 2 &&
          palette->getKeyframeCount(gradientId) == 2 && undo->getHistoryCount() == 1,
          "native animated unique gradient did not copy keys atomically");
  auto *copiedPage = palette->getStylePage(gradientId);
  require(animatedCopy > queuedStyle && copiedPage->getStyleId(copiedPage->search(gradientId)+1) == animatedCopy &&
              palette->getStyle(queuedStyle)->getMainColor() == TPixel32(12,34,56),
          "accepted Make Unique used stale palette insertion state");
  for (int frame : {0, 3, 5, 10}) {
    palette->setFrame(frame);
    require(palette->getStyle(animatedCopy)->getMainColor() == palette->getStyle(gradientId)->getMainColor(),
            "native copied gradient animation differs");
  }
  undo->undo();
  require(first->getRegion(TPointD(0,0))->getStyle() == gradientId &&
          palette->getKeyframeCount(gradientId) == 2 && palette->getStylePage(queuedStyle) &&
          palette->getStyle(queuedStyle)->getMainColor() == TPixel32(12,34,56),
          "native animated unique undo affected source or earlier palette edit");
  undo->redo();
  require(first->getRegion(TPointD(0,0))->getStyle() == animatedCopy &&
          palette->getKeyframeCount(animatedCopy) == 2, "native animated unique redo lost keys");
  checkCopy(palette.getPointer(),animatedCopy,true); reopenCopy(animatedCopy,true);
  undo->reset();

  // The gradient oracle deliberately sampled through frame 10. Return its
  // shared palette to the actual editing frame before freezing width baselines:
  // PaletteViewer evaluates this same frame after a native frame round trip.
  const int widthFrame = app->getCurrentFrame()->getFrameIndex();
  std::cout << "vector-width-context-native: palette baseline frame=" << widthFrame
            << " previous-evaluation=" << palette->getFrame() << std::endl;
  palette->setFrame(widthFrame);
  checkCopy(palette.getPointer(),animatedCopy,true);

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
  const QString profilePath = (TEnv::getConfigDir() + "width_profiles.txt").getQString();
  require(WidthProfile::writeLibrary(profilePath, {taper}, error),
          "native profile fixture library setup failed");
  auto *saved = CommandManager::instance()->getAction(MI_ApplySavedWidthProfile);
  require(saved && saved->isEnabled(), "visual profile command disabled");
  app->getCurrentScene()->setDirtyFlag(false);
  reviewWidthManager(saved, 50., false); settle();
  require(undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "canceling visual width manager dirtied artwork");
  const auto freezeWidth = [](const TVectorImageP &image) {
    TVectorImageP frozen = image->cloneWithGroupContext();
    if (auto *sourcePalette = image->getPalette()) {
      TPaletteP copy = sourcePalette->clone(); frozen->setPalette(copy.getPointer());
    }
    frozen->findRegions();
    return frozen;
  };
  std::array<TVectorImageP,3> widthSources{{
      level->getFrame(TFrameId(1), false), level->getFrame(TFrameId(2), false),
      level->getFrame(TFrameId(3), false)}};
  std::array<TVectorImageP,3> widthOriginals{{freezeWidth(widthSources[0]),
      freezeWidth(widthSources[1]), freezeWidth(widthSources[2])}};
  const auto unchangedWidth = [&](const char *phase) {
    for (int i = 0; i < 3; ++i) {
      const TVectorImageP live = level->getFrame(TFrameId(i+1),false);
      const auto &expected = widthOriginals[i];
      if (!VectorImageSnapshot::sameContent(live, expected)) {
        const TPalette *actualPalette = live ? live->getPalette() : nullptr;
        const TPalette *expectedPalette = expected->getPalette();
        int differentStyle = -1;
        if (actualPalette && expectedPalette)
          for (int style = 0; style < std::min(actualPalette->getStyleCount(),
                                              expectedPalette->getStyleCount()); ++style)
            if (!(*actualPalette->getStyle(style) == *expectedPalette->getStyle(style))) {
              differentStyle = style; break;
            }
        std::cerr << "vector-width-context-native: phase=" << phase << " drawing=" << (i+1)
                  << " strokes=" << (live ? int(live->getStrokeCount()) : -1)
                  << '/' << expected->getStrokeCount()
                  << " palette-frame=" << (actualPalette ? actualPalette->getFrame() : -1)
                  << '/' << (expectedPalette ? expectedPalette->getFrame() : -1)
                  << " first-different-style=" << differentStyle << std::endl;
        throw std::runtime_error("invalidated width manager changed an original drawing");
      }
    }
    require(undo->getHistoryCount() == 0, "invalidated width manager added undo history");
  };
  unchangedWidth("baseline");
  const auto restoreWidthScope = [&] {
    app->getCurrentColumn()->setColumnIndex(0);
    app->getCurrentFrame()->setFrame(0);
    app->getCurrentLevel()->setLevel(level);
    app->getCurrentTool()->setTool("T_Selection");
    tool = app->getCurrentTool()->getTool();
    mode = dynamic_cast<TEnumProperty *>(tool->getProperties(0)->getProperty("Mode:"));
    require(mode, "width context recovery lost Selection mode");
    frames.selectNone(); frames.select(TFrameId(1)); frames.select(TFrameId(2));
    mode->setValue(L"Selected Frames"); tool->onPropertyChanged("Mode:"); settle();
  };
  for (int destroy = 0; destroy < 2; ++destroy) {
    std::cout << "vector-width-context-native: original owner " << (destroy ? "delete" : "hide/show") << std::endl;
    QPointer<QWidget> previous = QApplication::activeWindow();
    std::unique_ptr<QWidget> owner(new QWidget);
    VectorModalTest::activateOwner(*owner, "width");
    const QByteArray libraryBefore = readBytes(profilePath);
    reviewWidthManager(saved,50.,true,[&] {
      require(VectorModalTest::hasReviewOwner(QApplication::activeModalWidget(), owner.get()),
              "width review did not capture its actual native owner");
      if (destroy) owner.reset(); else { owner->hide(); owner->show(); }
    });
    unchangedWidth("original owner cancelled");
    require(readBytes(profilePath) == libraryBefore && !app->getCurrentScene()->getDirtyFlag(),
            "owner-cancelled width review changed library or scene state");
    owner.reset(); if (previous) QApplication::setActiveWindow(previous);
    restoreWidthScope();
  }
  // A frame switch to a repeated exposure and back still invalidates a
  // previously queued Apply. The source pointer and drawing ID end unchanged.
  for (int repeat = 0; repeat < 2; ++repeat) {
    std::cout << "vector-width-context-native: held-frame round trip " << repeat << std::endl;
    reviewWidthManager(saved, 50., true, [&] {
      app->getCurrentFrame()->setFrame(2); app->getCurrentFrame()->setFrame(0);
    });
    unchangedWidth("held-frame round trip"); restoreWidthScope();
  }
  std::cout << "vector-width-context-native: selection replacement" << std::endl;
  reviewWidthManager(saved, 50., true, [&] {
    app->getCurrentSelection()->setSelection(nullptr);
  });
  unchangedWidth("selection replacement"); restoreWidthScope();

  // A real second level/column is a tempting stale target in Whole Level mode.
  auto *otherLevel = scene->createNewLevel(PLI_XSHLEVEL,L"WidthContextOther")->getSimpleLevel();
  otherLevel->setPalette(palette.getPointer());
  TVectorImageP otherDrawing = drawing(palette.getPointer(),gradientId);
  varyWidths(otherDrawing,9.);
  otherLevel->setFrame(TFrameId(1),otherDrawing);
  xsheet->setCell(0,1,TXshCell(otherLevel,TFrameId(1)));
  app->getCurrentXsheet()->notifyXsheetChanged(); restoreWidthScope();
  const TVectorImageP otherOriginal = freezeWidth(otherDrawing);
  mode->setValue(L"Whole Level"); tool->onPropertyChanged("Mode:"); settle();
  app->getCurrentScene()->setDirtyFlag(false);
  std::cout << "vector-width-context-native: level replacement" << std::endl;
  reviewWidthManager(saved, 50., true, [&] {
    app->getCurrentColumn()->setColumnIndex(1);
    app->getCurrentLevel()->setLevel(otherLevel);
  });
  unchangedWidth("level replacement");
  require(VectorImageSnapshot::sameContent(otherDrawing,otherOriginal),
          "queued width Apply edited the replacement current level");
  restoreWidthScope();

  // Save Selected Profile opens a nested naming prompt before the manager's
  // own event loop. Its context guard must already be active there.
  mode->setValue(L"Standard"); tool->onPropertyChanged("Mode:"); settle();
  selection = dynamic_cast<StrokeSelection *>(tool->getSelection());
  require(selection, "width capture selection unavailable");
  selection->selectNone(); selection->select(1,true); selection->makeCurrent();
  auto *captureWidth = CommandManager::instance()->getAction(MI_SaveWidthProfile);
  require(captureWidth && captureWidth->isEnabled(), "native width capture action unavailable");
  const QString libraryPath = (TEnv::getConfigDir() + "width_profiles.txt").getQString();
  QFile libraryBefore(libraryPath); require(libraryBefore.open(QIODevice::ReadOnly), "width library read failed");
  const QByteArray originalLibrary = libraryBefore.readAll(); libraryBefore.close();
  bool captureVisited = false;
  std::cout << "vector-width-context-native: nested capture frame round trip" << std::endl;
  std::exception_ptr captureFailure;
  QTimer capturePrompt;
  QObject::connect(&capturePrompt,&QTimer::timeout,[&] {
    capturePrompt.stop();
    auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
    try {
      require(input,"width capture name prompt unavailable"); captureVisited = true;
      input->setTextValue("Context changed during capture");
      app->getCurrentFrame()->setFrame(2); app->getCurrentFrame()->setFrame(0);
      input->accept();
    } catch (...) { captureFailure = std::current_exception(); if (input) input->reject(); }
  });
  capturePrompt.start(0); captureWidth->trigger(); capturePrompt.stop();
  if (captureFailure) std::rethrow_exception(captureFailure);
  require(captureVisited,"width capture bypassed naming prompt");
  QFile libraryAfter(libraryPath); require(libraryAfter.open(QIODevice::ReadOnly), "width library reread failed");
  require(libraryAfter.readAll() == originalLibrary,
          "invalidated nested capture committed an unreviewed profile library");
  unchangedWidth("nested capture frame round trip"); restoreWidthScope();

  // Direct same-scene authoring may omit notifications. Compare all selected
  // drawings, including an off-frame stroke's scalar/profile and fill state.
  for (int change = 0; change < 4; ++change) {
    std::cout << "vector-width-context-native: intervening authoring " << change << std::endl;
    TVectorImageP liveSecond = level->getFrame(TFrameId(2),false);
    TVectorImageP expectedSecond;
    reviewWidthManager(saved, 50., true, [&] {
      if (change == 0) {
        TStroke previous(*liveSecond->getStroke(1));
        auto point = liveSecond->getStroke(1)->getControlPoint(1); point.x += 6.;
        liveSecond->getStroke(1)->setControlPoint(1,point);
        liveSecond->notifyChangedStrokes(1,&previous);
      } else if (change == 1) {
        liveSecond->getStroke(1)->setOpacity(.31);
        liveSecond->getStroke(1)->setOpacityProfile({{0.,.2},{1.,.8}});
      } else if (change == 2) {
        liveSecond->findRegions(); liveSecond->getRegion(TPointD(0,0))->setStyle(1);
      } else {
        liveSecond = liveSecond->cloneWithGroupContext();
        level->setFrame(TFrameId(2),liveSecond);
      }
      expectedSecond = freezeWidth(liveSecond);
      app->getCurrentScene()->setDirtyFlag(true);
    }, true);
    require(VectorImageSnapshot::sameContent(level->getFrame(TFrameId(2),false),expectedSecond) &&
            VectorImageSnapshot::sameContent(level->getFrame(TFrameId(1),false),widthOriginals[0]) &&
            VectorImageSnapshot::sameContent(level->getFrame(TFrameId(3),false),widthOriginals[2]) &&
            undo->getHistoryCount() == 0 && app->getCurrentScene()->getDirtyFlag(),
            "width manager overwrote intervening authoring or added history");
    level->setFrame(TFrameId(2),widthOriginals[1]->cloneWithGroupContext());
    // Keep the original live palette; only snapshot palettes are detached.
    TVectorImageP restored = level->getFrame(TFrameId(2),false);
    restored->setPalette(palette.getPointer());
    restoreWidthScope(); app->getCurrentScene()->setDirtyFlag(false);
  }
  unchangedWidth("intervening authoring restored");
  std::cout << "vector-width-context-native: fresh zero-strength and blend Apply" << std::endl;
  reviewWidthManager(saved, 0., true); settle();
  require(undo->getHistoryCount() == 0, "zero-strength width profile created undo");
  reviewWidthManager(saved, 50., true); settle();
  afterFirst = level->getFrame(TFrameId(1), false); afterSecond = level->getFrame(TFrameId(2), false);
  const double endpoint = WidthProfile::evaluate(taper, 0.);
  require(std::abs(afterFirst->getStroke(1)->getControlPoint(0).thick - (.5 + .5 * endpoint)) < 1e-9 &&
          std::abs(afterSecond->getStroke(1)->getControlPoint(0).thick - (.5 + .5 * endpoint)) < 1e-9 &&
          undo->getHistoryCount() == 1, "native width-pressure blend differs from preview contract");
  const std::array<TVectorImageP,2> appliedWidths{{freezeWidth(afterFirst),freezeWidth(afterSecond)}};
  std::cout << "vector-width-context-native: Undo/Redo before native PLI write" << std::endl;
  undo->undo(); settle();
  require(VectorImageSnapshot::sameContent(level->getFrame(TFrameId(1),false),widthOriginals[0]) &&
          VectorImageSnapshot::sameContent(level->getFrame(TFrameId(2),false),widthOriginals[1]),
          "width undo before serialization lost original drawing state");
  undo->redo(); settle();
  require(VectorImageSnapshot::sameContent(level->getFrame(TFrameId(1),false),appliedWidths[0]) &&
          VectorImageSnapshot::sameContent(level->getFrame(TFrameId(2),false),appliedWidths[1]),
          "width redo before serialization lost reviewed drawing state");
  afterFirst = level->getFrame(TFrameId(1), false);
  afterSecond = level->getFrame(TFrameId(2), false);
  QTemporaryDir widthDirectory;
  require(widthDirectory.isValid(), "width profile reload directory unavailable");
  const TFilePath widthPath(widthDirectory.filePath("width-profiles.pli"));
  const int beforeWriteFrame = palette->getFrame();
  const bool beforeWriteDirty = palette->getDirtyFlag();
  const QByteArray beforeWritePalette = paletteBytes();
  std::cout << "vector-width-context-native: native PLI write preserves live palette frame="
            << beforeWriteFrame << std::endl;
  {
    TLevelWriterP writer(widthPath);
    writer->getFrameWriter(TFrameId(1))->save(afterFirst);
    writer->getFrameWriter(TFrameId(2))->save(afterSecond);
  }
  if (palette->getFrame() != beforeWriteFrame)
    std::cerr << "vector-width-context-native: PLI writer changed palette frame "
              << beforeWriteFrame << " -> " << palette->getFrame() << std::endl;
  require(palette->getFrame() == beforeWriteFrame &&
          palette->getDirtyFlag() == beforeWriteDirty && paletteBytes() == beforeWritePalette &&
          level->getPalette() == palette.getPointer() &&
          afterFirst->getPalette() == palette.getPointer() &&
          afterSecond->getPalette() == palette.getPointer() &&
          TVectorImageP(level->getFrame(TFrameId(1),false)) == afterFirst &&
          TVectorImageP(level->getFrame(TFrameId(2),false)) == afterSecond &&
          VectorImageSnapshot::sameContent(afterFirst,appliedWidths[0]) &&
          VectorImageSnapshot::sameContent(afterSecond,appliedWidths[1]) &&
          undo->getHistoryCount() == 1,
          "native PLI write changed live drawing/palette/frame/animation/history state");
  checkCopy(palette.getPointer(),animatedCopy,true);
  TLevelReaderP widthReader(widthPath); TLevelP savedWidthLevel = widthReader->loadInfo();
  require(savedWidthLevel && savedWidthLevel->getPalette(), "saved width level has no palette");
  checkCopy(savedWidthLevel->getPalette(),animatedCopy,true);
  for (int i = 0; i < 2; ++i) {
    TVectorImageP reopened = widthReader->getFrameReader(TFrameId(i+1))->load();
    require(reopened && reopened->getStrokeCount() == appliedWidths[i]->getStrokeCount(),
            "saved width blend lost a stroke");
    reopened->setPalette(savedWidthLevel->getPalette());
    for (UINT index = 0; index < reopened->getStrokeCount(); ++index) {
      const auto *actual = reopened->getStroke(index), *expected = appliedWidths[i]->getStroke(index);
      require(actual->getControlPointCount() == expected->getControlPointCount() &&
              actual->getStyle() == expected->getStyle() && actual->isSelfLoop() == expected->isSelfLoop() &&
              actual->getOpacity() == expected->getOpacity() &&
              actual->getOpacityProfile() == expected->getOpacityProfile(),
              "saved width blend lost geometry topology, style or authored opacity");
      double peak = 0.;
      for (int point = 0; point < expected->getControlPointCount(); ++point)
        peak = std::max(peak,expected->getControlPoint(point).thick);
      // Native PLI stores each width as an unsigned byte relative to ceil(peak).
      // Validate that format's exact quantizer, retaining exact XY coordinates;
      // in-memory Apply/Undo/Redo comparisons above and below remain exact.
      const double unit = std::ceil(peak) / 255.;
      for (int point = 0; point < expected->getControlPointCount(); ++point) {
        const auto a = actual->getControlPoint(point), b = expected->getControlPoint(point);
        const double encoded = unit > 0. ? std::floor(b.thick / unit + .5) * unit : 0.;
        require(a.x == b.x && a.y == b.y && std::abs(a.thick - encoded) < 1e-12,
                "saved width blend differs from native PLI width encoding");
      }
    }
    reopened->findRegions();
    require(reopened->getRegion(TPointD(0,0)) &&
            reopened->getRegion(TPointD(0,0))->getStyle() == appliedWidths[i]->getRegion(TPointD(0,0))->getStyle(),
            "saved width blend lost its filled region");
  }
  for (int repeat = 0; repeat < 2; ++repeat) {
    std::cout << "vector-width-context-native: Undo/Redo after native PLI write " << repeat << std::endl;
    undo->undo();
    std::cout << "vector-width-context-native: post-write Undo returned; settle " << repeat << std::endl;
    settle();
    require(VectorImageSnapshot::sameContent(level->getFrame(TFrameId(1),false),widthOriginals[0]) &&
            VectorImageSnapshot::sameContent(level->getFrame(TFrameId(2),false),widthOriginals[1]),
            "repeated width undo lost original drawing state");
    std::cout << "vector-width-context-native: post-write Undo verified; Redo " << repeat << std::endl;
    undo->redo();
    std::cout << "vector-width-context-native: post-write Redo returned; settle " << repeat << std::endl;
    settle();
    require(VectorImageSnapshot::sameContent(level->getFrame(TFrameId(1),false),appliedWidths[0]) &&
            VectorImageSnapshot::sameContent(level->getFrame(TFrameId(2),false),appliedWidths[1]),
            "repeated width redo did not restore reviewed pressure blend");
    std::cout << "vector-width-context-native: post-write Redo verified " << repeat << std::endl;
  }
  std::cout << "vector-alignment-native: restore width baseline" << std::endl;
  undo->undo();
  std::cout << "vector-alignment-native: width baseline Undo returned; settle" << std::endl;
  settle(); undo->reset();
  std::cout << "vector-alignment-native: prepare source geometry" << std::endl;
  // Give alignment an unmistakable translation and unequal object widths;
  // this makes both left/right previews visibly different at native scale.
  for (int i = 1; i <= 3; ++i) {
    std::cout << "vector-alignment-native: prepare drawing " << i << std::endl;
    TVectorImageP image = level->getFrame(TFrameId(i), false);
    TStroke original(*image->getStroke(1));
    for (int point = 0; point < image->getStroke(1)->getControlPointCount(); ++point) {
      auto position = image->getStroke(1)->getControlPoint(point);
      position.x = position.x * 1.5 + 20.;
      image->getStroke(1)->setControlPoint(point, position);
    }
    image->getStroke(1)->setOpacityProfile({{0., .25}, {1., 1.}});
    image->notifyChangedStrokes(std::vector<int>{1}, std::vector<TStroke *>{&original});
  }
  std::cout << "vector-alignment-native: notify prepared source drawings" << std::endl;
  tool->notifyImageChanged();
  std::cout << "vector-alignment-native: settle prepared source drawings" << std::endl;
  settle();
  tool->setAlignMethod(VectorAlignment::SELECT_AREA);
  auto *align = CommandManager::instance()->getAction(MI_AlignLeft);
  require(align && align->isEnabled(), "selected-drawing alignment action disabled");
  for (int destroy = 0; destroy < 2; ++destroy) {
    std::cout << "vector-alignment-native: original owner " << (destroy ? "delete" : "hide/show") << std::endl;
    std::array<TVectorImageP,3> before;
    for (int i = 0; i < 3; ++i) before[i] = freezeWidth(level->getFrame(TFrameId(i+1),false));
    const bool dirty = app->getCurrentScene()->getDirtyFlag();
    const int history = undo->getHistoryCount();
    QPointer<QWidget> previous = QApplication::activeWindow();
    std::unique_ptr<QWidget> owner(new QWidget);
    VectorModalTest::activateOwner(*owner, "alignment");
    reviewAlignment(align,true,2,[&] {
      require(VectorModalTest::hasReviewOwner(QApplication::activeModalWidget(), owner.get()),
              "alignment review did not capture its actual native owner");
      if (destroy) owner.reset(); else { owner->hide(); owner->show(); }
    },false,true);
    for (int i = 0; i < 3; ++i)
      require(VectorImageSnapshot::sameContent(level->getFrame(TFrameId(i+1),false),before[i]),
              "owner cancellation changed an alignment drawing");
    require(undo->getHistoryCount() == history && app->getCurrentScene()->getDirtyFlag() == dirty,
            "owner cancellation changed alignment history or dirty state");
    owner.reset(); if (previous) QApplication::setActiveWindow(previous);
  }
  afterFirst = level->getFrame(TFrameId(1), false); afterSecond = level->getFrame(TFrameId(2), false);
  const TStroke beforeAlignFirst(*afterFirst->getStroke(1)), beforeAlignSecond(*afterSecond->getStroke(1));
  const TVectorImageP untouchedThird = third->cloneWithGroupContext();
  app->getCurrentScene()->setDirtyFlag(false);
  reviewAlignment(align, false, 2, [&] {
    require(*afterFirst->getStroke(1) == beforeAlignFirst &&
            *afterSecond->getStroke(1) == beforeAlignSecond &&
            undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
            "opening alignment preview mutated a drawing or history");
  }); settle();
  require(*afterFirst->getStroke(1) == beforeAlignFirst &&
          *afterSecond->getStroke(1) == beforeAlignSecond &&
          undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "canceling alignment preview changed artwork or history");

  // Script/reload changes can happen inside a modal event loop. A change in
  // another selected drawing must invalidate the entire reviewed transaction.
  for (bool replaceIdentity : {false, true}) {
    const double originalOpacity = afterSecond->getStroke(1)->getOpacity();
    const TVectorImageP originalFirst = freezeWidth(afterFirst);
    TVectorImageP interveningDrawing, interveningSnapshot;
    reviewAlignment(align, true, 2, [&] {
      if (replaceIdentity) level->setFrame(TFrameId(2), afterSecond->cloneWithGroupContext());
      else afterSecond->getStroke(1)->setOpacity(.61);
      interveningDrawing = level->getFrame(TFrameId(2),false);
      interveningSnapshot = freezeWidth(interveningDrawing);
    }, true);
    require(undo->getHistoryCount() == 0 &&
            *afterFirst->getStroke(1) == beforeAlignFirst &&
            *afterSecond->getStroke(1) == beforeAlignSecond,
            "alignment applied a stale review or partially changed earlier drawings");
    require(TVectorImageP(level->getFrame(TFrameId(2),false)) == interveningDrawing &&
            VectorImageSnapshot::sameContent(interveningDrawing,interveningSnapshot) &&
            VectorImageSnapshot::sameContent(afterFirst,originalFirst) &&
            (replaceIdentity ? interveningDrawing != afterSecond
                             : interveningDrawing->getStroke(1)->getOpacity() == .61),
            "rejected alignment erased the intervening opacity edit or drawing replacement");
    if (replaceIdentity) level->setFrame(TFrameId(2), afterSecond);
    else afterSecond->getStroke(1)->setOpacity(originalOpacity);
  }
  const auto reviewed = reviewAlignment(align, true, 2); settle();
  require(undo->getHistoryCount() == 1, "selected-drawing alignment was not a single undo");
  require(std::abs(afterFirst->getStroke(0)->getBBox().x0 - afterFirst->getStroke(1)->getBBox().x0) < 1e-9 &&
          std::abs(afterSecond->getStroke(0)->getBBox().x0 - afterSecond->getStroke(1)->getBBox().x0) < 1e-9,
          "alignment did not apply independently to both selected drawings");
  for (UINT i = 0; i < third->getStrokeCount(); ++i)
    require(*third->getStroke(i) == *untouchedThird->getStroke(i),
            "alignment touched an unselected drawing");
  checkAlignmentOutput(afterFirst, reviewed[0]);
  checkAlignmentOutput(afterSecond, reviewed[1]);
  bool noopDialog = false;
  QTimer guard;
  QObject::connect(&guard, &QTimer::timeout, [&] {
    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
      noopDialog = true;
      dialog->reject();
    }
  });
  guard.start(0); align->trigger(); guard.stop();
  require(!noopDialog && undo->getHistoryCount() == 1,
          "repeating unchanged alignment opened review or created an empty undo");
  for (int cycle = 0; cycle < 2; ++cycle) {
    undo->undo(); settle();
    require(*afterFirst->getStroke(1) == beforeAlignFirst && *afterSecond->getStroke(1) == beforeAlignSecond,
            "multidrawing alignment undo lost geometry");
    undo->redo(); settle();
    checkAlignmentOutput(afterFirst, reviewed[0]);
    checkAlignmentOutput(afterSecond, reviewed[1]);
  }
  QTemporaryDir directory;
  require(directory.isValid(), "alignment save/reopen fixture directory");
  const TFilePath path(directory.filePath("aligned.pli"));
  {
    TLevelWriterP writer(path);
    writer->getFrameWriter(TFrameId(1))->save(afterFirst);
    writer->getFrameWriter(TFrameId(2))->save(afterSecond);
  }
  TLevelReaderP reader(path); reader->loadInfo();
  for (int i = 0; i < 2; ++i) {
    TVectorImageP reopened = reader->getFrameReader(TFrameId(i+1))->load();
    // The level palette is a companion asset in the application; this fixture
    // writes standalone drawing data and attaches that same reviewed palette.
    reopened->setPalette(reviewed[i].after->getPalette());
    checkAlignmentOutput(reopened, reviewed[i]);
  }
  undo->reset();
  mode->setValue(L"Standard"); tool->onPropertyChanged("Mode:");
  frames.selectNone();
  app->getCurrentScene()->setDirtyFlag(false);
  // The same explicit preview is used for a manual stroke selection and a
  // whole-level operation, with only the drawings that will actually change.
  selection = dynamic_cast<StrokeSelection *>(tool->getSelection());
  require(selection, "alignment manual scope selection missing");
  selection->selectNone(); selection->select(0, true); selection->select(1, true);
  selection->makeCurrent();
  auto *alignRight = CommandManager::instance()->getAction(MI_AlignRight);
  require(alignRight && alignRight->isEnabled(), "manual alignment command disabled");
  for (int sceneRow : {1, 2}) { // row 2 holds the same drawing as row 0
    reviewAlignment(alignRight, true, 1, [&] {
      app->getCurrentFrame()->setFrame(sceneRow);
      // Simulate a queued Apply after a context switch, including returning
      // to the original held drawing before that click is delivered.
      if (sceneRow == 2) app->getCurrentFrame()->setFrame(0);
    });
    require(undo->getHistoryCount() == 0, "frame switch applied a dismissed alignment preview");
    app->getCurrentFrame()->setFrame(0); settle();
    selection = dynamic_cast<StrokeSelection *>(tool->getSelection());
    require(selection, "frame switch lost native stroke selection");
    selection->selectNone(); selection->select(0, true); selection->select(1, true);
    selection->makeCurrent();
  }
  const auto currentReview = reviewAlignment(alignRight, true, 1);
  require(undo->getHistoryCount() == 1, "manual alignment was not one undo");
  checkAlignmentOutput(afterFirst, currentReview[0]);
  checkAlignmentGeometry(afterSecond, reviewed[1]); // frame switches evaluate its animated palette
  undo->undo(); undo->reset();
  mode->setValue(L"Whole Level"); tool->onPropertyChanged("Mode:"); settle();
  reviewAlignment(alignRight, false, 3);
  require(undo->getHistoryCount() == 0, "canceling whole-level alignment changed history");
  const auto levelReview = reviewAlignment(alignRight, true, 3);
  require(undo->getHistoryCount() == 1, "whole-level alignment was not one undo");
  for (int i = 0; i < 3; ++i)
    checkAlignmentOutput(level->getFrame(TFrameId(i+1), false), levelReview[i]);
  undo->undo(); undo->reset();
  mode->setValue(L"Standard"); tool->onPropertyChanged("Mode:");
  std::cout << "vector-alignment-native: nested group workflows begin" << std::endl;
  nestedGroupChecks(frames);
  std::cout << "vector-alignment-native: nested group workflows returned" << std::endl;

  // Exercise a genuine scene replacement last: IoCmd owns the old scene's
  // delayed deletion. Retain only drawing snapshots across that transition.
  TXshSimpleLevelP sceneSource = app->getCurrentLevel()->getSimpleLevel();
  require(sceneSource,"scene-switch width fixture has no source level");
  std::vector<TFrameId> sceneFids; sceneSource->getFids(sceneFids);
  std::vector<TVectorImageP> sceneDrawings, sceneOriginals;
  for (const auto &id : sceneFids) {
    TVectorImageP image = sceneSource->getFrame(id,false);
    sceneDrawings.push_back(image); sceneOriginals.push_back(freezeWidth(image));
  }
  tool = app->getCurrentTool()->getTool();
  mode = dynamic_cast<TEnumProperty *>(tool->getProperties(0)->getProperty("Mode:"));
  require(mode,"scene-switch width mode unavailable");
  mode->setValue(L"Whole Level"); tool->onPropertyChanged("Mode:"); settle();
  app->getCurrentScene()->setDirtyFlag(false);
  TVectorImageP replacementDrawing, replacementOriginal;
  auto *oldScene = app->getCurrentScene()->getScene();
  QStringList modifiedResources;
  {
    SceneResources resources(oldScene, nullptr);
    resources.getDirtyResources(modifiedResources);
    resources.commit(); // Read-only inventory: avoid path-restoration setters.
  }
  require(!modifiedResources.empty(),
          "scene-switch width fixture lost its modified resource precondition");
  std::cout << "vector-width-context-native: actual scene replacement manager begin" << std::endl;
  reviewWidthManager(saved,50.,true,[&] {
    // New Scene asks about modified drawing resources independently of the
    // scene dirty flag. This callback already runs inside the manager's timer;
    // a separate timer must drive the nested native confirmation.
    bool discardVisited = false, discardExpired = false;
    std::exception_ptr discardFailure;
    QTimer discard, discardDeadline;
    discard.setInterval(10);
    discardDeadline.setSingleShot(true);
    QObject::connect(&discard, &QTimer::timeout, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog || dialog->objectName() == "widthProfileManager") return;
      discard.stop();
      try {
        QString text;
        for (auto *label : dialog->findChildren<QLabel *>()) text += label->text();
        QPushButton *saveChanges = nullptr, *newSceneAnyway = nullptr, *cancel = nullptr;
        const auto buttons = dialog->findChildren<QPushButton *>();
        for (auto *button : buttons) {
          if (button->text() == QObject::tr("Save Changes")) saveChanges = button;
          if (button->text() == QApplication::tr("New Scene") + QObject::tr(" Anyway"))
            newSceneAnyway = button;
          if (button->text() == QObject::tr("Cancel")) cancel = button;
        }
        if (buttons.size() != 3 || !saveChanges || !newSceneAnyway || !cancel ||
            !text.contains(QObject::tr(" The following file(s) have been modified.\n\n")))
          throw std::runtime_error("unexpected New Scene resource dialog: " + text.left(400).toStdString());
        require(saveChanges->isEnabled() && newSceneAnyway->isVisible() &&
                    newSceneAnyway->isEnabled() && cancel->isEnabled(),
                "New Scene resource choices are not actionable");
        discardVisited = true;
        std::cout << "vector-width-context-native: native New Scene Anyway choice" << std::endl;
        require(QMetaObject::invokeMethod(newSceneAnyway,"click",Qt::QueuedConnection),
                "New Scene resource choice could not queue its real button");
      } catch (...) {
        discardFailure = std::current_exception();
        dialog->reject();
      }
    });
    QObject::connect(&discardDeadline, &QTimer::timeout, [&] {
      discardExpired = true;
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
        std::cerr << "vector-width-context-native: New Scene deadline modal="
                  << dialog->metaObject()->className() << " name="
                  << dialog->objectName().left(100).toStdString() << std::endl;
        dialog->reject();
      }
    });
    discard.start(); discardDeadline.start(10000);
    std::cout << "vector-width-context-native: actual New Scene begin" << std::endl;
    IoCmd::newScene();
    discard.stop(); discardDeadline.stop();
    std::cout << "vector-width-context-native: actual New Scene returned" << std::endl;
    if (discardFailure) std::rethrow_exception(discardFailure);
    require(discardVisited && !discardExpired,
            "native New Scene did not complete its modified-resource choice");
    auto *replacementScene = app->getCurrentScene()->getScene();
    require(replacementScene != oldScene,"native New Scene did not switch context");
    auto *replacementLevel = replacementScene->createNewLevel(
        PLI_XSHLEVEL,L"WidthAfterSceneSwitch")->getSimpleLevel();
    TPaletteP replacementPalette = new TPalette;
    replacementLevel->setPalette(replacementPalette.getPointer());
    replacementDrawing = drawing(replacementPalette.getPointer(),1);
    varyWidths(replacementDrawing,11.);
    replacementLevel->setFrame(TFrameId(1),replacementDrawing);
    app->getCurrentXsheet()->getXsheet()->setCell(0,0,TXshCell(replacementLevel,TFrameId(1)));
    app->getCurrentXsheet()->notifyXsheetChanged();
    app->getCurrentColumn()->setColumnIndex(0);
    app->getCurrentFrame()->setFrame(0);
    app->getCurrentLevel()->setLevel(replacementLevel);
    app->getCurrentTool()->setTool("T_Selection");
    auto *replacementTool = app->getCurrentTool()->getTool();
    auto *replacementMode = dynamic_cast<TEnumProperty *>(replacementTool->getProperties(0)->getProperty("Mode:"));
    require(replacementMode,"replacement width mode unavailable");
    replacementMode->setValue(L"Whole Level"); replacementTool->onPropertyChanged("Mode:");
    replacementOriginal = freezeWidth(replacementDrawing);
    app->getCurrentScene()->setDirtyFlag(false);
  });
  for (size_t i = 0; i < sceneDrawings.size(); ++i)
    require(VectorImageSnapshot::sameContent(sceneDrawings[i],sceneOriginals[i]),
            "scene-switch width Apply changed an original drawing");
  require(replacementDrawing && VectorImageSnapshot::sameContent(replacementDrawing,replacementOriginal) &&
          undo->getHistoryCount() == 0 && !app->getCurrentScene()->getDirtyFlag(),
          "queued width Apply edited the replacement scene");
  tool = app->getCurrentTool()->getTool();
  mode = dynamic_cast<TEnumProperty *>(tool->getProperties(0)->getProperty("Mode:"));
  mode->setValue(L"Standard"); tool->onPropertyChanged("Mode:");
  std::cout << "vector-width-context-native: PASS" << std::endl;
}
