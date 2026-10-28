#include "posespane.h"

// Tnz6 includes
#include "tapp.h"
#include "columnselection.h"

// TnzQt includes
#include "stageobjectselection.h"
#include "toonzqt/tselectionhandle.h"
#include "toonzqt/dvdialog.h"

// TnzLib includes
#include "toonz/poselibrary.h"
#include "toonz/toonzscene.h"
#include "toonz/childstack.h"
#include "toonz/txsheet.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/doubleparamcmd.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tstageobjectcmd.h"

// TnzCore includes
#include "tdoubleparam.h"
#include "tmsgcore.h"
#include "tundo.h"
#include "historytypes.h"

// Qt includes
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QVBoxLayout>

// STD includes
#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace {

//-----------------------------------------------------------------------------

bool isPoseTarget(const TStageObjectId &id) {
  return id.isColumn() || id.isPegbar() || id.isTable() || id.isCamera();
}

//-----------------------------------------------------------------------------

void notifyPoseApplied() {
  TApp *app = TApp::instance();
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentObject()->notifyObjectIdChanged(false);
}

//=============================================================================
// ApplyPoseNotifyUndo
//-----------------------------------------------------------------------------
/*! Placed at both ends of the undo block of an applied pose, so the viewers
    are refreshed after the keyframes are restored (undo) or set (redo).  */

class ApplyPoseNotifyUndo final : public TUndo {
  QString m_poseName;
  bool m_notifyOnRedo;

public:
  ApplyPoseNotifyUndo(const QString &poseName, bool notifyOnRedo)
      : m_poseName(poseName), m_notifyOnRedo(notifyOnRedo) {}

  void undo() const override {
    if (!m_notifyOnRedo) notifyPoseApplied();
  }
  void redo() const override {
    if (m_notifyOnRedo) notifyPoseApplied();
  }
  int getSize() const override { return sizeof(*this); }

  QString getHistoryString() override {
    return QObject::tr("Apply Pose  %1").arg(m_poseName);
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

}  // namespace

//=============================================================================
// PoseLibraryNotifier
//-----------------------------------------------------------------------------

PoseLibraryNotifier *PoseLibraryNotifier::instance() {
  static PoseLibraryNotifier theInstance;
  return &theInstance;
}

//=============================================================================
// PosesPane
//-----------------------------------------------------------------------------

PosesPane::PosesPane(QWidget *parent, Qt::WindowFlags flags)
    : QFrame(parent, flags) {
  setObjectName("PosesPane");

  QPushButton *setBaseButton = new QPushButton(tr("Set Base"), this);
  setBaseButton->setToolTip(
      tr("Store the current values of the selected columns and pegbars as "
         "the base pose"));
  QPushButton *addPoseButton = new QPushButton(tr("Add Pose..."), this);
  addPoseButton->setToolTip(
      tr("Store the difference between the current values and the base pose "
         "as a new pose"));

  m_baseLabel = new QLabel(this);

  // Poses can be applied to an offset layer instead of the base keys
  m_asLayerCheck = new QCheckBox(tr("Apply as offset layer"), this);
  m_asLayerCheck->setToolTip(
      tr("Apply the poses to the offset layer with this name, which is added "
         "to the objects that don't have it, instead of the base keys. "
         "Only the channels of the stage objects can be applied."));
  m_layerNameEdit = new QLineEdit(tr("Pose Layer"), this);
  m_layerNameEdit->setToolTip(tr("Name of the offset layer"));
  m_layerNameEdit->setEnabled(false);

  QScrollArea *scrollArea = new QScrollArea(this);
  scrollArea->setWidgetResizable(true);
  QWidget *poseListWidget = new QWidget(scrollArea);
  m_poseListLayout        = new QVBoxLayout();

  // layout
  QVBoxLayout *poseListWidgetLayout = new QVBoxLayout();
  poseListWidgetLayout->setContentsMargins(2, 2, 2, 2);
  poseListWidgetLayout->setSpacing(0);
  {
    m_poseListLayout->setContentsMargins(0, 0, 0, 0);
    m_poseListLayout->setSpacing(2);
    poseListWidgetLayout->addLayout(m_poseListLayout);
    poseListWidgetLayout->addStretch(1);
  }
  poseListWidget->setLayout(poseListWidgetLayout);
  scrollArea->setWidget(poseListWidget);

  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(5, 5, 5, 5);
  mainLayout->setSpacing(5);
  {
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(5);
    {
      buttonLayout->addWidget(setBaseButton);
      buttonLayout->addWidget(addPoseButton);
      buttonLayout->addStretch(1);
    }
    mainLayout->addLayout(buttonLayout);
    QHBoxLayout *layerLayout = new QHBoxLayout();
    layerLayout->setContentsMargins(0, 0, 0, 0);
    layerLayout->setSpacing(5);
    {
      layerLayout->addWidget(m_asLayerCheck);
      layerLayout->addWidget(m_layerNameEdit, 1);
    }
    mainLayout->addLayout(layerLayout);
    mainLayout->addWidget(m_baseLabel);
    mainLayout->addWidget(scrollArea, 1);
  }
  setLayout(mainLayout);

  connect(setBaseButton, SIGNAL(clicked()), this, SLOT(onSetBase()));
  connect(addPoseButton, SIGNAL(clicked()), this, SLOT(onAddPose()));
  connect(m_asLayerCheck, &QCheckBox::toggled, m_layerNameEdit,
          &QLineEdit::setEnabled);
}

//-----------------------------------------------------------------------------

void PosesPane::showEvent(QShowEvent *) {
  connect(TApp::instance()->getCurrentScene(), SIGNAL(sceneSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  // Another Poses panel can add or delete poses: the rows refer to the poses
  // by index, so they must follow
  connect(PoseLibraryNotifier::instance(), SIGNAL(changed()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  refresh();
}

//-----------------------------------------------------------------------------

void PosesPane::hideEvent(QHideEvent *) {
  disconnect(TApp::instance()->getCurrentScene(), SIGNAL(sceneSwitched()), this,
             SLOT(refresh()));
  disconnect(PoseLibraryNotifier::instance(), SIGNAL(changed()), this,
             SLOT(refresh()));
}

//-----------------------------------------------------------------------------

void PosesPane::refresh() {
  // Rows are deleted later, since this may be called by one of their buttons.
  while (QLayoutItem *item = m_poseListLayout->takeAt(0)) {
    if (QWidget *widget = item->widget()) {
      widget->hide();
      widget->deleteLater();
    }
    delete item;
  }

  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (!scene) return;
  PoseLibrary *poseLibrary = scene->getPoseLibrary();

  if (poseLibrary->getBase().empty())
    m_baseLabel->setText(
        tr("No base pose. Select the rig and click Set Base."));
  else
    m_baseLabel->setText(
        tr("Base pose: %1 channels").arg((int)poseLibrary->getBase().size()));

  for (int i = 0; i < poseLibrary->getPoseCount(); ++i) {
    const PoseLibrary::Pose &pose = poseLibrary->getPose(i);

    QWidget *row              = new QWidget(this);
    QLabel *nameLabel         = new QLabel(pose.m_name, row);
    QSlider *slider           = new QSlider(Qt::Horizontal, row);
    QLabel *valueLabel        = new QLabel(row);
    QPushButton *applyButton  = new QPushButton(tr("Apply"), row);
    QPushButton *deleteButton = new QPushButton(tr("Delete"), row);

    nameLabel->setToolTip(tr("%1 channels").arg((int)pose.m_entries.size()));
    slider->setRange(-100, 100);
    slider->setValue(100);
    slider->setToolTip(tr("Weight"));
    valueLabel->setText(QString("%1%").arg(slider->value()));
    valueLabel->setMinimumWidth(40);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    applyButton->setToolTip(
        tr("Add the weighted pose to the current values and set keyframes at "
           "the current frame"));

    QHBoxLayout *rowLayout = new QHBoxLayout();
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);
    {
      rowLayout->addWidget(nameLabel, 1);
      rowLayout->addWidget(slider, 2);
      rowLayout->addWidget(valueLabel);
      rowLayout->addWidget(applyButton);
      rowLayout->addWidget(deleteButton);
    }
    row->setLayout(rowLayout);
    m_poseListLayout->addWidget(row);

    connect(slider, &QSlider::valueChanged, valueLabel,
            [valueLabel](int value) {
              valueLabel->setText(QString("%1%").arg(value));
            });
    connect(applyButton, &QPushButton::clicked, this, [this, i, slider]() {
      std::wstring layerName;
      if (m_asLayerCheck->isChecked()) {
        layerName = m_layerNameEdit->text().trimmed().toStdWString();
        if (layerName.empty()) {
          DVGui::warning(tr("Enter the name of the offset layer."));
          return;
        }
      }
      applyPose(i, slider->value() / 100.0, layerName);
    });
    connect(deleteButton, &QPushButton::clicked, this,
            [this, i]() { deletePose(i); });
  }
}

//-----------------------------------------------------------------------------

bool PosesPane::checkTopXsheet() {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (!scene) return false;
  if (scene->getChildStack()->getAncestorCount() > 0) {
    DVGui::warning(tr("Poses can be used only in the top-level xsheet."));
    return false;
  }
  return true;
}

//-----------------------------------------------------------------------------

void PosesPane::getTargetObjects(std::vector<TStageObjectId> &ids) const {
  TApp *app             = TApp::instance();
  TSelection *selection = app->getCurrentSelection()->getSelection();

  if (TColumnSelection *columnSelection =
          dynamic_cast<TColumnSelection *>(selection)) {
    for (int index : columnSelection->getIndices())
      ids.push_back(TStageObjectId::ColumnId(index));
  } else if (StageObjectSelection *objectSelection =
                 dynamic_cast<StageObjectSelection *>(selection)) {
    for (const TStageObjectId &id : objectSelection->getObjects())
      if (isPoseTarget(id)) ids.push_back(id);
  }

  // Fall back to the current object, i.e. the one the Animate tool edits.
  if (ids.empty()) {
    TStageObjectId id = app->getCurrentObject()->getObjectId();
    if (isPoseTarget(id)) ids.push_back(id);
  }
}

//-----------------------------------------------------------------------------

void PosesPane::onSetBase() {
  if (!checkTopXsheet()) return;
  TApp *app         = TApp::instance();
  ToonzScene *scene = app->getCurrentScene()->getScene();

  std::vector<TStageObjectId> ids;
  getTargetObjects(ids);

  std::vector<PoseLibrary::Entry> values;
  PoseLibrary::captureValues(scene->getTopXsheet(), ids,
                             app->getCurrentFrame()->getFrame(), values);
  if (values.empty()) {
    DVGui::warning(tr("Select the columns or pegbars of the rig first."));
    return;
  }

  scene->getPoseLibrary()->setBase(values);
  app->getCurrentScene()->setDirtyFlag(true);
  refresh();
}

//-----------------------------------------------------------------------------

void PosesPane::onAddPose() {
  if (!checkTopXsheet()) return;
  TApp *app                = TApp::instance();
  ToonzScene *scene        = app->getCurrentScene()->getScene();
  PoseLibrary *poseLibrary = scene->getPoseLibrary();

  const std::vector<PoseLibrary::Entry> &base = poseLibrary->getBase();
  if (base.empty()) {
    DVGui::warning(tr("Set a base pose first."));
    return;
  }

  // The pose covers the channels of the base pose.
  std::vector<TStageObjectId> ids;
  for (const PoseLibrary::Entry &entry : base)
    if (std::find(ids.begin(), ids.end(), entry.m_objectId) == ids.end())
      ids.push_back(entry.m_objectId);

  std::vector<PoseLibrary::Entry> current;
  PoseLibrary::captureValues(scene->getTopXsheet(), ids,
                             app->getCurrentFrame()->getFrame(), current);
  std::map<std::pair<TStageObjectId, std::string>, double> currentValues;
  for (const PoseLibrary::Entry &entry : current)
    currentValues[std::make_pair(entry.m_objectId, entry.m_channel)] =
        entry.m_value;

  PoseLibrary::Pose pose;
  for (const PoseLibrary::Entry &entry : base) {
    auto it =
        currentValues.find(std::make_pair(entry.m_objectId, entry.m_channel));
    if (it == currentValues.end()) continue;
    double offset = it->second - entry.m_value;
    if (std::abs(offset) > 1e-9)
      pose.m_entries.push_back(
          PoseLibrary::Entry(entry.m_objectId, entry.m_channel, offset));
  }
  if (pose.m_entries.empty()) {
    DVGui::warning(tr("The current pose does not differ from the base pose."));
    return;
  }

  QString defaultName = tr("Pose %1").arg(poseLibrary->getPoseCount() + 1);
  bool ok             = false;
  QString name =
      DVGui::getText(tr("Add Pose"), tr("Pose name:"), defaultName, &ok);
  if (!ok) return;
  name = name.trimmed();
  if (name.isEmpty()) name = defaultName;

  pose.m_name = name;
  poseLibrary->addPose(pose);
  app->getCurrentScene()->setDirtyFlag(true);
  PoseLibraryNotifier::instance()->notifyChanged();
  refresh();
}

//-----------------------------------------------------------------------------

void PosesPane::getLayerObjects(TXsheet *xsh, const PoseLibrary::Pose &pose,
                                std::vector<TStageObjectId> &ids) {
  // The curves of the layer are not there yet: look for the channels of
  // the pose having an offset curve
  for (const PoseLibrary::Entry &entry : pose.m_entries) {
    int channel = PoseLibrary::getStageObjectChannel(entry.m_channel);
    if (channel < 0 ||
        !TStageObject::isOffsetChannel((TStageObject::Channel)channel) ||
        !xsh->getStageObjectTree()->getStageObject(entry.m_objectId, false))
      continue;
    if (std::find(ids.begin(), ids.end(), entry.m_objectId) == ids.end())
      ids.push_back(entry.m_objectId);
  }
}

//-----------------------------------------------------------------------------

bool PosesPane::applyPoseKeys(const PoseLibrary::Pose &pose, double weight,
                              const std::wstring &offsetLayerName) {
  TApp *app         = TApp::instance();
  ToonzScene *scene = app->getCurrentScene()->getScene();
  TXsheet *xsh      = scene->getTopXsheet();
  int frame         = app->getCurrentFrame()->getFrame();

  const bool asLayer = !offsetLayerName.empty();

  // Compute every new value before setting any keyframe.
  std::vector<std::pair<TDoubleParam *, double>> keys;
  if (asLayer) {
    // Add the layer to the objects that don't have it, then compute the values
    std::vector<TStageObjectId> layerObjects;
    getLayerObjects(xsh, pose, layerObjects);
    if (layerObjects.empty()) return false;
    for (const TStageObjectId &id : layerObjects) {
      TStageObject *obj = xsh->getStageObjectTree()->getStageObject(id, false);
      if (obj->findOffsetLayer(offsetLayerName) < 0)
        TStageObjectCmd::addOffsetLayer(id, offsetLayerName,
                                        app->getCurrentXsheet());
    }
    for (const PoseLibrary::Entry &entry : pose.m_entries) {
      TDoubleParam *param = PoseLibrary::getOffsetLayerParam(
          xsh, entry.m_objectId, offsetLayerName, entry.m_channel);
      if (param)
        keys.push_back(std::make_pair(
            param, param->getValue(frame) + weight * entry.m_value));
    }
  } else {
    for (const PoseLibrary::Entry &entry : pose.m_entries) {
      TDoubleParam *param =
          PoseLibrary::getChannelParam(xsh, entry.m_objectId, entry.m_channel);
      if (param)
        keys.push_back(std::make_pair(
            param, param->getValue(frame) + weight * entry.m_value));
    }
  }
  if (keys.empty()) return false;

  for (const auto &key : keys)
    KeyframeSetter::setValue(key.first, frame, key.second);
  return true;
}

//-----------------------------------------------------------------------------

void PosesPane::applyPose(int index, double weight,
                          const std::wstring &offsetLayerName) {
  if (!checkTopXsheet()) return;
  TApp *app                = TApp::instance();
  ToonzScene *scene        = app->getCurrentScene()->getScene();
  PoseLibrary *poseLibrary = scene->getPoseLibrary();
  if (index < 0 || index >= poseLibrary->getPoseCount() || weight == 0) return;

  const PoseLibrary::Pose &pose = poseLibrary->getPose(index);
  TXsheet *xsh                  = scene->getTopXsheet();

  const bool asLayer = !offsetLayerName.empty();

  bool applicable = false;
  if (asLayer) {
    std::vector<TStageObjectId> layerObjects;
    getLayerObjects(xsh, pose, layerObjects);
    applicable = !layerObjects.empty();
  } else {
    for (const PoseLibrary::Entry &entry : pose.m_entries)
      if (PoseLibrary::getChannelParam(xsh, entry.m_objectId,
                                       entry.m_channel)) {
        applicable = true;
        break;
      }
  }
  if (!applicable) {
    DVGui::warning(
        asLayer ? tr("None of the channels of this pose can be applied to an "
                     "offset layer.")
                : tr("None of the channels of this pose exist in the scene."));
    return;
  }

  // One click is one undo step.
  TUndoManager::manager()->beginBlock();
  TUndoManager::manager()->add(new ApplyPoseNotifyUndo(pose.m_name, false));
  applyPoseKeys(pose, weight, offsetLayerName);
  TUndoManager::manager()->add(new ApplyPoseNotifyUndo(pose.m_name, true));
  TUndoManager::manager()->endBlock();

  notifyPoseApplied();
  app->getCurrentScene()->setDirtyFlag(true);
}

//-----------------------------------------------------------------------------

void PosesPane::deletePose(int index) {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (!scene) return;
  PoseLibrary *poseLibrary = scene->getPoseLibrary();
  if (index < 0 || index >= poseLibrary->getPoseCount()) return;

  QString question =
      tr("Delete the pose \"%1\"?").arg(poseLibrary->getPose(index).m_name);
  if (DVGui::MsgBox(question, tr("Delete"), tr("Cancel")) != 1) return;

  poseLibrary->removePose(index);
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
  PoseLibraryNotifier::instance()->notifyChanged();
  refresh();
}
