#include "castinterruptionintegrationtest.h"
#include "castviewer.h"
#include "castselection.h"
#include "nativeuiintegrationtest.h"
#include "nativefiledialogtest.h"
#include "iocommand.h"
#include "tapp.h"
#include "toonz/childstack.h"
#include "toonz/levelset.h"
#include "toonz/levelproperties.h"
#include "toonz/sceneproperties.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tcolumnhandle.h"
#include "tpalette.h"
#include "trasterimage.h"
#include "tundo.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void resetScene() {
  QString error;
  const bool reset = resetNativeUiIntegrationScene(&error);
  require(reset, qPrintable(error));
}
ToonzScene *scene() { return TApp::instance()->getCurrentScene()->getScene(); }
void settle(const std::function<bool()> &ready) {
  QElapsedTimer elapsed; elapsed.start();
  while (!ready() && elapsed.elapsed() < 5000)
    QApplication::processEvents(QEventLoop::AllEvents, 10);
  require(ready(), "Cast interruption controls did not become visible");
}

using Files = std::map<QString, QByteArray>;
std::set<TFilePath> foldersOf(TLevelSet *levels) {
  std::set<TFilePath> result;
  std::vector<TFilePath> pending{TFilePath()};
  for (size_t at = 0; at < pending.size(); ++at) {
    std::vector<TFilePath> children; levels->listFolders(children, pending[at]);
    for (const auto &folder : children)
      if (result.insert(folder).second) pending.push_back(folder);
  }
  return result;
}
Files filesUnder(const QString &root) {
  Files result;
  QDirIterator it(root, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString path = it.next();
    const QString key = QDir(root).relativeFilePath(path);
    if (it.fileInfo().isDir()) result[key + '/'] = {};
    else {
      QFile file(path);
      require(file.open(QIODevice::ReadOnly), "cannot inspect interruption fixture file");
      result[key] = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    }
  }
  return result;
}

struct LevelState {
  TXshLevelP level;
  QString id;
  std::wstring name;
  TFilePath path, folder;
  QStringList tags;
  TXshLevel::LibraryProvenance origin;
  bool expected;
  std::vector<TFrameId> frames;
  std::vector<TImageP> images;
  std::vector<std::vector<TPixel32>> pixels;
  LevelState(ToonzScene *source, TXshLevel *value)
      : level(value), id(value->getCastId()), name(value->getName()),
        path(value->getPath()), folder(source->getLevelSet()->getFolder(value)),
        tags(value->getCastTags()), origin(value->getLibraryProvenance()),
        expected(value->sourceExpectedOnDisk()) {
    if (auto *simple = value->getSimpleLevel()) {
      simple->getFids(frames);
      for (const auto &frame : frames) {
        images.emplace_back(simple->getFrame(frame, false));
        TRasterImageP image = images.back();
        TRaster32P raster = image ? image->getRaster() : TRasterP();
        std::vector<TPixel32> copy;
        if (raster)
          for (int y = 0; y < raster->getLy(); ++y)
            copy.insert(copy.end(), raster->pixels(y), raster->pixels(y) + raster->getLx());
        pixels.push_back(std::move(copy));
      }
    }
  }
  bool unchanged(ToonzScene *source) const {
    if (source->getLevelSet()->getLevel(name) != level.getPointer() ||
        level->getCastId() != id || level->getName() != name ||
        level->getPath() != path || source->getLevelSet()->getFolder(level.getPointer()) != folder ||
        level->getCastTags() != tags || level->getLibraryProvenance() != origin ||
        level->sourceExpectedOnDisk() != expected) return false;
    if (auto *simple = level->getSimpleLevel()) {
      std::vector<TFrameId> now; simple->getFids(now);
      if (now != frames) return false;
      for (size_t i = 0; i < frames.size(); ++i) {
        if (simple->getFrame(frames[i], false) != images[i]) return false;
        TRasterImageP image = images[i];
        TRaster32P raster = image ? image->getRaster() : TRasterP();
        if (!raster) { if (!pixels[i].empty()) return false; continue; }
        if (pixels[i].size() != size_t(raster->getLx()) * raster->getLy()) return false;
        size_t at = 0;
        for (int y = 0; y < raster->getLy(); ++y)
          for (int x = 0; x < raster->getLx(); ++x)
            if (raster->pixels(y)[x] != pixels[i][at++]) return false;
      }
    }
    return true;
  }
};
struct SheetState {
  TXsheetP sheet;
  std::vector<std::tuple<int, int, TXshCell>> cells;
  int columnCount;
  explicit SheetState(TXsheet *value) : sheet(value), columnCount(value->getColumnCount()) {
    for (int column = 0; column < value->getColumnCount(); ++column) {
      int first, last; value->getCellRange(column, first, last);
      for (int row = first; row <= last; ++row)
        cells.emplace_back(row, column, value->getCell(row, column));
    }
  }
  bool unchanged() const {
    if (sheet->getColumnCount() != columnCount) return false;
    size_t count = 0;
    for (int column = 0; column < sheet->getColumnCount(); ++column) {
      int first, last; sheet->getCellRange(column, first, last);
      for (int row = first; row <= last; ++row) ++count;
    }
    if (count != cells.size()) return false;
    for (const auto &cell : cells)
      if (sheet->getCell(std::get<0>(cell), std::get<1>(cell)) != std::get<2>(cell)) return false;
    return true;
  }
};
struct Snapshot {
  ToonzScene *source = scene();
  std::vector<LevelState> levels;
  std::vector<SheetState> sheets;
  QList<TSceneProperties::CastCollection> collections;
  std::set<TFilePath> folders;
  TFilePath defaultFolder;
  TXsheet *currentSheet;
  TXshLevelP currentLevel;
  int frame, column, depth, history, historyCount;
  bool dirty;
  Files files;
  explicit Snapshot(const QString &root) : collections(source->getProperties()->getCastCollections()),
      folders(foldersOf(source->getLevelSet())), defaultFolder(source->getLevelSet()->getDefaultFolder()),
      currentSheet(TApp::instance()->getCurrentXsheet()->getXsheet()),
      currentLevel(TApp::instance()->getCurrentLevel()->getLevel()),
      frame(TApp::instance()->getCurrentFrame()->getFrame()),
      column(TApp::instance()->getCurrentColumn()->getColumnIndex()),
      depth(source->getChildStack()->getAncestorCount()),
      history(TUndoManager::manager()->getCurrentHistoryIndex()),
      historyCount(TUndoManager::manager()->getHistoryCount()),
      dirty(TApp::instance()->getCurrentScene()->getDirtyFlag()), files(filesUnder(root)) {
    std::vector<TXshLevel *> live; source->getLevelSet()->listLevels(live);
    sheets.emplace_back(source->getTopXsheet());
    for (auto *level : live) {
      levels.emplace_back(source, level);
      if (auto *child = level->getChildLevel()) sheets.emplace_back(child->getXsheet());
    }
  }
  void verify(const QString &root) const {
    require(scene() == source, "interrupted Cast operation switched the scene");
    require(source->getLevelSet()->getLevelCount() == int(levels.size()),
            "interrupted Cast operation changed Cast membership");
    for (const auto &level : levels)
      require(level.unchanged(source), "interrupted Cast operation changed an asset identity, source or metadata");
    for (const auto &sheet : sheets)
      require(sheet.unchanged(), "interrupted Cast operation changed nested exposures");
    require(source->getProperties()->getCastCollections() == collections,
            "interrupted Cast operation changed named collections");
    require(foldersOf(source->getLevelSet()) == folders &&
                source->getLevelSet()->getDefaultFolder() == defaultFolder,
            "interrupted Cast operation changed the folder hierarchy");
    require(TApp::instance()->getCurrentXsheet()->getXsheet() == currentSheet &&
                TApp::instance()->getCurrentLevel()->getLevel() == currentLevel.getPointer() &&
                TApp::instance()->getCurrentFrame()->getFrame() == frame &&
                TApp::instance()->getCurrentColumn()->getColumnIndex() == column &&
                source->getChildStack()->getAncestorCount() == depth,
            "interrupted Cast operation navigated to a stale exposure");
    require(TUndoManager::manager()->getCurrentHistoryIndex() == history &&
                TUndoManager::manager()->getHistoryCount() == historyCount &&
                TApp::instance()->getCurrentScene()->getDirtyFlag() == dirty,
            "interrupted Cast operation changed history or dirty state");
    require(filesUnder(root) == files, "interrupted Cast operation wrote, deleted or retained temporary files");
  }
};

QAbstractButton *acceptButton(QDialog *dialog) {
  if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
    const auto buttons = message->standardButtons();
    return message->button(buttons.testFlag(QMessageBox::Yes) ? QMessageBox::Yes :
                           buttons.testFlag(QMessageBox::Save) ? QMessageBox::Save : QMessageBox::Ok);
  }
  for (auto *box : dialog->findChildren<QDialogButtonBox *>())
    for (auto *button : box->buttons())
      if (box->buttonRole(button) == QDialogButtonBox::AcceptRole) return button;
  return nullptr;
}
QPushButton *namedButton(QWidget *widget, const QString &text) {
  for (auto *button : widget->findChildren<QPushButton *>())
    if (button->text() == text) return button;
  return nullptr;
}
bool confirmationMatches(QMessageBox *message, const QString &target) {
  if (!message || message->icon() != QMessageBox::Question ||
      !message->standardButtons().testFlag(QMessageBox::Yes) ||
      !message->standardButtons().testFlag(QMessageBox::No)) return false;
  // QMessageBox deliberately ignores windowTitle on macOS. Match the actual
  // reviewed operation text and native button roles instead of its caption.
  const QString text = message->text();
  if (target == "Delete Asset Collection")
    return text == "Delete collection “Interruption collection”? Assets and files will remain unchanged.";
  if (target == "Delete Cast Folder") return text == "Delete folder Interruption folder?";
  if (target == "Make Local Copy")
    return text.startsWith("Create 1 local asset copies in ") &&
           text.endsWith("? Undo restores the original links and retains these copies.");
  if (target == "Confirm Duplicate Consolidation")
    return text.startsWith("Replace ") && text.contains(" exposure cells and remove ") &&
           text.endsWith(" duplicate Cast entries? Files remain on disk; Undo restores the exact assets and exposures.");
  return false;
}

enum class Interruption { HideShow, Destroy, HideAncestor, NewScene };
struct Case {
  const char *action;
  const char *target;
  Interruption interruption;
  bool collection = false;
  bool nextProgressBoundary = false;
  bool contextMenu = false;
};

// Timer callbacks only enqueue work. The queued delivery can enter another
// exec() while the repeating timer continues to drive that nested native UI.
class Driver final : public QObject {
  QTimer timer;
  QElapsedTimer elapsed;
  bool queued = false;
  std::function<void()> action;
public:
  QString failure;
  bool expired = false;
  explicit Driver(std::function<void()> callback) : action(std::move(callback)) {
    elapsed.start();
    QObject::connect(&timer, &QTimer::timeout, this, [this]() { enqueue(); });
    timer.start(5);
  }
  void enqueue() {
    if (queued) return;
    queued = true;
    QTimer::singleShot(0, this, [this]() {
      queued = false;
      try {
        if (elapsed.elapsed() >= 12000) {
          expired = true;
          throw std::runtime_error("Cast interruption native modal deadline");
        }
        if (failure.isEmpty()) action();
      } catch (const std::exception &error) { failure = QString::fromUtf8(error.what()); }
      if (!failure.isEmpty()) {
        for (auto *widget : QApplication::topLevelWidgets()) {
          if (auto *menu = qobject_cast<QMenu *>(widget)) menu->close();
          else if (auto *dialog = qobject_cast<QDialog *>(widget)) dialog->reject();
        }
      }
    });
  }
};

struct Fixture {
  QTemporaryDir root;
  QString oldFolder, newFolder, localFolder, scenePath, reportPath;
  TXshSimpleLevelP drawing, duplicate, spare;
  TXshChildLevelP nested;
  TFilePath folder;
  QString collectionId = "cast-interruption-collection";
  Fixture() {
    resetScene();
    require(root.isValid(), "cannot prepare Cast interruption temporary folder");
    oldFolder = root.filePath("original"); newFolder = root.filePath("replacement");
    localFolder = root.filePath("local"); scenePath = localFolder + "/fixture.tnz";
    reportPath = root.filePath("interrupted-report.csv");
    require(QDir().mkpath(oldFolder) && QDir().mkpath(newFolder) && QDir().mkpath(localFolder),
            "cannot prepare Cast interruption source folders");
    QImage picture(19, 13, QImage::Format_ARGB32); picture.fill(QColor(180, 25, 70));
    require(picture.save(oldFolder + "/asset.png"), "cannot save Cast interruption drawing");
    picture.fill(QColor(25, 70, 180));
    require(picture.save(newFolder + "/asset.png"), "cannot save Cast interruption replacement");
    drawing = saved(L"Interruption source", oldFolder + "/asset.png");
    duplicate = saved(L"Interruption duplicate", oldFolder + "/asset.png");
    spare = saved(L"Interruption spare", newFolder + "/asset.png");
    folder = scene()->getLevelSet()->createFolder(scene()->getLevelSet()->getDefaultFolder(), L"Interruption folder");
    const auto childFolder = scene()->getLevelSet()->createFolder(folder, L"Nested folder");
    scene()->getLevelSet()->moveLevelToFolder(childFolder, spare.getPointer());
    drawing->setCastTags(QStringList{"original", "hero"});
    duplicate->setCastTags(QStringList{"duplicate"});
    TXshLevel::LibraryProvenance origin;
    origin.itemId = "cast-interruption-library"; origin.itemPath = root.filePath("template.tnz");
    origin.kind = "rig"; origin.revision = 1; origin.sourceAssetPath = oldFolder + "/asset.png";
    origin.relationship = TXshLevel::LibraryProvenance::Linked;
    drawing->setLibraryProvenance(origin);
    nested = scene()->createNewLevel(CHILD_XSHLEVEL, L"Interruption nested")->getChildLevel();
    nested->getXsheet()->setCell(3, 1, TXshCell(drawing.getPointer(), drawing->getFirstFid()));
    nested->getXsheet()->setCell(4, 2, TXshCell(duplicate.getPointer(), duplicate->getFirstFid()));
    scene()->getTopXsheet()->setCell(0, 0, TXshCell(nested.getPointer(), TFrameId(4)));
    scene()->getTopXsheet()->setCell(2, 1, TXshCell(nested.getPointer(), TFrameId(4)));
    TSceneProperties::CastCollection collection;
    collection.id = collectionId; collection.name = "Interruption collection";
    collection.members = QStringList{drawing->getCastId(), duplicate->getCastId()};
    scene()->getProperties()->setCastCollections({collection});
    scene()->setScenePath(TFilePath(scenePath)); scene()->saveCopy(TFilePath(scenePath));
    TUndoManager::manager()->reset();
    TApp::instance()->getCurrentScene()->setDirtyFlag(false);
  }
  TXshSimpleLevel *saved(const std::wstring &name, const QString &path) {
    auto *level = scene()->createNewLevel(OVL_XSHLEVEL, name)->getSimpleLevel();
    level->setPath(TFilePath(path)); level->load();
    level->getProperties()->setDirtyFlag(false);
    if (level->getPalette()) level->getPalette()->setDirtyFlag(false);
    require(level->getFrameCount() == 1, "Cast interruption drawing did not load");
    return level;
  }
  QString pathFor(QFileDialog *dialog) const {
    const QString title = dialog->windowTitle();
    if (title == "Search for Replacement Assets") return newFolder;
    if (title == "Choose a Project Folder for Local Copies") return localFolder;
    if (title == "Choose Replacement") return newFolder + "/asset.png";
    if (title == "Save Asset Report") return reportPath;
    if (title == "Choose Relocated Collected Scene") return scenePath;
    if (title == "Choose the Relocated Project Root") return root.path();
    throw std::runtime_error(("unexpected Cast file chooser: " + title).toStdString());
  }
  void prepareFile(QFileDialog *dialog) const {
    const QString path = pathFor(dialog);
    if (dialog->fileMode() == QFileDialog::Directory) {
      dialog->setDirectory(path);
      auto *edit = dialog->findChild<QLineEdit *>("fileNameEdit");
      auto *view = dialog->findChild<QListView *>("listView");
      require(edit && view && view->selectionModel(), "Cast directory chooser controls missing");
      view->selectionModel()->clear(); edit->clear();
      require(dialog->selectedFiles().size() == 1 &&
                  QFileInfo(dialog->selectedFiles().front()).canonicalFilePath() == QFileInfo(path).canonicalFilePath(),
              "Cast interruption directory identity differs from requested folder");
    } else {
      dialog->setDirectory(QFileInfo(path).absolutePath());
      auto *edit = dialog->findChild<QLineEdit *>("fileNameEdit");
      require(edit, "Cast file chooser has no filename control");
      edit->setText(QFileInfo(path).fileName());
      require(dialog->selectedFiles().size() == 1 &&
                  QFileInfo(dialog->selectedFiles().front()).absoluteFilePath() == QFileInfo(path).absoluteFilePath(),
              "Cast interruption file identity differs from requested file");
    }
  }
};

void runCase(Fixture &fixture, const Case &test) {
  std::cerr << "cast-interruption: " << test.action << " / " << test.target
            << " / " << int(test.interruption) << std::endl;
  QWidget host(TApp::instance()->getMainWindow(), Qt::Tool);
  auto *layout = new QVBoxLayout(&host);
  QPointer<CastBrowser> browser = new CastBrowser(&host);
  layout->addWidget(browser); host.resize(1000, 650); host.show();
  browser->findChild<QComboBox *>("CastSearchScope")->setCurrentIndex(1);
  browser->refreshData();
  auto *viewer = browser->findChild<DvItemViewer *>();
  int selected = -1;
  for (int i = 0; i < browser->getItemCount(); ++i)
    if (browser->getItemData(i, DvItemListModel::Name).toString() == "Interruption source") selected = i;
  require(selected >= 0 && viewer, "Cast interruption asset is not visible");
  viewer->getPanel()->getSelection()->selectNone();
  viewer->getPanel()->getSelection()->select(selected);
  if (test.collection) {
    auto *collections = browser->findChild<QComboBox *>("CastCollectionFilter");
    collections->setCurrentIndex(collections->findData(fixture.collectionId));
  }
  const bool folderAction = QString(test.action) == "Delete folder";
  auto *tree = browser->findChild<CastTreeViewer *>();
  if (folderAction) {
    const auto found = tree->findItems("Interruption folder", Qt::MatchExactly | Qt::MatchRecursive);
    require(found.size() == 1, "Cast interruption folder is absent from the tree");
    tree->setCurrentItem(found.front());
  }
  auto *launch = namedButton(browser, test.collection ? "Collections…" : "Asset Actions…");
  require(launch, "Cast interruption launch button missing");
  settle([&]() { return launch->isVisible() && launch->isEnabled(); });
  Snapshot before(fixture.root.path());
  std::unique_ptr<Snapshot> switchedState;
  bool launched = false, interrupted = false, lateAttempt = false;
  bool nestedOpened = false, nestedReturned = false;
  bool progressObserved = false;
  int acceptedPreparations = 0;
  auto interrupt = [&](QWidget *window, const std::function<void()> &late) {
    require(!interrupted, "Cast interruption target reached twice");
    interrupted = true;
    // Actual top-level native windows must survive deletion of their panel.
    QWidget *transient = window;
    if (QString(test.target) == "Overwrite CSV") {
      auto *chooser = qobject_cast<QFileDialog *>(window->parentWidget());
      require(chooser && !chooser->parent(), "overwrite confirmation is not owned by an independent chooser");
      transient = chooser;
    } else require(!window->parent(), "Cast modal or menu still has an owning QObject parent");
    require(transient->windowHandle() && transient->windowHandle()->transientParent(),
            "Cast modal or menu lost its native transient relationship");
    QPointer<QWidget> guardedWindow(window);
    if (test.interruption == Interruption::Destroy) {
      delete browser.data();
      require(browser.isNull(), "Cast interruption did not destroy the owner");
    } else if (test.interruption == Interruption::HideAncestor) {
      host.hide(); require(!browser->isVisible(), "Cast ancestor hide did not hide its browser");
      host.show();
    } else if (test.interruption == Interruption::NewScene) {
      ToonzScene *previous = scene();
      require(!TApp::instance()->getCurrentScene()->getDirtyFlag(), "New Scene interruption began dirty");
      IoCmd::newScene();
      require(scene() != previous && scene()->getLevelSet()->getLevelCount() == 0,
              "Cast interruption did not execute actual New Scene");
      switchedState.reset(new Snapshot(fixture.root.path()));
    } else {
      browser->hide(); require(browser->isHidden(), "Cast interruption did not hide owner");
      browser->show();
    }
    require(guardedWindow, "Cast owner destruction deleted a stack modal or menu");
    // Deliver an actual queued action after invalidation, while the native
    // exec frame is still live. A disabled button may correctly emit nothing.
    QObject delivery;
    QMetaObject::invokeMethod(&delivery, [&]() { lateAttempt = true; late(); }, Qt::QueuedConnection);
    QCoreApplication::sendPostedEvents(&delivery, QEvent::MetaCall);
    require(lateAttempt, "Cast interruption did not deliver its queued native action");
  };
  Driver *driverPointer = nullptr;
  Driver driver([&]() {
    if (interrupted) {
      // The parent review is allowed to unwind after its nested chooser closes.
      // A visible survivor here means invalidation failed to close the workflow.
      return;
    }
    if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
      if (launched) return;
      QAction *chosen = nullptr;
      for (auto *action : menu->actions()) if (action->text() == test.action) chosen = action;
      require(chosen && chosen->isEnabled(), "Cast interruption menu action unavailable");
      launched = true;
      if (QString(test.target) == "menu") {
        QPointer<QAction> held(chosen);
        interrupt(menu, [held]() { if (held) held->trigger(); });
      } else {
        menu->setActiveAction(chosen);
        driverPointer->enqueue();
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(menu, &enter);
      }
      return;
    }
    // Some small fixtures finish before QProgressDialog's default display
    // delay. Observe its real event-pumping boundary and display that control.
    for (auto *widget : QApplication::topLevelWidgets()) {
      auto *progress = qobject_cast<QProgressDialog *>(widget);
      if (!progress || progress->labelText() != test.target) continue;
      progress->setMinimumDuration(0); progress->show();
      if (test.nextProgressBoundary && !progressObserved) {
        // The first event boundary is the duplicate loop's progress update.
        // Defer to the next event-pumping boundary while that first asset is
        // still current: assetFiles invokes cancellation before traversing it.
        require(progress->value() == 0, "duplicate traversal was already past its first asset");
        progressObserved = true; driverPointer->enqueue(); return;
      }
      if (test.nextProgressBoundary)
        require(progress->value() == 0, "queued scene switch missed the first asset traversal");
      QPointer<QProgressDialog> held(progress);
      interrupt(progress, [held]() { if (held) held->cancel(); });
      return;
    }
    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
    if (!dialog || qobject_cast<QProgressDialog *>(dialog)) return;
    const QString title = dialog->windowTitle();
    auto *message = qobject_cast<QMessageBox *>(dialog);
    const bool overwrite = QString(test.target) == "Overwrite CSV" && message &&
        (message->standardButtons().testFlag(QMessageBox::Save) ||
         message->standardButtons().testFlag(QMessageBox::Yes)) &&
        message->text().contains(QFileInfo(fixture.reportPath).fileName());
    const bool target = overwrite || confirmationMatches(message, test.target) ||
        (!message && (title == test.target || dialog->objectName() == test.target));
    if (auto *input = qobject_cast<QInputDialog *>(dialog)) {
      if (title != "Add Assets to Collection") input->setTextValue("Interrupted change");
    }
    if (auto *files = qobject_cast<QFileDialog *>(dialog)) fixture.prepareFile(files);
    if (dialog->objectName() == "CastWhereUsed") {
      auto *table = dialog->findChild<QTableWidget *>();
      require(table && table->rowCount() >= 2, "Where Used interruption lacks nested instances");
      table->selectRow(1);
    }
    if (dialog->objectName() == "CastConsolidationReview") {
      auto *table = dialog->findChild<QTableWidget *>();
      require(table && table->rowCount() > 0, "consolidation interruption has no duplicate group");
      table->item(0, 0)->setCheckState(Qt::Checked);
      auto *keep = qobject_cast<QComboBox *>(table->cellWidget(0, 1));
      require(keep && keep->findText("Interruption source") >= 0, "consolidation survivor missing");
      keep->setCurrentIndex(keep->findText("Interruption source"));
    }
    if (target) {
      QPointer<QAbstractButton> button = dialog->objectName() == "CastWhereUsed"
          ? namedButton(dialog, "Go To Exposure") : acceptButton(dialog);
      if (!button) button = namedButton(dialog, "Save Report…");
      require(button && button->isEnabled(), "Cast interruption target has no enabled native action");
      interrupt(dialog, [button]() { if (button) button->click(); });
      return;
    }
    if (title == "Duplicate Asset Review" || title == "Scene Collection Report") {
      if (nestedOpened) {
        if (nestedReturned) dialog->reject();
        return;
      }
      QString buttonText;
      if (QString(test.target) == "Save Asset Report" || QString(test.target) == "Overwrite CSV") buttonText = "Save Report…";
      else if (title == "Duplicate Asset Review") buttonText = "Review Consolidation…";
      else buttonText = "Verify Relocated Scene…";
      auto *button = namedButton(dialog, buttonText);
      require(button && button->isEnabled(), "Cast nested review action missing");
      nestedOpened = true;
      driverPointer->enqueue(); button->click(); nestedReturned = true;
      return;
    }
    if (dialog->objectName() == "CastRelinkReview" && QString(test.target) == "Choose Replacement") {
      auto *browse = namedButton(dialog, "Browse…");
      require(browse && !nestedOpened, "Cast replacement Browse action missing or repeated");
      nestedOpened = true;
      driverPointer->enqueue(); browse->click(); nestedReturned = true;
      return;
    }
    // No warning is expected while preparing a valid interruption boundary.
    if (auto *message = qobject_cast<QMessageBox *>(dialog))
      require(message->standardButtons().testFlag(QMessageBox::Yes), qPrintable(message->text()));
    auto *button = acceptButton(dialog);
    require(button && button->isEnabled(), "Cast preparation has no enabled native accept control");
    ++acceptedPreparations;
    driverPointer->enqueue(); button->click();
  });
  driverPointer = &driver;
  driver.enqueue();
  if (folderAction) { launched = true; tree->deleteFolder(); }
  else if (test.contextMenu) {
    auto *panel = viewer->getPanel();
    const QPoint point = panel->index2pos(selected).center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, point, panel->mapToGlobal(point));
    QApplication::sendEvent(panel, &event);
  }
  else launch->click();
  QApplication::processEvents(QEventLoop::AllEvents, 20);
  if (!driver.failure.isEmpty())
    throw std::runtime_error((QString(test.action) + " / " + test.target + ": " + driver.failure).toStdString());
  require(launched && interrupted && lateAttempt && !driver.expired,
          "Cast interruption did not reach its required native boundary");
  if (QString(test.target) == "Choose Replacement" || QString(test.target) == "Save Asset Report" || QString(test.target) == "Overwrite CSV" ||
      QString(test.target).startsWith("Choose Relocated") || QString(test.target) == "Choose the Relocated Project Root")
    require(nestedOpened && nestedReturned, "Cast nested chooser did not return safely to its owner review");
  if (QString(test.target) == "Confirm Duplicate Consolidation" || QString(test.target) == "Make Local Copy")
    require(acceptedPreparations > 0, "Cast interruption did not pass preparation before final confirmation");
  if (switchedState) {
    switchedState->verify(fixture.root.path());
    require(filesUnder(fixture.root.path()) == before.files,
            "scene switch during Cast workflow changed original files");
    // New Scene deliberately clears the old levels. Their strongly retained
    // decoded drawings must still contain the original artwork.
    for (const auto &level : before.levels)
      for (size_t i = 0; i < level.images.size(); ++i) {
        TRasterImageP image = level.images[i];
        TRaster32P raster = image ? image->getRaster() : TRasterP();
        if (!raster) continue;
        size_t at = 0;
        for (int y = 0; y < raster->getLy(); ++y)
          for (int x = 0; x < raster->getLx(); ++x)
            require(raster->pixels(y)[x] == level.pixels[i][at++],
                    "late Cast action changed artwork retained across New Scene");
      }
  }
  else before.verify(fixture.root.path());
  require(!QApplication::activeModalWidget() && !QApplication::activePopupWidget(),
          "Cast interruption left a stale native modal or popup");
}

void normalTagRoundTrip(Fixture &fixture) {
  QWidget host(TApp::instance()->getMainWindow(), Qt::Tool);
  auto *layout = new QVBoxLayout(&host);
  CastBrowser browser(&host); layout->addWidget(&browser);
  host.resize(900, 600); host.show();
  browser.findChild<QComboBox *>("CastSearchScope")->setCurrentIndex(1);
  browser.refreshData();
  auto *viewer = browser.findChild<DvItemViewer *>();
  int row = -1;
  for (int i = 0; i < browser.getItemCount(); ++i)
    if (browser.getItemData(i, DvItemListModel::Name).toString() == "Interruption source") row = i;
  require(row >= 0, "normal Cast tag recovery target is absent");
  viewer->getPanel()->getSelection()->select(row);
  auto *launch = namedButton(&browser, "Asset Actions…");
  require(launch, "normal Cast tag recovery button is absent");
  settle([&]() { return launch->isVisible() && launch->isEnabled(); });
  const QStringList original = fixture.drawing->getCastTags();
  for (int round = 0; round < 3; ++round) {
    for (bool apply : {false, true}) {
      const Snapshot before(fixture.root.path());
      bool menuChosen = false, inputSeen = false, canceledLateAttempt = false;
      Driver *active = nullptr;
      Driver driver([&]() {
        if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
          if (menuChosen) return;
          for (auto *action : menu->actions()) if (action->text() == "Edit Tags…") {
            menuChosen = true; menu->setActiveAction(action); active->enqueue();
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(menu, &enter); return;
          }
        }
        if (auto *input = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
          inputSeen = true; input->setTextValue("Recovered tags");
          auto *box = input->findChild<QDialogButtonBox *>();
          require(box, "normal tag recovery has no button box");
          auto *button = box->button(apply ? QDialogButtonBox::Ok : QDialogButtonBox::Cancel);
          require(button && button->isEnabled(), "normal tag recovery button is disabled");
          QPointer<QPushButton> lateOk = box->button(QDialogButtonBox::Ok);
          button->click();
          if (!apply) {
            require(browser.isVisible() && lateOk && lateOk->isEnabled(),
                    "tag cancellation did not retain a visible owner and live queued OK target");
            QObject delivery;
            QMetaObject::invokeMethod(&delivery, [&]() {
              canceledLateAttempt = true;
              if (lateOk) lateOk->click();
            }, Qt::QueuedConnection);
            QCoreApplication::sendPostedEvents(&delivery, QEvent::MetaCall);
          }
        }
      });
      active = &driver; driver.enqueue(); launch->click();
      require(menuChosen && inputSeen && !driver.expired && driver.failure.isEmpty(),
              "normal repeated Cast tag workflow failed after interrupted workflows");
      if (!apply) {
        require(canceledLateAttempt, "canceled tag dialog did not receive its queued OK attempt");
        before.verify(fixture.root.path());
      }
      else {
        require(fixture.drawing->getCastTags() == QStringList{"Recovered tags"} &&
                    TUndoManager::manager()->getCurrentHistoryIndex() == before.history + 1,
                "fresh Cast tag acceptance did not apply one undoable edit");
        TUndoManager::manager()->undo();
        require(fixture.drawing->getCastTags() == original &&
                    TUndoManager::manager()->getCurrentHistoryIndex() == before.history,
                "fresh Cast tag undo did not restore exact original tags");
        TApp::instance()->getCurrentScene()->setDirtyFlag(before.dirty);
      }
    }
  }
  TUndoManager::manager()->reset();
}

void folderRefreshRoundTrip(Fixture &fixture) {
  QWidget host(TApp::instance()->getMainWindow(), Qt::Tool);
  auto *layout = new QVBoxLayout(&host);
  CastBrowser browser(&host); layout->addWidget(&browser);
  host.resize(900, 600); host.show();
  auto *tree = browser.findChild<CastTreeViewer *>();
  settle([&]() { return tree && tree->isVisible(); });
  const auto found = tree->findItems("Interruption folder", Qt::MatchExactly | Qt::MatchRecursive);
  require(found.size() == 1, "normal Cast folder fixture is absent");
  tree->setCurrentItem(found.front());
  TUndoManager::manager()->reset();
  const Snapshot before(fixture.root.path());
  bool confirmed = false, refreshed = false;
  Driver driver([&]() {
    auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (!confirmationMatches(message, "Delete Cast Folder") || confirmed) return;
    auto *yes = message->button(QMessageBox::Yes);
    require(yes && yes->isEnabled(), "folder confirmation has no native Yes button");
    confirmed = true;
    QObject delivery;
    QMetaObject::invokeMethod(&delivery, [&]() {
      browser.refreshData(); refreshed = true;
      // The real refresh rebuilt all QTreeWidgetItems while the confirmation
      // was open. Acceptance must resolve the new item, not the stale pointer.
      yes->click();
    }, Qt::QueuedConnection);
    QCoreApplication::sendPostedEvents(&delivery, QEvent::MetaCall);
  });
  tree->deleteFolder();
  require(confirmed && refreshed && driver.failure.isEmpty() && !driver.expired &&
              !foldersOf(scene()->getLevelSet()).count(fixture.folder) &&
              TUndoManager::manager()->getCurrentHistoryIndex() == before.history + 1,
          "confirmed Cast folder removal did not survive a native tree rebuild");
  for (int round = 0; round < 3; ++round) {
    TUndoManager::manager()->undo();
    require(foldersOf(scene()->getLevelSet()) == before.folders,
            "Cast folder undo lost the reviewed hierarchy after refresh");
    if (round < 2) TUndoManager::manager()->redo();
  }
  TUndoManager::manager()->reset();
  TApp::instance()->getCurrentScene()->setDirtyFlag(before.dirty);
  before.verify(fixture.root.path());
}
}  // namespace

void runCastInterruptionIntegrationTest() {
  const bool nativeDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
  struct RestoreDialogs {
    bool value;
    ~RestoreDialogs() { QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, value); }
  } restore{nativeDisabled};
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
  Fixture fixture;
  const Case cases[] = {
      {"Edit Tags…", "menu", Interruption::HideShow},
      {"New Collection from Selected…", "menu", Interruption::Destroy, true},
      {"Where Used…", "menu", Interruption::Destroy, false, false, true},
      {"Edit Tags…", "Edit Scene Cast Tags", Interruption::HideShow},
      {"Edit Tags…", "Edit Scene Cast Tags", Interruption::Destroy},
      {"New Collection from Selected…", "New Asset Collection", Interruption::Destroy, true},
      {"Add Selected to Collection…", "Add Assets to Collection", Interruption::HideShow, true},
      {"Rename Collection…", "Rename Asset Collection", Interruption::HideAncestor, true},
      {"Delete Collection…", "Delete Asset Collection", Interruption::Destroy, true},
      {"Delete folder", "Delete Cast Folder", Interruption::HideShow},
      {"Delete folder", "Delete Cast Folder", Interruption::Destroy},
      {"Where Used…", "CastWhereUsed", Interruption::HideShow},
      {"Where Used…", "CastWhereUsed", Interruption::Destroy},
      {"Where Used…", "Finding exposed level uses…", Interruption::HideAncestor},
      {"Review Unused Levels…", "CastUnusedReview", Interruption::Destroy},
      {"Relink Selected…", "Search for Replacement Assets", Interruption::Destroy},
      {"Relink Selected…", "CastRelinkReview", Interruption::HideShow},
      {"Relink Selected…", "Choose Replacement", Interruption::Destroy},
      {"Relink Selected…", "Validating replacement drawings…", Interruption::HideShow},
      {"Make Local Copy…", "Choose a Project Folder for Local Copies", Interruption::HideShow},
      {"Make Local Copy…", "Preparing local asset copies…", Interruption::Destroy},
      {"Make Local Copy…", "Make Local Copy", Interruption::Destroy},
      {"Review Duplicate Assets…", "Duplicate Asset Review", Interruption::HideShow},
      {"Review Duplicate Assets…", "CastConsolidationReview", Interruption::Destroy},
      {"Review Duplicate Assets…", "Confirm Duplicate Consolidation", Interruption::HideShow},
      {"Collection Report…", "Scene Collection Report", Interruption::Destroy},
      {"Collection Report…", "Save Asset Report", Interruption::Destroy},
      {"Collection Report…", "Choose Relocated Collected Scene", Interruption::HideShow},
      {"Collection Report…", "Choose the Relocated Project Root", Interruption::Destroy},
      {"Collection Report…", "Verifying relocated scene resources…", Interruption::HideShow}};
  for (const auto &test : cases) runCase(fixture, test);
  {
    QFile sentinel(fixture.reportPath);
    const QByteArray original("original report must survive interrupted overwrite\n");
    require(sentinel.open(QIODevice::WriteOnly) && sentinel.write(original) == original.size(),
            "cannot prepare existing CSV overwrite sentinel");
    sentinel.close();
    runCase(fixture, {"Collection Report…", "Overwrite CSV", Interruption::Destroy});
  }
  normalTagRoundTrip(fixture);
  folderRefreshRoundTrip(fixture);
  runCase(fixture, {"Edit Tags…", "Edit Scene Cast Tags", Interruption::NewScene});
  Fixture hashingFixture;
  hashingFixture.drawing->setScannedPath(TFilePath(hashingFixture.oldFolder + "/asset.png"));
  hashingFixture.duplicate->setScannedPath(TFilePath(hashingFixture.oldFolder + "/asset.png"));
  hashingFixture.drawing->getProperties()->setDirtyFlag(false);
  hashingFixture.duplicate->getProperties()->setDirtyFlag(false);
  std::vector<TXshLevel *> firstHashed; scene()->getLevelSet()->listLevels(firstHashed);
  require(!firstHashed.empty() && firstHashed.front()->getSimpleLevel() &&
              !firstHashed.front()->getSimpleLevel()->getScannedPath().isEmpty(),
          "duplicate scene-switch fixture has no first-asset scanned source");
  runCase(hashingFixture, {"Review Duplicate Assets…", "Comparing saved asset contents…",
                          Interruption::NewScene, false, true});
  resetScene();
  std::cout << "cast-interruptions: PASS (native owner hide, destruction, nested prompts and queued actions)\n";
}
