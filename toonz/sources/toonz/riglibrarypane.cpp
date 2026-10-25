#include "riglibrarypane.h"

// Tnz6 includes
#include "tapp.h"
#include "iocommand.h"
#include "posespane.h"

// TnzQt includes
#include "toonzqt/dvdialog.h"

// TnzLib includes
#include "toonz/poselibrary.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/toonzfolders.h"

// TnzCore includes
#include "tfilepath.h"

// STD includes
#include <algorithm>

// Qt includes
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

const QSize RigIconSize(128, 96);
const QSize PoseIconSize(96, 64);

//-----------------------------------------------------------------------------

// Templates are saved here by "Save Selection as Template"
TFilePath getTemplatesFolder() {
  return ToonzFolder::getLibraryFolder() + "templates";
}

//-----------------------------------------------------------------------------

// Returns the thumbnail of the template, i.e. the scene icon saved next to it
// (see ToonzScene::getIconPath()), or a placeholder if there is none.
QPixmap makeRigThumbnail(const TFilePath &templatePath, const QPalette &pal) {
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
  return pixmap;
}

//-----------------------------------------------------------------------------

// A pose has no picture: its thumbnail is a badge with its name and the
// number of channels it stores.
QPixmap makePoseThumbnail(const QString &name, int channelCount,
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
  p.drawText(QRect(4, h * 2 / 3, w - 8, h / 3 - 2), Qt::AlignCenter,
             QObject::tr("%1 channels").arg(channelCount));
  return pixmap;
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

}  // namespace

//=============================================================================
// RigLibraryPane
//-----------------------------------------------------------------------------

RigLibraryPane::RigLibraryPane(QWidget *parent, Qt::WindowFlags flags)
    : QFrame(parent, flags) {
  setObjectName("RigLibraryPane");

  // Rigs
  m_rigList = new RigListWidget(this);
  setupIconList(m_rigList, RigIconSize);
  m_rigList->setDragEnabled(true);
  m_rigList->setDragDropMode(QAbstractItemView::DragOnly);
  m_rigList->setDefaultDropAction(Qt::CopyAction);
  m_rigList->setToolTip(
      tr("Double-click a rig, or drag it into the xsheet, to insert it as a "
         "sub-xsheet"));
  m_rigEmptyLabel = new QLabel(
      tr("No rig templates. Select columns and use \"Save Selection as "
         "Template\" to add one."),
      this);
  m_rigEmptyLabel->setWordWrap(true);

  m_insertButton = new QPushButton(tr("Insert"), this);
  m_insertButton->setToolTip(
      tr("Insert the rig as a sub-xsheet at the current frame"));

  // Poses
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

  QPushButton *refreshButton = new QPushButton(tr("Refresh"), this);

  // layout
  QWidget *rigPage = new QWidget(this);
  {
    QVBoxLayout *layout = new QVBoxLayout();
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(5);
    layout->addWidget(m_rigEmptyLabel);
    layout->addWidget(m_rigList, 1);
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(5);
    buttonLayout->addWidget(m_insertButton);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(refreshButton);
    layout->addLayout(buttonLayout);
    rigPage->setLayout(layout);
  }
  QWidget *posePage = new QWidget(this);
  {
    QVBoxLayout *layout = new QVBoxLayout();
    layout->setContentsMargins(5, 5, 5, 5);
    layout->setSpacing(5);
    layout->addWidget(m_poseEmptyLabel);
    layout->addWidget(m_poseList, 1);
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(5);
    buttonLayout->addWidget(m_applyButton);
    buttonLayout->addWidget(m_applyLayerButton);
    buttonLayout->addStretch(1);
    layout->addLayout(buttonLayout);
    posePage->setLayout(layout);
  }
  QTabWidget *tabs = new QTabWidget(this);
  tabs->addTab(rigPage, tr("Rigs"));
  tabs->addTab(posePage, tr("Poses"));

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
  connect(m_refreshTimer, SIGNAL(timeout()), this, SLOT(refreshRigs()));
  connect(m_insertButton, SIGNAL(clicked()), this, SLOT(onInsert()));
  connect(m_applyButton, SIGNAL(clicked()), this, SLOT(onApply()));
  connect(m_applyLayerButton, SIGNAL(clicked()), this, SLOT(onApplyAsLayer()));
  connect(refreshButton, SIGNAL(clicked()), this, SLOT(refresh()));
  connect(m_rigList, &QListWidget::currentItemChanged, this,
          &RigLibraryPane::updateButtons);
  connect(m_poseList, &QListWidget::currentItemChanged, this,
          &RigLibraryPane::updateButtons);
  connect(m_rigList, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem *item) { insertRig(item); });
  connect(m_poseList, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem *item) { applyPose(item, false); });

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
  refreshRigs();
  refreshPoses();
}

//-----------------------------------------------------------------------------

// The folders may not exist yet, or may have been recreated
void RigLibraryPane::updateWatchedFolders() {
  QStringList oldPaths = m_watcher->directories();
  if (!oldPaths.isEmpty()) m_watcher->removePaths(oldPaths);

  TFilePath templatesFolder = getTemplatesFolder();
  QStringList paths;
  paths << templatesFolder.getParentDir().getQString()
        << templatesFolder.getQString()
        << (templatesFolder + "sceneIcons").getQString();
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

  TFilePath templatesFolder = getTemplatesFolder();
  QFileInfoList files =
      QDir(templatesFolder.getQString())
          .entryInfoList(QStringList() << "*.tnz", QDir::Files,
                         QDir::Name | QDir::IgnoreCase);
  for (const QFileInfo &file : files) {
    TFilePath path(file.absoluteFilePath().toStdWString());
    QListWidgetItem *item = new QListWidgetItem(
        QIcon(makeRigThumbnail(path, palette())), file.completeBaseName());
    item->setData(Qt::UserRole, file.absoluteFilePath());
    item->setToolTip(file.absoluteFilePath());
    m_rigList->addItem(item);
    if (file.absoluteFilePath() == currentPath) m_rigList->setCurrentItem(item);
  }
  m_rigEmptyLabel->setVisible(m_rigList->count() == 0);

  updateWatchedFolders();
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
  m_insertButton->setEnabled(m_rigList->currentItem() != 0);
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
  IoCmd::loadSubScene(TFilePath(path.toStdWString()));
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

void RigLibraryPane::onInsert() { insertRig(m_rigList->currentItem()); }

void RigLibraryPane::onApply() { applyPose(m_poseList->currentItem(), false); }

void RigLibraryPane::onApplyAsLayer() {
  applyPose(m_poseList->currentItem(), true);
}
