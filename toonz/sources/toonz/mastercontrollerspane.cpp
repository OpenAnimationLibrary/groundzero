#include "mastercontrollerspane.h"

// Tnz6 includes
#include "tapp.h"

// TnzQt includes
#include "toonzqt/dvdialog.h"
#include "toonzqt/functionviewer.h"

// TnzLib includes
#include "toonz/txsheet.h"
#include "toonz/txsheetexpr.h"
#include "toonz/txsheethandle.h"
#include "toonz/tframehandle.h"
#include "toonz/tobjecthandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/tstageobjectcmd.h"
#include "toonz/doubleparamcmd.h"

// TnzBase includes
#include "tdoubleparam.h"
#include "texpression.h"
#include "tunit.h"

// TnzCore includes
#include "tundo.h"
#include "historytypes.h"

// Qt includes
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPointF>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

// STD includes
#include <algorithm>
#include <cmath>

namespace {

//-----------------------------------------------------------------------------

const QString controllerPrefix = "ctrl_";

// The slider covers the controller values 0..1 in this many steps.
const int sliderSteps = 1000;

//-----------------------------------------------------------------------------

//! The expression reference of a controller's value, e.g. "peg3.rot". The
//! Rotation channel is used since its unit (degree) has no conversion, so
//! expressions read the stored value whatever the unit preferences are.
QString getControllerRef(const TStageObjectId &id) {
  return QString("peg%1.rot").arg(id.getIndex() + 1);
}

//-----------------------------------------------------------------------------

QString formatNumber(double value) {
  QString text = QString::number(value, 'f', 6);
  if (text.contains('.')) {
    while (text.endsWith('0')) text.chop(1);
    if (text.endsWith('.')) text.chop(1);
  }
  if (text == "-0") text = "0";
  return text;
}

//-----------------------------------------------------------------------------

QString makeExpression(const TStageObjectId &id, double multiplier,
                       double offset) {
  QString text = getControllerRef(id);
  if (multiplier != 1.0) {
    if (multiplier < 0)
      text += "*(" + formatNumber(multiplier) + ")";
    else
      text += "*" + formatNumber(multiplier);
  }
  if (offset > 0)
    text += "+" + formatNumber(offset);
  else if (offset < 0)
    text += "-" + formatNumber(-offset);
  return text;
}

//-----------------------------------------------------------------------------

void notifyControllersChanged() {
  TApp *app = TApp::instance();
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentObject()->notifyObjectIdChanged(false);
}

//=============================================================================
// MasterControllerNotifyUndo
//-----------------------------------------------------------------------------
/*! Placed at both ends of an undo block of the panel, so the viewers are
    refreshed after the keyframes are restored (undo) or set (redo).      */

class MasterControllerNotifyUndo final : public TUndo {
  QString m_name;
  bool m_notifyOnRedo;

public:
  MasterControllerNotifyUndo(const QString &name, bool notifyOnRedo)
      : m_name(name), m_notifyOnRedo(notifyOnRedo) {}

  void undo() const override {
    if (!m_notifyOnRedo) notifyControllersChanged();
  }
  void redo() const override {
    if (m_notifyOnRedo) notifyControllersChanged();
  }
  int getSize() const override { return sizeof(*this); }

  QString getHistoryString() override { return m_name; }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

//-----------------------------------------------------------------------------

void beginUndoBlock(const QString &name) {
  TUndoManager::manager()->beginBlock();
  TUndoManager::manager()->add(new MasterControllerNotifyUndo(name, false));
}

void endUndoBlock(const QString &name) {
  TUndoManager::manager()->add(new MasterControllerNotifyUndo(name, true));
  TUndoManager::manager()->endBlock();
  notifyControllersChanged();
  TApp::instance()->getCurrentScene()->setDirtyFlag(true);
}

//-----------------------------------------------------------------------------

//! Returns the current channel of a Function Editor, preferring a visible one.
TDoubleParam *getFunctionEditorCurve(QString &channelName) {
  QMainWindow *mainWindow = TApp::instance()->getMainWindow();
  if (!mainWindow) return 0;
  QList<FunctionViewer *> viewers =
      mainWindow->findChildren<FunctionViewer *>();
  TDoubleParam *hiddenCurve = 0;
  QString hiddenName;
  for (FunctionViewer *viewer : viewers) {
    QString name;
    TDoubleParam *curve = viewer->getCurrentChannelCurve(&name);
    if (!curve) continue;
    if (viewer->isVisible()) {
      channelName = name;
      return curve;
    }
    if (!hiddenCurve) {
      hiddenCurve = curve;
      hiddenName  = name;
    }
  }
  channelName = hiddenName;
  return hiddenCurve;
}

}  // namespace

//=============================================================================
// MasterControllersPane
//-----------------------------------------------------------------------------

MasterControllersPane::MasterControllersPane(QWidget *parent,
                                             Qt::WindowFlags flags)
    : QFrame(parent, flags), m_dragging(false), m_dragSetter(0) {
  setObjectName("MasterControllersPane");

  QPushButton *newButton = new QPushButton(tr("New Controller..."), this);
  newButton->setToolTip(
      tr("Create a master controller: a pegbar named \"ctrl_<name>\" whose "
         "Rotation channel holds the controller value"));
  QPushButton *bindButton =
      new QPushButton(tr("Bind Selected Channel..."), this);
  bindButton->setToolTip(
      tr("Drive the current channel of the Function Editor from a master "
         "controller through an expression"));

  m_emptyLabel = new QLabel(
      tr("No master controllers in the current xsheet.\nPegbars whose name "
         "starts with \"ctrl_\" are listed here."),
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
      buttonLayout->addWidget(bindButton);
      buttonLayout->addStretch(1);
    }
    mainLayout->addLayout(buttonLayout);
    mainLayout->addWidget(scrollArea, 1);
  }
  setLayout(mainLayout);

  connect(newButton, SIGNAL(clicked()), this, SLOT(onNewController()));
  connect(bindButton, SIGNAL(clicked()), this, SLOT(onBindChannel()));
}

//-----------------------------------------------------------------------------

MasterControllersPane::~MasterControllersPane() {
  // An unfinished drag still registers its undo.
  delete m_dragSetter;
}

//-----------------------------------------------------------------------------

bool MasterControllersPane::isMasterController(TStageObject *obj) {
  return obj && obj->getId().isPegbar() &&
         QString::fromStdString(obj->getName())
             .startsWith(controllerPrefix, Qt::CaseInsensitive);
}

//-----------------------------------------------------------------------------

MasterControllersPane::Controllers MasterControllersPane::getControllers()
    const {
  Controllers controllers;
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  if (!xsh) return controllers;
  TStageObjectTree *tree = xsh->getStageObjectTree();
  for (int i = 0; i < tree->getStageObjectCount(); ++i) {
    TStageObject *obj = tree->getStageObject(i);
    if (isMasterController(obj))
      controllers.push_back(std::make_pair(obj->getId(), obj->getName()));
  }
  std::sort(
      controllers.begin(), controllers.end(),
      [](const Controllers::value_type &a, const Controllers::value_type &b) {
        return a.second < b.second ||
               (a.second == b.second && a.first < b.first);
      });
  return controllers;
}

//-----------------------------------------------------------------------------

TDoubleParam *MasterControllersPane::getControllerCurve(
    const TStageObjectId &id) const {
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  if (!xsh) return 0;
  TStageObject *obj = xsh->getStageObjectTree()->getStageObject(id, false);
  if (!isMasterController(obj)) return 0;
  return obj->getParam(TStageObject::T_Angle);
}

//-----------------------------------------------------------------------------

void MasterControllersPane::showEvent(QShowEvent *) {
  TApp *app = TApp::instance();
  connect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
          SLOT(refresh()), Qt::UniqueConnection);
  connect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
          SLOT(updateValues()), Qt::UniqueConnection);
  connect(app->getCurrentObject(), SIGNAL(objectChanged(bool)), this,
          SLOT(updateValues()), Qt::UniqueConnection);
  refresh();
}

//-----------------------------------------------------------------------------

void MasterControllersPane::hideEvent(QHideEvent *) {
  if (m_dragging) endDrag();
  TApp *app = TApp::instance();
  disconnect(app->getCurrentScene(), SIGNAL(sceneSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetSwitched()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentXsheet(), SIGNAL(xsheetChanged()), this,
             SLOT(refresh()));
  disconnect(app->getCurrentFrame(), SIGNAL(frameSwitched()), this,
             SLOT(updateValues()));
  disconnect(app->getCurrentObject(), SIGNAL(objectChanged(bool)), this,
             SLOT(updateValues()));
}

//-----------------------------------------------------------------------------

void MasterControllersPane::refresh() {
  // Never rebuild the rows while one of their sliders is being dragged.
  if (m_dragging) return;
  if (getControllers() == m_controllers)
    updateValues();
  else
    rebuild();
}

//-----------------------------------------------------------------------------

void MasterControllersPane::rebuild() {
  // Rows are deleted later, since this may be called by one of their widgets.
  while (QLayoutItem *item = m_listLayout->takeAt(0)) {
    if (QWidget *widget = item->widget()) {
      widget->hide();
      widget->deleteLater();
    }
    delete item;
  }
  m_rows.clear();

  m_controllers = getControllers();
  m_emptyLabel->setVisible(m_controllers.empty());

  for (const Controllers::value_type &controller : m_controllers) {
    TStageObjectId id = controller.first;
    QString name      = QString::fromStdString(controller.second);
    QString shortName = name.mid(controllerPrefix.length());
    if (shortName.isEmpty()) shortName = name;

    QWidget *row           = new QWidget(this);
    QLabel *nameLabel      = new QLabel(shortName, row);
    QSlider *slider        = new QSlider(Qt::Horizontal, row);
    QDoubleSpinBox *field  = new QDoubleSpinBox(row);
    QPushButton *keyButton = new QPushButton(tr("Key"), row);

    nameLabel->setToolTip(tr("%1\nReference it in expressions as %2")
                              .arg(name)
                              .arg(getControllerRef(id)));
    slider->setRange(0, sliderSteps);
    slider->setToolTip(tr("Controller value from 0 to 1"));
    field->setRange(-10000.0, 10000.0);
    field->setDecimals(3);
    field->setSingleStep(0.01);
    field->setKeyboardTracking(false);
    field->setToolTip(tr("Controller value (may exceed the slider range)"));
    keyButton->setCheckable(true);
    keyButton->setToolTip(tr("Set or remove a key at the current frame"));

    QHBoxLayout *rowLayout = new QHBoxLayout();
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);
    {
      rowLayout->addWidget(nameLabel, 1);
      rowLayout->addWidget(slider, 2);
      rowLayout->addWidget(field);
      rowLayout->addWidget(keyButton);
    }
    row->setLayout(rowLayout);
    m_listLayout->addWidget(row);

    Row rowData = {id, slider, field, keyButton};
    m_rows.push_back(rowData);

    connect(slider, &QSlider::sliderPressed, this,
            [this, id]() { beginDrag(id); });
    connect(slider, &QSlider::sliderReleased, this, [this]() { endDrag(); });
    connect(slider, &QSlider::valueChanged, this,
            [this, id, slider](int value) {
              double v = (double)value / sliderSteps;
              if (slider->isSliderDown())
                dragTo(v);
              else
                setValue(id, v);
            });
    connect(field, &QDoubleSpinBox::editingFinished, this, [this, id, field]() {
      // Leaving the field unchanged must not set a key: the field shows the
      // value rounded to its decimals.
      TDoubleParam *curve = getControllerCurve(id);
      if (!curve) return;
      int frame = TApp::instance()->getCurrentFrame()->getFrame();
      if (std::abs(field->value() - curve->getValue(frame)) < 0.0005) return;
      setValue(id, field->value());
    });
    connect(keyButton, &QPushButton::clicked, this,
            [this, id]() { toggleKey(id); });
  }

  updateValues();
}

//-----------------------------------------------------------------------------

void MasterControllersPane::updateValues() {
  int frame = TApp::instance()->getCurrentFrame()->getFrame();
  for (const Row &row : m_rows) {
    TDoubleParam *curve = getControllerCurve(row.m_id);
    if (!curve) continue;
    double value = curve->getValue(frame);

    if (!(m_dragging && row.m_id == m_dragId)) {
      QSignalBlocker blocker(row.m_slider);
      row.m_slider->setValue(
          std::min(std::max(tround(value * sliderSteps), 0), sliderSteps));
    }
    if (!row.m_field->hasFocus()) {
      QSignalBlocker blocker(row.m_field);
      row.m_field->setValue(value);
    }
    {
      QSignalBlocker blocker(row.m_keyButton);
      row.m_keyButton->setChecked(curve->isKeyframe(frame));
    }
  }
}

//-----------------------------------------------------------------------------

void MasterControllersPane::setValue(const TStageObjectId &id, double value) {
  TDoubleParam *curve = getControllerCurve(id);
  if (!curve) return;
  int frame = TApp::instance()->getCurrentFrame()->getFrame();
  if (curve->isKeyframe(frame) && curve->getValue(frame) == value) return;

  QString undoName = tr("Set Master Controller");
  beginUndoBlock(undoName);
  KeyframeSetter::setValue(curve, frame, value);
  endUndoBlock(undoName);
}

//-----------------------------------------------------------------------------

void MasterControllersPane::toggleKey(const TStageObjectId &id) {
  TDoubleParam *curve = getControllerCurve(id);
  if (!curve) return;
  int frame = TApp::instance()->getCurrentFrame()->getFrame();

  QString undoName = tr("Master Controller Key");
  beginUndoBlock(undoName);
  if (curve->isKeyframe(frame))
    KeyframeSetter::removeKeyframeAt(curve, frame);
  else
    KeyframeSetter::setValue(curve, frame, curve->getValue(frame));
  endUndoBlock(undoName);
}

//-----------------------------------------------------------------------------

void MasterControllersPane::beginDrag(const TStageObjectId &id) {
  if (m_dragging) endDrag();
  m_dragging = true;
  m_dragId   = id;
}

//-----------------------------------------------------------------------------

void MasterControllersPane::dragTo(double value) {
  if (!m_dragging) return;
  if (!m_dragSetter) {
    TDoubleParam *curve = getControllerCurve(m_dragId);
    if (!curve) return;
    m_dragSetter = new KeyframeSetter(curve);
    m_dragSetter->createKeyframe(
        TApp::instance()->getCurrentFrame()->getFrame());
  }
  m_dragSetter->setValue(value);
  TApp::instance()->getCurrentObject()->notifyObjectIdChanged(true);
}

//-----------------------------------------------------------------------------

void MasterControllersPane::endDrag() {
  m_dragging = false;
  if (m_dragSetter) {
    QString undoName = tr("Set Master Controller");
    beginUndoBlock(undoName);
    // The setter registers the keyframe undo when deleted.
    delete m_dragSetter;
    m_dragSetter = 0;
    endUndoBlock(undoName);
  }
  refresh();
}

//-----------------------------------------------------------------------------

void MasterControllersPane::onNewController() {
  TApp *app                = TApp::instance();
  TXsheetHandle *xshHandle = app->getCurrentXsheet();
  TXsheet *xsh             = xshHandle->getXsheet();
  if (!xsh) return;

  Controllers controllers = getControllers();
  QString defaultName =
      QString("controller%1").arg((int)controllers.size() + 1);
  bool ok      = false;
  QString name = DVGui::getText(tr("New Master Controller"),
                                tr("Controller name:"), defaultName, &ok);
  if (!ok) return;
  name = name.trimmed();
  name.replace(QRegularExpression("\\s+"), "_");
  if (name.isEmpty()) name = defaultName;
  if (!name.startsWith(controllerPrefix, Qt::CaseInsensitive))
    name = controllerPrefix + name;

  for (const Controllers::value_type &controller : controllers) {
    if (QString::fromStdString(controller.second)
            .compare(name, Qt::CaseInsensitive) == 0) {
      DVGui::warning(
          tr("A master controller named %1 already exists.").arg(name));
      return;
    }
  }

  // The id addNewPegbar() is going to use.
  TStageObjectTree *tree = xsh->getStageObjectTree();
  int pegbarIndex        = 0;
  while (tree->getStageObject(TStageObjectId::PegbarId(pegbarIndex), false))
    pegbarIndex++;
  TStageObjectId id = TStageObjectId::PegbarId(pegbarIndex);

  // The controller is an unparented pegbar with no children: it moves
  // nothing on its own.
  QString undoName = tr("New Master Controller  %1").arg(name);
  beginUndoBlock(undoName);
  TStageObjectCmd::addNewPegbar(xshHandle, app->getCurrentObject());
  TStageObjectCmd::rename(id, name.toStdString(), xshHandle);
  endUndoBlock(undoName);

  refresh();
}

//-----------------------------------------------------------------------------

void MasterControllersPane::onBindChannel() {
  QString channelName;
  TDoubleParam *curve = getFunctionEditorCurve(channelName);
  if (!curve) {
    DVGui::warning(
        tr("Select the channel to drive in the Function Editor first."));
    return;
  }
  Controllers controllers = getControllers();
  if (controllers.empty()) {
    DVGui::warning(
        tr("There are no master controllers in the current xsheet. Create "
           "one with New Controller first."));
    return;
  }

  // The unit the expression result is read in; it is stored in the
  // segments, so the binding does not depend on later unit preferences.
  std::string unitName;
  if (TMeasure *measure = curve->getMeasure()) {
    if (const TUnit *unit = measure->getCurrentUnit())
      unitName =
          QString::fromStdWString(unit->getDefaultExtension()).toStdString();
  }

  // Dialog
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Bind Channel to Master Controller"));

  QComboBox *controllerCombo = new QComboBox(&dialog);
  for (const Controllers::value_type &controller : controllers)
    controllerCombo->addItem(QString::fromStdString(controller.second));

  QDoubleSpinBox *multiplierField = new QDoubleSpinBox(&dialog);
  multiplierField->setRange(-1000000.0, 1000000.0);
  multiplierField->setDecimals(4);
  multiplierField->setValue(1.0);

  QDoubleSpinBox *offsetField = new QDoubleSpinBox(&dialog);
  offsetField->setRange(-1000000.0, 1000000.0);
  offsetField->setDecimals(4);
  offsetField->setValue(0.0);

  QLabel *expressionLabel = new QLabel(&dialog);
  expressionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

  QDialogButtonBox *buttonBox = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);

  QFormLayout *formLayout = new QFormLayout();
  formLayout->addRow(tr("Channel:"),
                     new QLabel(channelName.isEmpty()
                                    ? QString::fromStdString(curve->getName())
                                    : channelName,
                                &dialog));
  formLayout->addRow(tr("Controller:"), controllerCombo);
  formLayout->addRow(tr("Multiplier:"), multiplierField);
  formLayout->addRow(tr("Offset:"), offsetField);
  formLayout->addRow(tr("Expression:"), expressionLabel);

  QLabel *noteLabel = new QLabel(
      tr("The expression replaces the interpolation of every segment of the "
         "channel over the scene frames. The result is read in %1.")
          .arg(unitName.empty() ? tr("the channel's unit")
                                : QString::fromStdString(unitName)),
      &dialog);
  noteLabel->setWordWrap(true);

  QVBoxLayout *dialogLayout = new QVBoxLayout();
  dialogLayout->addLayout(formLayout);
  dialogLayout->addWidget(noteLabel);
  dialogLayout->addWidget(buttonBox);
  dialog.setLayout(dialogLayout);

  auto updateExpression = [&]() {
    int index = controllerCombo->currentIndex();
    if (index < 0 || index >= (int)controllers.size()) return;
    expressionLabel->setText(makeExpression(controllers[index].first,
                                            multiplierField->value(),
                                            offsetField->value()));
  };
  updateExpression();
  connect(controllerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
          &dialog, [&](int) { updateExpression(); });
  connect(multiplierField, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
          &dialog, [&](double) { updateExpression(); });
  connect(offsetField, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
          &dialog, [&](double) { updateExpression(); });
  connect(buttonBox, SIGNAL(accepted()), &dialog, SLOT(accept()));
  connect(buttonBox, SIGNAL(rejected()), &dialog, SLOT(reject()));

  if (dialog.exec() != QDialog::Accepted) return;

  int index = controllerCombo->currentIndex();
  if (index < 0 || index >= (int)controllers.size()) return;
  std::string text =
      makeExpression(controllers[index].first, multiplierField->value(),
                     offsetField->value())
          .toStdString();

  // Check the expression the same way the Function Editor does.
  TExpression expr;
  expr.setGrammar(curve->getGrammar());
  expr.setText(text);
  if (!curve->getGrammar() || !expr.isValid()) {
    DVGui::warning(
        tr("The selected channel cannot reference the controller. Only "
           "channels of the current xsheet can be bound."));
    return;
  }
  if (dependsOn(expr, curve)) {
    DVGui::warning(
        tr("There is a circular reference in the definition of the "
           "interpolation."));
    return;
  }

  // Cover the scene frames and any existing key with expression segments.
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  double r0    = 0;
  double r1    = std::max(xsh ? xsh->getFrameCount() - 1 : 0, 1);
  int kCount   = curve->getKeyframeCount();
  if (kCount > 0) {
    r0 = std::min(r0, curve->keyframeIndexToFrame(0));
    r1 = std::max(r1, curve->keyframeIndexToFrame(kCount - 1));
  }

  QString undoName =
      tr("Bind to Master Controller  %1")
          .arg(QString::fromStdString(controllers[index].second));
  beginUndoBlock(undoName);
  {
    KeyframeSetter setter(curve);
    setter.createKeyframe(r0);
  }
  {
    KeyframeSetter setter(curve);
    setter.createKeyframe(r1);
  }
  kCount = curve->getKeyframeCount();
  for (int k = 0; k + 1 < kCount; ++k) {
    KeyframeSetter setter(curve, k);
    setter.setExpression(text);
    setter.setUnitName(unitName);
  }
  endUndoBlock(undoName);
}
