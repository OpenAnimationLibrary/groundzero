

#include "castviewer.h"

// Tnz6 includes
#include "castselection.h"
#include "castbrowseractions.h"
#include "castworkflowguard.h"
#include "castinspector.h"
#include "toonz/sceneproperties.h"
#include "toonz/castworkflow.h"
#include "tapp.h"
#include "menubarcommandids.h"
#include "floatingpanelcommand.h"
#include "iocommand.h"
#include "filmstripcommand.h"
#include "flipbook.h"

// TnzLib includes
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/levelset.h"
#include "toonz/toonzscene.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshpalettelevel.h"
#include "toonz/txshsoundlevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/levelproperties.h"
#include "tpalette.h"

// TnzQt includes
#include "toonzqt/dvdialog.h"
#include "toonzqt/gutil.h"
#include "toonzqt/trepetitionguard.h"
#include "toonzqt/icongenerator.h"
#include "toonzqt/infoviewer.h"
#include "historytypes.h"

// TnzCore includes
#include "tundo.h"
#include "tfiletype.h"
#include "tsystem.h"

// Qt includes
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDragLeaveEvent>
#include <QHeaderView>
#include <QPainter>
#include <QBoxLayout>
#include <QLabel>
#include <QByteArray>
#include <QDesktopServices>
#include <QUrl>
#include <QMenu>
#include <QDir>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QInputDialog>
#include <QMessageBox>
#include <QUuid>
#include <QSet>
#include <QElapsedTimer>

const char *AudioFolderName = "Audio";

using namespace DVGui;

namespace {
QJsonObject castRefreshMeasurements;
void addRefreshMeasurements(const QJsonObject &measurements) {
  for (auto it = measurements.begin(); it != measurements.end(); ++it)
    castRefreshMeasurements[it.key()] = castRefreshMeasurements[it.key()].toDouble() + it.value().toDouble();
}

QSet<QString> ambiguousCastIds(const std::vector<TXshLevel *> &levels) {
  QSet<QString> seen, ambiguous;
  for (const auto *level : levels) {
    const QString id = level->getCastId();
    if (id.isEmpty() || seen.contains(id)) ambiguous.insert(id);
    seen.insert(id);
  }
  return ambiguous;
}

class CastCollectionsUndo final : public TUndo {
  ToonzScene *m_scene;
  QList<TSceneProperties::CastCollection> m_before, m_after;
  void apply(const QList<TSceneProperties::CastCollection> &collections) const {
    if (TApp::instance()->getCurrentScene()->getScene() != m_scene) return;
    m_scene->getProperties()->setCastCollections(collections);
    TApp::instance()->getCurrentScene()->setDirtyFlag(true);
    TApp::instance()->getCurrentScene()->notifyCastChange();
  }
public:
  CastCollectionsUndo(ToonzScene *scene, const QList<TSceneProperties::CastCollection> &after)
      : m_scene(scene), m_before(scene->getProperties()->getCastCollections()), m_after(after) {}
  void undo() const override { apply(m_before); }
  void redo() const override { apply(m_after); }
  int getSize() const override {
    int size = sizeof(*this);
    for (const auto &list : {m_before, m_after}) for (const auto &collection : list) {
      size += 2 * (collection.id.size() + collection.name.size());
      for (const auto &id : collection.members) size += 2 * id.size();
    }
    return size;
  }
  QString getHistoryString() override { return QObject::tr("Edit Scene Cast Collections"); }
};

// Se il widget del cast viewer viene spostato in toonzqt (e quindi fatto
// dipendere dallo sceneHandle)
// questo undo puo' essere spostato in un nuovo file levelsetcommand in
// toonzlib.
class MoveLevelToFolderUndo final : public TUndo {
  TLevelSet *m_levelSet;
  TXshLevelP m_level;
  TFilePath m_oldFolder;
  TFilePath m_newFolder;

public:
  MoveLevelToFolderUndo(TLevelSet *levelSet, std::wstring levelName,
                        TFilePath newFolder)
      : m_levelSet(levelSet), m_level(levelSet->getLevel(levelName)), m_newFolder(newFolder) {
    TXshLevel *level = m_level.getPointer();
    m_oldFolder      = m_levelSet->getFolder(level);
  }

  void undo() const override {
    m_levelSet->moveLevelToFolder(m_oldFolder, m_level.getPointer());
    TApp::instance()->getCurrentScene()->setDirtyFlag(true);
    TApp::instance()->getCurrentScene()->notifyCastChange();
  }

  void redo() const override {
    m_levelSet->moveLevelToFolder(m_newFolder, m_level.getPointer());
    TApp::instance()->getCurrentScene()->setDirtyFlag(true);
    TApp::instance()->getCurrentScene()->notifyCastChange();
  }

  int getSize() const override { return sizeof(*this); }

  QString getHistoryString() override {
    return QObject::tr("Move Level to Cast Folder");
  }
};

struct CastFolders {
  TFilePath root;
  std::vector<TFilePath> folders;
  std::vector<std::pair<TXshLevelP, TFilePath>> membership;
  explicit CastFolders(TLevelSet *set) : root(set->getDefaultFolder()) {
    std::function<void(TFilePath)> walk = [&](TFilePath path) {
      std::vector<TFilePath> children;
      set->listFolders(children, path);
      for (const auto &child : children) { folders.push_back(child); walk(child); }
    };
    walk(TFilePath());
    std::vector<TXshLevel *> levels;
    set->listLevels(levels);
    for (auto *level : levels) membership.emplace_back(TXshLevelP(level), set->getFolder(level));
  }
  void apply(TLevelSet *set) const {
    if (set->getDefaultFolder() != root) {
      // Keep the default root's position in the native folder ordering.
      // A previous rename uses a distinct name; remove a conflicting custom
      // root defensively before restoring the snapshot's default name.
      set->removeFolder(root);
      set->renameFolder(set->getDefaultFolder(), root.getWideName());
    }
    set->setDefaultFolder(root);
    std::vector<TFilePath> roots;
    set->listFolders(roots, TFilePath());
    for (const auto &folder : roots) {
      if (folder != root && folder != TFilePath(AudioFolderName)) {
        set->removeFolder(folder);
        continue;
      }
      std::vector<TFilePath> children;
      set->listFolders(children, folder);
      for (const auto &child : children) set->removeFolder(child);
    }
    for (const auto &folder : folders)
      set->createFolder(folder.getParentDir(), folder.getWideName());
    for (const auto &entry : membership)
      if (set->getLevel(entry.first->getName()) == entry.first.getPointer())
        set->moveLevelToFolder(entry.second, entry.first.getPointer());
    auto *handle = TApp::instance()->getCurrentScene();
    handle->setDirtyFlag(true);
    handle->notifyCastChange();
  }
};
class CastFolderUndo final : public TUndo {
  TLevelSet *m_set;
  CastFolders m_before, m_after;
public:
  CastFolderUndo(TLevelSet *set, const CastFolders &before)
      : m_set(set), m_before(before), m_after(set) {}
  void undo() const override { m_before.apply(m_set); }
  void redo() const override { m_after.apply(m_set); }
  int getSize() const override { return sizeof(*this) + int(m_before.membership.size()) * 128; }
  QString getHistoryString() override { return QObject::tr("Organize Scene Cast Folders"); }
};
}  // namespace

//=============================================================================
//
// CastTreeViewer
//
//-----------------------------------------------------------------------------

CastTreeViewer::CastTreeViewer(QWidget *parent)
    : QTreeWidget(parent), m_dropTargetItem(0) {
  setObjectName("OnePixelMarginFrame");
  // setObjectName("BrowserTreeView");
  // setStyleSheet("#BrowserTreeView {border: 0px; margin: 1px;
  // qproperty-autoFillBackground: true;}");

  header()->close();
  setIconSize(QSize(16, 16));
  setUniformRowHeights(true);
  // m_treeViewer->setColumnCount(1);

  setAcceptDrops(true);
  connect(this, SIGNAL(itemChanged(QTreeWidgetItem *, int)), this,
          SLOT(onItemChanged(QTreeWidgetItem *, int)));
  connect(this,
          SIGNAL(currentItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)),
          this, SLOT(onFolderChanged(QTreeWidgetItem *, QTreeWidgetItem *)));
  connect(TApp::instance()->getCurrentScene(), SIGNAL(sceneSwitched()), this,
          SLOT(rebuildCastTree()));
  connect(TApp::instance()->getCurrentScene(),
          SIGNAL(castFolderAdded(const TFilePath &)), this,
          SLOT(onCastFolderAdded(const TFilePath &)));

  connect(TApp::instance()->getCurrentScene(), SIGNAL(nameSceneChanged()), this,
          SLOT(onSceneNameChanged()));

  // Connect all possible changes that can alter the
  // bottom horizontal scrollbar to resize contents...
  connect(this, SIGNAL(expanded(const QModelIndex &)), this,
          SLOT(resizeToConts()));

  connect(this, SIGNAL(collapsed(const QModelIndex &)), this,
          SLOT(resizeToConts()));

  connect(this->model(), SIGNAL(layoutChanged()), this, SLOT(resizeToConts()));

  connect(TApp::instance()->getCurrentScene(), &TSceneHandle::castChanged,
          this, &CastTreeViewer::rebuildCastTree);
  rebuildCastTree();
}

//-----------------------------------------------------------------------------

//! Resizes viewport to contents
void CastTreeViewer::resizeToConts(void) { resizeColumnToContents(0); }

//-----------------------------------------------------------------------------

void CastTreeViewer::resizeEvent(QResizeEvent *event) {
  resizeColumnToContents(0);
  QTreeView::resizeEvent(event);
}

//-----------------------------------------------------------------------------

TLevelSet *CastTreeViewer::getLevelSet() {
  return TApp::instance()->getCurrentScene()->getScene()->getLevelSet();
}

//-----------------------------------------------------------------------------

void CastTreeViewer::enableCommands() {
  enableCommand(this, MI_Clear, &CastTreeViewer::deleteFolder);
}

//-----------------------------------------------------------------------------

void CastTreeViewer::onFolderChanged(QTreeWidgetItem *current,
                                     QTreeWidgetItem *previous) {
  // makes the selection current; serves to intercept the MI_Clear command
  makeCurrent();
}

//-----------------------------------------------------------------------------

void CastTreeViewer::populateFolder(QTreeWidgetItem *folder) {
  TFilePath folderPath(
      folder->data(1, Qt::DisplayRole).toString().toStdWString());
  std::vector<TFilePath> folders;
  getLevelSet()->listFolders(folders, folderPath);
  int i;
  for (i = 0; i < (int)folders.size(); i++) {
    TFilePath fp   = folders[i];
    QString fpQstr = QString::fromStdWString(fp.getWideString());
    QString name =
        QString::fromStdWString(fp.withoutParentDir().getWideString());
    QTreeWidgetItem *child =
        new QTreeWidgetItem((QTreeWidgetItem *)0, QStringList(name) << fpQstr);
    if (name != AudioFolderName)
      child->setFlags(child->flags() | Qt::ItemIsEditable);
    folder->addChild(child);
    populateFolder(child);
  }
}

//-----------------------------------------------------------------------------

void CastTreeViewer::onSceneNameChanged() {
  QTreeWidgetItem *root = topLevelItem(0);
  if (!root) return;
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  QString rootName  = QString("Root");
  if (scene) {
    std::wstring name =
        (scene->isUntitled()) ? L"Untitled" : scene->getSceneName();
    rootName = rootName.fromStdWString(name);
  }
  root->setText(0, rootName);
}

//-----------------------------------------------------------------------------

void CastTreeViewer::onCastFolderAdded(const TFilePath &path) {
  QTreeWidgetItem *root = topLevelItem(0)->child(0);
  assert(root->data(1, Qt::DisplayRole).toString() ==
         toQString(getLevelSet()->getDefaultFolder()));
  QString childName     = QString::fromStdWString(path.getWideName());
  QString childPathQstr = QString::fromStdWString(path.getWideString());
  QTreeWidgetItem *childItem =
      new QTreeWidgetItem(root, QStringList(childName) << childPathQstr);
  childItem->setFlags(childItem->flags() | Qt::ItemIsEditable);
  root->addChild(childItem);
  setCurrentItem(childItem);
}

//-----------------------------------------------------------------------------

void CastTreeViewer::rebuildCastTree() {
  const TFilePath current = getCurrentFolder();
  std::set<QString> expanded;
  for (QTreeWidgetItemIterator it(this); *it; ++it)
    if ((*it)->isExpanded()) expanded.insert((*it)->data(1, Qt::DisplayRole).toString());
  QSignalBlocker blocked(this);
  clear();
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  QString rootName  = QString("Root");
  if (scene) {
    std::wstring name =
        (scene->isUntitled()) ? L"Untitled" : scene->getSceneName();
    rootName = rootName.fromStdWString(name);
  }

  QTreeWidgetItem *root =
      new QTreeWidgetItem((QTreeWidgetItem *)0, QStringList(rootName));
  static QIcon clapboard = createQIcon("clapboard");
  root->setIcon(0, clapboard);
  insertTopLevelItem(0, root);
  populateFolder(root);
  for (QTreeWidgetItemIterator it(this); *it; ++it) {
    QString path = (*it)->data(1, Qt::DisplayRole).toString();
    if (expanded.count(path)) (*it)->setExpanded(true);
    if (TFilePath(path) == current) setCurrentItem(*it);
  }

}

//-----------------------------------------------------------------------------

QSize CastTreeViewer::sizeHint() const { return QSize(100, 100); }

//-----------------------------------------------------------------------------

void CastTreeViewer::paintEvent(QPaintEvent *event) {
  QTreeWidget::paintEvent(event);
  QPainter p(viewport());
  if (m_dropTargetItem) {
    p.setPen(QColor(50, 105, 200));
    p.drawRect(visualItemRect(m_dropTargetItem).adjusted(0, 0, -1, 0));
  }
}

//-----------------------------------------------------------------------------

void CastTreeViewer::dragEnterEvent(QDragEnterEvent *e) {
  m_dropFilePath = TFilePath();
  if (acceptResourceOrFolderDrop(e->mimeData()->urls())) {
    if (e->mimeData()->urls().size() != 1) return;
    TFilePath path(e->mimeData()->urls()[0].toLocalFile().toStdWString());

    if (path.getType() == "tnz")
      m_dropFilePath = path;
    else
      return;
  }
  if (!e->mimeData()->hasFormat("application/vnd.toonz.levels") &&
      m_dropFilePath == TFilePath())
    return;
  m_dropTargetItem = itemAt(e->pos());
  if (m_dropTargetItem &&
      m_dropTargetItem->data(0, Qt::DisplayRole).toString() == AudioFolderName)
    m_dropTargetItem = 0;

  e->acceptProposedAction();
  viewport()->update();
}

//-----------------------------------------------------------------------------

void CastTreeViewer::dragMoveEvent(QDragMoveEvent *event) {
  if (!event->mimeData()->hasFormat("application/vnd.toonz.levels") ||
      m_dropFilePath != TFilePath())
    return;

  m_dropTargetItem  = itemAt(event->pos());
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  QString rootName  = QString("Root");
  if (scene) {
    std::wstring name =
        (scene->isUntitled()) ? L"Untitled" : scene->getSceneName();
    rootName = rootName.fromStdWString(name);
  }
  if ((m_dropTargetItem &&
       m_dropTargetItem->data(0, Qt::DisplayRole).toString() ==
           AudioFolderName) ||
      (m_dropFilePath != TFilePath() &&
       m_dropTargetItem->data(0, Qt::DisplayRole).toString() == rootName))
    m_dropTargetItem = 0;

  if (!m_dropTargetItem)
    event->ignore();
  else
    event->acceptProposedAction();
  viewport()->update();
}

//-----------------------------------------------------------------------------

void CastTreeViewer::dropEvent(QDropEvent *event) {
  m_dropTargetItem = 0;
  if (m_dropFilePath != TFilePath()) {
    IoCmd::loadScene(m_dropFilePath);
    m_dropFilePath   = TFilePath();
    m_dropTargetItem = 0;
    update();
    return;
  }

  if (!event->mimeData()->hasFormat("application/vnd.toonz.levels")) return;
  m_dropTargetItem = itemAt(event->pos());
  if (!m_dropTargetItem) return;
  TFilePath folderPath(
      m_dropTargetItem->data(1, Qt::DisplayRole).toString().toStdWString());
  m_dropTargetItem = 0;
  update();

  if (folderPath.getName() == AudioFolderName) {
    event->ignore();
    return;
  }
  event->acceptProposedAction();

  TLevelSet *levelSet = getLevelSet();

  const CastItems *castItems =
      dynamic_cast<const CastItems *>(event->mimeData());
  assert(castItems);
  if (!castItems) return;
  int i;
  TUndoManager::manager()->beginBlock();
  for (i = 0; i < castItems->getItemCount(); i++) {
    CastItem *item = castItems->getItem(i);
    if (item->getLevel() && !item->getLevel()->getSoundLevel()) {
      TXshLevel *level       = item->getLevel();
      if (levelSet->getFolder(level) == folderPath) continue;
      std::wstring levelName = level->getName();
      MoveLevelToFolderUndo *undo =
          new MoveLevelToFolderUndo(levelSet, levelName, folderPath);
      levelSet->moveLevelToFolder(folderPath, level);
      TUndoManager::manager()->add(undo);
    }
  }
  TUndoManager::manager()->endBlock();
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
  emit itemMovedToFolder();
}

//-----------------------------------------------------------------------------

void CastTreeViewer::dragLeaveEvent(QDragLeaveEvent *event) {
  m_dropTargetItem = 0;
  m_dropFilePath   = TFilePath();
  update();
}

//-----------------------------------------------------------------------------

void CastTreeViewer::onItemChanged(QTreeWidgetItem *item, int column) {
  if (column != 0) return;
  if (item->isSelected()) {
    TFilePath oldPath(item->data(1, Qt::DisplayRole).toString().toStdWString());
    const QString label = item->text(0).trimmed();
    const TFilePath proposed = oldPath.getParentDir() + label.toStdWString();
    std::vector<TFilePath> siblings;
    getLevelSet()->listFolders(siblings, oldPath.getParentDir());
    if (label.isEmpty() || label == "." || label == ".." ||
        label.contains('/') || label.contains('\\') ||
        (proposed != oldPath && std::find(siblings.begin(), siblings.end(), proposed) != siblings.end())) {
      QSignalBlocker blocked(this);
      item->setText(0, QString::fromStdWString(oldPath.getWideName()));
      return;
    }
    if (proposed == oldPath) return;
    CastFolders before(getLevelSet());
    TFilePath newPath =
        getLevelSet()->renameFolder(oldPath, label.toStdWString());
    item->setText(1, QString::fromStdWString(newPath.getWideString()));
    if (newPath != oldPath) {
      TUndoManager::manager()->add(new CastFolderUndo(getLevelSet(), before));
      TApp::instance()->getCurrentScene()->setDirtyFlag(true);
      TApp::instance()->getCurrentScene()->notifyCastChange();
    }
  }
}

//-----------------------------------------------------------------------------

TFilePath CastTreeViewer::getCurrentFolder() const {
  if (!currentItem()) return TFilePath();
  QVariant data = currentItem()->data(1, Qt::DisplayRole);
  if (data == QVariant()) return TFilePath();
  return TFilePath(data.toString().toStdWString());
}

//-----------------------------------------------------------------------------

void CastTreeViewer::folderUp() {
  QTreeWidgetItem *item = currentItem();
  if (item && item->parent() != (QTreeWidgetItem *)0)
    setCurrentItem(item->parent());
}

//-----------------------------------------------------------------------------

void CastTreeViewer::newFolder() {
  QTreeWidgetItem *parentItem = currentItem();
  if (parentItem == (QTreeWidgetItem *)0) return;
  QString parentName = parentItem->data(0, Qt::DisplayRole).toString();
  if (parentName == AudioFolderName) return;

  TFilePath parentPath =
      TFilePath(parentItem->data(1, Qt::DisplayRole).toString().toStdWString());
  CastFolders before(getLevelSet());
  QString childName("New Folder");
  std::vector<TFilePath> siblings;
  getLevelSet()->listFolders(siblings, parentPath);
  for (int suffix = 2; std::find(siblings.begin(), siblings.end(), parentPath + childName.toStdWString()) != siblings.end(); ++suffix)
    childName = tr("New Folder %1").arg(suffix);
  TFilePath childPath   = parentPath + childName.toStdWString();
  QString childPathQstr = QString::fromStdWString(childPath.getWideString());
  QTreeWidgetItem *childItem =
      new QTreeWidgetItem(parentItem, QStringList(childName) << childPathQstr);
  childItem->setFlags(childItem->flags() | Qt::ItemIsEditable);
  parentItem->addChild(childItem);
  setCurrentItem(childItem);
  editItem(childItem);
  TLevelSet *levelSet =
      TApp::instance()->getCurrentScene()->getScene()->getLevelSet();
  levelSet->createFolder(parentPath, childName.toStdWString());
  TUndoManager::manager()->add(new CastFolderUndo(levelSet, before));
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
}

//-----------------------------------------------------------------------------

void CastTreeViewer::deleteFolder() {
  CastWorkflowGuard workflow(this);
  QTreeWidgetItem *item = currentItem();
  if (!item || !item->parent()) return;
  if (getCurrentFolder() == getLevelSet()->getDefaultFolder()) return;
  QString itemName = item->data(0, Qt::DisplayRole).toString();
  if (itemName == AudioFolderName) return;
  const TFilePath childPath(item->data(1, Qt::DisplayRole).toString().toStdWString());
  TLevelSet *levelSet = getLevelSet();
  CastFolders before(levelSet);
  if (!workflow.confirm(tr("Delete Cast Folder"), tr("Delete folder ") + itemName + "?")) return;
  // A Cast refresh can replace every tree item inside the modal event loop.
  // Compare the reviewed hierarchy/membership and resolve the live tree again.
  const CastFolders current(levelSet);
  if (current.root != before.root || current.folders != before.folders ||
      current.membership != before.membership) return;
  item = nullptr;
  for (QTreeWidgetItemIterator it(this); *it; ++it)
    if (TFilePath((*it)->data(1, Qt::DisplayRole).toString().toStdWString()) == childPath)
      item = *it;
  if (!item || !item->parent()) return;
  const TFilePath parentPath(item->parent()->data(1, Qt::DisplayRole).toString().toStdWString());
  levelSet->removeFolder(childPath);
  TUndoManager::manager()->add(new CastFolderUndo(levelSet, before));
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
  if (!workflow.valid()) return;
  rebuildCastTree();
  for (QTreeWidgetItemIterator it(this); *it; ++it)
    if (TFilePath((*it)->data(1, Qt::DisplayRole).toString().toStdWString()) == parentPath) {
      setCurrentItem(*it);
      break;
    }
}

//=============================================================================
//
// CastBrowser
//
//-----------------------------------------------------------------------------

CastBrowser::CastBrowser(QWidget *parent, Qt::WindowFlags flags)
    : QSplitter(parent)
    , m_treeViewer(0)
    , m_folderName(0)
    , m_itemViewer(0)
    , m_castItems(new CastItems()) {
  // style sheet
  setObjectName("CastBrowser");
  setFrameStyle(QFrame::StyledPanel);

  m_treeViewer = new CastTreeViewer(this);
  m_treeViewer->resize(300, m_treeViewer->size().height());

  QFrame *box = new QFrame(this);
  box->setFrameStyle(QFrame::StyledPanel);
  QVBoxLayout *boxLayout = new QVBoxLayout(box);
  boxLayout->setContentsMargins(0, 0, 0, 0);
  boxLayout->setSpacing(0);

  m_folderName = new QLabel("", box);
  m_folderName->setObjectName("CastBrowserFolderName");
  m_folderName->setFrameStyle(QFrame::Box);
  m_itemViewer = new DvItemViewer(box, false, true, DvItemViewer::Cast);
  DvItemViewerPanel *viewerPanel = m_itemViewer->getPanel();
  viewerPanel->setMissingTextColor(QColor(200, 0, 0));
  CastSelection *castSelection = new CastSelection();
  castSelection->setBrowser(this);
  viewerPanel->setSelection(castSelection);
  viewerPanel->addColumn(DvItemListModel::FrameCount, 50);
  viewerPanel->addColumn(DvItemListModel::FullPath, 300);
  m_itemViewer->setModel(this);
  m_itemViewer->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

  DvItemViewerTitleBar *titleBar = new DvItemViewerTitleBar(m_itemViewer, box);
  // titleBar->hide();
  DvItemViewerButtonBar *buttonBar =
      new DvItemViewerButtonBar(m_itemViewer, box);

  m_search = new QLineEdit(box);
  m_search->setObjectName("CastSearch");
  m_search->setPlaceholderText(tr("Search names, paths and tags…"));
  m_search->setAccessibleName(tr("Search Scene Cast"));
  m_search->setClearButtonEnabled(true);
  m_scope = new QComboBox(box);
  m_scope->setObjectName("CastSearchScope");
  m_scope->setAccessibleName(tr("Search scope"));
  m_scope->addItems({tr("Current folder"), tr("All scene folders")});
  m_type = new QComboBox(box);
  m_type->setObjectName("CastTypeFilter");
  m_type->setAccessibleName(tr("Asset type"));
  m_type->addItem(tr("All types"), 0);
  m_type->addItem(tr("Vector"), PLI_XSHLEVEL);
  m_type->addItem(tr("Toonz Raster"), TZP_XSHLEVEL);
  m_type->addItem(tr("Raster"), OVL_XSHLEVEL);
  m_type->addItem(tr("Audio"), SND_XSHLEVEL);
  m_type->addItem(tr("Palette"), PLT_XSHLEVEL);
  m_type->addItem(tr("Mesh"), MESH_XSHLEVEL);
  m_status = new QComboBox(box);
  m_status->setObjectName("CastStatusFilter");
  m_status->setAccessibleName(tr("Asset status"));
  m_status->addItem(tr("All statuses"), CastBrowserActions::All);
  m_status->addItem(tr("Used in scene"), CastBrowserActions::Used);
  m_status->addItem(tr("Unused"), CastBrowserActions::Unused);
  m_status->addItem(tr("Absent on disk (new or missing)"), CastBrowserActions::Absent);
  m_status->addItem(tr("External links"), CastBrowserActions::External);
  m_status->addItem(tr("Modified assets"), CastBrowserActions::Modified);
  m_status->addItem(tr("New / not saved"), CastBrowserActions::Unsaved);
  m_status->addItem(tr("Missing saved assets"), CastBrowserActions::Missing);
  m_status->addItem(tr("Decoded drawings cached"), CastBrowserActions::Cached);
  auto *filterRow = new QHBoxLayout;
  filterRow->addWidget(m_scope); filterRow->addWidget(m_type);
  filterRow->addWidget(m_status);
  m_resultCount = new QLabel(box);
  m_resultCount->setWordWrap(true);
  m_actions = new QPushButton(tr("Asset Actions…"), box);
  m_actions->setAccessibleName(tr("Scene Cast asset actions"));
  connect(m_actions, &QPushButton::clicked, this, &CastBrowser::showActions);
  m_collection = new QComboBox(box);
  m_collection->setObjectName("CastCollectionFilter");
  m_collection->setAccessibleName(tr("Scene Cast collection"));
  m_collection->addItem(tr("All assets"), QString());
  m_collectionActions = new QPushButton(tr("Collections…"), box);
  m_collectionActions->setObjectName("CastCollectionActions");
  m_collectionActions->setAccessibleName(tr("Manage named asset collections"));
  connect(m_collectionActions, &QPushButton::clicked, this, &CastBrowser::showCollectionActions);
  connect(m_collection, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { refresh(); });
  m_filterTimer = new QTimer(this);
  m_filterTimer->setSingleShot(true);
  m_filterTimer->setInterval(150);
  connect(m_filterTimer, &QTimer::timeout, this, &CastBrowser::refresh);
  connect(m_search, &QLineEdit::textChanged, this, [this]() { m_filterTimer->start(); });
  for (QComboBox *combo : {m_scope, m_type, m_status})
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { refresh(); });
  connect(castSelection, &DvItemSelection::itemSelectionChanged,
          this, &CastBrowser::rememberSelection);
  boxLayout->addWidget(m_search);
  boxLayout->addLayout(filterRow);
  auto *collections = new QHBoxLayout;
  collections->addWidget(m_collection, 1); collections->addWidget(m_collectionActions);
  boxLayout->addLayout(collections);
  boxLayout->addWidget(m_resultCount);
  boxLayout->addWidget(m_actions);
  boxLayout->addWidget(m_folderName);
  boxLayout->addWidget(titleBar);
  boxLayout->addWidget(m_itemViewer);
  boxLayout->addWidget(buttonBar);

  boxLayout->setAlignment(buttonBar, Qt::AlignBottom);
  box->setLayout(boxLayout);

  addWidget(m_treeViewer);
  addWidget(box);
  m_inspector = new CastInspector(this);
  addWidget(m_inspector);
  m_inspector->hide();

  setStretchFactor(1, 2);

  TSceneHandle *sceneHandle = TApp::instance()->getCurrentScene();

  connect(sceneHandle, &TSceneHandle::sceneSwitched, this, [this]() {
    m_rememberedSelection.clear();
    { QSignalBlocker blocked(m_collection); m_collection->setCurrentIndex(0); }
    m_inspector->setLevels({});
    QSignalBlocker blocked(m_itemViewer->getPanel()->getSelection());
    m_itemViewer->getPanel()->getSelection()->selectNone();
    refresh();
  });
  connect(sceneHandle, SIGNAL(castChanged()), this, SLOT(refresh()));

  TXsheetHandle *xhseetHandle = TApp::instance()->getCurrentXsheet();
  connect(xhseetHandle, &TXsheetHandle::xsheetChanged, this, [this]() { m_filterTimer->start(); });
  connect(TApp::instance()->getCurrentLevel(), &TXshLevelHandle::xshLevelChanged,
          this, [this]() { m_filterTimer->start(); });

  connect(buttonBar, SIGNAL(folderUp()), m_treeViewer, SLOT(folderUp()));
  connect(buttonBar, SIGNAL(newFolder()), m_treeViewer, SLOT(newFolder()));

  connect(m_treeViewer,
          SIGNAL(currentItemChanged(QTreeWidgetItem *, QTreeWidgetItem *)),
          this, SLOT(folderChanged(QTreeWidgetItem *, QTreeWidgetItem *)));

  connect(m_treeViewer, SIGNAL(itemMovedToFolder()), this, SLOT(refresh()));
}

//-----------------------------------------------------------------------------

CastBrowser::~CastBrowser() {
  // Clear handlers targeting this browser while its model and controls still
  // exist. The panel subsequently releases the owned selection object.
  m_itemViewer->getPanel()->enableGlobalSelection(false);
}

//-----------------------------------------------------------------------------

void CastBrowser::sortByDataModel(DataType dataType, bool descending) {
  if (!m_rebuilding) rememberSelection();
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  std::map<const CastItem *, QString> keys;
  if (dataType != FrameCount)
    for (int i = 0; i < getItemCount(); ++i) {
      const CastItem *item = m_castItems->getItem(i);
      keys[item] = dataType == FullPath ? scene->decodeFilePath(item->getLevel()->getPath()).getQString() : item->getName();
    }
  m_castItems->sort([&](const CastItem *a, const CastItem *b) {
    int comparison = 0;
    if (dataType == FrameCount)
      comparison = (a->getFrameCount() > b->getFrameCount()) -
                   (a->getFrameCount() < b->getFrameCount());
    else {
      comparison = QString::compare(keys.at(a), keys.at(b), Qt::CaseInsensitive);
    }
    return descending ? comparison < 0 : comparison > 0;
  });
  setOrderType(dataType);
  setIsDiscendentOrder(descending);
  restoreSelection();
  m_itemViewer->getPanel()->update();
}

//-----------------------------------------------------------------------------

void CastBrowser::folderChanged(QTreeWidgetItem *current,
                                QTreeWidgetItem *previous) {
  refresh();
}

//-----------------------------------------------------------------------------

void CastBrowser::refresh() {
  if (isHidden()) return;
  const int position = m_itemViewer->verticalScrollBar()->value();
  refreshData();
  m_itemViewer->refresh();
  m_itemViewer->verticalScrollBar()->setValue(position);
}

QJsonObject CastBrowser::refreshMeasurements() { return castRefreshMeasurements; }

//-----------------------------------------------------------------------------

/*
void CastBrowser::getLevels(std::vector<TXshLevel*> &levels) const
{
  TLevelSet *levelSet = CastTreeViewer::getLevelSet();
  TFilePath folder = m_treeViewer->getCurrentFolder();
  if(folder != TFilePath())
    levelSet->listLevels(levels, folder);
  else
    levelSet->listLevels(levels);
}
*/

//-----------------------------------------------------------------------------

void CastBrowser::refreshData() {
  if (m_rebuilding) return;
  QElapsedTimer total, phase;
  total.start(); phase.start();
  QJsonObject measurements{{"refreshes", 1}, {"visibleRefreshes", isVisible() ? 1 : 0}};
  rememberSelection();
  measurements["selectionCaptureMs"] = phase.nsecsElapsed() / 1e6;
  phase.restart();
  m_rebuilding = true;
  refreshCollections();
  m_assetStatus.clear();
  m_statusBadge.clear();
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  TLevelSet *levelSet = scene->getLevelSet();
  TFilePath folder = m_treeViewer->getCurrentFolder();
  m_folderName->setText(m_scope->currentIndex() == 1 ? tr("All scene folders") : toQString(folder));
  std::vector<TXshLevel *> all, levels;
  levelSet->listLevels(all);
  const QSet<QString> ambiguousIds = ambiguousCastIds(all);
  std::set<TXshLevel *> live(all.begin(), all.end());
  m_rememberedSelection.erase(std::remove_if(m_rememberedSelection.begin(),
      m_rememberedSelection.end(), [&](const TXshLevelP &p) { return !live.count(p.getPointer()); }),
      m_rememberedSelection.end());
  if (m_scope->currentIndex() == 1 || folder == TFilePath()) levels = all;
  else levelSet->listLevels(levels, folder);
  CastWorkflow::Index usage;
  const int status = m_status->currentData().toInt();
  QSet<QString> members;
  const QString collectionId = m_collection->currentData().toString();
  if (!collectionId.isEmpty())
    for (const auto &collection : scene->getProperties()->getCastCollections())
      if (collection.id == collectionId)
        for (const QString &member : collection.members) members.insert(member);
  const int type = m_type->currentData().toInt();
  const QStringList words = CastWorkflow::queryWords(m_search->text());
  measurements["queryCompilations"] = 1;
  measurements["emptyQueryRefreshes"] = words.empty() ? 1 : 0;
  measurements["setupMs"] = phase.nsecsElapsed() / 1e6;
  phase.restart();
  if (status == 1 || status == 2) {
    CastWorkflow::Limits limits;
    limits.recordOccurrences = false;
    limits.expandInstances = false;
    usage = CastWorkflow::index(scene->getTopXsheet(), {}, {}, limits);
  }
  measurements["usageIndexMs"] = phase.nsecsElapsed() / 1e6;
  measurements["usageCells"] = double(usage.cells);
  phase.restart();
  CastBrowserActions::StatusContext statusContext(scene);
  quint64 queryChecks = 0;
  std::vector<TXshLevel *> visible;
  std::vector<TXshLevelP> retained;
  for (TXshLevel *level : levels) {
    if (!collectionId.isEmpty() &&
        (!members.contains(level->getCastId()) || ambiguousIds.contains(level->getCastId()))) continue;
    if (type && level->getType() != type &&
        !(type == OVL_XSHLEVEL && level->getType() == TZI_XSHLEVEL)) continue;
    CastBrowserActions::AssetStatus asset(statusContext, level);
    if (!words.empty()) {
      ++queryChecks;
      if (!CastWorkflow::matchesWords(words, QString::fromStdWString(level->getName()),
            asset.path().getQString(), level->getCastTags())) continue;
    }
    if (status == 1 && !usage.used.count(level)) continue;
    if (status == 2 && (!usage.complete || usage.cycle || usage.used.count(level))) continue;
    if (status == CastBrowserActions::Absent && !asset.missing()) continue;
    if (status == CastBrowserActions::External && !asset.external()) continue;
    if (status == CastBrowserActions::Modified && !asset.modified()) continue;
    if (status == CastBrowserActions::Unsaved && !asset.unsaved()) continue;
    if (status == CastBrowserActions::Missing && (!level->sourceExpectedOnDisk() || !asset.missing())) continue;
    if (status == CastBrowserActions::Cached && !asset.cached()) continue;
    m_assetStatus[level] = asset.text();
    const QChar badge = asset.badge();
    if (!badge.isNull()) m_statusBadge[level] = badge;
    if (level->getSimpleLevel() || level->getPaletteLevel() || level->getSoundLevel()) {
      visible.push_back(level);
      retained.emplace_back(level);
    }
  }
  measurements["filterAndStatusMs"] = phase.nsecsElapsed() / 1e6;
  measurements["candidateAssets"] = double(levels.size());
  measurements["shownAssets"] = double(visible.size());
  measurements["queryChecks"] = double(queryChecks);
  const auto &queries = statusContext.queries;
  measurements["assetSnapshots"] = double(queries.assets);
  measurements["statusPathDecodes"] = double(queries.decodedPaths);
  measurements["diskQueries"] = double(queries.diskQueries);
  measurements["canonicalPathQueries"] = double(queries.canonicalQueries);
  measurements["canonicalRootSets"] = double(queries.rootSets);
  measurements["cacheFrameQueries"] = double(queries.cacheQueries);
  measurements["libraryRelationshipQueries"] = double(queries.libraryQueries);
  phase.restart();
  // Native castChanged is coarse: predicates and ordering are recomputed, but
  // unchanged rows retain their identity and no longer require new CastItems.
  // Keep removed levels alive until old items and selection indices are gone.
  m_castItems->reconcile(visible, m_itemViewer->getPanel()->getIconSize());
  measurements["reconcileMs"] = phase.nsecsElapsed() / 1e6;
  phase.restart();
  sortByDataModel(getCurrentOrderType(), isDiscendentOrder());
  measurements["sortAndSelectionMs"] = phase.nsecsElapsed() / 1e6;
  phase.restart();
  m_displayedLevels = std::move(retained);
  m_rebuilding = false;
  QString count = tr("%1 assets shown").arg(getItemCount());
  if (!collectionId.isEmpty()) {
    QSet<QString> liveIds;
    for (auto *level : all) liveIds.insert(level->getCastId());
    int absent = 0;
    for (const QString &id : members) if (!liveIds.contains(id)) ++absent;
    count += tr(" / %1 collection members").arg(members.size());
    if (absent) count += tr("; %1 removed assets retained for Undo").arg(absent);
    int ambiguous = 0;
    for (const QString &id : members) if (ambiguousIds.contains(id)) ++ambiguous;
    if (ambiguous) count += tr("; %1 ambiguous asset IDs withheld").arg(ambiguous);
  }
  if ((status == 1 || status == 2) && (!usage.complete || usage.cycle))
    count += tr(" — incomplete usage index; unused results are withheld");
  m_resultCount->setText(count);
  m_inspector->setLevels(selectedLevels());
  measurements["finalInspectorAndLabelsMs"] = phase.nsecsElapsed() / 1e6;
  measurements["refreshDataMs"] = total.nsecsElapsed() / 1e6;
  for (auto it = measurements.begin(); it != measurements.end(); ++it)
    m_refreshMeasurements[it.key()] = m_refreshMeasurements[it.key()].toDouble() + it.value().toDouble();
  addRefreshMeasurements(measurements);
}

std::vector<TXshLevel *> CastBrowser::selectedLevels() const {
  std::vector<TXshLevel *> levels;
  for (int i : m_itemViewer->getSelectedIndices())
    if (i >= 0 && i < getItemCount()) levels.push_back(m_castItems->getItem(i)->getLevel());
  return levels;
}

void CastBrowser::rememberSelection() {
  if (m_rebuilding) return;
  std::set<TXshLevel *> visible;
  for (int i = 0; i < getItemCount(); ++i) visible.insert(m_castItems->getItem(i)->getLevel());
  m_rememberedSelection.erase(std::remove_if(m_rememberedSelection.begin(),
      m_rememberedSelection.end(), [&](const TXshLevelP &p) { return visible.count(p.getPointer()) != 0; }),
      m_rememberedSelection.end());
  for (auto *level : selectedLevels()) m_rememberedSelection.push_back(TXshLevelP(level));
  if (m_inspector) m_inspector->setLevels(selectedLevels());
}

void CastBrowser::restoreSelection() {
  auto *selection = m_itemViewer->getPanel()->getSelection();
  QSignalBlocker blocked(selection);
  selection->selectNone();
  std::set<TXshLevel *> remembered;
  for (const auto &level : m_rememberedSelection) remembered.insert(level.getPointer());
  for (int i = 0; i < getItemCount(); ++i)
    if (remembered.count(m_castItems->getItem(i)->getLevel())) selection->select(i);
}

void CastBrowser::showEvent(QShowEvent *event) {
  QSplitter::showEvent(event);
  refresh();
}

void CastBrowser::showInspector(const std::vector<TXshLevel *> &levels) {
  m_inspector->setLevels(levels);
  m_inspector->show();
  // Opening repeatedly must not collapse either the list or the inspector.
  const auto widths = sizes();
  if (widths.size() == 3 && widths[2] < 240)
    setSizes({std::max(120, width() / 6), std::max(240, width() / 2), std::max(260, width() / 3)});
}

void CastBrowser::refreshCollections() {
  const QString selected = m_collection->currentData().toString();
  QSignalBlocker blocked(m_collection);
  m_collection->clear(); m_collection->addItem(tr("All assets"), QString());
  for (const auto &collection : TApp::instance()->getCurrentScene()->getScene()->getProperties()->getCastCollections())
    m_collection->addItem(collection.name, collection.id);
  const int at = m_collection->findData(selected);
  m_collection->setCurrentIndex(at < 0 ? 0 : at);
}

void CastBrowser::showCollectionActions() {
  CastWorkflowGuard workflow(this);
  ToonzScene *source = TApp::instance()->getCurrentScene()->getScene();
  QMenu menu;
  connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched, &menu, &QMenu::close);
  auto *create = menu.addAction(tr("New Collection from Selected…"));
  auto *add = menu.addAction(tr("Add Selected to Collection…"));
  auto *remove = menu.addAction(tr("Remove Selected from Collection"));
  menu.addSeparator();
  auto *rename = menu.addAction(tr("Rename Collection…"));
  auto *erase = menu.addAction(tr("Delete Collection…"));
  const bool selectedCollection = !m_collection->currentData().toString().isEmpty();
  create->setEnabled(!selectedLevels().empty());
  add->setEnabled(!source->getProperties()->getCastCollections().empty() && !selectedLevels().empty());
  remove->setEnabled(selectedCollection && !selectedLevels().empty());
  rename->setEnabled(selectedCollection); erase->setEnabled(selectedCollection);
  QAction *action = workflow.exec(menu, m_collectionActions->mapToGlobal(QPoint(0, m_collectionActions->height())));
  if (!workflow.valid() || !action) return;
  if (action == create || action == add || action == remove) {
    std::vector<TXshLevel *> levels; source->getLevelSet()->listLevels(levels);
    const auto ambiguous = ambiguousCastIds(levels);
    for (auto *level : selectedLevels()) if (ambiguous.contains(level->getCastId())) {
      workflow.warning(tr("Asset Collections"),
          tr("Selected assets share an ambiguous identity in this scene. Collection membership was not changed."));
      return;
    }
  }
  const auto before = source->getProperties()->getCastCollections();
  auto after = before;
  QString collectionId = m_collection->currentData().toString();
  int index = -1;
  for (int i = 0; i < after.size(); ++i) if (after[i].id == collectionId) index = i;
  if (action != create && action != add && index < 0) return;
  if (action == add) {
    QInputDialog choose;
    choose.setObjectName("CastCollectionTarget");
    choose.setWindowTitle(tr("Add Assets to Collection"));
    choose.setLabelText(tr("Collection"));
    QStringList names;
    for (const auto &collection : after) names << collection.name;
    choose.setComboBoxItems(names); choose.setComboBoxEditable(false);
    if (index >= 0) choose.setTextValue(after[index].name);
    connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched, &choose, &QInputDialog::reject);
    if (workflow.exec(choose) != QDialog::Accepted) return;
    index = names.indexOf(choose.textValue());
    if (index < 0) return;
    collectionId = after[index].id;
  }
  if (action == create || action == rename) {
    if (action == create && (selectedLevels().empty() || after.size() >= 128)) return;
    QInputDialog input;
    input.setObjectName("CastCollectionName");
    input.setWindowTitle(action == create ? tr("New Asset Collection") : tr("Rename Asset Collection"));
    input.setLabelText(tr("Collection name (up to 80 characters)"));
    if (action == rename) input.setTextValue(after[index].name);
    connect(TApp::instance()->getCurrentScene(), &TSceneHandle::sceneSwitched, &input, &QInputDialog::reject);
    if (workflow.exec(input) != QDialog::Accepted) return;
    const QString name = input.textValue().trimmed();
    bool conflict = name.isEmpty() || name.size() > 80;
    for (int i = 0; i < after.size(); ++i)
      if (!(action == rename && i == index) && after[i].name.compare(name, Qt::CaseInsensitive) == 0) conflict = true;
    if (conflict) { workflow.warning(tr("Asset Collections"), tr("Use a unique, nonempty name of at most 80 characters.")); return; }
    if (action == create) {
      TSceneProperties::CastCollection collection;
      collection.id = QUuid::createUuid().toString(QUuid::WithoutBraces); collection.name = name;
      for (auto *level : selectedLevels()) collection.members.append(level->getCastId());
      if (collection.members.empty() || collection.members.size() > 100000) return;
      collectionId = collection.id; after.append(collection);
    } else after[index].name = name;
  } else if (action == erase) {
    if (!workflow.confirm(tr("Delete Asset Collection"),
          tr("Delete collection “%1”? Assets and files will remain unchanged.").arg(after[index].name))) return;
    after.removeAt(index);
  } else {
    auto &members = after[index].members;
    for (auto *level : selectedLevels()) {
      if (action == remove) members.removeAll(level->getCastId());
      else if (!members.contains(level->getCastId())) members.append(level->getCastId());
    }
    if (members.size() > 100000) {
      workflow.warning(tr("Asset Collections"), tr("A collection can contain at most 100,000 assets.")); return;
    }
  }
  // Another Cast may have edited collections during a native modal dialog.
  if (!workflow.valid() ||
      source->getProperties()->getCastCollections() != before || before == after) return;
  auto *undo = new CastCollectionsUndo(source, after);
  undo->redo(); TUndoManager::manager()->add(undo);
  if (!workflow.valid()) return;
  refreshCollections();
  const int at = m_collection->findData(collectionId);
  m_collection->setCurrentIndex(at < 0 ? 0 : at);
  refresh();
}

void CastBrowser::showActions() {
  CastWorkflowGuard workflow(this);
  QMenu menu;
  auto selectedAction = [&](const QString &title, void (*action)(QWidget *, const std::vector<TXshLevel *> &)) {
    QAction *item = menu.addAction(title, &workflow, [&, action]() {
      if (!workflow.valid()) return;
      // A debounce refresh may have run inside the menu's event loop.
      const auto levels = selectedLevels();
      if (!levels.empty()) action(this, levels);
    });
    item->setEnabled(!selectedLevels().empty());
  };
  selectedAction(tr("Where Used…"), CastBrowserActions::whereUsed);
  selectedAction(tr("Inspect / Preview / Audition…"), CastBrowserActions::inspect);
  selectedAction(tr("Edit Tags…"), CastBrowserActions::editTags);
  selectedAction(tr("Relink Selected…"), CastBrowserActions::relink);
  selectedAction(tr("Make Local Copy…"), CastBrowserActions::makeLocal);
  menu.addSeparator();
  menu.addAction(tr("Review Unused Levels…"), &workflow, [&]() { if (workflow.valid()) CastBrowserActions::removeUnused(this); });
  menu.addAction(tr("Review Duplicate Assets…"), &workflow, [&]() { if (workflow.valid()) CastBrowserActions::duplicates(this); });
  menu.addAction(tr("Collection Report…"), &workflow, [&]() { if (workflow.valid()) CastBrowserActions::collectionReport(this); });
  menu.addSeparator();
  menu.addAction(m_inspector->isHidden() ? tr("Show Inspector") : tr("Hide Inspector"), &workflow, [&]() {
    if (!workflow.valid()) return;
    if (m_inspector->isHidden()) showInspector(selectedLevels()); else m_inspector->hide();
  });
  menu.addAction(tr("Refresh Disk Status"), &workflow, [&]() { if (workflow.valid()) refresh(); });
  workflow.exec(menu, m_actions->mapToGlobal(QPoint(0, m_actions->height())));
}

//-----------------------------------------------------------------------------

int CastBrowser::getItemCount() const { return m_castItems->getItemCount(); }

//-----------------------------------------------------------------------------

QVariant CastBrowser::getItemData(int index, DataType dataType,
                                  bool isSelected) {
  if (index < 0 || index >= getItemCount()) return QVariant();
  CastItem *item = m_castItems->getItem(index);
  if (dataType == Name)
    return item->getName();
  else if (dataType == Thumbnail) {
    QPixmap pixmap = item->getPixmap(isSelected);
    const auto badge = m_statusBadge.find(item->getLevel());
    if (badge != m_statusBadge.end() && !pixmap.isNull()) {
      QPainter painter(&pixmap);
      painter.setRenderHint(QPainter::Antialiasing);
      QRect area(std::max(0, pixmap.width() - 19), 2, 17, 17);
      painter.setPen(Qt::black);
      painter.setBrush(badge->second == '!' ? QColor(255, 180, 100) : QColor(240, 225, 130));
      painter.drawEllipse(area);
      painter.drawText(area, Qt::AlignCenter, QString(badge->second));
    }
    return pixmap;
  }
  else if (dataType == ToolTip)
    return item->getToolTip() + "\n" + m_assetStatus[item->getLevel()] +
        "\n" + item->getLevel()->getCastTags().join(", ");
  else if (dataType == PlayAvailable)
    return item->getSimpleLevel() != nullptr;
  else if (dataType == FrameCount)
    return item->getFrameCount();
  else if (dataType == FullPath) {
    ToonzScene *scene   = TApp::instance()->getCurrentScene()->getScene();
    TXshSimpleLevel *sl = item->getSimpleLevel();
    if (sl) return scene->decodeFilePath(sl->getPath()).getQString();
    TXshPaletteLevel *pl = item->getPaletteLevel();
    if (pl) return scene->decodeFilePath(pl->getPath()).getQString();
    TXshSoundLevel *ml = item->getSoundLevel();
    if (ml) return scene->decodeFilePath(ml->getPath()).getQString();
    return "";
  } else if (dataType == VersionControlStatus) {
    if (!item->exists())
      return VC_Missing;
    else
      return 0;  // No version control status displayed
  } else
    return QVariant();
}

//-----------------------------------------------------------------------------

/*
TXshSimpleLevel *CastBrowser::getSelectedSimpleLevel() const
{
  const std::set<int> &indices = m_itemViewer->getSelectedIndices();
  if(indices.empty()) return 0;

  int index = *indices.begin();

  std::vector<TXshLevel*> levels;
  getLevels(levels);

  if(index<0 || index>=(int)levels.size()) return 0;
  TXshLevel*level = levels[index];
  if(!level) return 0;
  else return level->getSimpleLevel();
}
*/

//-----------------------------------------------------------------------------

void CastBrowser::startDragDrop() {
  TRepetitionGuard guard;
  if (!guard.hasLock()) return;

  const std::set<int> &indices = m_itemViewer->getSelectedIndices();
  if (indices.empty()) return;
  QDrag *drag = new QDrag(this);

  QVariant v = getItemData(*indices.begin(), Thumbnail);
  if (v != QVariant()) {
    QPixmap dropThumbnail = v.value<QPixmap>();
    if (!dropThumbnail.isNull()) drag->setPixmap(dropThumbnail);
  }
  drag->setMimeData(m_castItems->getSelectedItems(indices));
  Qt::DropAction dropAction = drag->exec(Qt::CopyAction | Qt::MoveAction);
}

//-----------------------------------------------------------------------------

bool CastBrowser::acceptDrop(const QMimeData *data) const {
  return acceptResourceOrFolderDrop(data->urls());
}

//-----------------------------------------------------------------------------

bool CastBrowser::drop(const QMimeData *data) {
  if (!acceptDrop(data)) return false;

  if (data->hasUrls()) {
    IoCmd::LoadResourceArguments args;

    for (const QUrl &url : data->urls()) {
      TFilePath fp(url.toLocalFile().toStdWString());
      args.resourceDatas.push_back(fp);
    }

    args.castFolder = m_treeViewer->getCurrentFolder();

    IoCmd::loadResources(args);
  }

  refresh();
  return true;
}

//-----------------------------------------------------------------------------

QMenu *CastBrowser::getContextMenu(QWidget *parent, int index) {
  const std::set<int> &indices = m_itemViewer->getSelectedIndices();
  if (indices.empty()) {
    // DvItemViewerPanel deletes the returned menu after exec(). Keep it alive
    // independently if its panel is destroyed during that nested event loop.
    QMenu *menu = new QMenu;
    auto *workflow = new CastWorkflowGuard(this);
    workflow->setParent(menu);
    workflow->watch(*menu);
    menu->addAction(tr("Review Unused Levels…"), workflow, [this, workflow]() {
      if (workflow->valid()) CastBrowserActions::removeUnused(this);
    });
    return menu;
  }
  // controllo cosa c'e' nella selezione.
  // => audioSelected, vectorLevelSelected, paletteSelected
  std::set<int>::const_iterator it;
  bool audioSelected       = false;
  bool paletteSelected     = false;
  bool vectorLevelSelected = false;
  bool meshLevelSelected   = false;
  bool toonzRasterLevelSelected = false;
  bool otherFileSelected   = false;
  int levelSelectedCount   = 0;
  for (it = indices.begin(); it != indices.end(); ++it) {
    int index = *it;
    if (index < 0 || index >= m_castItems->getItemCount())
      continue;  // should never happen
    TXshSimpleLevel *sl = m_castItems->getItem(index)->getSimpleLevel();
    if (!sl) {
      if (m_castItems->getItem(index)->getPaletteLevel())
        paletteSelected = true;
      else
        audioSelected = true;
      continue;
    }
    levelSelectedCount++;
    if (sl->getType() == PLI_XSHLEVEL)
        vectorLevelSelected = true;
    else if (sl->getType() == MESH_XSHLEVEL)
        meshLevelSelected = true;
    else if (sl->getType() == TZP_XSHLEVEL)
        toonzRasterLevelSelected = true;
    else
      otherFileSelected = true;
  }
  QMenu *menu        = new QMenu;
  auto *workflow = new CastWorkflowGuard(this);
  workflow->setParent(menu);
  workflow->watch(*menu);
  CommandManager *cm = CommandManager::instance();

  menu->addAction(cm->getAction(MI_LevelSettings));

  menu->addAction(cm->getAction(MI_ExposeResource));
  menu->addAction(cm->getAction(MI_ShowFolderContents));
  if (!audioSelected && !paletteSelected && !meshLevelSelected)
    menu->addAction(cm->getAction(MI_ViewFile));
  menu->addAction(cm->getAction(MI_FileInfo));

  // MI_EditLevel solo se e' stato selezionato un singolo diverso da livelli
  // palette a livelli audio
  if (indices.size() == 1 && !audioSelected && !paletteSelected)
    menu->addAction(cm->getAction(MI_EditLevel));
  if (!paletteSelected) menu->addAction(cm->getAction(MI_SaveLevel));
  menu->addSeparator();
  // MI_ConvertToVectors if only non-vector layers were selected
  if (!audioSelected && !paletteSelected && !vectorLevelSelected)
    menu->addAction(cm->getAction(MI_ConvertToVectors));
  if (!audioSelected && !paletteSelected && !toonzRasterLevelSelected)
      menu->addAction(cm->getAction(MI_ConvertToToonzRaster));
  menu->addSeparator();
  menu->addAction(cm->getAction(MI_RemoveLevel));
  menu->addAction(tr("Review Unused Levels…"), workflow, [this, workflow]() { if (workflow->valid()) CastBrowserActions::removeUnused(this); });
  menu->addAction(tr("Where Used…"), workflow, [this, workflow]() { if (workflow->valid()) CastBrowserActions::whereUsed(this, selectedLevels()); });
  menu->addAction(tr("Inspect / Preview / Audition…"), workflow, [this, workflow]() { if (workflow->valid()) CastBrowserActions::inspect(this, selectedLevels()); });

  return menu;
}

//-----------------------------------------------------------------------------

void CastBrowser::expose() {
  std::set<int> selectedIndices =
      m_itemViewer->getPanel()->getSelectedIndices();
  TUndoManager::manager()->beginBlock();
  for (int index : selectedIndices) {
    CastItem *item         = m_castItems->getItem(index);
    TXshSimpleLevel *sl    = item->getSimpleLevel();
    TXshPaletteLevel *pl   = item->getPaletteLevel();
    TXshSoundLevel *soundl = item->getSoundLevel();
    if (sl)
      FilmstripCmd::moveToScene(sl);
    else if (pl)
      FilmstripCmd::moveToScene(pl);
    else if (soundl)
      FilmstripCmd::moveToScene(soundl);
  }
  TUndoManager::manager()->endBlock();
}

//-----------------------------------------------------------------------------

void CastBrowser::edit() {
  CastSelection *castSelection =
      dynamic_cast<CastSelection *>(TSelection::getCurrent());
  if (!castSelection) return;
  std::vector<TXshLevel *> levels;
  castSelection->getSelectedLevels(levels);
  if ((int)levels.size() == 1) {
    TXshLevel *l = levels[0];
    if (l) {
      TXshSimpleLevel *sl = l->getSimpleLevel();
      if (sl) {
        TApp::instance()->getCurrentLevel()->setLevel(sl);
        return;
      }
    }
    error(tr("It is not possible to edit the selected file."));
  } else
    error(tr("It is not possible to edit more than one file at once."));
}

//-----------------------------------------------------------------------------

void CastBrowser::showFolderContents() {
  int i = 0;
  CastSelection *castSelection =
      dynamic_cast<CastSelection *>(TSelection::getCurrent());
  if (!castSelection) return;
  std::vector<TXshLevel *> levels;
  castSelection->getSelectedLevels(levels);
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  TFilePath folderPath, filePath;
  for (i = 0; i < levels.size(); i++) {
    folderPath = levels[i]->getPath().getParentDir();
    filePath   = levels[i]->getPath();

    folderPath = scene->decodeFilePath(folderPath);
    filePath   = scene->decodeFilePath(filePath);

    if (!TSystem::doesExistFileOrLevel(filePath)) {
      error(
          tr("It is not possible to show the folder containing the selected "
             "file, as the file has not been saved yet."));
    } else {
      if (TSystem::isUNC(folderPath))
        QDesktopServices::openUrl(
            QUrl(QString::fromStdWString(folderPath.getWideString())));
      else
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QString::fromStdWString(folderPath.getWideString())));
    }
  }
}
//-----------------------------------------------------------------------------

void CastBrowser::viewFile() {
  CastSelection *castSelection =
      dynamic_cast<CastSelection *>(TSelection::getCurrent());
  if (!castSelection) return;
  std::vector<TXshLevel *> levels;
  castSelection->getSelectedLevels(levels);
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  TFilePath filePath;
  if (levels.empty()) return;
  int i = 0;
  for (i = 0; i < levels.size(); i++) {
    filePath = levels[i]->getPath();

    filePath = scene->decodeFilePath(filePath);
    if (!TSystem::doesExistFileOrLevel(filePath)) {
      error(
          tr("It is not possible to view the selected file, as the file has "
             "not been saved yet."));
    } else {
      if (!TFileType::isViewable(TFileType::getInfo(filePath))) return;

      if (Preferences::instance()->isDefaultViewerEnabled() &&
          (filePath.getType() == "mov" || filePath.getType() == "avi" ||
           filePath.getType() == "3gp"))
        QDesktopServices::openUrl(QUrl("file:///" + toQString(filePath)));
      else
        ::viewFile(filePath);
    }
  }
}

//-----------------------------------------------------------------------------

void CastBrowser::viewFileInfo() {
  CastSelection *castSelection =
      dynamic_cast<CastSelection *>(TSelection::getCurrent());
  if (!castSelection) return;
  std::vector<TXshLevel *> levels;
  castSelection->getSelectedLevels(levels);
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  int i             = 0;

  if (levels.empty()) {
    return;
  } else {
    for (i = 0; i < levels.size(); i++) {
      TFilePath filePath;
      filePath = levels[i]->getPath();

      filePath = scene->decodeFilePath(filePath);
      if (!TSystem::doesExistFileOrLevel(filePath)) {
        error(
            tr("It is not possible to show the info of the selected file, as "
               "the file has not been saved yet."));
      } else {
        InfoViewer *infoViewer = 0;
        infoViewer             = new InfoViewer(this);
        infoViewer->setItem(0, 0, filePath);
      }
    }
  }
}
//=============================================================================

OpenFloatingPanel openCastPane(MI_OpenFileBrowser2, "SceneCast",
                               QObject::tr("Scene Cast"));
