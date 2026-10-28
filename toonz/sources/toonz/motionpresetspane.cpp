#include "motionpresetspane.h"

// Tnz6 includes
#include "tapp.h"
#include "cellselection.h"
#include "columnselection.h"
#include "keyframeselection.h"
#include "menubarcommandids.h"

// TnzQt includes
#include "stageobjectselection.h"
#include "toonzqt/dvdialog.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/tselectionhandle.h"

// TnzLib includes
#include "toonz/toonzfolders.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectcmd.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include "toonz/txsheethandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tscenehandle.h"

// TnzCore includes
#include "tdoubleparam.h"
#include "tmsgcore.h"
#include "tundo.h"
#include "historytypes.h"

// Qt includes
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

// STD includes
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

const QSize IconSize(96, 56);

// Roles of the data of the items of the list
const int PresetIndexRole = Qt::UserRole;      // index in the presets, or -1
const int PathRole        = Qt::UserRole + 1;  // file of the preset
const int UserPresetRole  = Qt::UserRole + 2;  // saved by the user

//-----------------------------------------------------------------------------

bool isTarget(const TStageObjectId &id) {
  if (id.isPegbar() || id.isTable() || id.isCamera()) return true;
  if (!id.isColumn()) return false;
  // Columns without drawings (sound, sound text, palette) do not move
  TXsheet *xsh       = TApp::instance()->getCurrentXsheet()->getXsheet();
  TXshColumn *column = xsh->getColumn(id.getIndex());
  return column && !column->isEmpty() && !column->getSoundColumn() &&
         !column->getSoundTextColumn() && !column->getPaletteColumn();
}

//-----------------------------------------------------------------------------

// The objects the preset is applied to: the selected columns, cells or
// objects, or else the current object, i.e. the one the Animate tool edits.
void getTargetObjects(std::vector<TStageObjectId> &ids) {
  TApp *app             = TApp::instance();
  TSelection *selection = app->getCurrentSelection()->getSelection();

  if (TColumnSelection *columnSelection =
          dynamic_cast<TColumnSelection *>(selection)) {
    for (int index : columnSelection->getIndices())
      if (isTarget(TStageObjectId::ColumnId(index)))
        ids.push_back(TStageObjectId::ColumnId(index));
  } else if (TCellSelection *cellSelection =
                 dynamic_cast<TCellSelection *>(selection)) {
    if (!cellSelection->isEmpty()) {
      const TCellSelection::Range range = cellSelection->getSelectedCells();
      const int c0 = std::max(0, std::min(range.m_c0, range.m_c1));
      const int c1 = std::max(range.m_c0, range.m_c1);
      for (int c = c0; c <= c1; ++c)
        if (isTarget(TStageObjectId::ColumnId(c)))
          ids.push_back(TStageObjectId::ColumnId(c));
    }
  } else if (StageObjectSelection *objectSelection =
                 dynamic_cast<StageObjectSelection *>(selection)) {
    for (const TStageObjectId &id : objectSelection->getObjects())
      if (isTarget(id)) ids.push_back(id);
  }

  if (ids.empty()) {
    TStageObjectId id = app->getCurrentObject()->getObjectId();
    if (isTarget(id)) ids.push_back(id);
  }
}

//-----------------------------------------------------------------------------

// The rows of the selected keyframes of the object (the keyframes selected in
// the xsheet). Returns false if none of the object is selected.
bool getSelectedFrames(const TStageObjectId &id, int &first, int &last) {
  TKeyframeSelection *selection = dynamic_cast<TKeyframeSelection *>(
      TApp::instance()->getCurrentSelection()->getSelection());
  if (!selection || selection->isEmpty()) return false;
  if (!id.isColumn() && !id.isCamera()) return false;

  // The camera is the column -1 of the key selection
  const int column = id.isColumn() ? id.getIndex() : -1;
  first            = std::numeric_limits<int>::max();
  last             = -1;
  for (const TKeyframeSelection::Position &position :
       selection->getSelection()) {
    if (position.second != column) continue;
    first = std::min(first, position.first);
    last  = std::max(last, position.first);
  }
  return last >= first;
}

//-----------------------------------------------------------------------------

void notifyMotionApplied() {
  TApp *app = TApp::instance();
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentObject()->notifyObjectIdChanged(false);
}

//=============================================================================
// MotionPresetNotifyUndo
//-----------------------------------------------------------------------------
/*! Placed at both ends of the undo block of an applied preset, so the viewers
    are refreshed after the keyframes are restored (undo) or set (redo).  */

class MotionPresetNotifyUndo final : public TUndo {
  QString m_name;
  bool m_notifyOnRedo;

public:
  MotionPresetNotifyUndo(const QString &name, bool notifyOnRedo)
      : m_name(name), m_notifyOnRedo(notifyOnRedo) {}

  void undo() const override {
    if (!m_notifyOnRedo) notifyMotionApplied();
  }
  void redo() const override {
    if (m_notifyOnRedo) notifyMotionApplied();
  }
  int getSize() const override { return sizeof(*this); }

  QString getHistoryString() override {
    return QObject::tr("Apply Motion Preset  %1").arg(m_name);
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

//-----------------------------------------------------------------------------

// A file name made of the name of a preset
QString makeFileName(const QString &name) {
  QString result;
  for (const QChar &c : name) {
    if (c.isLetterOrNumber() || c == QLatin1Char(' ') ||
        c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.'))
      result += c;
    else
      result += QLatin1Char('_');
  }
  result = result.trimmed();
  while (result.startsWith(QLatin1Char('.'))) result.remove(0, 1);
  if (result.isEmpty()) result = QLatin1String("preset");
  return result;
}

//-----------------------------------------------------------------------------

// A preset has no picture: its thumbnail plots its curves, each one as high as
// its own range, as they are when applied to a curve that starts from 0 (1 for
// the scales).
QPixmap makeThumbnail(const MotionPreset &preset, const QPalette &pal) {
  QPixmap pixmap(IconSize);
  pixmap.fill(Qt::transparent);

  QPainter p(&pixmap);
  p.setRenderHint(QPainter::Antialiasing, true);
  const QRectF frame(0.5, 0.5, IconSize.width() - 1, IconSize.height() - 1);
  p.setBrush(pal.color(QPalette::Base));
  p.setPen(pal.color(QPalette::Mid));
  p.drawRoundedRect(frame, 4, 4);

  const QColor colors[] = {QColor(230, 90, 80), QColor(80, 190, 100),
                           QColor(90, 140, 240), QColor(235, 170, 50)};
  const QRectF plot     = frame.adjusted(6, 6, -6, -6);
  const double duration = std::max(1.0, preset.getDuration());
  const int sampleCount = 40;

  int colorIndex = 0;
  for (const MotionPreset::Channel &channel : preset.getChannels()) {
    if (colorIndex >= 4) break;
    if (!MotionPreset::isAnimated(channel)) continue;

    TDoubleParam curve(channel.m_mode == MotionPreset::Multiply ? 1.0 : 0.0);
    if (!preset.applyChannel(channel, &curve, 0)) continue;

    std::vector<double> values;
    for (int i = 0; i <= sampleCount; ++i)
      values.push_back(curve.getValue(duration * i / sampleCount));
    const double lo    = *std::min_element(values.begin(), values.end());
    const double hi    = *std::max_element(values.begin(), values.end());
    const double range = hi > lo ? hi - lo : 1.0;

    QPolygonF polygon;
    for (int i = 0; i <= sampleCount; ++i)
      polygon << QPointF(
          plot.left() + plot.width() * i / sampleCount,
          plot.bottom() - plot.height() * (values[i] - lo) / range);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(colors[colorIndex], 1.6));
    p.drawPolyline(polygon);
    ++colorIndex;
  }
  return pixmap;
}

//=============================================================================

class SaveMotionPresetCommand final : public MenuItemHandler {
public:
  SaveMotionPresetCommand() : MenuItemHandler(MI_SaveMotionPreset) {}
  void execute() override { MotionPresetsPane::saveSelectionAsPreset(); }
} saveMotionPresetCommand;

}  // namespace

//=============================================================================
// MotionPresetsPane
//-----------------------------------------------------------------------------

MotionPresetsPane::MotionPresetsPane(QWidget *parent, Qt::WindowFlags flags)
    : QFrame(parent, flags) {
  setObjectName("MotionPresetsPane");

  m_list = new QListWidget(this);
  m_list->setIconSize(IconSize);
  m_list->setSelectionMode(QAbstractItemView::SingleSelection);
  m_list->setToolTip(
      tr("Double-click a preset to apply it to the selected columns at the "
         "current frame"));
  m_emptyLabel = new QLabel(
      tr("No motion presets. Select keyframes and use \"Save Selection as "
         "Preset\" to add one."),
      this);
  m_emptyLabel->setWordWrap(true);
  m_infoLabel = new QLabel(this);
  m_infoLabel->setWordWrap(true);

  m_asLayerCheck = new QCheckBox(tr("Apply as offset layer"), this);
  m_asLayerCheck->setToolTip(
      tr("Apply the preset to a new offset layer named after it, instead of "
         "the base keys of the objects"));

  m_applyButton = new QPushButton(tr("Apply"), this);
  m_applyButton->setToolTip(
      tr("Animate the selected columns and pegbars with the preset, starting "
         "at the current frame and from where they are there"));
  m_saveButton = new QPushButton(tr("Save Selection as Preset..."), this);
  m_saveButton->setToolTip(
      tr("Save the selected keyframes of the current object as a preset. "
         "Without a selection, its whole animation is saved."));
  m_deleteButton = new QPushButton(tr("Delete"), this);
  m_deleteButton->setToolTip(tr("Delete the selected preset of the user"));
  QPushButton *refreshButton = new QPushButton(tr("Refresh"), this);

  // layout
  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(5, 5, 5, 5);
  mainLayout->setSpacing(5);
  {
    mainLayout->addWidget(m_emptyLabel);
    mainLayout->addWidget(m_list, 1);
    mainLayout->addWidget(m_infoLabel);
    mainLayout->addWidget(m_asLayerCheck);
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(5);
    {
      buttonLayout->addWidget(m_applyButton);
      buttonLayout->addWidget(m_saveButton);
      buttonLayout->addWidget(m_deleteButton);
      buttonLayout->addStretch(1);
      buttonLayout->addWidget(refreshButton);
    }
    mainLayout->addLayout(buttonLayout);
  }
  setLayout(mainLayout);

  // The folders are watched. The files are saved in several steps, so the
  // changes are grouped.
  m_watcher      = new QFileSystemWatcher(this);
  m_refreshTimer = new QTimer(this);
  m_refreshTimer->setSingleShot(true);
  m_refreshTimer->setInterval(300);

  connect(m_watcher, SIGNAL(directoryChanged(const QString &)), this,
          SLOT(onFolderChanged()));
  connect(m_refreshTimer, SIGNAL(timeout()), this, SLOT(refresh()));
  connect(m_applyButton, SIGNAL(clicked()), this, SLOT(onApply()));
  connect(m_saveButton, SIGNAL(clicked()), this, SLOT(onSave()));
  connect(m_deleteButton, SIGNAL(clicked()), this, SLOT(onDelete()));
  connect(refreshButton, SIGNAL(clicked()), this, SLOT(refresh()));
  connect(m_list, &QListWidget::currentItemChanged, this,
          &MotionPresetsPane::updateButtons);
  connect(m_list, &QListWidget::itemDoubleClicked, this,
          [this](QListWidgetItem *item) { applyItem(item); });

  updateButtons();
}

//-----------------------------------------------------------------------------

TFilePath MotionPresetsPane::getStockFolder() {
  return ToonzFolder::getLibraryFolder() + "motionpresets";
}

//-----------------------------------------------------------------------------

TFilePath MotionPresetsPane::getUserFolder() {
  return getStockFolder() + "user";
}

//-----------------------------------------------------------------------------

void MotionPresetsPane::showEvent(QShowEvent *) { refresh(); }

//-----------------------------------------------------------------------------

void MotionPresetsPane::onFolderChanged() { m_refreshTimer->start(); }

//-----------------------------------------------------------------------------

// The folders may not exist yet, or may have been recreated
void MotionPresetsPane::updateWatchedFolders() {
  QStringList oldPaths = m_watcher->directories();
  if (!oldPaths.isEmpty()) m_watcher->removePaths(oldPaths);

  const TFilePath stockFolder = getStockFolder();
  QStringList paths;
  paths << stockFolder.getParentDir().getQString() << stockFolder.getQString()
        << getUserFolder().getQString();
  for (const QString &path : paths)
    if (QFileInfo(path).isDir()) m_watcher->addPath(path);
}

//-----------------------------------------------------------------------------

void MotionPresetsPane::addPresets(const TFilePath &folder, bool userPresets,
                                   const QString &title) {
  QFileInfoList files =
      QDir(folder.getQString())
          .entryInfoList(QStringList() << "*.tnzmotion", QDir::Files,
                         QDir::Name | QDir::IgnoreCase);

  bool titleAdded = false;
  for (const QFileInfo &file : files) {
    MotionPreset preset;
    std::string error;
    if (!MotionPreset::load(TFilePath(file.absoluteFilePath().toStdWString()),
                            preset, &error) ||
        !preset.isValid(&error))
      continue;
    if (preset.getName().isEmpty()) preset.setName(file.completeBaseName());

    if (!titleAdded) {
      QListWidgetItem *titleItem = new QListWidgetItem(title);
      QFont font                 = titleItem->font();
      font.setBold(true);
      titleItem->setFont(font);
      titleItem->setFlags(Qt::NoItemFlags);
      titleItem->setData(PresetIndexRole, -1);
      m_list->addItem(titleItem);
      titleAdded = true;
    }

    QListWidgetItem *item = new QListWidgetItem(
        QIcon(makeThumbnail(preset, palette())), preset.getName());
    item->setData(PresetIndexRole, (int)m_presets.size());
    item->setData(PathRole, file.absoluteFilePath());
    item->setData(UserPresetRole, userPresets);
    item->setToolTip(preset.getDescription());
    m_list->addItem(item);
    m_presets.push_back(preset);
  }
}

//-----------------------------------------------------------------------------

void MotionPresetsPane::refresh() {
  QString currentPath;
  getCurrentPreset(0, &currentPath);

  m_list->clear();
  m_presets.clear();

  addPresets(getStockFolder(), false, tr("Stock"));
  addPresets(getUserFolder(), true, tr("User"));

  if (!currentPath.isEmpty())
    for (int i = 0; i < m_list->count(); ++i)
      if (m_list->item(i)->data(PathRole).toString() == currentPath) {
        m_list->setCurrentItem(m_list->item(i));
        break;
      }

  m_emptyLabel->setVisible(m_presets.empty());
  updateWatchedFolders();
  updateButtons();
}

//-----------------------------------------------------------------------------

const MotionPreset *MotionPresetsPane::getCurrentPreset(bool *userPreset,
                                                        QString *path) const {
  QListWidgetItem *item = m_list->currentItem();
  if (!item) return 0;
  const int index = item->data(PresetIndexRole).toInt();
  if (index < 0 || index >= (int)m_presets.size()) return 0;
  if (userPreset) *userPreset = item->data(UserPresetRole).toBool();
  if (path) *path = item->data(PathRole).toString();
  return &m_presets[index];
}

//-----------------------------------------------------------------------------

void MotionPresetsPane::updateButtons() {
  bool userPreset            = false;
  const MotionPreset *preset = getCurrentPreset(&userPreset);
  m_applyButton->setEnabled(preset != 0);
  m_deleteButton->setEnabled(preset != 0 && userPreset);

  QString text;
  if (preset) {
    QStringList channels;
    for (const MotionPreset::Channel &channel : preset->getChannels())
      channels << QString::fromStdString(channel.m_name);
    if (!preset->getDescription().isEmpty())
      text = preset->getDescription() + "\n";
    text += tr("Lasts %1 frames. Channels: %2.")
                .arg((int)std::ceil(preset->getDuration()))
                .arg(channels.join(", "));
    if (preset->getAnchor() == MotionPreset::AnchorEnd)
      text += " " + tr("Ends at the current position.");
  }
  m_infoLabel->setText(text);
}

//-----------------------------------------------------------------------------

void MotionPresetsPane::applyItem(QListWidgetItem *item) {
  if (!item) return;
  const int index = item->data(PresetIndexRole).toInt();
  if (index < 0 || index >= (int)m_presets.size()) return;
  const MotionPreset preset = m_presets[index];
  applyPreset(preset, m_asLayerCheck->isChecked());
}

//-----------------------------------------------------------------------------

void MotionPresetsPane::onApply() { applyItem(m_list->currentItem()); }

//-----------------------------------------------------------------------------

void MotionPresetsPane::onSave() {
  saveSelectionAsPreset();
  refresh();
}

//-----------------------------------------------------------------------------

void MotionPresetsPane::onDelete() {
  bool userPreset = false;
  QString path;
  const MotionPreset *preset = getCurrentPreset(&userPreset, &path);
  if (!preset || !userPreset) return;

  const QString question =
      tr("Delete the preset \"%1\"?").arg(preset->getName());
  if (DVGui::MsgBox(question, tr("Delete"), tr("Cancel")) != 1) return;

  if (!QFile::remove(path))
    DVGui::warning(tr("The file of the preset could not be deleted."));
  refresh();
}

//-----------------------------------------------------------------------------

void MotionPresetsPane::applyPreset(const MotionPreset &preset,
                                    bool asOffsetLayer) {
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  if (!xsh) return;

  std::vector<TStageObjectId> ids;
  getTargetObjects(ids);
  if (ids.empty()) {
    DVGui::warning(tr("Select the columns or pegbars to animate first."));
    return;
  }

  const int frame  = app->getCurrentFrame()->getFrame();
  int appliedCount = 0;
  QStringList messages;

  // One click is one undo step.
  TUndoManager *undoManager = TUndoManager::manager();
  undoManager->beginBlock();
  undoManager->add(new MotionPresetNotifyUndo(preset.getName(), false));
  for (const TStageObjectId &id : ids) {
    TStageObject *obj = xsh->getStageObjectTree()->getStageObject(id, false);
    if (!obj) continue;
    const QString objectName = QString::fromStdString(obj->getName());

    if (id.isColumn()) {
      TXshColumn *column = xsh->getColumn(id.getIndex());
      if (column && column->isLocked()) {
        messages << tr("%1: the column is locked.").arg(objectName);
        continue;
      }
    }

    int layerIndex = -1;
    if (asOffsetLayer) {
      layerIndex = TStageObjectCmd::addOffsetLayer(
          id, preset.getName().toStdWString(), app->getCurrentXsheet());
      if (layerIndex < 0) continue;
    }

    const MotionPreset::ApplyResult result =
        preset.apply(obj, frame, layerIndex, true);
    if (result.m_channelCount > 0)
      ++appliedCount;
    else if (layerIndex >= 0)
      // Nothing went into the layer
      TStageObjectCmd::removeOffsetLayer(id, layerIndex,
                                         app->getCurrentXsheet());
    for (const std::string &skipped : result.m_skipped)
      messages << QString("%1: %2")
                      .arg(objectName)
                      .arg(QString::fromStdString(skipped));
  }
  undoManager->add(new MotionPresetNotifyUndo(preset.getName(), true));
  undoManager->endBlock();

  // Nothing was animated: no undo step either
  if (appliedCount == 0) undoManager->popUndo(1);

  notifyMotionApplied();
  if (appliedCount > 0) app->getCurrentScene()->setDirtyFlag(true);

  if (appliedCount == 0 && messages.isEmpty())
    messages << tr("The preset could not be applied.");
  if (!messages.isEmpty()) {
    const int maxLines = 8;
    if (messages.size() > maxLines) {
      const int more = messages.size() - maxLines;
      messages       = messages.mid(0, maxLines);
      messages << tr("... and %1 more.").arg(more);
    }
    DVGui::warning(messages.join("\n"));
  }
}

//-----------------------------------------------------------------------------

void MotionPresetsPane::saveSelectionAsPreset() {
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  if (!xsh) return;

  const TStageObjectId id = app->getCurrentObject()->getObjectId();
  TStageObject *obj =
      isTarget(id) ? xsh->getStageObjectTree()->getStageObject(id, false) : 0;
  if (!obj) {
    DVGui::warning(tr("Select the column or pegbar whose animation to save."));
    return;
  }

  int r0 = 0, r1 = -1;
  if (!obj->getKeyframeRange(r0, r1)) {
    DVGui::warning(tr("The current object has no keyframes."));
    return;
  }

  // The selected keyframes, or all of them. From a single selected key, the
  // preset runs to the last key of the object.
  int first = r0, last = r1;
  if (getSelectedFrames(id, first, last)) {
    if (first == last) last = std::max(last, r1);
  } else {
    first = r0;
    last  = r1;
  }

  MotionPreset preset;
  std::vector<std::string> skipped;
  if (!MotionPreset::captureFromKeys(obj, first, last, preset, &skipped)) {
    QString text = tr("No channel of the object is animated between the "
                      "frames %1 and %2.")
                       .arg(first + 1)
                       .arg(last + 1);
    for (const std::string &s : skipped)
      text += "\n" + QString::fromStdString(s);
    DVGui::warning(text);
    return;
  }

  bool ok      = false;
  QString name = DVGui::getText(tr("Save Motion Preset"), tr("Preset name:"),
                                tr("My Motion"), &ok);
  name         = name.trimmed();
  if (!ok || name.isEmpty()) return;
  preset.setName(name);

  const QString path = QDir(getUserFolder().getQString())
                           .filePath(makeFileName(name) + ".tnzmotion");
  if (QFileInfo(path).exists()) {
    const QString question =
        tr("The preset \"%1\" already exists. Overwrite it?").arg(name);
    if (DVGui::MsgBox(question, tr("Overwrite"), tr("Cancel")) != 1) return;
  }

  std::string error;
  if (!preset.save(TFilePath(path.toStdWString()), &error)) {
    DVGui::warning(tr("The preset could not be saved: %1")
                       .arg(QString::fromStdString(error)));
    return;
  }

  QString text = tr("The motion preset \"%1\" was saved.").arg(name);
  for (const std::string &s : skipped)
    text += "\n" + tr("Not saved, %1").arg(QString::fromStdString(s));
  DVGui::info(text);
}
