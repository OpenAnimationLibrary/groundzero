#include "riglibrarypane.h"

// Tnz6 includes
#include "tapp.h"
#include "iocommand.h"
#include "posespane.h"
#include "librarycommand.h"

// TnzQt includes
#include "toonzqt/dvdialog.h"

// TnzLib includes
#include "toonz/poselibrary.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"

// TnzCore includes
#include "tfilepath.h"

// STD includes
#include <algorithm>

// Qt includes
#include <QAction>
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QStringList>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

const QSize RigIconSize(128, 96);
const QSize PoseIconSize(96, 64);

//-----------------------------------------------------------------------------

// The name shown for a kind of library item (see LibraryCmd::ItemInfo).
QString getKindName(const QString &kind) {
  if (kind == LibraryCmd::SymbolKind) return QObject::tr("Symbol");
  if (kind == LibraryCmd::RigKind) return QObject::tr("Rig");
  if (kind == LibraryCmd::PoseSetKind) return QObject::tr("Pose Set");
  return QString();
}

//-----------------------------------------------------------------------------

// The tooltip of a library item: its name, its kind and revision, the scene
// it comes from and its file.
QString getItemToolTip(const QFileInfo &file, const QString &detail) {
  QStringList lines;
  lines << file.completeBaseName();
  LibraryCmd::ItemInfo info;
  if (LibraryCmd::readItemInfo(TFilePath(file.absoluteFilePath()), info)) {
    QString kind = getKindName(info.m_kind);
    lines << (kind.isEmpty() ? QObject::tr("Revision %1").arg(info.m_revision)
                             : QObject::tr("%1, revision %2")
                                   .arg(kind)
                                   .arg(info.m_revision));
    if (!info.m_sourceScene.isEmpty())
      lines << QObject::tr("From the scene %1").arg(info.m_sourceScene);
  }
  if (!detail.isEmpty()) lines << detail;
  lines << file.absoluteFilePath();
  return lines.join("\n");
}

//-----------------------------------------------------------------------------

// Returns the thumbnail of the template, i.e. the scene icon saved next to it
// (see ToonzScene::getIconPath()), or a placeholder if there is none, with
// the kind of the item in a corner.
QPixmap makeRigThumbnail(const TFilePath &templatePath, const QString &badge,
                         const QPalette &pal) {
  QPixmap pixmap(RigIconSize);
  pixmap.fill(Qt::transparent);

  QImage icon(ToonzScene::getIconPath(templatePath).getQString());

  QPainter p(&pixmap);
  p.setRenderHint(QPainter::Antialiasing, true);
  QRect frame(0, 0, RigIconSize.width(), RigIconSize.height());
  p.fillRect(frame, pal.color(QPalette::Base));
  if (icon.isNull()) {
    p.setPen(pal.color(QPalette::Mid));
    p.drawText(frame, Qt::AlignCenter, QObject::tr("No preview"));
  } else {
    QImage scaled =
        icon.scaled(RigIconSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    p.drawImage((frame.width() - scaled.width()) / 2,
                (frame.height() - scaled.height()) / 2, scaled);
  }
  p.setPen(pal.color(QPalette::Mid));
  p.drawRect(frame.adjusted(0, 0, -1, -1));

  if (!badge.isEmpty()) {
    QFont font = p.font();
    font.setPointSizeF(std::max(6.0, font.pointSizeF() * 0.8));
    p.setFont(font);
    QFontMetrics fm(font);
    QRect badgeRect(3, 0, fm.horizontalAdvance(badge) + 8, fm.height() + 2);
    badgeRect.moveBottom(frame.height() - 4);
    p.setPen(Qt::NoPen);
    p.setBrush(pal.color(QPalette::Highlight));
    p.drawRoundedRect(badgeRect, 3, 3);
    p.setPen(pal.color(QPalette::HighlightedText));
    p.drawText(badgeRect, Qt::AlignCenter, badge);
  }
  return pixmap;
}

//-----------------------------------------------------------------------------

// A pose or a pose set has no picture: its thumbnail is a badge with its name
// and a detail (the number of channels or poses it stores).
QPixmap makeBadgeThumbnail(const QString &name, const QString &detail,
                           const QPalette &pal) {
  QPixmap pixmap(PoseIconSize);
  pixmap.fill(Qt::transparent);

  QPainter p(&pixmap);
  p.setRenderHint(QPainter::Antialiasing, true);
  const int w = PoseIconSize.width(), h = PoseIconSize.height();

  p.setBrush(pal.color(QPalette::Button));
  p.setPen(pal.color(QPalette::Mid));
  p.drawRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), 6, 6);

  p.setPen(pal.color(QPalette::ButtonText));
  QFont font = p.font();
  font.setBold(true);
  p.setFont(font);
  p.drawText(QRect(4, 4, w - 8, h * 2 / 3 - 4),
             Qt::AlignCenter | Qt::TextWordWrap, name);

  font.setBold(false);
  font.setPointSizeF(std::max(6.0, font.pointSizeF() * 0.85));
  p.setFont(font);
  p.drawText(QRect(4, h * 2 / 3, w - 8, h / 3 - 2), Qt::AlignCenter, detail);
  return pixmap;
}

//-----------------------------------------------------------------------------

QPixmap makePoseThumbnail(const QString &name, int channelCount,
                          const QPalette &pal) {
  return makeBadgeThumbnail(name, QObject::tr("%1 channels").arg(channelCount),
                            pal);
}

//=============================================================================
// RigListWidget
//-----------------------------------------------------------------------------
/*! The list of the templates. Dragging a template gives the url of its file,
    as the File Browser does, so the xsheet loads it as a sub-xsheet.  */

class RigListWidget final : public QListWidget {
public:
  RigListWidget(QWidget *parent) : QListWidget(parent) {}

protected:
  void startDrag(Qt::DropActions supportedActions) override {
    QListWidgetItem *item = currentItem();
    if (!item) return;

    QMimeData *mimeData = new QMimeData;
    mimeData->setUrls(QList<QUrl>() << QUrl::fromLocalFile(
                          item->data(Qt::UserRole).toString()));
    QDrag *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    drag->setPixmap(item->icon().pixmap(iconSize()));
    drag->exec(Qt::CopyAction);
  }
};

//-----------------------------------------------------------------------------

void setupIconList(QListWidget *list, const QSize &iconSize) {
  list->setViewMode(QListView::IconMode);
  list->setMovement(QListView::Static);
  list->setResizeMode(QListView::Adjust);
  list->setWrapping(true);
  list->setUniformItemSizes(true);
  list->setWordWrap(true);
  list->setSpacing(6);
  list->setIconSize(iconSize);
  list->setSelectionMode(QAbstractItemView::SingleSelection);
}

//-----------------------------------------------------------------------------

QFileInfoList listItemFiles(const TFilePath &folder, const QString &pattern) {
  return QDir(folder.getQString())
      .entryInfoList(QStringList() << pattern, QDir::Files,
                     QDir::Name | QDir::IgnoreCase);
}

//-----------------------------------------------------------------------------

QWidget *makePage(QWidget *parent, QLabel *emptyLabel, QListWidget *list,
                  QHBoxLayout *buttonLayout) {
  QWidget *page       = new QWidget(parent);
  QVBoxLayout *layout = new QVBoxLayout();
  layout->setContentsMargins(5, 5, 5, 5);
  layout->setSpacing(5);
  layout->addWidget(emptyLabel);
  layout->addWidget(list, 1);
  buttonLayout->setContentsMargins(0, 0, 0, 0);
  buttonLayout->setSpacing(5);
  layout->addLayout(buttonLayout);
  page->setLayout(layout);
  return page;
}

}  // namespace

//=============================================================================
// RigLibraryPane
//-----------------------------------------------------------------------------

RigLibraryPane::RigLibraryPane(QWidget *parent, Qt::WindowFlags flags)
    : QFrame(parent, flags) {
  setObjectName("RigLibraryPane");

  // Rigs & Symbols
  m_rigList = new RigListWidget(this);
  setupIconList(m_rigList, RigIconSize);
  m_rigList->setDragEnabled(true);
  m_rigList->setDragDropMode(QAbstractItemView::DragOnly);
  m_rigList->setDefaultDropAction(Qt::CopyAction);
  m_rigList->setContextMenuPolicy(Qt::CustomContextMenu);
  m_rigList->setToolTip(
      tr("Double-click a rig or a symbol, or drag it into the xsheet, to "
         "insert it as a sub-xsheet"));
  m_rigEmptyLabel = new QLabel(
      tr("No rigs or symbols. Select columns and use \"Save Selection as "
         "Template\", or use \"Save to Library\" on a symbol of the Symbols "
         "panel, to add one."),
      this);
  m_rigEmptyLabel->setWordWrap(true);

  m_insertButton = new QPushButton(tr("Insert"), this);
  m_insertButton->setToolTip(
      tr("Insert the rig or the symbol as a sub-xsheet at the current frame"));
  m_renameRigButton = new QPushButton(tr("Rename..."), this);
  m_deleteRigButton = new QPushButton(tr("Delete"), this);
  m_deleteRigButton->setToolTip(
      tr("Delete the item and the copies of its level files from the "
         "Library"));
  QPushButton *refreshRigsButton = new QPushButton(tr("Refresh"), this);

  // Pose Sets
  m_poseSetList = new QListWidget(this);
  setupIconList(m_poseSetList, PoseIconSize);
  m_poseSetList->setContextMenuPolicy(Qt::CustomContextMenu);
  m_poseSetList->setToolTip(
      tr("Double-click a pose set to add its poses to the scene"));
  m_poseSetEmptyLabel = new QLabel(
      tr("No pose sets. Use \"Save Scene Poses\" here or in the Poses panel to "
         "store the poses of the scene in the Library."),
      this);
  m_poseSetEmptyLabel->setWordWrap(true);

  m_loadPoseSetButton = new QPushButton(tr("Load into Scene"), this);
  m_loadPoseSetButton->setToolTip(
      tr("Add the poses of the set to the poses of the scene"));
  QPushButton *savePoseSetButton =
      new QPushButton(tr("Save Scene Poses..."), this);
  savePoseSetButton->setToolTip(
      tr("Save all the poses of the scene as a pose set of the Library"));
  m_renamePoseSetButton              = new QPushButton(tr("Rename..."), this);
  m_deletePoseSetButton              = new QPushButton(tr("Delete"), this);
  QPushButton *refreshPoseSetsButton = new QPushButton(tr("Refresh"), this);

  // Scene Poses
  m_poseList = new QListWidget(this);
  setupIconList(m_poseList, PoseIconSize);
  m_poseList->setToolTip(
      tr("Double-click a pose to apply it at the current frame"));
  m_poseEmptyLabel = new QLabel(
      tr("The scene has no poses. Create them in the Poses panel."), this);
  m_poseEmptyLabel->setWordWrap(true);

  m_applyButton = new QPushButton(tr("Apply"), this);
  m_applyButton->setToolTip(
      tr("Add the pose to the current values and set keyframes at the "
         "current frame"));
  m_applyLayerButton = new QPushButton(tr("Apply as Offset Layer"), this);
  m_applyLayerButton->setToolTip(
      tr("Apply the pose to an offset layer named after the pose, which is "
         "added to the objects that don't have it"));

  // layout
  QHBoxLayout *rigButtons = new QHBoxLayout();
  rigButtons->addWidget(m_insertButton);
  rigButtons->addWidget(m_renameRigButton);
  rigButtons->addWidget(m_deleteRigButton);
  rigButtons->addStretch(1);
  rigButtons->addWidget(refreshRigsButton);
  QWidget *rigPage = makePage(this, m_rigEmptyLabel, m_rigList, rigButtons);

  QHBoxLayout *poseSetButtons = new QHBoxLayout();
  poseSetButtons->addWidget(m_loadPoseSetButton);
  poseSetButtons->addWidget(savePoseSetButton);
  poseSetButtons->addWidget(m_renamePoseSetButton);
  poseSetButtons->addWidget(m_deletePoseSetButton);
  poseSetButtons->addStretch(1);
  poseSetButtons->addWidget(refreshPoseSetsButton);
  QWidget *poseSetPage =
      makePage(this, m_poseSetEmptyLabel, m_poseSetList, poseSetButtons);

  QHBoxLayout *poseButtons = new QHBoxLayout();
  poseButtons->addWidget(m_applyButton);
  poseButtons->addWidget(m_applyLayerButton);
  poseButtons->addStretch(1);
  QWidget *posePage = makePage(this, m_poseEmptyLabel, m_poseList, poseButtons);

  QTabWidget *tabs = new QTabWidget(this);
  tabs->addTab(rigPage, tr("Rigs && Symbols"));
  tabs->addTab(poseSetPage, tr("Pose Sets"));
  tabs->addTab(posePage, tr("Scene Poses"));

  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(0, 0, 0, 0);
  mainLayout->setSpacing(0);
  mainLayout->addWidget(tabs);
  setLayout(mainLayout);

  // The Library folder is watched, the template files are saved in several
  // steps, so the changes are grouped.
  m_watcher      = new QFileSystemWatcher(this);
  m_refreshTimer = new QTimer(this);
  m_refreshTimer->setSingleShot(true);
  m_refreshTimer->setInterval(300);

  connect(m_watcher, SIGNAL(directoryChanged(const QString &)), this,
          SLOT(onFolderChanged()));
  connect(m_refreshTimer, SIGNAL(timeout()), this, SLOT(refreshLibrary()));
  connect(m_insertButton, SIGNAL(clicked()), this, SLOT(onInsert()));
  connect(m_renameRigButton, SIGNAL(clicked()), this, SLOT(onRenameRig()));
  connect(m_deleteRigButton, SIGNAL(clicked()), this, SLOT(onDeleteRig()));
  connect(refreshRigsButton, SIGNAL(clicked()), this, SLOT(refresh()));
  connect(m_loadPoseSetButton, SIGNAL(clicked()), this, SLOT(onLoadPoseSet()));
  connect(savePoseSetButton, SIGNAL(clicked()), this, SLOT(onSavePoseSet()));
  connect(m_renamePoseSetButton, SIGNAL(clicked()), this,
          SLOT(onRenamePoseSet()));
  connect(m_deletePoseSetButton, SIGNAL(clicked()), this,
          SLOT(onDeletePoseSet()));
  connect(refreshPoseSetsButton, SIGNAL(clicked()), this, SLOT(refresh()));
  connect(m_applyButton, SIGNAL(clicked()), this, SLOT(onApply()));
  connect(m_applyLayerButton, SIGNAL(clicked()), this, SLOT(onApplyAsLayer()));
  connect(m_rigList, &QListWidget::currentItemChanged, this,
          &RigLibraryPane::updateButtons);
  connect(m_poseSetList, &QListWidget::currentItemChanged, this,
          &RigLibraryPane::updateButtons);
  connect(m_poseList, &QListWidget::currentItemChanged, this,
          &RigLibraryPane::updateButtons);
  connect(m_rigList, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem *item) { insertRig(item); });
  connect(m_poseSetList, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem *item) { loadPoseSet(item); });
  connect(m_poseList, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem *item) { applyPose(item, false); });
  connect(m_rigList, &QWidget::customContextMenuRequested, this,
          &RigLibraryPane::onRigContextMenu);
  connect(m_poseSetList, &QWidget::customContextMenuRequested, this,
          &RigLibraryPane::onPoseSetContextMenu);

  updateButtons();
}

//-----------------------------------------------------------------------------

void RigLibraryPane::showEvent(QShowEvent *) {
  TApp *app = TApp::instance();
  connect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
          SLOT(refreshPoses()), Qt::UniqueConnection);
  connect(PoseLibraryNotifier::instance(), SIGNAL(changed()), this,
          SLOT(refreshPoses()), Qt::UniqueConnection);
  refresh();
}

//-----------------------------------------------------------------------------

void RigLibraryPane::hideEvent(QHideEvent *) {
  disconnect(TApp::instance()->getCurrentScene(), SIGNAL(sceneSwitched()), this,
             SLOT(refreshPoses()));
  disconnect(PoseLibraryNotifier::instance(), SIGNAL(changed()), this,
             SLOT(refreshPoses()));
}

//-----------------------------------------------------------------------------

void RigLibraryPane::refresh() {
  refreshLibrary();
  refreshPoses();
}

//-----------------------------------------------------------------------------

void RigLibraryPane::refreshLibrary() {
  refreshRigs();
  refreshPoseSets();
  updateWatchedFolders();
}

//-----------------------------------------------------------------------------

// The folders may not exist yet, or may have been recreated
void RigLibraryPane::updateWatchedFolders() {
  QStringList oldPaths = m_watcher->directories();
  if (!oldPaths.isEmpty()) m_watcher->removePaths(oldPaths);

  TFilePath templatesFolder = LibraryCmd::getTemplatesFolder();
  QStringList paths;
  paths << templatesFolder.getParentDir().getQString()
        << templatesFolder.getQString()
        << (templatesFolder + "sceneIcons").getQString()
        << LibraryCmd::getPosesFolder().getQString();
  for (const QString &path : paths)
    if (QFileInfo(path).isDir()) m_watcher->addPath(path);
}

//-----------------------------------------------------------------------------

void RigLibraryPane::onFolderChanged() { m_refreshTimer->start(); }

//-----------------------------------------------------------------------------

void RigLibraryPane::refreshRigs() {
  QString currentPath;
  if (QListWidgetItem *current = m_rigList->currentItem())
    currentPath = current->data(Qt::UserRole).toString();

  m_rigList->clear();

  QFileInfoList files =
      listItemFiles(LibraryCmd::getTemplatesFolder(), "*.tnz");
  for (const QFileInfo &file : files) {
    TFilePath path(file.absoluteFilePath());
    LibraryCmd::ItemInfo info;
    QString badge;
    if (LibraryCmd::readItemInfo(path, info)) badge = getKindName(info.m_kind);
    QListWidgetItem *item =
        new QListWidgetItem(QIcon(makeRigThumbnail(path, badge, palette())),
                            file.completeBaseName());
    item->setData(Qt::UserRole, file.absoluteFilePath());
    item->setToolTip(getItemToolTip(file, QString()));
    m_rigList->addItem(item);
    if (file.absoluteFilePath() == currentPath) m_rigList->setCurrentItem(item);
  }
  m_rigEmptyLabel->setVisible(m_rigList->count() == 0);

  updateButtons();
}

//-----------------------------------------------------------------------------

void RigLibraryPane::refreshPoseSets() {
  QString currentPath;
  if (QListWidgetItem *current = m_poseSetList->currentItem())
    currentPath = current->data(Qt::UserRole).toString();

  m_poseSetList->clear();

  QString pattern     = QString("*.") + LibraryCmd::PoseSetExtension;
  QFileInfoList files = listItemFiles(LibraryCmd::getPosesFolder(), pattern);
  for (const QFileInfo &file : files) {
    PoseLibrary poses;
    QString detail =
        LibraryCmd::readPoseSet(TFilePath(file.absoluteFilePath()), poses)
            ? tr("%1 poses").arg(poses.getPoseCount())
            : tr("Unreadable");
    QListWidgetItem *item = new QListWidgetItem(
        QIcon(makeBadgeThumbnail(file.completeBaseName(), detail, palette())),
        QString());
    item->setData(Qt::UserRole, file.absoluteFilePath());
    item->setToolTip(getItemToolTip(file, detail));
    m_poseSetList->addItem(item);
    if (file.absoluteFilePath() == currentPath)
      m_poseSetList->setCurrentItem(item);
  }
  m_poseSetEmptyLabel->setVisible(m_poseSetList->count() == 0);

  updateButtons();
}

//-----------------------------------------------------------------------------

void RigLibraryPane::refreshPoses() {
  int currentRow = m_poseList->currentRow();
  m_poseList->clear();

  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (scene) {
    PoseLibrary *poseLibrary = scene->getPoseLibrary();
    for (int i = 0; i < poseLibrary->getPoseCount(); ++i) {
      const PoseLibrary::Pose &pose = poseLibrary->getPose(i);
      QListWidgetItem *item         = new QListWidgetItem(
                  QIcon(makePoseThumbnail(pose.m_name, (int)pose.m_entries.size(),
                                          palette())),
                  QString());
      item->setData(Qt::UserRole, i);
      item->setToolTip(tr("%1 (%2 channels)")
                           .arg(pose.m_name)
                           .arg((int)pose.m_entries.size()));
      m_poseList->addItem(item);
    }
    if (currentRow >= 0 && currentRow < m_poseList->count())
      m_poseList->setCurrentRow(currentRow);
  }
  m_poseEmptyLabel->setVisible(m_poseList->count() == 0);
  updateButtons();
}

//-----------------------------------------------------------------------------

void RigLibraryPane::updateButtons() {
  bool rigSelected = m_rigList->currentItem() != 0;
  m_insertButton->setEnabled(rigSelected);
  m_renameRigButton->setEnabled(rigSelected);
  m_deleteRigButton->setEnabled(rigSelected);
  bool poseSetSelected = m_poseSetList->currentItem() != 0;
  m_loadPoseSetButton->setEnabled(poseSetSelected);
  m_renamePoseSetButton->setEnabled(poseSetSelected);
  m_deletePoseSetButton->setEnabled(poseSetSelected);
  bool poseSelected = m_poseList->currentItem() != 0;
  m_applyButton->setEnabled(poseSelected);
  m_applyLayerButton->setEnabled(poseSelected);
}

//-----------------------------------------------------------------------------

void RigLibraryPane::insertRig(QListWidgetItem *item) {
  if (!item) return;
  QString path = item->data(Qt::UserRole).toString();
  if (!QFileInfo(path).isFile()) {
    DVGui::warning(tr("The template %1 does not exist anymore.").arg(path));
    refreshRigs();
    return;
  }
  // The template is loaded as "Load as Sub-xsheet" does, at the current frame
  // (IoCmd::loadResources() registers the undo of the load)
  IoCmd::loadSubScene(TFilePath(path.toStdWString()));
}

//-----------------------------------------------------------------------------

void RigLibraryPane::loadPoseSet(QListWidgetItem *item) {
  if (!item) return;
  QString path = item->data(Qt::UserRole).toString();
  if (!QFileInfo(path).isFile()) {
    DVGui::warning(tr("The pose set %1 does not exist anymore.").arg(path));
    refreshPoseSets();
    return;
  }
  if (LibraryCmd::loadPoseSet(TFilePath(path.toStdWString()))) refreshPoses();
}

//-----------------------------------------------------------------------------

void RigLibraryPane::applyPose(QListWidgetItem *item, bool asOffsetLayer) {
  if (!item) return;
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (!scene) return;
  PoseLibrary *poseLibrary = scene->getPoseLibrary();
  int index                = item->data(Qt::UserRole).toInt();
  if (index < 0 || index >= poseLibrary->getPoseCount()) {
    refreshPoses();
    return;
  }

  std::wstring layerName;
  if (asOffsetLayer) {
    layerName = poseLibrary->getPose(index).m_name.trimmed().toStdWString();
    if (layerName.empty()) layerName = tr("Pose Layer").toStdWString();
  }
  PosesPane::applyPose(index, 1.0, layerName);
}

//-----------------------------------------------------------------------------

void RigLibraryPane::renameItem(QListWidget *list) {
  QListWidgetItem *item = list->currentItem();
  if (!item) return;
  QString path    = item->data(Qt::UserRole).toString();
  QString oldName = QFileInfo(path).completeBaseName();
  bool ok         = false;
  QString name =
      DVGui::getText(tr("Rename Library Item"), tr("New Name:"), oldName, &ok);
  if (!ok || name.trimmed().isEmpty() || name.trimmed() == oldName) return;
  if (LibraryCmd::renameItem(TFilePath(path.toStdWString()), name))
    refreshLibrary();
}

//-----------------------------------------------------------------------------

void RigLibraryPane::deleteItem(QListWidget *list) {
  QListWidgetItem *item = list->currentItem();
  if (!item) return;
  QString path = item->data(Qt::UserRole).toString();
  QString name = QFileInfo(path).completeBaseName();

  QString question =
      (list == m_rigList)
          ? tr("Delete %1 from the Library, with the copies of its level "
               "files?\nThe scenes where it was inserted with Import keep "
               "their own copies; the ones where it was inserted with Load "
               "use these files and will miss them.\nThis cannot be undone.")
                .arg(name)
          : tr("Delete the pose set %1 from the Library?\nThe poses already "
               "loaded into scenes stay there. This cannot be undone.")
                .arg(name);
  if (DVGui::MsgBox(question, tr("Delete"), tr("Cancel"), 1) != 1) return;

  LibraryCmd::deleteItem(TFilePath(path.toStdWString()));
  refreshLibrary();
}

//-----------------------------------------------------------------------------

void RigLibraryPane::onInsert() { insertRig(m_rigList->currentItem()); }

void RigLibraryPane::onRenameRig() { renameItem(m_rigList); }

void RigLibraryPane::onDeleteRig() { deleteItem(m_rigList); }

void RigLibraryPane::onLoadPoseSet() {
  loadPoseSet(m_poseSetList->currentItem());
}

void RigLibraryPane::onSavePoseSet() {
  if (LibraryCmd::savePoseSet()) refreshPoseSets();
}

void RigLibraryPane::onRenamePoseSet() { renameItem(m_poseSetList); }

void RigLibraryPane::onDeletePoseSet() { deleteItem(m_poseSetList); }

void RigLibraryPane::onApply() { applyPose(m_poseList->currentItem(), false); }

void RigLibraryPane::onApplyAsLayer() {
  applyPose(m_poseList->currentItem(), true);
}

//-----------------------------------------------------------------------------

void RigLibraryPane::onRigContextMenu(const QPoint &pos) {
  QListWidgetItem *item = m_rigList->itemAt(pos);
  if (item) m_rigList->setCurrentItem(item);

  QMenu menu(this);
  if (item) {
    QAction *insertAction = menu.addAction(tr("Insert"));
    connect(insertAction, &QAction::triggered, this, &RigLibraryPane::onInsert);
    QAction *renameAction = menu.addAction(tr("Rename..."));
    connect(renameAction, &QAction::triggered, this,
            &RigLibraryPane::onRenameRig);
    QAction *deleteAction = menu.addAction(tr("Delete"));
    connect(deleteAction, &QAction::triggered, this,
            &RigLibraryPane::onDeleteRig);
    menu.addSeparator();
  }
  QAction *refreshAction = menu.addAction(tr("Refresh"));
  connect(refreshAction, &QAction::triggered, this, &RigLibraryPane::refresh);
  menu.exec(m_rigList->viewport()->mapToGlobal(pos));
}

//-----------------------------------------------------------------------------

void RigLibraryPane::onPoseSetContextMenu(const QPoint &pos) {
  QListWidgetItem *item = m_poseSetList->itemAt(pos);
  if (item) m_poseSetList->setCurrentItem(item);

  QMenu menu(this);
  if (item) {
    QAction *loadAction = menu.addAction(tr("Load into Scene"));
    connect(loadAction, &QAction::triggered, this,
            &RigLibraryPane::onLoadPoseSet);
    QAction *renameAction = menu.addAction(tr("Rename..."));
    connect(renameAction, &QAction::triggered, this,
            &RigLibraryPane::onRenamePoseSet);
    QAction *deleteAction = menu.addAction(tr("Delete"));
    connect(deleteAction, &QAction::triggered, this,
            &RigLibraryPane::onDeletePoseSet);
    menu.addSeparator();
  }
  QAction *saveAction = menu.addAction(tr("Save Scene Poses..."));
  connect(saveAction, &QAction::triggered, this,
          &RigLibraryPane::onSavePoseSet);
  QAction *refreshAction = menu.addAction(tr("Refresh"));
  connect(refreshAction, &QAction::triggered, this, &RigLibraryPane::refresh);
  menu.exec(m_poseSetList->viewport()->mapToGlobal(pos));
}
