#include "toonzqt/uitranslations.h"
#include "offsetlayerspane.h"

// Tnz6 includes
#include "tapp.h"

// TnzQt includes
#include "toonzqt/dvdialog.h"

// TnzLib includes
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tstageobjectcmd.h"

// TnzBase includes
#include "tdoubleparam.h"

// Qt includes
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

// STD includes
#include <algorithm>
#include <cmath>

namespace {

//-----------------------------------------------------------------------------

// The weight slider covers these percentages. Values out of the range can be
// keyed in the Function Editor.
const int minWeightPercent = -200;
const int maxWeightPercent = 200;

//-----------------------------------------------------------------------------

int weightToPercent(double weight) {
  return (int)std::floor(weight * 100.0 + 0.5);
}

}  // namespace

//=============================================================================
// OffsetLayersPane
//-----------------------------------------------------------------------------

OffsetLayersPane::OffsetLayersPane(QWidget *parent, Qt::WindowFlags flags)
    : QFrame(parent, flags) {
  setObjectName("OffsetLayersPane");

  QPushButton *newButton = new QPushButton(tr("New Layer"), this);
  newButton->setToolTip(
      tr("Add an empty offset layer to the current object. Key its channels "
         "in the Function Editor."));
  QPushButton *trainButton = new QPushButton(tr("Train from Keys"), this);
  trainButton->setToolTip(
      tr("Add a layer, with weight 0, whose curves are the keyframed curves "
         "of the object minus their value at the first frame, so that the "
         "animation can be repeated scaled by the weight"));

  m_objectLabel = new QLabel(this);
  m_emptyLabel  = new QLabel(
       tr("No offset layers. An offset layer adds weighted, separate curves to "
           "the animation of the object, and is saved in the scene only when "
           "it exists."),
       this);
  m_emptyLabel->setWordWrap(true);

  QScrollArea *scrollArea = new QScrollArea(this);
  scrollArea->setWidgetResizable(true);
  QWidget *listWidget = new QWidget(scrollArea);
  m_listLayout        = new QVBoxLayout();

  // layout
  QVBoxLayout *listWidgetLayout = new QVBoxLayout();
  listWidgetLayout->setContentsMargins(2, 2, 2, 2);
  listWidgetLayout->setSpacing(0);
  {
    m_listLayout->setContentsMargins(0, 0, 0, 0);
    m_listLayout->setSpacing(2);
    listWidgetLayout->addWidget(m_emptyLabel);
    listWidgetLayout->addLayout(m_listLayout);
    listWidgetLayout->addStretch(1);
  }
  listWidget->setLayout(listWidgetLayout);
  scrollArea->setWidget(listWidget);

  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(5, 5, 5, 5);
  mainLayout->setSpacing(5);
  {
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(5);
    {
      buttonLayout->addWidget(newButton);
      buttonLayout->addWidget(trainButton);
      buttonLayout->addStretch(1);
    }
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(m_objectLabel);
    mainLayout->addWidget(scrollArea, 1);
  }
  setLayout(mainLayout);

  connect(newButton, SIGNAL(clicked()), this, SLOT(onNewLayer()));
  connect(trainButton, SIGNAL(clicked()), this, SLOT(onTrainFromKeys()));
}

//-----------------------------------------------------------------------------

TStageObject *OffsetLayersPane::getCurrentObject() const {
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  if (!xsh) return 0;
  TStageObjectId id = app->getCurrentObject()->getObjectId();
  if (id == TStageObjectId::NoneId) return 0;
  return xsh->getStageObjectTree()->getStageObject(id, false);
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::showEvent(QShowEvent *) {
  TApp *app = TApp::instance();
  connect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentObject(), SIGNAL(objectSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentObject(), SIGNAL(objectChanged(bool)), this,
          SLOT(updateValues()), Qt::UniqueConnection);
  connect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
          SLOT(updateValues()), Qt::UniqueConnection);
  refresh();
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::hideEvent(QHideEvent *) {
  TApp *app = TApp::instance();
  disconnect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentObject(), SIGNAL(objectSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentObject(), SIGNAL(objectChanged(bool)), this,
             SLOT(updateValues()));
  disconnect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
             SLOT(updateValues()));
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::refresh() {
  TStageObject *obj = getCurrentObject();

  // The rows are rebuilt only if the layers are not the same
  Signature signature;
  if (obj)
    for (int i = 0; i < obj->getOffsetLayerCount(); ++i) {
      const TStageObject::OffsetLayer &layer = obj->getOffsetLayer(i);
      signature.push_back(std::make_pair(layer.m_name, layer.m_enabled));
    }
  TStageObjectId id = obj ? obj->getId() : TStageObjectId::NoneId;

  if (id == m_objectId && signature == m_signature &&
      m_rows.size() == signature.size())
    updateValues();
  else
    rebuild();
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::rebuild() {
  // Rows are deleted later, since this may be called by one of their widgets.
  while (QLayoutItem *item = m_listLayout->takeAt(0)) {
    if (QWidget *widget = item->widget()) {
      widget->hide();
      widget->deleteLater();
    }
    delete item;
  }
  m_rows.clear();
  m_signature.clear();

  TStageObject *obj = getCurrentObject();
  m_objectId        = obj ? obj->getId() : TStageObjectId::NoneId;
  m_objectLabel->setText(
      obj ? UiTranslations::text(tr("Object: %1")).arg(QString::fromStdString(obj->getFullName()))
          : tr("No current object."));
  m_emptyLabel->setVisible(!obj || !obj->hasOffsetLayers());
  if (!obj) return;

  for (int i = 0; i < obj->getOffsetLayerCount(); ++i) {
    const TStageObject::OffsetLayer &layer = obj->getOffsetLayer(i);
    m_signature.push_back(std::make_pair(layer.m_name, layer.m_enabled));

    QWidget *row       = new QWidget(this);
    QCheckBox *enabled = new QCheckBox(row);
    QLineEdit *name = new QLineEdit(QString::fromStdWString(layer.m_name), row);
    QSlider *slider = new QSlider(Qt::Horizontal, row);
    QLabel *valueLabel        = new QLabel(row);
    QPushButton *bakeButton   = new QPushButton(tr("Bake into Base"), row);
    QPushButton *deleteButton = new QPushButton(tr("Delete"), row);

    enabled->setChecked(layer.m_enabled);
    enabled->setToolTip(tr("Enable the layer"));
    name->setToolTip(tr("Layer name"));
    slider->setRange(minWeightPercent, maxWeightPercent);
    // The weight changes when the slider is released: a change is an undo
    slider->setTracking(false);
    slider->setToolTip(
        tr("Weight of the layer. It is the value of the weight curve if it "
           "has no keys, and a key at the current frame otherwise."));
    valueLabel->setMinimumWidth(40);
    valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bakeButton->setToolTip(
        tr("Add the weighted layer to the keys of the base animation and "
           "clear the curves of the layer. The result is exact at the keys "
           "only."));
    deleteButton->setToolTip(tr("Delete the layer"));

    QHBoxLayout *rowLayout = new QHBoxLayout();
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);
    {
      rowLayout->addWidget(enabled);
      rowLayout->addWidget(name, 1);
      rowLayout->addWidget(slider, 2);
      rowLayout->addWidget(valueLabel);
      rowLayout->addWidget(bakeButton);
      rowLayout->addWidget(deleteButton);
    }
    row->setLayout(rowLayout);
    m_listLayout->addWidget(row);

    Row rowData = {enabled, name, slider, valueLabel};
    m_rows.push_back(rowData);

    connect(enabled, &QCheckBox::clicked, this, [this, i](bool checked) {
      TStageObjectCmd::setOffsetLayerEnabled(
          m_objectId, i, checked, TApp::instance()->getCurrentXsheet());
      notifyLayersChanged();
    });
    connect(name, &QLineEdit::editingFinished, this,
            [this, i, name]() { rename(i, name->text()); });
    connect(slider, &QSlider::sliderMoved, this, [valueLabel](int value) {
      valueLabel->setText(QString("%1%").arg(value));
    });
    connect(slider, &QSlider::valueChanged, this,
            [this, i](int value) { setWeight(i, value); });
    connect(bakeButton, &QPushButton::clicked, this,
            [this, i]() { onBakeLayer(i); });
    connect(deleteButton, &QPushButton::clicked, this,
            [this, i]() { onDeleteLayer(i); });
  }

  updateValues();
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::updateValues() {
  TStageObject *obj = getCurrentObject();
  if (!obj || obj->getId() != m_objectId ||
      obj->getOffsetLayerCount() != (int)m_rows.size())
    return;

  int frame = TApp::instance()->getCurrentFrame()->getFrame();
  for (int i = 0; i < (int)m_rows.size(); ++i) {
    const Row &row = m_rows[i];
    double weight  = obj->getOffsetLayer(i).m_weight->getValue(frame);
    int percent    = weightToPercent(weight);
    {
      QSignalBlocker blocker(row.m_slider);
      if (!row.m_slider->isSliderDown())
        row.m_slider->setValue(
            std::min(std::max(percent, minWeightPercent), maxWeightPercent));
    }
    if (!row.m_slider->isSliderDown())
      row.m_value->setText(QString("%1%").arg(percent));
  }
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::notifyLayersChanged() {
  TApp *app = TApp::instance();
  app->getCurrentObject()->notifyObjectIdChanged(false);
  app->getCurrentScene()->setDirtyFlag(true);
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::setWeight(int index, int percent) {
  TApp *app = TApp::instance();
  int frame = app->getCurrentFrame()->getFrame();

  // Do not set a key or a value when the slider only shows the value rounded
  TStageObject *obj = getCurrentObject();
  if (!obj || index < 0 || index >= obj->getOffsetLayerCount()) return;
  double weight = obj->getOffsetLayer(index).m_weight->getValue(frame);
  if (weightToPercent(weight) == percent) return;

  TStageObjectCmd::setOffsetLayerWeight(m_objectId, index, percent / 100.0,
                                        frame, app->getCurrentXsheet());
  notifyLayersChanged();
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::rename(int index, const QString &name) {
  TStageObject *obj = getCurrentObject();
  if (!obj || index < 0 || index >= obj->getOffsetLayerCount()) return;

  QString oldName = QString::fromStdWString(obj->getOffsetLayer(index).m_name);
  QString newName = name.trimmed();
  if (newName == oldName) return;

  if (!TStageObjectCmd::renameOffsetLayer(
          m_objectId, index, newName.toStdWString(),
          TApp::instance()->getCurrentXsheet())) {
    // Restore the name before warning, as the warning takes the focus
    if (index < (int)m_rows.size()) m_rows[index].m_name->setText(oldName);
    DVGui::warning(
        tr("The name of a layer can't be empty or the one of "
           "another layer of the object."));
    return;
  }
  notifyLayersChanged();
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::onNewLayer() {
  TStageObject *obj = getCurrentObject();
  if (!obj) {
    DVGui::warning(tr("Select an object first."));
    return;
  }
  TStageObjectCmd::addOffsetLayer(obj->getId(), std::wstring(),
                                  TApp::instance()->getCurrentXsheet());
  notifyLayersChanged();
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::onTrainFromKeys() {
  TStageObject *obj = getCurrentObject();
  if (!obj) {
    DVGui::warning(tr("Select an object first."));
    return;
  }
  int index = TStageObjectCmd::trainOffsetLayerFromKeys(
      obj->getId(), tr("Trained").toStdWString(),
      TApp::instance()->getCurrentXsheet());
  if (index < 0) {
    DVGui::warning(tr("The object has no animated channels to train from."));
    return;
  }
  notifyLayersChanged();
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::onDeleteLayer(int index) {
  TStageObject *obj = getCurrentObject();
  if (!obj || index < 0 || index >= obj->getOffsetLayerCount()) return;
  TStageObjectCmd::removeOffsetLayer(obj->getId(), index,
                                     TApp::instance()->getCurrentXsheet());
  notifyLayersChanged();
}

//-----------------------------------------------------------------------------

void OffsetLayersPane::onBakeLayer(int index) {
  TStageObject *obj = getCurrentObject();
  if (!obj || index < 0 || index >= obj->getOffsetLayerCount()) return;
  if (!TStageObjectCmd::bakeOffsetLayer(obj->getId(), index,
                                        TApp::instance()->getCurrentXsheet()))
    DVGui::warning(tr("The layer has nothing to bake."));
  notifyLayersChanged();
}
