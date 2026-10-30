#include "toonzqt/functionviewer.h"
#include "toonzqt/functiontreeviewer.h"
#include "toonzqt/functionpanel.h"
#include "toonzqt/functionsheet.h"
#include "toonzqt/functionselection.h"
#include "toonzqt/functioncurveoperations.h"
#include "toonzqt/menubarcommand.h"
#include "toonz/doubleparamcmd.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/toonzfolders.h"
#include "tenv.h"
#include "tthread.h"
#include "tfunctorinvoker.h"
#include "tundo.h"
#include "tstream.h"
#include "tunit.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <functional>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace {
namespace Ops = FunctionCurveOperations;
void checkpoint(const char *message) {
  // Flush before entering native callbacks so a fatal signal retains the last
  // completed phase in CTest's report on every supported platform.
  std::cerr << "function-workflow: " << message << std::endl;
}
void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void requireNear(double a, double b) {
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
void key(QWidget *widget, int code, Qt::KeyboardModifiers modifiers = {}) {
  QKeyEvent press(QEvent::KeyPress, code, modifiers);
  QKeyEvent release(QEvent::KeyRelease, code, modifiers);
  QApplication::sendEvent(widget, &press); QApplication::sendEvent(widget, &release);
}
void inspectModal(FunctionViewer *viewer, const char *name,
                  const std::function<void(QDialog *)> &inspect,
                  const std::function<void()> &open) {
  QObject delivery;
  QTimer deadline;
  deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &delivery, [name]() {
    std::cerr << "function-workflow: expected modal " << name << std::endl;
    if (auto *modal = QApplication::activeModalWidget())
      std::cerr << "function-workflow: blocked modal " << modal->objectName().toStdString()
                << " (" << modal->windowTitle().toStdString() << ")" << std::endl;
    qFatal("Function workflow modal exceeded its 15-second deadline");
  });
  bool visited = false; QString failure;
  QTimer::singleShot(0, &delivery, [&]() {
    QPointer<QDialog> dialog = viewer->findChild<QDialog *>(name);
    try { require(dialog, "expected Function Editor modal is missing"); visited = true; inspect(dialog); }
    catch (const std::exception &problem) { failure = QString::fromUtf8(problem.what()); }
    if (dialog) dialog->reject();
  });
  deadline.start(15000); open(); deadline.stop();
  require(visited, "Function Editor modal callback did not run");
  if (!failure.isEmpty()) throw std::runtime_error(failure.toStdString());
}
QString channelSetPath() {
  return (ToonzFolder::getMyModuleDir() + TFilePath("function-editor.ini")).getQString();
}
QJsonArray savedSets() {
  QSettings settings(channelSetPath(), QSettings::IniFormat); settings.sync();
  require(settings.status() == QSettings::NoError, "channel-set settings read failed");
  return QJsonDocument::fromJson(settings.value("channelSets").toByteArray()).array();
}
void persistSets(const QJsonArray &sets) {
  QSettings settings(channelSetPath(), QSettings::IniFormat);
  settings.setValue("channelSets", QJsonDocument(sets).toJson(QJsonDocument::Compact)); settings.sync();
  require(settings.status() == QSettings::NoError, "channel-set fixture could not be written");
}
void savedSetRename() {
  TSceneHandle scene; TXsheetHandle xsheet;
  Fixture f; f.viewer.setSceneHandle(&scene); f.viewer.setXsheetHandle(&xsheet);
  f.a->setWorkspaceId("set-fixture-arm"); f.b->setWorkspaceId("set-fixture-head");
  QJsonArray expected{
      QJsonObject{{"name", "Body"}, {"ids", QJsonArray{"set-fixture-arm", "missing-channel"}}, {"extra", QJsonObject{{"preserve", 7}}}},
      QJsonObject{{"name", "Face"}, {"ids", QJsonArray{"set-fixture-head"}}}};
  persistSets(expected);
  auto *action = f.viewer.findChild<QAction *>("FunctionChannelSetsAction");
  require(action, "Channel Sets action is missing");
  const int history = TUndoManager::manager()->getHistoryCount();
  const int historyIndex = TUndoManager::manager()->getCurrentHistoryIndex();
  scene.setDirtyFlag(false);
  inspectModal(&f.viewer, "FunctionChannelSets", [&](QDialog *dialog) {
    auto *list = dialog->findChild<QListWidget *>("FunctionChannelSetList");
    auto *name = dialog->findChild<QLineEdit *>("FunctionChannelSetName");
    auto *rename = dialog->findChild<QPushButton *>("FunctionChannelSetRename");
    require(list && name && rename && list->count() == 2, "Channel Sets rename controls are missing");
    list->setCurrentRow(0); list->setFocus(); QApplication::processEvents();
    key(list, Qt::Key_Down); require(list->currentRow() == 1, "saved-set list cannot move down with the keyboard");
    key(list, Qt::Key_Up); require(list->currentRow() == 0, "saved-set list cannot move up with the keyboard");
    key(list, Qt::Key_F2);
    require(name->hasFocus() && name->selectedText() == "Body", "F2 did not focus and select the saved-set name");
    name->setText(" Body "); require(!rename->isEnabled(), "unchanged trimmed name enabled Rename"); key(name, Qt::Key_Return);
    require(savedSets() == expected, "unchanged name modified persisted sets");
    name->setText("   "); require(!rename->isEnabled(), "empty saved-set rename was enabled"); key(name, Qt::Key_Return);
    require(savedSets() == expected, "empty rename changed persisted sets");
    name->setText(" fAcE "); require(!rename->isEnabled(), "case-insensitive duplicate rename was enabled"); key(name, Qt::Key_Return);
    require(savedSets() == expected, "duplicate rename changed persisted sets");
    name->setText(" Body timing "); require(rename->isEnabled(), "valid saved-set rename was disabled");
    require(savedSets() == expected, "typing a pending name committed it before Rename");
    key(name, Qt::Key_Return);
    QJsonObject entry = expected[0].toObject(); entry["name"] = "Body timing"; expected[0] = entry;
    require(savedSets() == expected && list->count() == 2 && list->currentRow() == 0 &&
                list->item(0)->text() == "Body timing", "Rename changed set metadata, membership, order or selection");
    name->setText("Body poses"); rename->click();
    entry["name"] = "Body poses"; expected[0] = entry;
    require(savedSets() == expected, "second Rename did not persist");
    name->setText("Unfinished name");
  }, [&]() { action->trigger(); });
  require(savedSets() == expected, "closing Channel Sets committed an unfinished rename");
  require(!scene.getDirtyFlag() && TUndoManager::manager()->getHistoryCount() == history &&
              TUndoManager::manager()->getCurrentHistoryIndex() == historyIndex,
          "workspace set rename modified scene dirty state or animation undo");
  require(f.a->getWorkspaceId() == "set-fixture-arm" && f.b->getWorkspaceId() == "set-fixture-head",
          "set rename replaced channel identities");
  QProcess reader;
  reader.start(QCoreApplication::applicationFilePath(),
               {"--verify-channel-sets", channelSetPath(),
                QString::fromLatin1(QJsonDocument(expected).toJson(QJsonDocument::Compact).toBase64())});
  require(reader.waitForStarted(10000), "fresh settings reader did not start");
  const bool finished = reader.waitForFinished(15000);
  if (!finished) { reader.kill(); reader.waitForFinished(5000); }
  require(finished && reader.exitStatus() == QProcess::NormalExit && reader.exitCode() == 0,
          "saved-set rename did not survive a fresh process reading preferences");
  f.a->setName("renamed native channel");
  inspectModal(&f.viewer, "FunctionChannelSets", [&](QDialog *dialog) {
    auto *list = dialog->findChild<QListWidget *>("FunctionChannelSetList");
    auto *show = dialog->findChild<QPushButton *>("FunctionChannelSetShow");
    auto *status = dialog->findChild<QLabel *>("FunctionChannelSetStatus");
    require(list && show && status && list->item(0)->text() == "Body poses", "renamed set did not reopen");
    list->setCurrentRow(0); show->click();
    for (auto *channel : f.tree->channels())
      require(channel->isActive() == (channel->getParam() == f.a.getPointer()),
              "renamed set matched channel text instead of its saved identity");
    require(status->text().contains("1 saved channels are unavailable"), "missing set member was not reported");
  }, [&]() { action->trigger(); });
  for (int context = 0; context < 3; ++context) {
    inspectModal(&f.viewer, "FunctionChannelSets", [&](QDialog *dialog) {
      auto *name = dialog->findChild<QLineEdit *>("FunctionChannelSetName");
      require(name, "pending rename field disappeared"); name->setText("Canceled context name");
      if (context == 0) scene.notifySceneSwitched();
      else if (context == 1) xsheet.notifyXsheetSwitched();
      else f.model->resetAll();
      require(!dialog->isVisible(), "context replacement left saved-set rename open");
    }, [&]() { action->trigger(); });
    require(savedSets() == expected, "context replacement committed pending set rename");
  }
  Fixture captureContext;
  auto *captureAction = captureContext.viewer.findChild<QAction *>("FunctionChannelSetsAction");
  inspectModal(&captureContext.viewer, "FunctionChannelSets", [&](QDialog *dialog) {
    QObject delivery; bool canceledPrompt = false;
    QTimer::singleShot(0, &delivery, [&]() {
      auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
      if (!prompt || prompt->objectName() != "FunctionChannelSetCaptureName") return;
      prompt->setTextValue("Uncommitted capture"); captureContext.model->resetAll();
      canceledPrompt = !prompt->isVisible() && !dialog->isVisible();
    });
    dialog->findChild<QPushButton *>("FunctionChannelSetSave")->click();
    require(canceledPrompt && savedSets() == expected && captureContext.a->getWorkspaceId().empty(),
            "context replacement retained a set-capture prompt or assigned unpublished identities");
  }, [&]() { captureAction->trigger(); });
  checkpoint("saved-set rename persisted across process/reopen and canceled across contexts");
}
void nativeTargetRangeDialog() {
  Fixture f;
  auto *selection = f.viewer.getSelection();
  selection->select(f.a.getPointer(), 1); selection->select(f.a.getPointer(), 2);
  selection->select(f.b.getPointer(), 1); selection->select(f.b.getPointer(), 2);
  TDoubleParamP originalA = new TDoubleParam(*f.a), originalB = new TDoubleParam(*f.b);
  auto *edit = f.viewer.findChild<QToolButton *>("FunctionEditKeys");
  require(edit, "native target-range editor action is missing");
  auto *undo = TUndoManager::manager(); undo->reset();
  int emitted = 0;
  QObject listener;
  QObject::connect(&f.viewer, &FunctionViewer::curveChanged, &listener, [&]() { ++emitted; });
  inspectModal(&f.viewer, "FunctionKeyEditor", [&](QDialog *dialog) {
    auto *mode = dialog->findChild<QComboBox *>("FunctionEditOperation");
    auto *start = dialog->findChild<QDoubleSpinBox *>("FunctionEditFirstFrame");
    auto *end = dialog->findChild<QDoubleSpinBox *>("FunctionEditLastFrame");
    auto *preview = dialog->findChild<QPushButton *>("FunctionEditPreview");
    auto *status = dialog->findChild<QLabel *>("FunctionEditStatus");
    auto *range = dialog->findChild<QLabel *>("FunctionEditRangePreview");
    auto *buttons = dialog->findChild<QDialogButtonBox *>();
    require(mode && start && end && preview && status && range && buttons,
            "target-range modal controls are missing");
    mode->setCurrentIndex(8);
    require(start->isEnabled() && end->isEnabled() && range->isVisible() &&
                start->accessibleName() == "Target first frame", "target-range labels or inputs are unavailable");
    auto *apply = buttons->button(QDialogButtonBox::Apply);
    apply->click();
    require(undo->getHistoryCount() == 0 && emitted == 0,
            "unchanged target range created history or an edit notification");
    start->setValue(9.5); end->setValue(27.5); preview->click();
    require(Ops::matchesSnapshot(f.a.getPointer(), originalA.getPointer()) &&
                Ops::matchesSnapshot(f.b.getPointer(), originalB.getPointer()) && undo->getHistoryCount() == 0,
            "target-range Preview changed native keys/history");
    require(f.graph->comparison().size() == 2, "target-range Preview omitted a selected channel");
    apply->click();
    requireNear(f.a->keyframeIndexToFrame(1), 8.5); requireNear(f.a->keyframeIndexToFrame(2), 20.5);
    requireNear(f.b->keyframeIndexToFrame(1), 14.5); requireNear(f.b->keyframeIndexToFrame(2), 26.5);
    require(selection->getSelectedKeyframeCount() == 4 && undo->getHistoryCount() == 1,
            "target-range Apply changed selection membership or undo transaction count");
    const int notifications = emitted;
    apply->click();
    require(undo->getHistoryCount() == 1 && emitted == notifications,
            "repeated target-range Apply added history or emitted a false edit");
    start->setValue(31); end->setValue(11); preview->click(); apply->click();
    require(!status->text().isEmpty() && f.graph->comparison().empty() && undo->getHistoryCount() == 1,
            "invalid target range changed keys or retained a misleading preview");
    requireNear(f.a->keyframeIndexToFrame(1), 8.5); requireNear(f.b->keyframeIndexToFrame(2), 26.5);
    start->setValue(10.125); end->setValue(25.875); apply->click();
    requireNear(f.a->keyframeIndexToFrame(1), 9.125); requireNear(f.a->keyframeIndexToFrame(2), 19.625);
    requireNear(f.b->keyframeIndexToFrame(1), 14.375); requireNear(f.b->keyframeIndexToFrame(2), 24.875);
    require(undo->getHistoryCount() == 2 && selection->getSelectedKeyframeCount() == 4,
            "second range retime lost the original selection or undo boundary");
    start->setValue(13); end->setValue(29); preview->click();
  }, [&]() { edit->click(); });
  requireNear(f.a->keyframeIndexToFrame(1), 9.125); requireNear(f.b->keyframeIndexToFrame(2), 24.875);
  require(undo->undo(), "target-range second Apply has no undo");
  requireNear(f.a->keyframeIndexToFrame(1), 8.5); requireNear(f.b->keyframeIndexToFrame(2), 26.5);
  require(undo->undo(), "target-range first Apply has no undo");
  require(Ops::matchesSnapshot(f.a.getPointer(), originalA.getPointer()) &&
              Ops::matchesSnapshot(f.b.getPointer(), originalB.getPointer()), "target-range undo did not restore exact original curves");
  require(undo->redo() && undo->redo(), "target-range redo history is incomplete");
  requireNear(f.a->keyframeIndexToFrame(1), 9.125); requireNear(f.b->keyframeIndexToFrame(2), 24.875);
  undo->reset();
  checkpoint("target-range modal preview, repeat, cancel and undo/redo completed");
}
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
  requireNear(bounds.left(), 10); requireNear(bounds.right(), 15); requireNear(bounds.top(), 2); requireNear(bounds.bottom(), 100);
  f.graph->fitSelectedPoints();
  requireNear(f.graph->frameToX(10), 70);
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
void commandActionLifetime() {
  auto *manager = CommandManager::instance();
  const std::string shortcut = "Ctrl+Alt+F11";
  require(!manager->getAction("MI_Copy"), "standalone fixture unexpectedly owns a Copy action");
  for (int cycle = 0; cycle < 2; ++cycle) {
    Fixture f;
    auto owner = std::make_unique<QObject>();
    auto *copy = new QAction("Copy", owner.get());
    QPointer<QAction> guarded(copy);
    manager->define("MI_Copy", MenuEditCommandType, shortcut, copy);
    f.viewer.getSelection()->select(f.a.getPointer(), 1);
    require(copy->isEnabled(), "real FunctionSelection did not enable registered Copy");
    require(manager->getActionFromShortcut(shortcut) == copy,
            "Copy shortcut did not resolve to its registered action");
    // MainWindow owns actions before it owns room widgets. Reproduce that
    // destruction order while the real editor and its selection remain alive.
    owner.reset();
    require(guarded.isNull(), "action owner did not destroy Copy");
    require(!manager->getAction("MI_Copy") &&
                !manager->getActionFromShortcut(shortcut) &&
                manager->getIdFromAction(copy).empty(),
            "command registry retained a destroyed Copy action");
    std::vector<QAction *> actions;
    manager->getActions(MenuEditCommandType, actions);
    require(std::find(actions.begin(), actions.end(), copy) == actions.end(),
            "command enumeration returned a destroyed Copy action");
    require(TSelection::getCurrent() == f.viewer.getSelection(),
            "destroying a command action unexpectedly changed editor selection");
    checkpoint("registered action destroyed before current editor selection");
  }
  require(!TSelection::getCurrent(), "action-first teardown retained a current selection");

  class NamedSelection final : public TSelection {
  public:
    NamedSelection() { m_alternativeCommandNames["FunctionWorkflowNamedAction"] = "Selected keys"; }
    ~NamedSelection() override { makeNotCurrent(); }
    bool isEmpty() const override { return false; }
    void selectNone() override {}
  } selection;
  for (int cycle = 0; cycle < 2; ++cycle) {
    auto menu = std::make_unique<QMenu>();
    auto *action = menu->menuAction();
    action->setText("Default"); action->setIconText("Default");
    manager->define("FunctionWorkflowNamedAction", MenuEditCommandType,
                    "Ctrl+Alt+F10", action);
    selection.makeCurrent();
    require(action->text() == "Selected keys", "selection did not apply alternative action name");
    if (cycle == 1) {
      selection.makeNotCurrent();
      require(action->text() == "Default", "live action did not restore its default name");
      selection.makeCurrent();
    }
    QPointer<QAction> guarded(action);
    menu.reset();
    require(guarded.isNull() && !manager->getAction("FunctionWorkflowNamedAction") &&
                !manager->getActionFromShortcut("Ctrl+Alt+F10"),
            "menu destruction retained a registered menu action");
    selection.makeNotCurrent(); // old selection names may refer to dead actions
    selection.makeCurrent();    // new selection names may refer to absent actions
    selection.makeNotCurrent();
  }
  checkpoint("menu actions and alternative names survive action-first teardown");
}
void pendingShortcutLifetime() {
  auto *manager = CommandManager::instance();
  constexpr const char *id = "FunctionWorkflowReusedAction";
  struct Probe {
    int count = 0;
    bool shortcut = false;
    void invoke() { ++count; shortcut = CommandManager::instance()->executeTriggeredByShortcut(); }
  } probe;
  struct HandlerReset {
    const char *id;
    ~HandlerReset() { CommandManager::instance()->setHandler(id, nullptr); }
  } reset{id};
  // Reuse precisely the same QAction address so a stale pending-shortcut
  // pointer cannot accidentally pass this regression through allocator luck.
  std::aligned_storage<sizeof(QAction), alignof(QAction)>::type storage;
  const auto destroy = [](QAction *action) { action->~QAction(); };
  std::unique_ptr<QAction, decltype(destroy)> action(nullptr, destroy);
  action.reset(new (&storage) QAction());
  manager->define(id, MenuEditCommandType, "Ctrl+Alt+F9", action.get());
  setCommandHandler(id, &probe, &Probe::invoke);
  manager->enable(id, false);
  manager->setPendingShortcutAction(action.get());
  action.reset();
  require(!manager->getAction(id), "destroyed pending action remained registered");
  action.reset(new (&storage) QAction());
  manager->define(id, MenuEditCommandType, "Ctrl+Alt+F9", action.get());
  require(!action->isEnabled(), "action replacement discarded retained enabled state");
  manager->enable(id, true);
  require(action->isEnabled(), "action replacement discarded the retained handler");
  manager->execute(action.get());
  require(probe.count == 1 && !probe.shortcut,
          "new action inherited the destroyed action's pending shortcut dispatch");
  manager->setPendingShortcutAction(action.get());
  manager->execute(action.get());
  require(probe.count == 2 && probe.shortcut,
          "live replacement action lost legitimate shortcut dispatch");
  manager->execute(action.get());
  require(probe.count == 3 && !probe.shortcut,
          "shortcut dispatch state leaked into the next command execution");
  checkpoint("same-address action replacement retained handlers without stale shortcuts");
}
void normalizationAndLocks() {
  Fixture f; TDoubleParamP original = new TDoubleParam(*f.a);
  f.graph->setNormalized(true);
  double y = f.graph->valueToY(f.a.getPointer(), 150);
  requireNear(f.graph->yToValue(f.a.getPointer(), y), 150);
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
  requireNear(f.graph->snapFrameDelta(2.9, 10, false), 3);
  f.graph->setSnapMarkers(false); requireNear(f.graph->snapFrameDelta(2.9, 10, false), 4);
  requireNear(f.graph->snapFrameDelta(2.9, 10, true), 3);
  QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier); QApplication::sendEvent(f.graph, &right);
  requireNear(f.a->keyframeIndexToFrame(1), 14);
  require(TUndoManager::manager()->undo(), "keyboard edit has no undo");
  requireNear(f.a->keyframeIndexToFrame(1), 10);
  f.graph->setSnap(1, false);
  QApplication::sendEvent(f.graph, &right);
  requireNear(f.a->keyframeIndexToFrame(1), 11);
  require(TUndoManager::manager()->undo(), "single-frame keyboard edit has no undo");
  requireNear(f.a->keyframeIndexToFrame(1), 10);
}
void nativeBatchDialog() {
  Fixture f; auto *selection = f.viewer.getSelection();
  selection->select(f.a.getPointer(), 1); selection->select(f.a.getPointer(), 2);
  auto *button = f.viewer.findChild<QToolButton *>("FunctionEditKeys"); require(button, "native batch action missing");
  TDoubleParamP comparison = new TDoubleParam(*f.a); f.graph->setComparison({{f.a, comparison}});
  QString captureError; require(f.graph->freezeComparison(captureError), qPrintable(captureError));
  const auto frozenBefore = f.graph->comparisonState().frozen.front().samples;
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
      requireNear(f.a->getKeyframe(1).m_value, 105); requireNear(f.a->getKeyframe(2).m_value, 205);
      require(selection->getSelectedKeyframeCount() == 2, "Apply broadened the original selection");
      buttons->button(QDialogButtonBox::Apply)->click();
      requireNear(f.a->getKeyframe(1).m_value, 110); requireNear(f.a->getKeyframe(0).m_value, 0); requireNear(f.a->getKeyframe(3).m_value, 300);
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
  require(f.graph->comparisonState().frozen.size() == 1 &&
              f.graph->comparisonState().frozen.front().samples == frozenBefore,
          "Modal edit reevaluated the captured comparison display");
  requireNear(comparison->getKeyframe(1).m_value, 100);
  requireNear(f.a->getKeyframe(1).m_value, 110);
  require(TUndoManager::manager()->undo(), "native batch undo missing"); requireNear(f.a->getKeyframe(1).m_value, 105);
  require(TUndoManager::manager()->undo(), "second native batch undo missing"); requireNear(f.a->getKeyframe(1).m_value, 100);
}
void contextCancellation() {
  for (int operation : {0, 8}) for (int context = 0; context < 3; ++context) {
    TSceneHandle scene; TXsheetHandle xsheet;
    Fixture f; f.viewer.setSceneHandle(&scene); f.viewer.setXsheetHandle(&xsheet);
    TDoubleParamP original = new TDoubleParam(*f.a);
    f.graph->setComparison({{f.a, original}});
    QString captureError; require(f.graph->freezeComparison(captureError), qPrintable(captureError));
    f.viewer.getSelection()->select(f.a.getPointer(), 1);
    auto *button = f.viewer.findChild<QToolButton *>("FunctionEditKeys");
    require(button, "context cancellation editor action is missing");
    inspectModal(&f.viewer, "FunctionKeyEditor", [&](QDialog *dialog) {
      checkpoint("context cancellation dialog entered");
      dialog->findChild<QComboBox *>("FunctionEditOperation")->setCurrentIndex(operation);
      if (operation == 8) {
        dialog->findChild<QDoubleSpinBox *>("FunctionEditFirstFrame")->setValue(12.25);
        dialog->findChild<QDoubleSpinBox *>("FunctionEditLastFrame")->setValue(12.25);
        dialog->findChild<QPushButton *>("FunctionEditPreview")->click();
      } else dialog->findChild<QDoubleSpinBox *>("FunctionEditAmount")->setValue(999);
      if (context == 0) scene.notifySceneSwitched();
      else if (context == 1) xsheet.notifyXsheetSwitched();
      else f.model->resetAll();
      require(!dialog->isVisible(), "context change left a pending curve edit open");
    }, [&]() { button->click(); });
    checkpoint("context cancellation dialog closed");
    require(FunctionCurveOperations::matchesSnapshot(f.a.getPointer(), original.getPointer()), "context change committed a pending edit");
    require(f.graph->comparison().empty() && f.graph->comparisonState().frozen.empty(),
            "context cancellation retained a comparison bound to the old scene");
  }
  checkpoint("stale comparison fixture begins");
  TSceneHandle scene; Fixture f; f.viewer.setSceneHandle(&scene);
  TDoubleParamP original = new TDoubleParam(*f.a);
  auto *capture = f.viewer.findChild<QAction *>("FunctionCaptureComparison");
  auto *restore = f.viewer.findChild<QAction *>("FunctionRestoreComparison");
  require(capture && restore, "Comparison context controls are missing");
  checkpoint("stale comparison capture begins"); capture->trigger();
  checkpoint("stale comparison capture completed");
  TUndoManager::manager()->reset(); scene.setDirtyFlag(false);
  QObject delivery; bool confirmedAfterReset = false; int editNotifications = 0;
  QString confirmationFailure;
  QObject::connect(&f.viewer, &FunctionViewer::curveChanged, &delivery, [&]() { ++editNotifications; });
  QTimer deadline; deadline.setSingleShot(true);
  QObject::connect(&deadline, &QTimer::timeout, &delivery, []() { qFatal("Stale comparison confirmation timed out"); });
  QTimer::singleShot(0, &delivery, [&]() {
    checkpoint("stale comparison confirmation callback entered");
    auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    try {
      // QMessageBox window titles are intentionally ignored on macOS.
      require(box, "Expected stale comparison message box was not shown");
      require(box->parentWidget() == &f.viewer, "Stale comparison confirmation has the wrong owner");
      require(box->icon() == QMessageBox::Question && box->text() ==
                  "Restore all captured channels? Edits since capture will be replaced as one undo step.",
              "Stale comparison confirmation has the wrong question");
      require(box->standardButtons() == (QMessageBox::Yes | QMessageBox::No) &&
                  box->button(QMessageBox::Yes) && box->button(QMessageBox::Yes)->isEnabled(),
              "Stale comparison confirmation has missing or disabled answer buttons");
      checkpoint("stale comparison confirmation identity verified");
      checkpoint("stale comparison model reset begins"); f.model->resetAll();
      checkpoint("stale comparison model reset completed"); confirmedAfterReset = true;
      checkpoint("stale comparison Yes click begins"); box->button(QMessageBox::Yes)->click();
      checkpoint("stale comparison Yes click completed");
    } catch (const std::exception &problem) {
      confirmationFailure = QString::fromUtf8(problem.what());
      if (box) box->reject();
    }
  });
  checkpoint("stale comparison question begins");
  deadline.start(15000); restore->trigger(); deadline.stop();
  checkpoint("stale comparison question returned");
  require(confirmationFailure.isEmpty(), qPrintable(confirmationFailure));
  require(confirmedAfterReset && editNotifications == 0 && !scene.getDirtyFlag() && TUndoManager::manager()->getHistoryCount() == 0 &&
              f.graph->comparisonState().frozen.empty() && Ops::matchesSnapshot(f.a.getPointer(), original.getPointer()),
          "stale comparison confirmation edited or dirtied its replacement context");
  checkpoint("stale comparison context assertions completed");
}
void nativePresetImport() {
  Fixture f;
  auto *measure = new TMeasure("function_widget_preset",
                               new TUnit(L"internal", new TSimpleUnitConverter));
  TMeasureManager::instance()->add(measure);
  f.a->setMeasureName("function_widget_preset"); f.a->setWorkspaceId("preset-target-owner");
  f.a->enableCycle(true);
  for (auto *channel : f.tree->channels()) if (channel->getParam() == f.a.getPointer()) channel->setIsCurrent(true);
  TDoubleParamP original = new TDoubleParam(*f.a);
  QTemporaryDir sourceDirectory; require(sourceDirectory.isValid(), "preset source storage unavailable");
  TDoubleParamP raw = new TDoubleParam;
  raw->setWorkspaceId("foreign-owner-must-not-transfer");
  raw->setKeyframe(TDoubleKeyframe(2, 2)); raw->setKeyframe(TDoubleKeyframe(12, 6));
  const QString nativeFile = sourceDirectory.filePath("existing.curve");
  { TOStream output{TFilePath(nativeFile)}; raw->saveData(output); }
  auto driven = raw->getKeyframe(0); driven.m_type = TDoubleKeyframe::Expression; driven.m_expressionText = "frame";
  raw->setKeyframe(0, driven);
  const QString drivenFile = sourceDirectory.filePath("driven.curve");
  { TOStream output{TFilePath(drivenFile)}; raw->saveData(output); }
  raw->setKeyframe(0, TDoubleKeyframe(2, 2)); raw->enableCycle(true);
  const QString cyclicFile = sourceDirectory.filePath("cyclic.curve");
  { TOStream output{TFilePath(cyclicFile)}; raw->saveData(output); }
  const QString library = (ToonzFolder::getMyModuleDir() + TFilePath("function-presets")).getQString();
  const QString published = QDir(library).filePath("Imported native.fcurve");
  auto *action = f.viewer.findChild<QAction *>("FunctionMotionPresetsAction"); require(action, "Motion Presets action missing");
  TUndoManager::manager()->reset();
  auto importFile = [&](QDialog *dialog, const QString &path, int cancelStage,
                        const QString &presetName = QString("Imported native")) {
    QTimer driver; bool selectedFile = false, named = false; QString failure;
    QObject::connect(&driver, &QTimer::timeout, dialog, [&]() {
      try {
        if (auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
          if (selectedFile) return;
          require(file->objectName() == "FunctionPresetImportFile", "unexpected preset file dialog");
          selectedFile = true;
          if (cancelStage == 1) { file->reject(); return; }
          const QFileInfo source(path);
          require(source.isFile(), "native preset source file is missing");
          file->setDirectory(source.absolutePath());
          auto *filename = file->findChild<QLineEdit *>("fileNameEdit");
          auto *buttons = file->findChild<QDialogButtonBox *>();
          auto *open = buttons ? buttons->button(QDialogButtonBox::Open) : nullptr;
          require(filename && open, "native preset file chooser controls are missing");
          // Qt leaves a visible focused filename field unchanged in
          // selectFile(). Enter the path through that field, as an artist
          // would, and retain QFileDialog's ordinary ExistingFile validation.
          filename->setFocus(); filename->setText(source.absoluteFilePath());
          const QStringList selected = file->selectedFiles();
          require(selected.size() == 1 &&
                      QFileInfo(selected.front()).canonicalFilePath() == source.canonicalFilePath() &&
                      open->isEnabled(),
                  "native preset file chooser did not select the exact existing source");
          checkpoint("native preset file path verified; opening chooser");
          QTimer::singleShot(0, file, [open]() { open->click(); });
        } else if (auto *name = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
          if (named) return;
          require(name->objectName() == "FunctionPresetImportName" && name->labelText().contains("current channel"),
                  "native curve import did not disclose its measure interpretation");
          named = true;
          if (cancelStage == 2) name->reject();
          else { name->setTextValue(presetName); name->accept(); }
        }
      } catch (const std::exception &problem) {
        failure = QString::fromUtf8(problem.what());
        if (auto *active = qobject_cast<QDialog *>(QApplication::activeModalWidget())) active->reject();
      }
    });
    driver.start(5); dialog->findChild<QPushButton *>("FunctionPresetImport")->click(); driver.stop();
    require(failure.isEmpty() && selectedFile, qPrintable(failure));
    return named;
  };
  inspectModal(&f.viewer, "FunctionMotionPresets", [&](QDialog *dialog) {
    auto *list = dialog->findChild<QListWidget *>("FunctionPresetList");
    auto *status = dialog->findChild<QLabel *>("FunctionPresetStatus");
    require(list && status && list->count() == 0, "preset fixture did not start with an empty library");
    importFile(dialog, nativeFile, 1); importFile(dialog, nativeFile, 2);
    require(!QFileInfo::exists(published) && list->count() == 0 && Ops::matchesSnapshot(f.a.getPointer(), original.getPointer()),
            "canceled preset import changed the library or native destination");
    require(!importFile(dialog, drivenFile, 0) && list->count() == 0 && !QFileInfo::exists(published),
            "driven native curve was silently imported as numeric data");
    require(importFile(dialog, nativeFile, 0) && list->count() == 1 && QFileInfo::exists(published),
            "preexisting raw native curve could not be imported into the browser");
    require(Ops::matchesSnapshot(f.a.getPointer(), original.getPointer()) && TUndoManager::manager()->getHistoryCount() == 0,
            "importing a preset changed destination animation/history");
    QFile file(published); require(file.open(QIODevice::ReadOnly), "imported preset was not readable");
    const QByteArray bytes = file.readAll(); file.close();
    require(!bytes.contains("foreign-owner"), "native curve import persisted a foreign owner identity");
    importFile(dialog, nativeFile, 0);
    require(file.open(QIODevice::ReadOnly) && file.readAll() == bytes && list->count() == 1,
            "duplicate native preset import overwrote the previous file"); file.close();
    auto *search = dialog->findChild<QLineEdit *>("FunctionPresetSearch");
    search->setText("absent"); require(list->item(0)->isHidden(), "preset search did not filter imported native curve");
    search->setText("native"); require(!list->item(0)->isHidden(), "preset search did not restore imported curve"); list->setCurrentRow(0);
    dialog->findChild<QCheckBox *>("FunctionPresetFavorite")->setChecked(true);
    dialog->findChild<QDoubleSpinBox *>("FunctionPresetFirstFrame")->setValue(21);
    dialog->findChild<QDoubleSpinBox *>("FunctionPresetDuration")->setValue(8);
    dialog->findChild<QDoubleSpinBox *>("FunctionPresetAmplitude")->setValue(2);
    dialog->findChild<QDoubleSpinBox *>("FunctionPresetOffset")->setValue(3);
    dialog->findChild<QPushButton *>("FunctionPresetPreview")->click();
    require(Ops::matchesSnapshot(f.a.getPointer(), original.getPointer()) && f.graph->comparison().size() == 1,
            "native imported preset preview mutated destination or omitted overlay");
    auto *apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply);
    f.a->setEditorLocked(true); apply->click();
    require(Ops::matchesSnapshot(f.a.getPointer(), original.getPointer()) && TUndoManager::manager()->getHistoryCount() == 0,
            "native preset Apply ignored the destination lock");
    f.a->setEditorLocked(false); apply->click();
    require(f.a->getKeyframeCount() == 2 && f.a->getWorkspaceId() == "preset-target-owner" &&
                f.a->getMeasureName() == "function_widget_preset" && !f.a->isCycleEnabled() &&
                TUndoManager::manager()->getHistoryCount() == 1,
            "native imported preset lost owner/units/history or inherited the destination's cycle flag");
    requireNear(f.a->keyframeIndexToFrame(0), 20); requireNear(f.a->keyframeIndexToFrame(1), 28);
    requireNear(f.a->getKeyframe(0).m_value, 5); requireNear(f.a->getKeyframe(1).m_value, 13);
    apply->click(); require(TUndoManager::manager()->getHistoryCount() == 1, "repeated identical preset Apply added history");
    require(importFile(dialog, cyclicFile, 0, "Imported cyclic") && list->count() == 2 && !f.a->isCycleEnabled(),
            "cyclic preset import changed destination animation before Apply");
    require(search->text().isEmpty() && list->currentItem() && !list->currentItem()->isHidden() &&
                list->currentItem()->data(Qt::UserRole).toString() == "Imported cyclic.fcurve",
            "import selected a hidden preset behind the previous search filter");
    apply->click(); require(f.a->isCycleEnabled(), "cyclic raw curve lost its explicit cycle setting");
    require(TUndoManager::manager()->undo() && !f.a->isCycleEnabled(), "cyclic preset undo did not restore the prior noncyclic animation");
    dialog->findChild<QDoubleSpinBox *>("FunctionPresetOffset")->setValue(999);
    dialog->findChild<QPushButton *>("FunctionPresetPreview")->click();
  }, [&]() { action->trigger(); });
  requireNear(f.a->getKeyframe(1).m_value, 13);
  require(TUndoManager::manager()->undo() && Ops::matchesSnapshot(f.a.getPointer(), original.getPointer()),
          "native imported preset undo did not restore the exact destination");
  require(TUndoManager::manager()->redo(), "native imported preset redo missing"); requireNear(f.a->getKeyframe(1).m_value, 13);
  inspectModal(&f.viewer, "FunctionMotionPresets", [&](QDialog *dialog) {
    auto *list = dialog->findChild<QListWidget *>("FunctionPresetList");
    require(list && list->count() == 2, "imported native presets did not survive reopening"); list->setCurrentRow(0);
    require(dialog->findChild<QCheckBox *>("FunctionPresetFavorite")->isChecked(), "native preset favorite did not persist");
    QObject delivery; bool canceledFile = false;
    QTimer::singleShot(0, &delivery, [&]() {
      auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
      if (!file || file->objectName() != "FunctionPresetImportFile") return;
      f.model->resetAll();
      canceledFile = !file->isVisible() && !dialog->isVisible();
    });
    dialog->findChild<QPushButton *>("FunctionPresetImport")->click();
    require(canceledFile && QFileInfo::exists(published),
            "context replacement left the native import modal active or removed the saved preset");
  }, [&]() { action->trigger(); });
  TUndoManager::manager()->reset();
  Fixture saveContext;
  auto *saveAction = saveContext.viewer.findChild<QAction *>("FunctionMotionPresetsAction");
  const QString canceledFile = QDir(library).filePath("Uncommitted save.fcurve");
  inspectModal(&saveContext.viewer, "FunctionMotionPresets", [&](QDialog *dialog) {
    QObject delivery; bool canceledPrompt = false;
    QTimer::singleShot(0, &delivery, [&]() {
      auto *prompt = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
      if (!prompt || prompt->objectName() != "FunctionPresetSaveName") return;
      prompt->setTextValue("Uncommitted save"); saveContext.model->resetAll();
      canceledPrompt = !prompt->isVisible() && !dialog->isVisible();
    });
    dialog->findChild<QPushButton *>("FunctionPresetSave")->click();
    require(canceledPrompt && !QFileInfo::exists(canceledFile),
            "context replacement retained a preset-save prompt or published its pending name");
  }, [&]() { saveAction->trigger(); });
  checkpoint("native curve preset import, adaptation, cancellation and persistence completed");
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
  requireNear(f.a->getValue(10011.5), 11.5);
}
}
int main(int argc, char **argv) {
  if (argc == 4 && std::string(argv[1]) == "--verify-channel-sets") {
    QCoreApplication reader(argc, argv);
    const auto arguments = reader.arguments();
    const QJsonArray expected = QJsonDocument::fromJson(
        QByteArray::fromBase64(arguments[3].toLatin1())).array();
    QSettings settings(arguments[2], QSettings::IniFormat); settings.sync();
    const QJsonArray observed = QJsonDocument::fromJson(settings.value("channelSets").toByteArray()).array();
    if (expected.isEmpty() || settings.status() != QSettings::NoError || observed != expected) {
      std::cerr << "Fresh process did not recover the exact saved channel sets\n"; return 1;
    }
    return 0;
  }
  checkpoint("application initialization begins");
#ifdef Q_OS_WIN
  // The Windows QMessageBox implementation dereferences the platform native
  // interface, which the Qt 5 offscreen plugin does not implement.
  qputenv("QT_QPA_PLATFORM", "windows");
#else
  qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
  QApplication application(argc, argv); TThread::init();
  std::cerr << "function-workflow: qt=" << qVersion()
            << " platform=" << QApplication::platformName().toStdString()
            << " native-interface=" << bool(QApplication::platformNativeInterface()) << std::endl;
  try {
    QTemporaryDir directory; require(directory.isValid(), "temporary directory unavailable");
    QDir().mkpath(directory.filePath("stuff/config")); QDir().mkpath(directory.filePath("stuff/profiles"));
    TEnv::setStuffDir(TFilePath(directory.filePath("stuff")));
    require(TEnv::setArgPathValue(TEnv::getSystemVarPrefix() + "PROFILES",
                                  directory.filePath("stuff/profiles").toStdString()),
            "cannot isolate Function Editor profile settings");
    const QString module = QDir::fromNativeSeparators(ToonzFolder::getMyModuleDir().getQString());
    require(module.startsWith(QDir::fromNativeSeparators(directory.path()) + "/") && QDir().mkpath(module),
            "Function Editor settings escaped disposable fixture storage");
    checkpoint("scene-free views begin"); sceneFreeViews();
    checkpoint("queued model lifetime begins"); queuedModelLifetime();
    checkpoint("fit/search begin"); fitAndSearch();
    checkpoint("selection lifetime begins"); selectionLifetime();
    checkpoint("command action lifetime begins"); commandActionLifetime();
    checkpoint("pending shortcut lifetime begins"); pendingShortcutLifetime();
    checkpoint("normalization/locks begin"); normalizationAndLocks();
    checkpoint("batch editing begins"); nativeBatchDialog();
    checkpoint("saved-set rename begins"); savedSetRename();
    checkpoint("target-range modal begins"); nativeTargetRangeDialog();
    checkpoint("context cancellation begins"); contextCancellation();
    checkpoint("native preset import begins"); nativePresetImport();
    checkpoint("dense graph measurement begins"); denseGraphMeasurement();
    checkpoint("all workflow checks completed");
    TUndoManager::manager()->reset(); std::cout << "Native Function Editor workflow checks passed\n"; return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
