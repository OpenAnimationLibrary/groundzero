#include "castviewer.h"
#include "castselection.h"
#include "castbrowseractions.h"
#include "nativeuiintegrationtest.h"
#include "nativefiledialogtest.h"
#include "libraryprovenanceintegrationtest.h"
#include "castinterruptionintegrationtest.h"
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
#include "toonz/sceneproperties.h"
#include "toonz/txshsoundlevel.h"
#include "toonz/sceneresources.h"
#include "toonz/tproject.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/icongenerator.h"
#include "menubarcommandids.h"
#include "trasterimage.h"
#include "tpalette.h"
#include "tvectorimage.h"
#include "timagecache.h"
#include "tundo.h"
#include <QApplication>
#include <QAbstractButton>
#include <QComboBox>
#include <QColor>
#include <QInputDialog>
#include <QLineEdit>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QTimer>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTableWidget>
#include <QSlider>
#include <QFileDialog>
#include <QMessageBox>
#include <QImage>
#include <QPixmap>
#include <QTemporaryDir>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <functional>
#include <cmath>
#include "tstream.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSaveFile>
#include <QScrollBar>
#include <QDirIterator>
#include <QMenu>
#include <QProgressDialog>
#include <QPointer>
#include <stdexcept>
#include <iostream>

namespace {
QJsonObject refreshDifference(const QJsonObject &before, const QJsonObject &after) {
  QJsonObject result;
  for (auto it = after.begin(); it != after.end(); ++it)
    result[it.key()] = it.value().toDouble() - before.value(it.key()).toDouble();
  return result;
}
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void waitUntil(const std::function<bool()> &done, const char *message) {
  QElapsedTimer deadline; deadline.start();
  while (!done() && deadline.elapsed() < 5000)
    QApplication::processEvents(QEventLoop::AllEvents, 20);
  require(done(), message);
}
void nextPreviewRequest(QWidget *inspector) {
  auto *delay = inspector->findChild<QTimer *>("CastPreviewDelay");
  require(delay, "inspector has no preview coalescing timer");
  QEventLoop loop; QTimer deadline; deadline.setSingleShot(true);
  bool submitted = false, fired = false;
  QObject::connect(delay, &QTimer::timeout, &loop, [&]() {
    fired = true; submitted = inspector->property("PreviewPending").toBool(); loop.quit();
  });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  deadline.start(5000); loop.exec();
  // Return at the actual timer boundary, before the queued worker completion.
  if (!submitted) {
    auto *table = inspector->findChild<QTableWidget *>("CastInspectorDetails");
    auto *slider = inspector->findChild<QSlider *>("CastDrawingPreview");
    const int row = table ? table->currentRow() : -1;
    const QString target = table && row >= 0 && table->item(row, 0) ? table->item(row, 0)->text() : QString();
    throw std::runtime_error(QString("native inspector failed to submit its desired preview: "
        "timer fired=%1 active=%2 visible=%3 rows=%4 target=%5 slider=%6/%7 enabled=%8 displayed=%9")
        .arg(fired).arg(delay->isActive()).arg(inspector->isVisible()).arg(table ? table->rowCount() : -1)
        .arg(target).arg(slider ? slider->value() : -1).arg(slider ? slider->maximum() : -1)
        .arg(slider && slider->isEnabled()).arg(inspector->property("PreviewFrame").toString()).toStdString());
  }
}
void reopenOwnership(ToonzScene *source, TXshSimpleLevel *level,
                     const TFilePath &copyPath,
                     TXshLevel::LibraryProvenance::Relationship relationship,
                     const TPixel32 &expectedPixel) {
  const auto origin = level->getLibraryProvenance();
  const QString identity = level->getCastId();
  const bool expectedSource = level->sourceExpectedOnDisk();
  const auto storedPath = level->getPath();
  const auto actualPath = source->decodeFilePath(storedPath);
  const auto workingPath = source->getScenePath();
  const int history = TUndoManager::manager()->getCurrentHistoryIndex();
  require(level->getLibraryRelationship() == relationship,
          "ownership reopen fixture began with the wrong relationship");
  source->saveCopy(copyPath);
  ToonzScene restored;
  TProjectManager::instance()->initializeScene(&restored);
  restored.setProject(source->getProject());
  restored.loadTnzFile(copyPath);
  TXshSimpleLevel *reopened = nullptr;
  for (int index = 0; index < restored.getLevelSet()->getLevelCount(); ++index) {
    auto *candidate = restored.getLevelSet()->getLevel(index);
    if (candidate->getCastId() != identity) continue;
    require(!reopened && candidate->getSimpleLevel(), "native reopen duplicated or changed the asset UUID");
    reopened = candidate->getSimpleLevel();
  }
  require(reopened && reopened->getLibraryProvenance() == origin &&
              reopened->sourceExpectedOnDisk() == expectedSource &&
              reopened->getLibraryRelationship() == relationship &&
              restored.decodeFilePath(reopened->getPath()) == actualPath,
          "native reopen changed Library provenance, ownership, source state or path");
  // Load this saved asset through the native reader. Other fixture drawings
  // intentionally remain unsaved; reopening must not prompt to resolve them.
  reopened->load();
  std::vector<TFrameId> frames; reopened->getFids(frames);
  require(!frames.empty(), "native ownership reopen lost the saved drawing");
  TRasterImageP image = reopened->getFrame(frames.front(), false);
  TRaster32P pixels = image ? image->getRaster() : TRasterP();
  require(pixels && pixels->pixels(0)[0] == expectedPixel,
          "native ownership reopen decoded the wrong source content");
  require(reopened->getCastId() == identity && reopened->getLibraryProvenance() == origin &&
              reopened->sourceExpectedOnDisk() == expectedSource &&
              reopened->getLibraryRelationship() == relationship,
          "native asset loading changed reopened ownership metadata");
  require(TApp::instance()->getCurrentScene()->getScene() == source &&
              source->getScenePath() == workingPath && level->getPath() == storedPath &&
              level->getCastId() == identity && level->getLibraryProvenance() == origin &&
              level->sourceExpectedOnDisk() == expectedSource &&
              TUndoManager::manager()->getCurrentHistoryIndex() == history,
          "ownership verification changed the working scene or undo history");
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
      if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
        std::cerr << "cast-workflow: modal deadline: " << dialog->metaObject()->className()
                  << " / " << dialog->objectName().toStdString() << " / "
                  << dialog->windowTitle().toStdString() << std::endl;
        dialog->reject();
      } else std::cerr << "cast-workflow: modal deadline without active dialog" << std::endl;
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
void collectionAction(CastBrowser &browser, const QString &actionText,
                      const QString &value = QString(), bool accept = true) {
  bool activated = false;
  QTimer::singleShot(0, [&]() {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu) return;
    for (auto *action : menu->actions()) if (action->text() == actionText) {
      activated = true; menu->setActiveAction(action);
      QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
      QApplication::sendEvent(menu, &enter); return;
    }
    menu->close();
  });
  DialogDriver driver([&](QDialog *dialog) {
    if (auto *input = qobject_cast<QInputDialog *>(dialog)) {
      if (!value.isNull()) input->setTextValue(value);
      if (accept) input->accept(); else input->reject();
    } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
      if (auto *button = message->button(accept ? QMessageBox::Yes : QMessageBox::No)) button->click();
      else message->reject();
    }
  });
  browser.findChild<QPushButton *>("CastCollectionActions")->click();
  require(activated && !driver.expired, "native collection action failed to complete");
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

  // Status predicates distinguish a new planned path from a lost saved source,
  // and inspect the image cache without decoding missing drawings.
  status->setCurrentIndex(status->findData(CastBrowserActions::Unsaved)); browser.refreshData();
  require(browser.getItemCount() == 1 && !hero->sourceExpectedOnDisk(), "new asset filter lost unsaved artwork");
  hero->setSourceExpectedOnDisk(true);
  status->setCurrentIndex(status->findData(CastBrowserActions::Missing)); browser.refreshData();
  require(browser.getItemCount() == 1 && CastBrowserActions::status(source, hero).contains("Missing on disk"),
          "saved missing asset was still presented as new");
  hero->setSourceExpectedOnDisk(false);
  hero->getProperties()->setDirtyFlag(true);
  status->setCurrentIndex(status->findData(CastBrowserActions::Modified)); browser.refreshData();
  require(browser.getItemCount() == 1, "modified artwork is absent from status filter");
  hero->getProperties()->setDirtyFlag(false); hero->getPalette()->setDirtyFlag(false); browser.refreshData();
  require(browser.getItemCount() == 0, "clean artwork remains in modified filter");
  hero->getPalette()->setDirtyFlag(true); browser.refreshData();
  require(browser.getItemCount() == 1, "palette-only modification is absent from modified filter");
  status->setCurrentIndex(status->findData(CastBrowserActions::Cached)); browser.refreshData();
  require(browser.getItemCount() == 1, "resident decoded drawing missing from cache filter");
  status->setCurrentIndex(0); search->clear(); browser.refreshData();

  // Refresh-local snapshots must observe file/dirty/cache changes made between
  // refreshes; filtering must not turn the hidden selection into a new identity.
  {
    QTemporaryDir files;
    require(files.isValid(), "no fresh-status fixture directory");
    const QString path = files.filePath("fresh-status.png");
    TXshSimpleLevelP fresh = new TXshSimpleLevel(L"Cast fresh status");
    fresh->setScene(source); fresh->setType(OVL_XSHLEVEL);
    fresh->setPath(TFilePath(path), true); fresh->setSourceExpectedOnDisk(true);
    source->getLevelSet()->insertLevel(fresh.getPointer());
    const QString identity = fresh->getCastId();
    search->setText("Cast fresh status");
    status->setCurrentIndex(status->findData(CastBrowserActions::Missing));
    browser.refreshData();
    require(browser.getItemCount() == 1, "fresh-status fixture is not initially missing");
    viewer->getPanel()->getSelection()->selectNone();
    viewer->getPanel()->getSelection()->select(0);
    QImage file(8, 8, QImage::Format_ARGB32); file.fill(Qt::red);
    for (int repeat = 0; repeat < 2; ++repeat) {
      require(file.save(path), "cannot create fresh-status source");
      browser.refreshData();
      require(browser.getItemCount() == 0 && viewer->getSelectedIndices().empty(),
              "file creation retained a missing asset or actionable hidden selection");
      status->setCurrentIndex(0); browser.refreshData();
      require(browser.getItemCount() == 1 &&
              browser.getItemData(0, DvItemListModel::ToolTip).toString().contains("Saved source exists"),
              "status tooltip retained stale file absence");
      selected.clear();
      static_cast<CastSelection *>(viewer->getPanel()->getSelection())->getSelectedLevels(selected);
      require(selected.size() == 1 && selected.front() == fresh.getPointer() && fresh->getCastId() == identity,
              "file creation changed the restored selected asset identity");
      require(QFile::remove(path), "cannot remove fresh-status source");
      browser.refreshData();
      require(browser.getItemData(0, DvItemListModel::ToolTip).toString().contains("Missing on disk"),
              "status tooltip retained stale file existence");
      status->setCurrentIndex(status->findData(CastBrowserActions::Missing));
      browser.refreshData();
      require(browser.getItemCount() == 1, "file removal did not restore missing status");
    }
    viewer->getPanel()->getSelection()->selectNone();
    fresh->getProperties()->setDirtyFlag(true);
    status->setCurrentIndex(status->findData(CastBrowserActions::Modified)); browser.refreshData();
    require(browser.getItemCount() == 1, "refresh snapshot missed a new dirty flag");
    fresh->getProperties()->setDirtyFlag(false); browser.refreshData();
    require(browser.getItemCount() == 0, "refresh snapshot retained a cleared dirty flag");
    TRaster32P pixels(8, 8); pixels->fill(TPixel32::Red);
    fresh->setFrame(TFrameId(1), TRasterImageP(pixels));
    const std::string imageId = fresh->getImageId(TFrameId(1));
    status->setCurrentIndex(status->findData(CastBrowserActions::Cached)); browser.refreshData();
    require(browser.getItemCount() == 1 && TImageCache::instance()->isCached(imageId),
            "refresh snapshot missed newly cached drawing data");
    TImageCache::instance()->remove(imageId); browser.refreshData();
    require(browser.getItemCount() == 0 && !TImageCache::instance()->isCached(imageId),
            "refresh retained stale decoded state or decoded a file to inspect the cache");
    source->getLevelSet()->removeLevel(fresh.getPointer());
  }
  status->setCurrentIndex(0); search->clear(); browser.refreshData();

  auto *collections = browser.findChild<QComboBox *>("CastCollectionFilter");
  require(collections, "named collection control is absent");
  viewer->getPanel()->getSelection()->selectNone();
  viewer->getPanel()->getSelection()->select(find(browser, "Cast fixture Hero"));
  const int collectionHistory = TUndoManager::manager()->getCurrentHistoryIndex();
  collectionAction(browser, "New Collection from Selected…", "café & Hero");
  const auto savedCollection = source->getProperties()->getCastCollections();
  require(savedCollection.size() == 1 && savedCollection.front().members == QStringList{hero->getCastId()} &&
          browser.getItemCount() == 1 && TUndoManager::manager()->getCurrentHistoryIndex() == collectionHistory + 1,
          "creating collection lost selected asset UUID or applied no undo");
  const QString collectionId = savedCollection.front().id;
  collectionAction(browser, "New Collection from Selected…", "CAFÉ & HERO");
  require(source->getProperties()->getCastCollections() == savedCollection &&
          TUndoManager::manager()->getCurrentHistoryIndex() == collectionHistory + 1,
          "duplicate collection name while selected created a phantom undo or changed membership");
  collectionAction(browser, "New Collection from Selected…", "Canceled collection", false);
  require(source->getProperties()->getCastCollections() == savedCollection &&
          TUndoManager::manager()->getCurrentHistoryIndex() == collectionHistory + 1,
          "canceling new collection changed scene metadata/history");
  collectionAction(browser, "Rename Collection…", "Discarded", false);
  require(source->getProperties()->getCastCollections() == savedCollection &&
          TUndoManager::manager()->getCurrentHistoryIndex() == collectionHistory + 1,
          "canceling collection rename changed metadata/history");
  collectionAction(browser, "Rename Collection…", "Character assets");
  require(source->getProperties()->getCastCollections().front().name == "Character assets", "collection rename failed");
  TUndoManager::manager()->undo();
  require(source->getProperties()->getCastCollections() == savedCollection, "collection rename undo changed members");
  TUndoManager::manager()->redo();
  collections->setCurrentIndex(0); browser.refreshData();
  viewer->getPanel()->getSelection()->selectNone();
  viewer->getPanel()->getSelection()->select(find(browser, "Cast fixture Spare"));
  collectionAction(browser, "Add Selected to Collection…", "Character assets");
  require(source->getProperties()->getCastCollections().front().members.contains(spare->getCastId()) &&
          browser.getItemCount() == 2, "adding an asset from outside collection failed");
  const int membershipHistory = TUndoManager::manager()->getCurrentHistoryIndex();
  collectionAction(browser, "Add Selected to Collection…", "Character assets");
  require(TUndoManager::manager()->getCurrentHistoryIndex() == membershipHistory &&
          source->getProperties()->getCastCollections().front().members.size() == 2,
          "repeated membership addition duplicated IDs or created a phantom undo");
  collectionAction(browser, "Remove Selected from Collection");
  require(source->getProperties()->getCastCollections().front().members == QStringList{hero->getCastId()},
          "removing selected collection membership removed wrong identity");
  TUndoManager::manager()->undo();
  require(source->getProperties()->getCastCollections().front().members.contains(spare->getCastId()),
          "membership undo did not restore asset UUID");
  const auto bothMembers = source->getProperties()->getCastCollections();
  TXshLevelP retainedSpare(spare);
  source->getLevelSet()->removeLevel(spare); browser.refreshData();
  require(browser.getItemCount() == 1 && source->getProperties()->getCastCollections() == bothMembers,
          "asset removal destroyed dormant collection membership");
  source->getLevelSet()->insertLevel(spare); browser.refreshData();
  require(browser.getItemCount() == 2, "restoring asset identity did not restore collection membership");
  collectionAction(browser, "Delete Collection…", QString(), false);
  require(source->getProperties()->getCastCollections() == bothMembers, "canceling collection delete changed data");
  collectionAction(browser, "Delete Collection…");
  require(source->getProperties()->getCastCollections().empty() && source->getLevelSet()->getLevel(hero->getName()) == hero,
          "deleting collection removed assets or retained collection");
  TUndoManager::manager()->undo();
  require(source->getProperties()->getCastCollections() == bothMembers, "collection delete undo lost identity");
  for (int repeat = 0; repeat < 3; ++repeat) {
    TUndoManager::manager()->redo();
    require(source->getProperties()->getCastCollections().empty(), "repeated collection delete redo changed result");
    TUndoManager::manager()->undo();
    require(source->getProperties()->getCastCollections() == bothMembers,
            "repeated collection delete undo lost original membership");
  }
  collections->setCurrentIndex(collections->findData(collectionId)); browser.refreshData();
  require(browser.getItemCount() == 2, "restored collection cannot be selected");
  {
    QTemporaryDir malformed; require(malformed.isValid(), "no malformed-ID fixture directory");
    const TFilePath metadata(malformed.path() + "/duplicate-identity.xml");
    { TOStream os(metadata); os.openChild("path", hero->getCastAttributes()); os.closeChild(); }
    TXshSimpleLevelP ambiguous = new TXshSimpleLevel(L"Ambiguous imported identity");
    ambiguous->setScene(source); ambiguous->setType(PLI_XSHLEVEL);
    ambiguous->setFrame(TFrameId(1), new TVectorImage);
    { TIStream is(metadata); std::string tag; require(is.matchTag(tag), "no malformed-ID metadata"); ambiguous->loadCastTags(is); is.closeChild(); }
    source->getLevelSet()->insertLevel(ambiguous.getPointer()); browser.refreshData();
    require(find(browser, "Cast fixture Hero") < 0 && find(browser, "Ambiguous imported identity") < 0 &&
            find(browser, "Cast fixture Spare") >= 0, "collection silently expanded ambiguous imported asset identity");
    collections->setCurrentIndex(0); browser.refreshData();
    viewer->getPanel()->getSelection()->selectNone();
    viewer->getPanel()->getSelection()->select(find(browser, "Ambiguous imported identity"));
    const int count = TUndoManager::manager()->getCurrentHistoryIndex();
    collectionAction(browser, "New Collection from Selected…", "Ambiguous members");
    require(source->getProperties()->getCastCollections() == bothMembers &&
            TUndoManager::manager()->getCurrentHistoryIndex() == count,
            "ambiguous identity created actionable collection membership");
    source->getLevelSet()->removeLevel(ambiguous.getPointer());
  }
  collections->setCurrentIndex(0); browser.refreshData();

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
  viewer->getPanel()->getSelection()->selectNone();
  viewer->getPanel()->getSelection()->select(find(browser, "Cast fixture Hero"));
  {
    const int count = TUndoManager::manager()->getHistoryCount();
    CastBrowserActions::inspect(&browser, {hero});
    auto *inspector = browser.findChild<QWidget *>("CastInspector");
    auto *slider = inspector ? inspector->findChild<QSlider *>("CastDrawingPreview") : nullptr;
    require(inspector && inspector->isVisible() && !qobject_cast<QDialog *>(inspector) &&
            slider && slider->isEnabled() && slider->maximum() == 1,
            "docked inspector did not return to editing or missed drawings");
    slider->setValue(1);
    require(TUndoManager::manager()->getHistoryCount() == count,
            "Inspector scrubbing changed the scene");
    inspector->hide();
    CastBrowserActions::inspect(&browser, {hero});
    require(inspector == browser.findChild<QWidget *>("CastInspector") && inspector->isVisible(),
            "reopening inspector replaced its persistent pane");
  }
  hero->setFrame(TFrameId(5), new TVectorImage);
  {
    CastBrowserActions::inspect(&browser, {hero});
    auto *inspector = browser.findChild<QWidget *>("CastInspector");
    auto *slider = inspector->findChild<QSlider *>("CastDrawingPreview");
    auto *view = inspector->findChild<QTableWidget *>("CastInspectorDetails");
    require(view && view->item(0, 3)->text().contains("3–4") && view->item(0, 5)->text().contains("styles"),
            "inspector omitted drawing gaps or palette details");
    slider->setValue(2);
    inspector->findChild<QPushButton *>("CastChooseCover")->click();
    require(hero->hasCastCover() && hero->getCastCover() == TFrameId(5), "cover choice failed");
    TUndoManager::manager()->undo();
    require(!hero->hasCastCover(), "cover undo did not restore default cover");
    TUndoManager::manager()->redo();
    require(hero->getCastCover() == TFrameId(5), "cover redo failed");
    viewer->getPanel()->getSelection()->select(find(browser, "Cast fixture Spare"));
    require(view->rowCount() == 2, "docked inspector did not follow native multi-selection");
    for (int row = 0; row < view->rowCount(); ++row)
      if (view->item(row, 0)->text() == "Cast fixture Spare") view->selectRow(row);
    browser.sortByDataModel(DvItemListModel::Name, true); browser.refreshData();
    require(view->currentRow() >= 0 && view->item(view->currentRow(), 0)->text() == "Cast fixture Spare",
            "inspector refresh changed active multi-selection identity");
    viewer->getPanel()->getSelection()->selectNone();
    viewer->getPanel()->getSelection()->select(find(browser, "Cast fixture Hero"));
    search->setText("Spare"); browser.refreshData();
    require(view->rowCount() == 0 && !slider->isEnabled(), "inspector retained a filtered-out target");
    search->clear(); browser.refreshData();
    require(view->rowCount() == 1 && view->item(0, 0)->text() == "Cast fixture Hero",
            "inspector did not follow restored asset identity");
  }
  {
    // Different solid drawings make final-frame verification independent of
    // the request counters. The browser may share first-drawing thumbnails.
    TXshSimpleLevelP scrub = new TXshSimpleLevel(L"Cast scrub raster");
    scrub->setScene(source); scrub->setType(OVL_XSHLEVEL);
    for (int frame = 1; frame <= 12; ++frame) {
      TRaster32P raster(80, 60); raster->fill(TPixel32(frame * 15, 30, 220 - frame * 10));
      scrub->setFrame(TFrameId(frame), TRasterImageP(raster));
    }
    source->getLevelSet()->insertLevel(scrub.getPointer()); browser.refreshData();
    viewer->getPanel()->getSelection()->selectNone();
    viewer->getPanel()->getSelection()->select(find(browser, "Cast scrub raster"));
    CastBrowserActions::inspect(&browser, {scrub.getPointer()});
    auto *inspector = browser.findChild<QWidget *>("CastInspector");
    auto *slider = inspector->findChild<QSlider *>("CastDrawingPreview");
    auto *image = inspector->findChild<QLabel *>("CastDrawingImage");
    auto *icons = IconGenerator::instance();
    const auto displayed = [&](int frame) {
      if (inspector->property("PreviewFrame").toString() != QString::fromStdString(TFrameId(frame).expand())) return false;
      const QImage rendered = qvariant_cast<QPixmap>(image->property("pixmap")).toImage();
      return !rendered.isNull() && rendered.pixelColor(rendered.width() / 2, rendered.height() / 2) ==
          QColor(frame * 15, 30, 220 - frame * 10);
    };
    const int before = inspector->property("PreviewRequests").toInt();
    for (int index = 1; index < 12; ++index) slider->setValue(index);
    require(inspector->property("PreviewRequests").toInt() == before,
            "rapid scrub queued work before the coalescing window");
    waitUntil([&]() { return displayed(12); }, "rapid scrub did not display the actual latest drawing");
    require(inspector->property("PreviewRequests").toInt() == before + 1,
            "rapid scrub queued intermediate drawings");

    slider->setValue(1); nextPreviewRequest(inspector);
    const int inFlight = inspector->property("PreviewRequests").toInt();
    inspector->hide(); inspector->show();
    require(inspector->property("PreviewRequests").toInt() == inFlight &&
            inspector->property("PreviewPending").toBool(),
            "hide/show duplicated a pending same-frame request");
    for (int index = 2; index <= 9; ++index) slider->setValue(index);
    require(inspector->property("PreviewRequests").toInt() == inFlight,
            "scrubbing queued more work while one preview was pending");
    waitUntil([&]() { return displayed(10); }, "late preview completion replaced the latest scrubbed drawing");
    require(inspector->property("PreviewRequests").toInt() == inFlight + 1,
            "pending scrub did not coalesce to one latest request");

    // Native generation identity survives cancellation and a same-key requeue.
    // Even a worker that already finished cannot publish its queued old result.
    std::map<quint64, std::pair<int, bool>> completions;
    QObject completionContext;
    const auto completion = QObject::connect(icons, &IconGenerator::iconRequestFinished, &completionContext,
        [&](quint64 token, bool generated) { ++completions[token].first; completions[token].second = generated; });
    const auto older = icons->requestIcon(scrub.getPointer(), TFrameId(6), false);
    icons->clearRequests(); icons->remove(scrub.getPointer(), TFrameId(6));
    const auto newer = icons->requestIcon(scrub.getPointer(), TFrameId(6), false);
    require(older && newer && older != newer && icons->pendingIconRequest(scrub.getPointer(), TFrameId(6), false) == newer,
            "requeued icon reused an old generation token");
    waitUntil([&]() { return completions.count(older) && completions.count(newer); },
              "canceled/replaced icon request never completed");
    require(completions[older] == std::make_pair(1, false) && completions[newer] == std::make_pair(1, true) &&
            !icons->getIcon(scrub.getPointer(), TFrameId(6), false, true).isNull(),
            "late canceled icon retired or overwrote its replacement");
    icons->getIcon(scrub.getPointer(), TFrameId(7), false, true);
    require(!icons->pendingIconRequest(scrub.getPointer(), TFrameId(7), false), "cache probe queued a drawing");
    const auto unavailable = icons->requestIcon(scrub.getPointer(), TFrameId(99), false);
    waitUntil([&]() { return completions.count(unavailable) != 0; }, "failed icon request remained pending");
    require(unavailable && completions[unavailable] == std::make_pair(1, false) &&
            !icons->pendingIconRequest(scrub.getPointer(), TFrameId(99), false),
            "failed icon request did not retire exactly once");
    QObject::disconnect(completion);

    slider->setValue(2); nextPreviewRequest(inspector);
    const int beforeRemoval = inspector->property("PreviewRequests").toInt();
    source->getLevelSet()->removeLevel(scrub.getPointer());
    TApp::instance()->getCurrentScene()->notifyCastChange();
    waitUntil([&]() { return !inspector->property("PreviewPending").toBool(); },
              "removed asset retained a pending inspector request");
    require(inspector->property("PreviewFrame").toString().isEmpty() &&
            inspector->findChild<QTableWidget *>("CastInspectorDetails")->rowCount() == 0 &&
            inspector->property("PreviewRequests").toInt() == beforeRemoval,
            "late preview for removed asset changed the inspector or queued more work");
    viewer->getPanel()->getSelection()->select(find(browser, "Cast fixture Hero"));
  }
  {
    // A real stereo PCM impulse must appear in the peak envelope. Replacing
    // audio and changing selection cancel old computation without exposures.
    TXshSoundLevelP retainedSound = new TXshSoundLevel(L"Cast waveform audio");
    auto *sound = retainedSound.getPointer();
    sound->setScene(source); source->getLevelSet()->insertLevel(sound);
    std::vector<short> samples(16000, 0);
    samples[4000] = 24576; samples[4001] = -16384;
    auto track = TSoundTrack::create(8000, 16, 2, 8000, TSound::INT, samples.data());
    sound->setSoundTrack(track);
    browser.refreshData();
    viewer->getPanel()->getSelection()->selectNone();
    viewer->getPanel()->getSelection()->select(find(browser, "Cast waveform audio"));
    const int history = TUndoManager::manager()->getCurrentHistoryIndex();
    CastBrowserActions::inspect(&browser, {sound});
    auto *inspector = browser.findChild<QWidget *>("CastInspector");
    auto *waveform = inspector->findChild<QWidget *>("CastAudioWaveform");
    auto *seek = inspector->findChild<QSlider *>("CastAudioSeek");
    QElapsedTimer deadline; deadline.start();
    while (!waveform->property("WaveformReady").toBool() && deadline.elapsed() < 5000)
      QApplication::processEvents();
    require(waveform->property("WaveformReady").toBool() &&
            waveform->property("WaveformSampleCount").toInt() == 8000 &&
            std::abs(waveform->property("WaveformPeak").toDouble() - .75) < 1.e-6 && seek->isEnabled(),
            "native waveform did not reflect source PCM peaks/duration");
    seek->setValue(5000);
    require(inspector->findChild<QLabel *>("CastAudioTime")->text().contains("0.50 / 1.00"),
            "waveform seeking lost audio duration");
    auto expectPeak = [&](const TSoundTrackP &replacement, double peak) {
      sound->setSoundTrack(replacement); TApp::instance()->getCurrentScene()->notifyCastChange();
      CastBrowserActions::inspect(&browser, {sound});
      deadline.restart();
      while (!waveform->property("WaveformReady").toBool() && deadline.elapsed() < 5000)
        QApplication::processEvents();
      require(waveform->property("WaveformReady").toBool() &&
              std::abs(waveform->property("WaveformPeak").toDouble() - peak) < 1.e-6,
              "waveform amplitude differs from actual source sample format");
    };
    sound->setSoundTrack(TSoundTrack::create(8000, 16, 1, 131072, TSound::INT));
    TApp::instance()->getCurrentScene()->notifyCastChange();
    require(!waveform->property("WaveformReady").toBool() &&
            waveform->property("WaveformSampleCount").toInt() == 131072,
            "long waveform fixture did not begin a pending scan");
    inspector->hide();
    QEventLoop pause; QTimer::singleShot(30, &pause, &QEventLoop::quit); pause.exec();
    require(waveform->property("WaveformSamplesProcessed").toInt() == 0,
            "hiding inspector did not cancel pending waveform work");
    std::vector<UCHAR> unsigned8(8, 128); unsigned8[2] = 192; unsigned8[3] = 64;
    expectPeak(TSoundTrack::create(8000, 8, 1, 8, TSound::UINT, unsigned8.data()), .5);
    std::vector<UCHAR> integer24(24, 0); integer24[5] = 64;
    expectPeak(TSoundTrack::create(8000, 24, 1, 8, TSound::INT, integer24.data()), .5);
    std::vector<float> float32(8, 0); float32[2] = .375f; float32[3] = -.25f;
    expectPeak(TSoundTrack::create(8000, 32, 1, 8, TSound::FLOAT, float32.data()), .375);
    sound->setSoundTrack(TSoundTrack::create(8000, 16, 1, 16000, TSound::INT));
    TApp::instance()->getCurrentScene()->notifyCastChange();
    deadline.restart();
    while (!waveform->property("WaveformReady").toBool() && deadline.elapsed() < 5000)
      QApplication::processEvents();
    require(waveform->property("WaveformSampleCount").toInt() == 16000 &&
            waveform->property("WaveformPeak").toDouble() == 0., "replacement waveform retained old samples");
    sound->setSoundTrack(TSoundTrack::create(8000, 16, 1, 131072, TSound::INT));
    TApp::instance()->getCurrentScene()->notifyCastChange();
    viewer->getPanel()->getSelection()->selectNone();
    browser.refreshData();
    require(!seek->isEnabled() && waveform->property("WaveformSampleCount").toInt() == 0 &&
            TUndoManager::manager()->getCurrentHistoryIndex() == history,
            "audio preview changed scene history or retained a stale selection");
    viewer->getPanel()->getSelection()->select(find(browser, "Cast waveform audio"));
    source->getLevelSet()->removeLevel(sound); browser.refreshData();
    require(waveform->property("WaveformSampleCount").toInt() == 0 && !seek->isEnabled(),
            "deleting an asset retained its pending waveform or audition target");
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
  TXshLevel::LibraryProvenance origin;
  origin.itemId = "cast-fixture-library-item"; origin.itemPath = assets.path() + "/fixture.tnz";
  origin.kind = "rig"; origin.revision = 1; origin.sourceAssetPath = oldDir + "/asset.png";
  origin.relationship = TXshLevel::LibraryProvenance::Linked;
  raster->setLibraryProvenance(origin);
  const QString rasterId = raster->getCastId();
  const bool expectedOriginal = raster->sourceExpectedOnDisk();
  const bool nativeDialogsDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
  {
    bool pickedFolder = false, reviewed = false;
    QString candidate;
    QStringList messages;
    const int history = TUndoManager::manager()->getCurrentHistoryIndex();
    std::cerr << "cast-workflow: relinking saved raster" << std::endl;
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        pickedFolder = NativeUiTest::acceptDirectory(files, newDir);
      } else if (dialog->objectName() == "CastRelinkReview") {
        reviewed = true;
        if (auto *table = dialog->findChild<QTableWidget *>())
          if (auto *choice = qobject_cast<QComboBox *>(table->cellWidget(0, 2))) {
            candidate = choice->currentData().toString();
            std::cerr << "cast-workflow: relink choices: " << choice->count() << std::endl;
            for (int i = 0; i < choice->count(); ++i)
              std::cerr << "cast-workflow: replacement option: " << choice->itemData(i).toString().toStdString() << std::endl;
          }
        std::cerr << "cast-workflow: relink candidate: " << candidate.toStdString() << std::endl;
        dialog->accept();
        showProgressNextTurn("Validating replacement drawings…");
      } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
        messages << message->text();
        std::cerr << "cast-workflow: relink dialog: " << message->text().toStdString() << std::endl;
        dialog->reject();
      }
    });
    CastBrowserActions::relink(&browser, {raster});
    require(!driver.expired, "batch relink native dialog timed out");
    require(pickedFolder && reviewed, "batch relink did not reach replacement review");
    require(!candidate.isEmpty(), "batch relink did not discover its replacement drawing");
    if (!messages.empty())
      throw std::runtime_error(("batch relink reported: " + messages.join("; ")).toStdString());
    const auto actual = pixel();
    const QString path = source->decodeFilePath(raster->getPath()).getQString();
    if (actual.b != 255 ||
        TUndoManager::manager()->getCurrentHistoryIndex() != history + 1 ||
        QFileInfo(path).canonicalFilePath() != QFileInfo(newDir + "/asset.png").canonicalFilePath())
      throw std::runtime_error(QString("batch relink did not replace drawing contents: "
          "RGBA=%1,%2,%3,%4; history delta=%5; path=%6; candidate=%7")
          .arg(int(actual.r)).arg(int(actual.g)).arg(int(actual.b)).arg(int(actual.m))
          .arg(TUndoManager::manager()->getCurrentHistoryIndex() - history)
          .arg(path, candidate).toStdString());
  }
  require(raster->getLibraryRelationship() == TXshLevel::LibraryProvenance::Replaced &&
          raster->getLibraryProvenance().itemId == origin.itemId && raster->getCastId() == rasterId,
          "relink lost Library origin or asset identity instead of recording replaced source");
  reopenOwnership(source, raster, TFilePath(assets.path() + "/relinked.tnz"),
                  TXshLevel::LibraryProvenance::Replaced, TPixel32::Blue);
  TUndoManager::manager()->undo();
  require(pixel().r == 255, "relink undo did not restore original pixels");
  require(raster->getLibraryProvenance() == origin && raster->sourceExpectedOnDisk() == expectedOriginal &&
          raster->getCastId() == rasterId, "relink undo lost exact provenance or asset identity");
  reopenOwnership(source, raster, TFilePath(assets.path() + "/relink-undone.tnz"),
                  TXshLevel::LibraryProvenance::Linked, TPixel32::Red);
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
        NativeUiTest::acceptDirectory(files, oldDir);
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
        NativeUiTest::acceptDirectory(files, oldDir);
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
  origin.sourceAssetPath = source->decodeFilePath(linked).getQString();
  raster->setLibraryProvenance(origin);
  require(raster->getLibraryRelationship() == TXshLevel::LibraryProvenance::Linked,
          "local-copy provenance fixture is not a linked Library source");
  require(CastBrowserActions::external(source, raster), "external asset was classified as project owned");
  {
    const int count = TUndoManager::manager()->getHistoryCount();
    const auto folders = QDir(localDir).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    bool mutated = false, rejected = false;
    QStringList messages;
    std::cerr << "cast-workflow: stale local-copy preparation" << std::endl;
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        NativeUiTest::acceptDirectory(files, localDir);
        showProgressNextTurn("Preparing local asset copies…");
      } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
        messages << message->text();
        std::cerr << "cast-workflow: local-copy dialog: "
                  << message->text().toStdString() << std::endl;
        if (message->standardButtons().testFlag(QMessageBox::Yes)) {
          mutated = true;
          raster->getProperties()->setDirtyFlag(true);
          TApp::instance()->getCurrentLevel()->notifyLevelChange();
          // Static QMessageBox::question reads clickedButton(), not result().
          message->button(QMessageBox::Yes)->click();
        } else { rejected = message->text().contains("changed during preparation"); message->reject(); }
      }
    });
    CastBrowserActions::makeLocal(&browser, {raster});
    require(!driver.expired, "stale local-copy preparation did not finish its native dialogs");
    if (!mutated)
      throw std::runtime_error(("stale local-copy preparation never reached confirmation: " +
                                messages.join("; ")).toStdString());
    if (!rejected)
      throw std::runtime_error(("stale local-copy preparation did not reject the intervening edit: " +
                                messages.join("; ")).toStdString());
    require(raster->getPath() == linked, "stale local-copy preparation changed the original link");
    require(TUndoManager::manager()->getHistoryCount() == count,
            "stale local-copy preparation changed undo history");
    const auto remaining = QDir(localDir).entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    if (remaining != folders)
      throw std::runtime_error(("stale local-copy preparation left folders: " + remaining.join(", ")).toStdString());
    raster->getProperties()->setDirtyFlag(false);
  }
  {
    DialogDriver driver([&](QDialog *dialog) {
      if (auto *files = qobject_cast<QFileDialog *>(dialog)) {
        NativeUiTest::acceptDirectory(files, localDir);
        showProgressNextTurn("Preparing local asset copies…");
      } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
        if (message->standardButtons().testFlag(QMessageBox::Yes)) message->button(QMessageBox::Yes)->click();
        else message->reject();
      }
    });
    CastBrowserActions::makeLocal(&browser, {raster});
    require(!driver.expired && !CastBrowserActions::external(source, raster), "make local copy did not change ownership");
  }
  const TFilePath copied = source->decodeFilePath(raster->getPath());
  require(raster->getLibraryRelationship() == TXshLevel::LibraryProvenance::LocalCopy &&
          raster->getCastId() == rasterId && raster->sourceExpectedOnDisk(),
          "making Library source local lost origin or stable identity");
  require(QFileInfo::exists(copied.getQString()) && pixel().b == 255, "local copy is unreadable");
  reopenOwnership(source, raster, TFilePath(localDir + "/local-copy.tnz"),
                  TXshLevel::LibraryProvenance::LocalCopy, TPixel32::Blue);
  TUndoManager::manager()->undo();
  require(raster->getPath() == linked && QFileInfo::exists(copied.getQString()) &&
          QFileInfo::exists(newDir + "/asset.png"), "local-copy undo removed files or failed to restore links");
  require(raster->getLibraryProvenance() == origin && raster->sourceExpectedOnDisk() == expectedOriginal &&
          raster->getCastId() == rasterId, "local-copy undo lost exact linked provenance");
  reopenOwnership(source, raster, TFilePath(localDir + "/local-copy-undone.tnz"),
                  TXshLevel::LibraryProvenance::Linked, TPixel32::Blue);
  TUndoManager::manager()->redo();
  require(source->decodeFilePath(raster->getPath()) == copied, "local-copy redo generated a second folder");
  raster->invalidateFrames();
  require(pixel().b == 255, "local copy retained a stale file builder");
  reopenOwnership(source, raster, TFilePath(localDir + "/local-copy-redone.tnz"),
                  TXshLevel::LibraryProvenance::LocalCopy, TPixel32::Blue);
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
  auto collectionsBeforeConsolidation = source->getProperties()->getCastCollections();
  TSceneProperties::CastCollection duplicatesCollection;
  duplicatesCollection.id = "duplicate-fixture-collection"; duplicatesCollection.name = "Duplicate membership";
  duplicatesCollection.members = QStringList{duplicate->getCastId()};
  collectionsBeforeConsolidation.append(duplicatesCollection);
  source->getProperties()->setCastCollections(collectionsBeforeConsolidation);
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
          confirmed = true; message->button(QMessageBox::Yes)->click();
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
  require(source->getProperties()->getCastCollections().back().members == QStringList{rasterId},
          "consolidation emptied collection instead of remapping to surviving asset identity");
  TUndoManager::manager()->undo();
  require(source->getLevelSet()->getLevel(L"Cast fixture duplicate") == duplicate &&
          top->getCell(7, column + 2) == TXshCell(duplicate, ids.front()) &&
          unexposed->getXsheet()->getCell(9, 1) == TXshCell(duplicate, ids.front()) &&
          !raster->getCastTags().contains("duplicate-tag"), "consolidation undo did not restore asset identity, exposures or metadata");
  require(source->getProperties()->getCastCollections() == collectionsBeforeConsolidation,
          "consolidation undo did not restore exact original collection memberships");
  TUndoManager::manager()->redo();
  require(!source->getLevelSet()->getLevel(L"Cast fixture duplicate"), "consolidation redo failed");
  require(source->getProperties()->getCastCollections().back().members == QStringList{rasterId},
          "consolidation redo did not repeat collection membership remapping");
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
  QJsonObject metrics{{"schema", 2}, {"syntheticAssets", largeCount},
                      {"commit", qEnvironmentVariable("GITHUB_SHA")},
                      {"timingScope", "Synchronous native mutation and castChanged subscribers; excludes later paint. Initial refresh includes show/event processing plus explicit refresh."},
                      {"queryScope", "Logical snapshot API calls, not OS syscalls. Includes search/status paths and shared roots; excludes independent inspector snapshots and Library relationship internals."}};
  QJsonObject allBefore, instanceBefore;
  const auto captureRefresh = [&]() {
    allBefore = CastBrowser::refreshMeasurements();
    instanceBefore = browser.instanceRefreshMeasurements();
  };
  const auto recordRefresh = [&](const QString &key, bool emptyQuery) {
    const auto local = refreshDifference(instanceBefore, browser.instanceRefreshMeasurements());
    const auto all = refreshDifference(allBefore, CastBrowser::refreshMeasurements());
    require(local.value("refreshes").toDouble() >= 1 &&
            all.value("refreshes").toDouble() >= local.value("refreshes").toDouble(),
            "native Cast edit did not record its actual refresh invocation");
    require(local.value("canonicalRootSets").toDouble() <= local.value("refreshes").toDouble() &&
            local.value("diskQueries").toDouble() <= local.value("assetSnapshots").toDouble(),
            "refresh repeated project-root or per-asset source queries");
    require(local.value("queryCompilations").toDouble() == local.value("refreshes").toDouble(),
            "search terms were not prepared once per refresh");
    if (emptyQuery)
      require(local.value("queryChecks").toDouble() == 0,
              "empty Cast search still evaluated every asset haystack");
    metrics[key] = QJsonObject{{"fixtureBrowser", local}, {"allBrowsers", all}};
  };
  captureRefresh();
  measurement.start(); browser.show(); QApplication::processEvents();
  QMetaObject::invokeMethod(&browser, "refresh", Qt::DirectConnection);
  metrics["refreshMs"] = double(measurement.elapsed());
  recordRefresh("initialRefreshWork", true);
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
  browser.sortByDataModel(DvItemListModel::Name, true);
  std::map<TXshLevel *, const CastItem *> survivingItems;
  for (int index = 0; index < browser.getItemCount(); ++index) {
    const auto *item = browser.getCastItems().getItem(index);
    survivingItems[item->getLevel()] = item;
  }
  auto *renamed = large[1234].getPointer();
  viewer->getPanel()->getSelection()->selectNone();
  viewer->getPanel()->getSelection()->select(find(browser, "Cast scale 01234"));
  const int deltaCount = browser.getItemCount();
  const auto verifyDelta = [&](int expectedCount) {
    require(browser.getItemCount() == expectedCount, "native Cast delta lost or duplicated assets");
    QString previous;
    for (int index = 0; index < browser.getItemCount(); ++index) {
      const auto *item = browser.getCastItems().getItem(index);
      const auto old = survivingItems.find(item->getLevel());
      require(old == survivingItems.end() || old->second == item,
              "native Cast delta rebuilt a surviving asset item");
      const QString name = item->getName();
      require(index == 0 || QString::compare(previous, name, Qt::CaseInsensitive) <= 0,
              "native Cast delta did not restore name ordering");
      previous = name;
    }
    selected.clear();
    static_cast<CastSelection *>(viewer->getPanel()->getSelection())->getSelectedLevels(selected);
    require(selected.size() == 1 && selected.front() == renamed,
            "native Cast delta changed selected asset identity");
    require(scroll->value() == scrollBefore, "native Cast delta lost scroll position");
  };
  TXshSimpleLevelP added = new TXshSimpleLevel(L"Cast scale added");
  added->setScene(source); added->setType(PLI_XSHLEVEL);
  added->setPath(TFilePath(localDir + "/Cast scale added.pli"), true);
  captureRefresh();
  measurement.restart(); source->getLevelSet()->insertLevel(added.getPointer());
  TApp::instance()->getCurrentScene()->notifyCastChange();
  metrics["singleAddMs"] = double(measurement.elapsed());
  recordRefresh("singleAddWork", true);
  verifyDelta(deltaCount + 1);
  require(find(browser, "Cast scale added") >= 0, "native Cast add event did not expose the asset");
  captureRefresh(); measurement.restart();
  require(source->getLevelSet()->renameLevel(renamed, L"Cast scale 09999 renamed"), "large Cast rename failed");
  TApp::instance()->getCurrentScene()->notifyCastChange();
  metrics["singleRenameMs"] = double(measurement.elapsed());
  recordRefresh("singleRenameWork", true);
  verifyDelta(deltaCount + 1);
  require(find(browser, "Cast scale 01234") < 0 && find(browser, "Cast scale 09999 renamed") >= 0,
          "native Cast rename retained stale display data");
  captureRefresh(); measurement.restart(); source->getLevelSet()->removeLevel(added.getPointer());
  TApp::instance()->getCurrentScene()->notifyCastChange();
  metrics["singleRemoveMs"] = double(measurement.elapsed());
  recordRefresh("singleRemoveWork", true);
  verifyDelta(deltaCount);
  require(find(browser, "Cast scale added") < 0, "native Cast remove event retained a deleted asset");
  metrics["survivingItemsRetained"] = true;
  require(source->getLevelSet()->renameLevel(renamed, L"Cast scale 01234"), "large Cast name restoration failed");
  TApp::instance()->getCurrentScene()->notifyCastChange();
  captureRefresh(); measurement.restart(); search->setText("Cast scale 01234"); browser.refreshData();
  metrics["searchMs"] = double(measurement.elapsed());
  recordRefresh("searchWork", false);
  require(browser.getItemCount() == 1 && find(browser, "Cast scale 01234") == 0,
          "large Cast search returned incorrect identities");
  search->clear(); status->setCurrentIndex(1); captureRefresh(); measurement.restart(); browser.refreshData();
  metrics["usageFilterMs"] = double(measurement.elapsed());
  recordRefresh("usageFilterWork", true);
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
  TXshSimpleLevelP switchingDrawing = new TXshSimpleLevel(L"Cast scene switch drawing");
  switchingDrawing->setScene(source); switchingDrawing->setType(OVL_XSHLEVEL);
  TRaster32P switchingRaster(80, 60); switchingRaster->fill(TPixel32(70, 110, 180));
  switchingDrawing->setFrame(TFrameId(1), TRasterImageP(switchingRaster));
  switchingDrawing->setFrame(TFrameId(2), TRasterImageP(switchingRaster->clone()));
  source->getLevelSet()->insertLevel(switchingDrawing.getPointer()); browser.refreshData();
  // A queued search refresh still follows the Cast selection. Keep the target
  // selected, as required by the real Inspect action, across that refresh.
  const int drawingIndex = find(browser, "Cast scene switch drawing");
  require(drawingIndex >= 0, "scene-switch preview target is absent from Cast");
  viewer->getPanel()->getSelection()->selectNone();
  viewer->getPanel()->getSelection()->select(drawingIndex);
  selected.clear();
  static_cast<CastSelection *>(viewer->getPanel()->getSelection())->getSelectedLevels(selected);
  require(selected.size() == 1 && selected.front() == switchingDrawing.getPointer(),
          "scene-switch preview target was not selected in Cast");
  CastBrowserActions::inspect(&browser, {switchingDrawing.getPointer()});
  auto *switchInspector = browser.findChild<QWidget *>("CastInspector");
  switchInspector->findChild<QSlider *>("CastDrawingPreview")->setValue(1);
  nextPreviewRequest(switchInspector);
  TXshSoundLevelP switchingSound = new TXshSoundLevel(L"Cast scene switch audio");
  switchingSound->setScene(source);
  switchingSound->setSoundTrack(TSoundTrack::create(8000, 16, 1, 131072, TSound::INT));
  source->getLevelSet()->insertLevel(switchingSound.getPointer()); browser.refreshData();
  viewer->getPanel()->getSelection()->selectNone();
  viewer->getPanel()->getSelection()->select(find(browser, "Cast scene switch audio"));
  CastBrowserActions::inspect(&browser, {switchingSound.getPointer()});
  auto *switchWaveform = browser.findChild<QWidget *>("CastAudioWaveform");
  require(!switchWaveform->property("WaveformReady").toBool(), "scene-switch fixture has no pending waveform");
  require(switchInspector->property("PreviewPending").toBool(), "scene-switch fixture has no pending drawing");
  const int requestsBeforeSwitch = switchInspector->property("PreviewRequests").toInt();
  // The Library fixture begins with the actual native New Scene command.
  runLibraryProvenanceIntegrationTest();
  require(switchWaveform->property("WaveformSampleCount").toInt() == 0 &&
          !browser.findChild<QPushButton *>("CastAudioPlay")->isEnabled(),
          "scene switching retained a pending waveform or stale audio target");
  waitUntil([&]() { return !switchInspector->property("PreviewPending").toBool(); },
            "scene switching retained a pending drawing request");
  require(switchInspector->property("PreviewFrame").toString().isEmpty() &&
          switchInspector->property("PreviewRequests").toInt() == requestsBeforeSwitch,
          "scene switching displayed or requeued an old drawing preview");
  browser.hide();
  runCastInterruptionIntegrationTest();
  std::cout << "cast-native-workflow: PASS\n";
}
