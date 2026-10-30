#include "toonzqt/functionviewer.h"
#include "toonzqt/functiontreeviewer.h"
#include "toonzqt/functionpanel.h"
#include "toonzqt/functionsheet.h"
#include "toonzqt/functionselection.h"
#include "toonzqt/functioncurveoperations.h"
#include "toonz/doubleparamcmd.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "tenv.h"
#include "tthread.h"
#include "tfunctorinvoker.h"
#include "tundo.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void checkpoint(const char *message) {
  // Flush before entering native callbacks so a fatal signal retains the last
  // completed phase in CTest's report on every supported platform.
  std::cerr << "function-workflow: " << message << std::endl;
}
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void near(double a, double b) {
  if (!std::isfinite(a) || std::abs(a - b) >= 1.e-8)
    throw std::runtime_error(QString("Expected %1, observed %2")
        .arg(b, 0, 'g', 17).arg(a, 0, 'g', 17).toStdString());
}
struct Fixture {
  FunctionViewer viewer;
  FunctionTreeView *tree;
  FunctionPanel *graph;
  FunctionTreeModel *model;
  TDoubleParamP a = new TDoubleParam, b = new TDoubleParam;
  Fixture() {
    checkpoint("viewer constructed");
    tree = viewer.findChild<FunctionTreeView *>(); graph = viewer.findChild<FunctionPanel *>();
    require(tree && graph, "native Function Editor children missing");
    model = qobject_cast<FunctionTreeModel *>(tree->model());
    require(model, "native Function Editor model missing");
    model->refreshData(nullptr);
    checkpoint("empty model initialized");
    a->setName("rotation"); b->setName("scale");
    for (int i = 0; i < 4; ++i) {
      a->setKeyframe(TDoubleKeyframe(i * 10, i * 100));
      b->setKeyframe(TDoubleKeyframe(i * 10 + 5, i * 2));
    }
    model->beginRefresh();
    viewer.addParameter(a.getPointer(), TFilePath("Hero/Arm"));
    viewer.addParameter(b.getPointer(), TFilePath("Hero/Head"));
    model->endRefresh();
    checkpoint("generic channels added");
    for (auto *channel : tree->channels()) channel->setIsActive(true);
    graph->resize(800, 400);
    checkpoint("generic channels activated");
  }
};
void sceneFreeViews() {
  // Optional handles must be initialized before any caller can query the
  // spreadsheet's zoom or channel state, even without attaching a scene.
  FunctionSheet unattached;
  require(unattached.getFrameZoomFactor() == 100 &&
              unattached.getChannelCount() == 0,
          "unattached spreadsheet did not initialize its optional handles");
  checkpoint("unattached spreadsheet constructed");
  TXsheetHandle emptyHandle;
  Fixture f;
  auto *sheet = f.viewer.findChild<FunctionSheet *>();
  require(sheet, "native spreadsheet missing");
  for (int attached = 0; attached < 2; ++attached) {
    if (attached) f.viewer.setXsheetHandle(&emptyHandle);
    for (auto *channel : f.tree->channels()) {
      require(!channel->isIgnored(), "scene-free channel acquired a reference warning");
      require(channel->data(Qt::DecorationRole).isValid(), "scene-free channel icon missing");
      require(channel->data(Qt::ToolTipRole).toString().isEmpty(), "scene-free channel warning tooltip is not empty");
    }
    checkpoint(attached ? "empty Xsheet handle roles queried" : "scene-free roles queried");
    f.tree->resize(240, 300); f.tree->expandAll();
    QImage treeImage(240, 300, QImage::Format_ARGB32_Premultiplied);
    treeImage.fill(Qt::transparent); f.tree->render(&treeImage);
    sheet->resize(640, 320); sheet->setRowCount(40);
    QImage sheetImage(640, 320, QImage::Format_ARGB32_Premultiplied);
    sheetImage.fill(Qt::transparent); sheet->render(&sheetImage);
    checkpoint(attached ? "empty Xsheet handle views painted" : "scene-free views painted");
  }
}
void queuedModelLifetime() {
  QObject receiver;
  int delivered = 0;
  QPointer<FunctionTreeModel> destroyed;
  {
    Fixture f;
    destroyed = f.model;
    QObject::connect(f.model, &FunctionTreeModel::curveChanged, &receiver,
                     [&](bool) { ++delivered; });
    f.a->setValue(10, 101);
    require(delivered == 0, "curve update unexpectedly bypassed native event coalescing");
  }
  require(destroyed.isNull(), "fixture kept its tree model alive");
  checkpoint("closed model pending update delivery begins");
  QApplication::sendPostedEvents(TFunctorInvoker::instance(), QEvent::MetaCall);
  require(delivered == 0, "closed tree model delivered a stale curve update");
  checkpoint("closed model pending update safely discarded");
  Fixture live;
  QObject::connect(live.model, &FunctionTreeModel::curveChanged, &receiver,
                   [&](bool) { ++delivered; });
  live.a->setValue(10, 102); live.a->setValue(20, 202);
  QApplication::sendPostedEvents(TFunctorInvoker::instance(), QEvent::MetaCall);
  require(delivered == 1, "live tree model lost its coalesced curve update");
}
void fitAndSearch() {
  Fixture f;
  auto *selection = f.viewer.getSelection();
  selection->select(f.a.getPointer(), 1); selection->select(f.b.getPointer(), 1);
  QRectF bounds = f.graph->selectionBounds();
  near(bounds.left(), 10); near(bounds.right(), 15); near(bounds.top(), 2); near(bounds.bottom(), 100);
  f.graph->fitSelectedPoints();
  near(f.graph->frameToX(10), 70);
  require(f.graph->frameToX(30) > f.graph->width(), "Fit Selection framed the entire curve");
  auto channels = f.tree->channels(); require(channels.size() == 2, "missing native channels");
  QModelIndex parent = channels[0]->createIndex().parent(); f.tree->setExpanded(parent, false);
  auto *search = f.viewer.findChild<QLineEdit *>("FunctionChannelSearch"); require(search, "search field missing");
  search->setText("Arm");
  require(!f.tree->isRowHidden(channels[0]->createIndex().row(), parent), "ancestor search hid matching channel");
  require(f.tree->isRowHidden(channels[1]->createIndex().row(), channels[1]->createIndex().parent()), "search retained nonmatching channel");
  require(f.model->getActiveChannelCount() == 2 && selection->getSelectedKeyframeCount() == 2, "search hid graph selection");
  search->clear(); require(!f.tree->isExpanded(parent), "clearing search did not restore expansion");
  selection->selectNone(); selection->select(f.a.getPointer(), 0);
  f.graph->fitSelectedPoints(); require(std::isfinite(f.graph->getViewTransform().m11()), "single key fit invalid");
}
void selectionLifetime() {
  require(!TSelection::getCurrent(), "a closed editor retained the current selection");
  QPointer<FunctionSelection> closed;
  {
    Fixture first;
    closed = first.viewer.getSelection();
    closed->select(first.a.getPointer(), 1);
    require(TSelection::getCurrent() == closed.data(),
            "native key selection did not become current");
  }
  require(closed.isNull(), "closed editor retained its selection object");
  require(!TSelection::getCurrent(), "closed editor left a dangling current selection");
  checkpoint("current selection released with editor");
  {
    Fixture current;
    auto *selection = current.viewer.getSelection();
    selection->select(current.a.getPointer(), 1);
    {
      // Closing an inactive editor must not clear another editor's selection.
      Fixture inactive;
      require(TSelection::getCurrent() == selection,
              "constructing another editor changed the current selection");
    }
    require(TSelection::getCurrent() == selection &&
                selection->isSelected(current.a.getPointer(), 1),
            "closing an inactive editor cleared the current selection");
    selection->select(current.b.getPointer(), 1);
    require(selection->getSelectedKeyframeCount() == 2,
            "replacement editor could not retain a multi-channel selection");
  }
  require(!TSelection::getCurrent(), "replacement editor retained a dangling selection");
}
void normalizationAndLocks() {
  Fixture f; TDoubleParamP original = new TDoubleParam(*f.a);
  f.graph->setNormalized(true);
  double y = f.graph->valueToY(f.a.getPointer(), 150);
  near(f.graph->yToValue(f.a.getPointer(), y), 150);
  require(FunctionCurveOperations::matchesSnapshot(f.a.getPointer(), original.getPointer()), "normalization edited native values");
  f.viewer.getSelection()->select(f.a.getPointer(), 1);
  f.a->setEditorLocked(true);
  f.viewer.getSelection()->doDelete();
  KeyframeSetter::setValue(f.a.getPointer(), 10, 999);
  KeyframeSetter::removeKeyframeAt(f.a.getPointer(), 10);
  f.viewer.getSelection()->setSegmentType(TDoubleKeyframe::Constant);
  require(FunctionCurveOperations::matchesSnapshot(f.a.getPointer(), original.getPointer()), "locked curve changed through native command");
  f.a->setEditorLocked(false); f.graph->setNormalized(false);
  f.graph->fitRegion(0, 0, 24, 20); f.graph->setSnap(4, false);
  f.graph->setSceneMarkers(6, 1, 0); f.graph->setSnapMarkers(true);
  near(f.graph->snapFrameDelta(2.9, 10, false), 3);
  f.graph->setSnapMarkers(false); near(f.graph->snapFrameDelta(2.9, 10, false), 4);
  near(f.graph->snapFrameDelta(2.9, 10, true), 3);
  QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier); QApplication::sendEvent(f.graph, &right);
  near(f.a->keyframeIndexToFrame(1), 14);
  require(TUndoManager::manager()->undo(), "keyboard edit has no undo");
  near(f.a->keyframeIndexToFrame(1), 10);
  f.graph->setSnap(1, false);
  QApplication::sendEvent(f.graph, &right);
  near(f.a->keyframeIndexToFrame(1), 11);
  require(TUndoManager::manager()->undo(), "single-frame keyboard edit has no undo");
  near(f.a->keyframeIndexToFrame(1), 10);
}
void nativeBatchDialog() {
  Fixture f; auto *selection = f.viewer.getSelection();
  selection->select(f.a.getPointer(), 1); selection->select(f.a.getPointer(), 2);
  auto *button = f.viewer.findChild<QToolButton *>("FunctionEditKeys"); require(button, "native batch action missing");
  TDoubleParamP comparison = new TDoubleParam(*f.a); f.graph->setComparison({{f.a, comparison}});
  QString failure;
  QTimer::singleShot(0, [&]() {
    try {
      auto *dialog = f.viewer.findChild<QDialog *>("FunctionKeyEditor"); require(dialog, "batch dialog did not open");
      checkpoint("batch dialog entered");
      auto *mode = dialog->findChild<QComboBox *>("FunctionEditOperation");
      auto *amount = dialog->findChild<QDoubleSpinBox *>("FunctionEditAmount");
      auto *buttons = dialog->findChild<QDialogButtonBox *>(); require(mode && amount && buttons, "batch controls missing");
      mode->setCurrentIndex(1); amount->setValue(5);
      buttons->button(QDialogButtonBox::Apply)->click();
      near(f.a->getKeyframe(1).m_value, 105); near(f.a->getKeyframe(2).m_value, 205);
      require(selection->getSelectedKeyframeCount() == 2, "Apply broadened the original selection");
      buttons->button(QDialogButtonBox::Apply)->click();
      near(f.a->getKeyframe(1).m_value, 110); near(f.a->getKeyframe(0).m_value, 0); near(f.a->getKeyframe(3).m_value, 300);
      mode->setCurrentIndex(0); amount->setValue(999); // close is a true cancel
      dialog->reject();
    } catch (const std::exception &error) {
      failure = QString::fromUtf8(error.what());
      if (auto *dialog = f.viewer.findChild<QDialog *>("FunctionKeyEditor")) dialog->reject();
    }
  });
  checkpoint("opening batch dialog");
  button->click(); require(failure.isEmpty(), qPrintable(failure));
  checkpoint("batch dialog closed");
  require(f.graph->comparison().size() == 1 && f.graph->comparison()[0].second == comparison, "Modal edit discarded an artist comparison snapshot");
  near(comparison->getKeyframe(1).m_value, 100);
  near(f.a->getKeyframe(1).m_value, 110);
  require(TUndoManager::manager()->undo(), "native batch undo missing"); near(f.a->getKeyframe(1).m_value, 105);
  require(TUndoManager::manager()->undo(), "second native batch undo missing"); near(f.a->getKeyframe(1).m_value, 100);
}
void contextCancellation() {
  for (int context = 0; context < 3; ++context) {
    TSceneHandle scene; TXsheetHandle xsheet;
    Fixture f; f.viewer.setSceneHandle(&scene); f.viewer.setXsheetHandle(&xsheet);
    TDoubleParamP original = new TDoubleParam(*f.a);
    f.viewer.getSelection()->select(f.a.getPointer(), 1);
    auto *button = f.viewer.findChild<QToolButton *>("FunctionEditKeys");
    bool opened = false;
    QTimer::singleShot(0, [&]() {
      auto *dialog = f.viewer.findChild<QDialog *>("FunctionKeyEditor");
      if (!dialog) return;
      opened = true;
      checkpoint("context cancellation dialog entered");
      dialog->findChild<QDoubleSpinBox *>("FunctionEditAmount")->setValue(999);
      if (context == 0) scene.notifySceneSwitched();
      else if (context == 1) xsheet.notifyXsheetSwitched();
      else f.model->resetAll();
    });
    checkpoint("opening context cancellation dialog");
    button->click();
    checkpoint("context cancellation dialog closed");
    require(opened, "context cancellation did not enter native dialog");
    require(FunctionCurveOperations::matchesSnapshot(f.a.getPointer(), original.getPointer()), "context change committed a pending edit");
  }
}
void denseGraphMeasurement() {
  Fixture f;
  f.a->clearKeyframes();
  for (int i = 0; i < 20000; ++i) f.a->setKeyframe(TDoubleKeyframe(i, i % 20));
  f.graph->fitRegion(10000, 0, 10050, 20);
  QImage image(800, 400, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
  QElapsedTimer timer; timer.start();
  checkpoint("dense graph paint begins");
  for (int i = 0; i < 8; ++i) f.graph->render(&image);
  checkpoint("dense graph paint completed");
  bool painted = false;
  for (int y = 0; y < image.height() && !painted; ++y)
    for (int x = 0; x < image.width(); ++x) if (qAlpha(image.pixel(x, y))) { painted = true; break; }
  require(painted, "native graph render produced no pixels");
  require(f.a->getKeyframeCount() == 20000, "viewport rendering changed keys");
  std::cout << "NOTICE: function graph 20000 keys, 50-frame viewport, 8 paints: " << timer.elapsed() << " ms\n";
  // Source correctness, not a hosted-runner speed threshold.
  near(f.a->getValue(10011.5), 11.5);
}
}
int main(int argc, char **argv) {
  checkpoint("application initialization begins");
  qputenv("QT_QPA_PLATFORM", "offscreen"); QApplication application(argc, argv); TThread::init();
  try {
    QTemporaryDir directory; require(directory.isValid(), "temporary directory unavailable");
    QDir().mkpath(directory.filePath("stuff/config")); QDir().mkpath(directory.filePath("stuff/profiles"));
    TEnv::setStuffDir(TFilePath(directory.filePath("stuff")));
    checkpoint("scene-free views begin"); sceneFreeViews();
    checkpoint("queued model lifetime begins"); queuedModelLifetime();
    checkpoint("fit/search begin"); fitAndSearch();
    checkpoint("selection lifetime begins"); selectionLifetime();
    checkpoint("normalization/locks begin"); normalizationAndLocks();
    checkpoint("batch editing begins"); nativeBatchDialog();
    checkpoint("context cancellation begins"); contextCancellation();
    checkpoint("dense graph measurement begins"); denseGraphMeasurement();
    checkpoint("all workflow checks completed");
    TUndoManager::manager()->reset(); std::cout << "Native Function Editor workflow checks passed\n"; return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
