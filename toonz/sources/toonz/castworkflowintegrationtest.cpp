#include "castviewer.h"
#include "castselection.h"
#include "castbrowseractions.h"
#include "nativeuiintegrationtest.h"
#include "tapp.h"
#include "iocommand.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/childstack.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/levelproperties.h"
#include "toonz/sceneresources.h"
#include "toonz/tproject.h"
#include "toonzqt/menubarcommand.h"
#include "menubarcommandids.h"
#include "trasterimage.h"
#include "tpalette.h"
#include "tvectorimage.h"
#include "tundo.h"
#include <QApplication>
#include <QComboBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QMainWindow>
#include <QTimer>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTableWidget>
#include <QSlider>
#include <QFileDialog>
#include <QMessageBox>
#include <QImage>
#include <QTemporaryDir>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <functional>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScrollBar>
#include <QDirIterator>
#include <QKeyEvent>
#include <QMenu>
#include <QProgressDialog>
#include <QPointer>
#include <stdexcept>
#include <iostream>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int find(CastBrowser &browser, const QString &name) {
  for (int i = 0; i < browser.getItemCount(); ++i)
    if (browser.getItemData(i, DvItemListModel::Name).toString() == name) return i;
  return -1;
}
// Real modal widgets, driven within the disposable application profile.
// A deadline closes a missed dialog so a CI failure is reported rather than hung.
class DialogDriver {
  QTimer timer, deadline;
public:
  bool expired = false;
  explicit DialogDriver(const std::function<void(QDialog *)> &action) {
    QObject::connect(&timer, &QTimer::timeout, [action]() {
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) action(dialog);
    });
    QObject::connect(&deadline, &QTimer::timeout, [this]() {
      expired = true;
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
    });
    timer.start(30); deadline.setSingleShot(true); deadline.start(15000);
  }
};
void showProgressNextTurn(const QString &label) {
  QTimer::singleShot(0, [label]() {
    for (auto *widget : QApplication::topLevelWidgets())
      if (auto *progress = qobject_cast<QProgressDialog *>(widget))
        if (progress->labelText() == label) progress->show();
  });
}
void selectionLifetime(QMainWindow *window) {
  QPointer<DvItemSelection> owned;
  {
    CastBrowser current(window);
    auto *panel = current.findChild<DvItemViewer *>()->getPanel();
    owned = panel->getSelection();
    owned->makeCurrent();
    require(TSelection::getCurrent() == owned.data(), "Cast selection did not become current");
    {
      CastBrowser inactive(window);
    }
    require(TSelection::getCurrent() == owned.data(), "closing an inactive Cast cleared the active selection");
    panel->setSelection(new DvItemSelection);
    require(owned.isNull() && !TSelection::getCurrent(),
            "replacing a Cast selection retained a stale global handle");
    owned = panel->getSelection();
    owned->makeCurrent();
  }
  require(owned.isNull() && !TSelection::getCurrent(),
          "closing Cast retained its selection or command handlers");
  {
    CastBrowser reopened(window);
    auto *selection = reopened.findChild<DvItemViewer *>()->getPanel()->getSelection();
    selection->makeCurrent();
    require(TSelection::getCurrent() == selection, "reopened Cast could not acquire selection");
  }
  require(!TSelection::getCurrent(), "reopened Cast left a stale global selection");
}
}

// Runs only in the existing disposable-profile native UI qualification process.
void runCastWorkflowIntegrationTest(QMainWindow *window) {
  QString resetError;
  require(resetNativeUiIntegrationScene(&resetError), qPrintable(resetError));
  QApplication::processEvents();
  selectionLifetime(window);
  auto *source = TApp::instance()->getCurrentScene()->getScene();
  auto *hero = source->createNewLevel(PLI_XSHLEVEL, L"Cast fixture Hero")->getSimpleLevel();
  auto *spare = source->createNewLevel(PLI_XSHLEVEL, L"Cast fixture Spare")->getSimpleLevel();
  hero->setFrame(TFrameId(1), new TVectorImage);
  spare->setFrame(TFrameId(1), new TVectorImage);
  const TFilePath folder = source->getLevelSet()->createFolder(source->getLevelSet()->getDefaultFolder(), L"Cast fixture folder");
  source->getLevelSet()->moveLevelToFolder(folder, hero);
  hero->setCastTags({"hero", QString::fromUtf8("café")});

  CastBrowser browser(window);
  browser.resize(900, 600); browser.show();
  QApplication::processEvents();
  auto *scope = browser.findChild<QComboBox *>("CastSearchScope");
  auto *search = browser.findChild<QLineEdit *>("CastSearch");
  auto *status = browser.findChild<QComboBox *>("CastStatusFilter");
  auto *type = browser.findChild<QComboBox *>("CastTypeFilter");
  auto *viewer = browser.findChild<DvItemViewer *>();
  require(scope && search && status && type && viewer, "native Cast controls missing");
  scope->setCurrentIndex(1); browser.refreshData();
  int heroIndex = find(browser, "Cast fixture Hero");
  require(heroIndex >= 0, "global scope lost nested Cast folder");
  viewer->getPanel()->getSelection()->select(heroIndex);
  browser.sortByDataModel(DvItemListModel::Name, false);
  std::vector<TXshLevel *> selected;
  static_cast<CastSelection *>(viewer->getPanel()->getSelection())->getSelectedLevels(selected);
  require(selected.size() == 1 && selected.front() == hero, "sorting changed selected asset identity");
  search->setText("Spare"); browser.refreshData();
  require(find(browser, "Cast fixture Hero") < 0 && viewer->getSelectedIndices().empty(), "filtered-out selection remains actionable");
  search->setText(QString::fromUtf8("hero café")); browser.refreshData();
  require(browser.getItemCount() == 1 && !viewer->getSelectedIndices().empty(), "tag search did not restore hidden identity selection");
  type->setCurrentIndex(type->findData(SND_XSHLEVEL)); browser.refreshData();
  require(browser.getItemCount() == 0, "audio filter includes vector asset");
  type->setCurrentIndex(0); status->setCurrentIndex(3); browser.refreshData();
  require(browser.getItemCount() == 1, "unsaved vector is absent from missing-on-disk filter");
  status->setCurrentIndex(0);

  // A pending search refresh inside a menu must not leave hidden assets
  // actionable through a previously captured raw selection.
  search->clear(); browser.refreshData();
  viewer->getPanel()->getSelection()->selectNone();
  viewer->getPanel()->getSelection()->select(find(browser, "Cast fixture Hero"));
  bool staleTagDialog = false, menuTriggered = false;
  QTimer menuWatch;
  QObject::connect(&menuWatch, &QTimer::timeout, [&]() {
    if (auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
      staleTagDialog = true; input->reject();
    }
  });
  menuWatch.start(30);
  QTimer::singleShot(0, [&]() {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu) return;
    search->setText("Spare"); browser.refreshData();
    for (auto *action : menu->actions()) if (action->text() == "Edit Tags…") {
      menuTriggered = true; action->trigger(); break;
    }
    menu->close();
  });
  QTimer::singleShot(5000, &browser, []() {
    if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) menu->close();
  });
  for (auto *button : browser.findChildren<QPushButton *>())
    if (button->text() == "Asset Actions…") { button->click(); break; }
  menuWatch.stop();
  require(menuTriggered && !staleTagDialog, "menu action retained a filtered-out selection");
  search->clear(); browser.refreshData();

  const auto before = hero->getCastTags();
  QTimer::singleShot(0, []() {
    for (QWidget *widget : QApplication::topLevelWidgets())
      if (auto *dialog = qobject_cast<QInputDialog *>(widget)) {
        dialog->setTextValue("reviewed, keep, A&B"); dialog->accept();
      }
  });
  CastBrowserActions::editTags(&browser, {hero, spare});
  require(hero->getCastTags() == spare->getCastTags() && hero->getCastTags().contains("A&B"), "native multi-asset tag edit failed");
  TUndoManager::manager()->undo();
  require(hero->getCastTags() == before && spare->getCastTags().isEmpty(), "tag undo lost per-level state");
  TUndoManager::manager()->redo();
  require(spare->getCastTags().contains("keep"), "tag redo failed");
  QTimer::singleShot(0, []() {
    for (QWidget *widget : QApplication::topLevelWidgets())
      if (auto *dialog = qobject_cast<QInputDialog *>(widget)) dialog->reject();
  });
  const int history = TUndoManager::manager()->getHistoryCount();
  CastBrowserActions::editTags(&browser, {hero});
  require(TUndoManager::manager()->getHistoryCount() == history, "cancel created tag undo entry");

  // Folder renaming restores membership and descendants through the native undo stack.
  auto *tree = browser.findChild<CastTreeViewer *>();
  require(tree, "Cast folder tree missing");
  tree->rebuildCastTree();
  auto folders = tree->findItems("Cast fixture folder", Qt::MatchExactly | Qt::MatchRecursive);
  require(folders.size() == 1, "Cast nested folder is absent from tree");
  tree->setCurrentItem(folders.front()); folders.front()->setSelected(true);
  folders.front()->setText(0, "Cast renamed folder");
  require(source->getLevelSet()->getFolder(hero).getWideName() == L"Cast renamed folder", "folder rename lost membership");
  TUndoManager::manager()->undo();
  require(source->getLevelSet()->getFolder(hero) == folder, "folder undo lost original membership");
  TUndoManager::manager()->redo();
  require(source->getLevelSet()->getFolder(hero).getWideName() == L"Cast renamed folder", "folder redo failed");

  // Native New Folder also permits custom top-level folders under the scene.
  tree->setCurrentItem(tree->topLevelItem(0));
  tree->newFolder();
  const TFilePath topFolder = tree->getCurrentFolder();
  if (auto *focus = QApplication::focusWidget()) {
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(focus, &escape);
  }
  auto hasFolder = [&](const TFilePath &wanted) {
    std::vector<TFilePath> roots; source->getLevelSet()->listFolders(roots, TFilePath());
    return std::find(roots.begin(), roots.end(), wanted) != roots.end();
  };
  require(!topFolder.isEmpty() && topFolder.getParentDir().isEmpty() && hasFolder(topFolder), "native top-level folder creation failed");
  TUndoManager::manager()->undo(); require(!hasFolder(topFolder), "top-level create undo left the folder behind");
  TUndoManager::manager()->redo(); require(hasFolder(topFolder), "top-level create redo failed");
  source->getLevelSet()->moveLevelToFolder(topFolder, spare);
  tree->rebuildCastTree();
  auto roots = tree->findItems(QString::fromStdWString(topFolder.getWideName()), Qt::MatchExactly | Qt::MatchRecursive);
  require(roots.size() == 1, "new top-level folder missing from tree");
  tree->setCurrentItem(roots.front()); roots.front()->setSelected(true);
  roots.front()->setText(0, "Cast top renamed");
  TUndoManager::manager()->undo();
  require(hasFolder(topFolder) && source->getLevelSet()->getFolder(spare) == topFolder, "top-level rename undo lost membership");
  TUndoManager::manager()->redo();
  roots = tree->findItems("Cast top renamed", Qt::MatchExactly | Qt::MatchRecursive);
  require(roots.size() == 1, "renamed top-level folder missing");
  tree->setCurrentItem(roots.front());
  {
    DialogDriver driver([](QDialog *dialog) {
      for (auto *button : dialog->findChildren<QPushButton *>())
        if (QString(button->text()).remove('&') == "Yes") { button->click(); return; }
    });
    tree->deleteFolder();
    require(!driver.expired && !hasFolder(TFilePath("Cast top renamed")), "top-level folder delete failed");
  }
  TUndoManager::manager()->undo();
  require(hasFolder(TFilePath("Cast top renamed")) &&
          source->getLevelSet()->getFolder(spare) == TFilePath("Cast top renamed"), "top-level folder delete undo lost membership");

  // Three distinct exposed instances share one child; navigate specifically to the second run.
  auto *child = source->createNewLevel(CHILD_XSHLEVEL, L"Cast fixture Shared")->getChildLevel();
  child->getXsheet()->setCell(3, 2, TXshCell(hero, TFrameId(1)));
  auto *top = source->getTopXsheet();
  const int column = top->getColumnCount();
  top->setCell(10, column, TXshCell(child, TFrameId(4)));
  top->setCell(20, column, TXshCell(child, TFrameId(4)));
  top->setCell(0, column + 1, TXshCell(child, TFrameId(4)));
  {
    bool selectedInstance = false;
    DialogDriver driver([&](QDialog *dialog) {
      if (dialog->objectName() != "CastWhereUsed") return;
      auto *table = dialog->findChild<QTableWidget *>();
      if (!table || table->rowCount() != 3) { dialog->reject(); return; }
      table->selectRow(1);
      for (auto *button : dialog->findChildren<QPushButton *>())
        if (button->text() == "Go To Exposure") { selectedInstance = true; button->click(); return; }
    });
    CastBrowserActions::whereUsed(&browser, {hero});
    require(!driver.expired && selectedInstance, "native Where Used did not list shared instances");
  }
  require(source->getChildStack()->getAncestorCount() == 1 &&
          source->getChildStack()->getAncestorInfo(0)->m_row == 20 &&
          source->getChildStack()->getAncestorInfo(0)->m_col == column &&
          TApp::instance()->getCurrentXsheet()->getXsheet() == child->getXsheet() &&
          TApp::instance()->getCurrentFrame()->getFrame() == 3 &&
          TApp::instance()->getCurrentColumn()->getColumnIndex() == 2,
          "Where Used opened the wrong child instance or local exposure");
  CommandManager::instance()->execute(MI_CloseChild);

  hero->setFrame(TFrameId(2), new TVectorImage);
  {
    bool scrubbed = false;
    DialogDriver driver([&](QDialog *dialog) {
      if (dialog->objectName() != "CastInspector") return;
      auto *slider = dialog->findChild<QSlider *>();
      scrubbed = slider && slider->isEnabled() && slider->maximum() == 1;
      if (slider) slider->setValue(1);
      dialog->reject();
    });
    const int count = TUndoManager::manager()->getHistoryCount();
    CastBrowserActions::inspect(&browser, {hero});
    require(scrubbed && !driver.expired && TUndoManager::manager()->getHistoryCount() == count,
            "Inspector scrubbing changed the scene or missed drawings");
  }
  hero->setFrame(TFrameId(5), new TVectorImage);
  {
    bool details = false;
    DialogDriver driver([&](QDialog *dialog) {
      if (dialog->objectName() != "CastInspector") return;
      auto *slider = dialog->findChild<QSlider *>();
      auto *view = dialog->findChild<QTableWidget *>();
      details = view && view->item(0, 3)->text().contains("3–4") && view->item(0, 5)->text().contains("styles");
      if (slider) slider->setValue(2);
      if (auto *choose = dialog->findChild<QPushButton *>("CastChooseCover")) choose->click();
      dialog->reject();
    });
    CastBrowserActions::inspect(&browser, {hero});
    require(details && hero->hasCastCover() && hero->getCastCover() == TFrameId(5),
            "inspector gaps/palette details or cover choice failed");
    TUndoManager::manager()->undo();
    require(!hero->hasCastCover(), "cover undo did not restore default cover");
    TUndoManager::manager()->redo();
    require(hero->getCastCover() == TFrameId(5), "cover redo failed");
  }
  {
    bool reviewed = false;
    DialogDriver driver([&](QDialog *dialog) {
      if (dialog->objectName() != "CastUnusedReview") return;
      auto *table = dialog->findChild<QTableWidget *>();
      if (!table) { dialog->reject(); return; }
      for (int row = 0; row < table->rowCount(); ++row) {
        const bool target = table->item(row, 1)->text() == "Cast fixture Spare";
        if (target) reviewed = table->item(row, 0)->checkState() == Qt::Unchecked;
        table->item(row, 0)->setCheckState(target ? Qt::Checked : Qt::Unchecked);
      }
      dialog->accept();
    });
    CastBrowserActions::removeUnused(&browser);
    require(reviewed && !source->getLevelSet()->getLevel(L"Cast fixture Spare"),
            "unused preview ignored keep tag or failed reviewed removal");
    TUndoManager::manager()->undo();
    require(source->getLevelSet()->getLevel(L"Cast fixture Spare") == spare, "unused removal undo changed asset identity");
  }

  // Saved raster relink: actual file discovery, review, pixel change, undo and redo.
  QTemporaryDir assets;
  require(assets.isValid(), "no temporary asset directory");
  const QString oldDir = assets.path() + "/old", newDir = assets.path() + "/replacement";
  QDir().mkpath(oldDir); QDir().mkpath(newDir);
  QImage picture(8, 8, QImage::Format_ARGB32);
  picture.fill(Qt::red); require(picture.save(oldDir + "/asset.png"), "cannot save red fixture");
  picture.fill(Qt::blue); require(picture.save(newDir + "/asset.png"), "cannot save blue fixture");
  auto *raster = source->createNewLevel(OVL_XSHLEVEL, L"Cast fixture raster")->getSimpleLevel();
  raster->setPath(TFilePath(oldDir + "/asset.png")); raster->load();
  raster->getProperties()->setDirtyFlag(false);
  if (raster->getPalette()) raster->getPalette()->setDirtyFlag(false);
  std::vector<TFrameId> ids; raster->getFids(ids);
  require(!ids.empty(), "saved raster fixture has no drawings");
  auto pixel = [&]() {
    TRasterImageP image = raster->getFrame(ids.front(), false);
    TRaster32P pixels = image ? image->getRaster() : TRasterP();
    require(bool(pixels), "relinked raster cannot be read");
    return pixels->pixels(0)[0];
  };
  require(pixel().r == 255, "source fixture pixel is incorrect");
  const bool nativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
  {
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        files->setDirectory(newDir); files->selectFile(newDir); dialog->accept();
      } else if (dialog->objectName() == "CastRelinkReview") {
        dialog->accept();
        showProgressNextTurn("Validating replacement drawings…");
      } else if (qobject_cast<QMessageBox *>(dialog)) dialog->reject();
    });
    CastBrowserActions::relink(&browser, {raster});
    require(!driver.expired && pixel().b == 255, "batch relink did not replace drawing contents");
  }
  TUndoManager::manager()->undo();
  require(pixel().r == 255, "relink undo did not restore original pixels");
  TUndoManager::manager()->redo();
  require(pixel().b == 255, "relink redo did not restore replacement pixels");
  raster->invalidateFrames();
  require(pixel().b == 255, "relink retained the original file builder after cache invalidation");
  // Undo a relink that started in Scanned state, then switch to the inactive
  // Normal state and evict the cache. Both source builders must be restored.
  raster->setScannedPath(TFilePath(oldDir + "/asset.png"));
  raster->setFrameStatus(ids.front(), TXshSimpleLevel::Scanned);
  raster->rebindFrameSources();
  require(pixel().r == 255, "scanned fixture source is incorrect");
  {
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        files->setDirectory(oldDir); files->selectFile(oldDir); dialog->accept();
      } else if (dialog->objectName() == "CastRelinkReview") {
        dialog->accept();
        showProgressNextTurn("Validating replacement drawings…");
      } else if (qobject_cast<QMessageBox *>(dialog)) dialog->reject();
    });
    CastBrowserActions::relink(&browser, {raster});
    require(!driver.expired && raster->getFrameStatus(ids.front()) == TXshSimpleLevel::Normal,
            "relink did not prepare normal replacement drawing state");
  }
  TUndoManager::manager()->undo();
  require(raster->getFrameStatus(ids.front()) == TXshSimpleLevel::Scanned && pixel().r == 255,
          "relink undo lost scanned state");
  raster->setFrameStatus(ids.front(), TXshSimpleLevel::Normal); raster->invalidateFrames();
  require(pixel().b == 255, "inactive Normal builder retained replacement after scanned undo");
  raster->setScannedPath(TFilePath()); raster->rebindFrameSources();

  TXshSimpleLevelP missing = new TXshSimpleLevel(L"Cast fixture missing");
  missing->setScene(source); missing->setType(OVL_XSHLEVEL);
  missing->setPath(TFilePath(assets.path() + "/missing/asset.png"), true);
  *missing->getProperties() = *raster->getProperties();
  missing->setPalette(raster->getPalette());
  missing->setFrame(ids.front(), raster->getFrame(ids.front(), false)->cloneImage());
  source->getLevelSet()->insertLevel(missing.getPointer());
  {
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        files->setDirectory(oldDir); files->selectFile(oldDir); dialog->accept();
      } else if (dialog->objectName() == "CastRelinkReview") {
        dialog->accept();
        showProgressNextTurn("Validating replacement drawings…");
      } else if (qobject_cast<QMessageBox *>(dialog)) dialog->reject();
    });
    CastBrowserActions::relink(&browser, {missing.getPointer()});
    require(!driver.expired && source->decodeFilePath(missing->getPath()) == TFilePath(oldDir + "/asset.png"),
            "missing original could not be relinked");
  }
  TUndoManager::manager()->undo();
  TRasterImageP restoredMissing = missing->getFrame(ids.front(), false);
  require(restoredMissing && TRaster32P(restoredMissing->getRaster())->pixels(0)[0].b == 255,
          "missing-original undo discarded previously cached drawing");
  missing->invalidateFrames();
  require(!missing->getFrame(ids.front(), false), "missing-original undo retained replacement builder after eviction");
  {
    DialogDriver driver([](QDialog *dialog) { dialog->reject(); });
    const int count = TUndoManager::manager()->getHistoryCount();
    CastBrowserActions::relink(&browser, {raster});
    require(TUndoManager::manager()->getHistoryCount() == count, "canceling relink created an undo entry");
  }

  // Local copies use a new reviewed folder and undo only changes links.
  const QString localDir = assets.path() + "/local";
  QDir().mkpath(localDir);
  source->setScenePath(TFilePath(localDir + "/fixture.tnz"));
  const TFilePath linked = raster->getPath();
  require(CastBrowserActions::external(source, raster), "external asset was classified as project owned");
  {
    const int count = TUndoManager::manager()->getHistoryCount();
    const auto folders = QDir(localDir).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    bool mutated = false, rejected = false;
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        files->setDirectory(localDir); files->selectFile(localDir); dialog->accept();
        showProgressNextTurn("Preparing local asset copies…");
      } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
        if (message->standardButtons().testFlag(QMessageBox::Yes)) {
          mutated = true;
          raster->getProperties()->setDirtyFlag(true);
          TApp::instance()->getCurrentLevel()->notifyLevelChange();
          message->done(QMessageBox::Yes);
        } else { rejected = message->text().contains("changed during preparation"); message->reject(); }
      }
    });
    CastBrowserActions::makeLocal(&browser, {raster});
    require(mutated && rejected && !driver.expired && raster->getPath() == linked &&
            TUndoManager::manager()->getHistoryCount() == count &&
            QDir(localDir).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot) == folders,
            "stale local-copy preparation overwrote an edit or left staging/publication files");
    raster->getProperties()->setDirtyFlag(false);
  }
  {
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        files->setDirectory(localDir); files->selectFile(localDir); dialog->accept();
        showProgressNextTurn("Preparing local asset copies…");
      } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
        if (message->standardButtons().testFlag(QMessageBox::Yes)) message->done(QMessageBox::Yes);
        else message->reject();
      }
    });
    CastBrowserActions::makeLocal(&browser, {raster});
    require(!driver.expired && !CastBrowserActions::external(source, raster), "make local copy did not change ownership");
  }
  const TFilePath copied = source->decodeFilePath(raster->getPath());
  require(QFileInfo::exists(copied.getQString()) && pixel().b == 255, "local copy is unreadable");
  TUndoManager::manager()->undo();
  require(raster->getPath() == linked && QFileInfo::exists(copied.getQString()) &&
          QFileInfo::exists(newDir + "/asset.png"), "local-copy undo removed files or failed to restore links");
  TUndoManager::manager()->redo();
  require(source->decodeFilePath(raster->getPath()) == copied, "local-copy redo generated a second folder");
  raster->invalidateFrames();
  require(pixel().b == 255, "local copy retained a stale file builder");
  {
    const int count = TUndoManager::manager()->getHistoryCount();
    DialogDriver driver([](QDialog *dialog) { dialog->reject(); });
    CastBrowserActions::makeLocal(&browser, {raster});
    require(TUndoManager::manager()->getHistoryCount() == count, "canceling local copy added undo");
  }
  auto *duplicate = source->createNewLevel(OVL_XSHLEVEL, L"Cast fixture duplicate")->getSimpleLevel();
  duplicate->setPath(TFilePath(newDir + "/asset.png")); duplicate->load();
  duplicate->getProperties()->setDirtyFlag(false);
  if (duplicate->getPalette()) duplicate->getPalette()->setDirtyFlag(false);
  {
    bool matched = false;
    DialogDriver driver([&](QDialog *dialog) {
      if (dialog->windowTitle() != "Duplicate Asset Review") return;
      if (auto *table = dialog->findChild<QTableWidget *>())
        for (int row = 0; row < table->rowCount(); ++row)
          if (table->item(row, 0)->text() == "Cast fixture duplicate" &&
              table->item(row, 1)->text().contains("Matching saved content")) matched = true;
      dialog->reject();
    });
    const int count = TUndoManager::manager()->getHistoryCount();
    CastBrowserActions::duplicates(&browser);
    require(matched && !driver.expired && TUndoManager::manager()->getHistoryCount() == count,
            "duplicate preview missed matching files or mutated the scene");
  }
  // Explicit consolidation updates all unique sheets, including an unexposed
  // child, while keeping drawing IDs and a complete undo transaction.
  auto *unexposed = source->createNewLevel(CHILD_XSHLEVEL, L"Cast fixture Unexposed")->getChildLevel();
  top->setCell(7, column + 2, TXshCell(duplicate, ids.front()));
  child->getXsheet()->setCell(8, 4, TXshCell(duplicate, ids.front()));
  unexposed->getXsheet()->setCell(9, 1, TXshCell(duplicate, ids.front()));
  raster->setCastTags({"canonical-tag"}); duplicate->setCastTags({"duplicate-tag"});
  {
    bool confirmed = false, reviewQueued = false, reviewReturned = false;
    QString failure;
    DialogDriver driver([&](QDialog *dialog) {
      if (dialog->windowTitle() == "Duplicate Asset Review") {
        if (reviewQueued) {
          // A rejected nested review leaves the report open. Close it after
          // the queued click has returned so the assertion reports the cause.
          if (reviewReturned) dialog->reject();
          return;
        }
        if (auto *review = dialog->findChild<QPushButton *>("CastReviewConsolidation")) {
          reviewQueued = true;
          // Qt does not recursively deliver a repeating timer while its slot
          // is running. Leave that slot before entering the nested modal review
          // so DialogDriver can service its review and confirmation dialogs.
          // Binding the callback to the button cancels it if the report closes.
          QTimer::singleShot(0, review, [review, &reviewReturned]() {
            review->click();
            reviewReturned = true;
          });
        }
      } else if (dialog->objectName() == "CastConsolidationReview") {
        auto *table = dialog->findChild<QTableWidget *>();
        for (int row = 0; table && row < table->rowCount(); ++row) {
          if (!table->item(row, 2)->text().contains("Cast fixture duplicate")) continue;
          table->item(row, 0)->setCheckState(Qt::Checked);
          auto *keep = qobject_cast<QComboBox *>(table->cellWidget(row, 1));
          if (keep) keep->setCurrentIndex(keep->findText("Cast fixture raster"));
        }
        dialog->accept();
      } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
        if (message->standardButtons().testFlag(QMessageBox::Yes)) {
          confirmed = true; message->done(QMessageBox::Yes);
        } else { failure = message->text(); message->reject(); }
      }
    });
    CastBrowserActions::duplicates(&browser);
    require(!driver.expired && failure.isEmpty() && reviewQueued && reviewReturned && confirmed &&
            !source->getLevelSet()->getLevel(L"Cast fixture duplicate"), "explicit consolidation was not applied");
  }
  require(top->getCell(7, column + 2) == TXshCell(raster, ids.front()) &&
          child->getXsheet()->getCell(8, 4) == TXshCell(raster, ids.front()) &&
          unexposed->getXsheet()->getCell(9, 1) == TXshCell(raster, ids.front()) &&
          raster->getCastTags().contains("duplicate-tag"), "consolidation lost nested exposures, drawing IDs or tags");
  TUndoManager::manager()->undo();
  require(source->getLevelSet()->getLevel(L"Cast fixture duplicate") == duplicate &&
          top->getCell(7, column + 2) == TXshCell(duplicate, ids.front()) &&
          unexposed->getXsheet()->getCell(9, 1) == TXshCell(duplicate, ids.front()) &&
          !raster->getCastTags().contains("duplicate-tag"), "consolidation undo did not restore asset identity, exposures or metadata");
  TUndoManager::manager()->redo();
  require(!source->getLevelSet()->getLevel(L"Cast fixture duplicate"), "consolidation redo failed");
  TUndoManager::manager()->undo();
  {
    bool inventoried = false;
    DialogDriver driver([&](QDialog *dialog) {
      if (dialog->windowTitle() != "Scene Collection Report") return;
      if (auto *table = dialog->findChild<QTableWidget *>()) inventoried = table->rowCount() > 0;
      dialog->reject();
    });
    const int count = TUndoManager::manager()->getHistoryCount();
    CastBrowserActions::collectionReport(&browser);
    require(inventoried && !driver.expired && TUndoManager::manager()->getHistoryCount() == count,
            "collection report omitted scene resources or mutated the scene");
  }
  // Collect Assets must produce a scene that still works after the entire
  // project is moved and the external original is unavailable.
  const QString package = assets.path() + "/package";
  const QString relocated = assets.path() + "/relocated";
  QDir().mkpath(package);
  auto collectionProject = std::make_shared<TProject>();
  for (const std::string &folderName : {TProject::Inputs, TProject::Drawings, TProject::Scenes,
                                     TProject::Extras, TProject::Outputs, TProject::Palettes})
    collectionProject->setFolder(folderName, TFilePath(folderName));
  require(collectionProject->save(TFilePath(package + "/cast_otprj.xml")), "cannot save collection project");
  const QString externalFile = assets.path() + "/collection-source.png";
  picture.fill(Qt::green); require(picture.save(externalFile), "cannot save collection source");
  {
    ToonzScene collection;
    TProjectManager::instance()->initializeScene(&collection);
    collection.setProject(collectionProject);
    collection.setScenePath(TFilePath(package + "/scenes/collected.tnz"));
    TXshSimpleLevelP level = new TXshSimpleLevel(L"Collected fixture");
    level->setScene(&collection); level->setType(OVL_XSHLEVEL); level->setPath(TFilePath(externalFile));
    level->getProperties()->setDirtyFlag(false);
    if (level->getPalette()) level->getPalette()->setDirtyFlag(false);
    collection.getLevelSet()->insertLevel(level.getPointer());
    collection.getTopXsheet()->setCell(0, 0, TXshCell(level.getPointer(), level->getFirstFid()));
    ResourceCollector collector(&collection);
    { SceneResources resources(&collection, collection.getTopXsheet()); resources.accept(&collector); }
    require(!collector.aborted() && collector.getCollectedResourceCount() == 1, "Collect Assets did not copy external fixture");
    collection.save(collection.getScenePath(), nullptr, false);
  }
  require(QDir().rename(package, relocated), "cannot relocate collected project");
  require(QFile::rename(externalFile, externalFile + ".unavailable"), "cannot make original collection source unavailable");
  {
    bool localFiles = false, decoded = false, failed = false;
    DialogDriver driver([&](QDialog *dialog) {
      if (dialog->windowTitle() != "Relocated Scene Verification") return;
      if (auto *table = dialog->findChild<QTableWidget *>())
        for (int row = 0; row < table->rowCount(); ++row) {
          const QString status = table->item(row, 1)->text();
          localFiles = localFiles || status == "Verified: available inside relocated root";
          decoded = decoded || status == "Verified: exposed drawings decode";
          failed = failed || !status.startsWith("Verified:");
        }
      dialog->reject();
    });
    const auto workingPath = source->getScenePath();
    CastBrowserActions::verifyCollectedScene(&browser, TFilePath(relocated + "/scenes/collected.tnz"), TFilePath(relocated));
    require(!driver.expired && localFiles && decoded && !failed && source->getScenePath() == workingPath,
            "relocated Collect Assets verification failed or changed the working scene");
  }

  // Exercise the actual native Cast at scale and persist measured timings.
  // The broad upper bound catches pathological stalls; CI keeps exact timings
  // as evidence rather than treating runner speed as a product guarantee.
  constexpr int largeCount = 3000;
  std::vector<TXshLevelP> large;
  browser.hide();
  for (int index = 0; index < largeCount; ++index) {
    const QString name = QString("Cast scale %1").arg(index, 5, 10, QChar('0'));
    TXshSimpleLevelP level = new TXshSimpleLevel(name.toStdWString());
    level->setScene(source); level->setType(PLI_XSHLEVEL);
    level->setPath(TFilePath(localDir + '/' + name + ".pli"), true);
    source->getLevelSet()->insertLevel(level.getPointer());
    large.push_back(TXshLevelP(level.getPointer()));
  }
  search->clear(); scope->setCurrentIndex(1); type->setCurrentIndex(0); status->setCurrentIndex(0);
  QElapsedTimer measurement;
  QJsonObject metrics{{"schema", 1}, {"syntheticAssets", largeCount},
                      {"commit", qEnvironmentVariable("GITHUB_SHA")}};
  measurement.start(); browser.show(); QApplication::processEvents();
  QMetaObject::invokeMethod(&browser, "refresh", Qt::DirectConnection);
  metrics["refreshMs"] = double(measurement.elapsed());
  require(browser.getItemCount() >= largeCount && measurement.elapsed() < 30000, "large Cast refresh incomplete or stalled");
  const int selectedIndex = find(browser, "Cast fixture Hero");
  viewer->getPanel()->getSelection()->selectNone(); viewer->getPanel()->getSelection()->select(selectedIndex);
  measurement.restart(); browser.sortByDataModel(DvItemListModel::FullPath, false);
  metrics["sortMs"] = double(measurement.elapsed());
  selected.clear(); static_cast<CastSelection *>(viewer->getPanel()->getSelection())->getSelectedLevels(selected);
  require(selected.size() == 1 && selected.front() == hero, "large Cast sort changed selected identity");
  QApplication::processEvents();
  auto *scroll = viewer->verticalScrollBar();
  require(scroll->maximum() > 0, "large Cast fixture did not create scrolling content");
  scroll->setValue(scroll->maximum() / 2);
  const int scrollBefore = scroll->value();
  QMetaObject::invokeMethod(&browser, "refresh", Qt::DirectConnection);
  require(scroll->value() == scrollBefore, "refresh lost Cast scroll position");
  metrics["scrollPreserved"] = true;
  measurement.restart(); search->setText("Cast scale 01234"); browser.refreshData();
  metrics["searchMs"] = double(measurement.elapsed());
  require(browser.getItemCount() == 1 && find(browser, "Cast scale 01234") == 0,
          "large Cast search returned incorrect identities");
  search->clear(); status->setCurrentIndex(1); measurement.restart(); browser.refreshData();
  metrics["usageFilterMs"] = double(measurement.elapsed());
  require(find(browser, "Cast fixture Hero") >= 0 && find(browser, "Cast scale 01234") < 0,
          "large Cast usage filter is incorrect");
  metrics["correctness"] = "passed";
  const QByteArray json = QJsonDocument(metrics).toJson(QJsonDocument::Compact);
  std::cout << "cast-workflow-metrics: " << json.constData() << '\n';
  const QString reportPath = qEnvironmentVariable("GZ_CAST_WORKFLOW_REPORT");
  if (!reportPath.isEmpty()) {
    QSaveFile output(reportPath);
    require(output.open(QIODevice::WriteOnly) && output.write(json) == json.size() && output.commit(),
            "cannot write required Cast CI timing artifact");
  }
  status->setCurrentIndex(0);
  for (const auto &level : large) source->getLevelSet()->removeLevel(level.getPointer());
  browser.refreshData();
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogsDisabled);
  browser.hide();
  std::cout << "cast-native-workflow: PASS\n";
}
