#include "toonzqt/uitranslations.h"
#include "mastercontrollerspane.h"

// Tnz6 includes
#include "tapp.h"
#include "posespane.h"

// TnzQt includes
#include "toonzqt/dvdialog.h"
#include "toonzqt/functionviewer.h"

// TnzLib includes
#include "toonz/toonzscene.h"
#include "toonz/childstack.h"
#include "toonz/poselibrary.h"
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshsimplelevel.h"
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
#include "tconvert.h"
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
#include <QMouseEvent>
#include <QPainter>
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
#include <functional>

namespace {

//-----------------------------------------------------------------------------

const QString controllerPrefix = "ctrl_";

// The slider covers the controller values 0..1 in this many steps.
const int sliderSteps = 1000;

// The size of the 2D pad, and the margin around the square it draws.
const int padSize   = 110;
const int padMargin = 6;

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

//! Drives a curve by an expression: the curve gets a key at the start and at
//! the end of the scene (and keeps its other ones), and the expression
//! replaces the interpolation of every segment. Must be called in an undo
//! block.
void bindCurveToExpression(TDoubleParam *curve, const std::string &text,
                           const std::string &unitName) {
  // Cover the scene frames and any existing key with expression segments.
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  double r0    = 0;
  double r1    = std::max(xsh ? xsh->getFrameCount() - 1 : 0, 1);
  int kCount   = curve->getKeyframeCount();
  if (kCount > 0) {
    r0 = std::min(r0, curve->keyframeIndexToFrame(0));
    r1 = std::max(r1, curve->keyframeIndexToFrame(kCount - 1));
  }

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
}

//=============================================================================
// BakeDrawingSwapUndo
//-----------------------------------------------------------------------------
/*! Replaces the cells of a column over a range of rows.                    */

class BakeDrawingSwapUndo final : public TUndo {
  int m_col;
  std::vector<TXshCell> m_before, m_after;

  void setCells(const std::vector<TXshCell> &cells) const {
    TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
    if (!xsh) return;
    for (int r = 0; r < (int)cells.size(); ++r)
      xsh->setCell(r, m_col, cells[r]);
    notifyControllersChanged();
  }

public:
  BakeDrawingSwapUndo(int col, const std::vector<TXshCell> &before,
                      const std::vector<TXshCell> &after)
      : m_col(col), m_before(before), m_after(after) {}

  void undo() const override { setCells(m_before); }
  void redo() const override { setCells(m_after); }
  int getSize() const override {
    return sizeof(*this) +
           (m_before.size() + m_after.size()) * sizeof(TXshCell);
  }

  QString getHistoryString() override {
    return QObject::tr("Bake Drawing Swap");
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

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
// MasterControllerPad
//-----------------------------------------------------------------------------
/*! A square pad whose horizontal and vertical positions are two controller
    values in the range 0..1 (the vertical one grows upwards). The owner
    reacts to the callbacks, and sets the values with setValues().          */

class MasterControllerPad final : public QWidget {
  double m_x, m_y;
  bool m_dragging;

  QRect padRect() const {
    int side = std::min(width(), height()) - 2 * padMargin;
    return QRect(padMargin, padMargin, std::max(side, 2), std::max(side, 2));
  }

  void moveTo(const QPoint &pos) {
    QRect r = padRect();
    m_x     = std::min(
            std::max((pos.x() - r.left()) / (double)(r.width() - 1), 0.0), 1.0);
    m_y = std::min(
        std::max((r.bottom() - pos.y()) / (double)(r.height() - 1), 0.0), 1.0);
    update();
    if (m_onMove) m_onMove(m_x, m_y);
  }

public:
  std::function<void()> m_onPress;
  std::function<void(double, double)> m_onMove;
  std::function<void()> m_onRelease;

  MasterControllerPad(QWidget *parent)
      : QWidget(parent), m_x(0), m_y(0), m_dragging(false) {
    setFixedSize(padSize, padSize);
    setToolTip(tr("Drag to set the X and Y controllers, from 0 to 1"));
  }

  void setValues(double x, double y) {
    m_x = std::min(std::max(x, 0.0), 1.0);
    m_y = std::min(std::max(y, 0.0), 1.0);
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QRect r = padRect();
    painter.fillRect(r, palette().color(QPalette::Base));
    painter.setPen(palette().color(QPalette::Mid));
    painter.drawLine(r.center().x(), r.top(), r.center().x(), r.bottom());
    painter.drawLine(r.left(), r.center().y(), r.right(), r.center().y());
    painter.drawRect(r.adjusted(0, 0, -1, -1));
    QPointF point(r.left() + m_x * (r.width() - 1),
                  r.bottom() - m_y * (r.height() - 1));
    painter.setPen(palette().color(QPalette::Text));
    painter.setBrush(palette().color(QPalette::Highlight));
    painter.drawEllipse(point, 5.0, 5.0);
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton) return;
    m_dragging = true;
    if (m_onPress) m_onPress();
    moveTo(event->pos());
  }
  void mouseMoveEvent(QMouseEvent *event) override {
    if (m_dragging) moveTo(event->pos());
  }
  void mouseReleaseEvent(QMouseEvent *event) override {
    if (!m_dragging || event->button() != Qt::LeftButton) return;
    m_dragging = false;
    if (m_onRelease) m_onRelease();
  }
};

//=============================================================================
// MasterControllersPane
//-----------------------------------------------------------------------------

MasterControllersPane::MasterControllersPane(QWidget *parent,
                                             Qt::WindowFlags flags)
    : QFrame(parent, flags)
    , m_dragging(false)
    , m_dragHasY(false)
    , m_dragSetter(0)
    , m_dragYSetter(0) {
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

  QPushButton *bindPosesButton = new QPushButton(tr("Bind Poses..."), this);
  bindPosesButton->setToolTip(
      tr("Blend two poses of the Poses panel with a master controller, using "
         "two offset layers whose weights the controller drives"));
  QPushButton *bakeSwapButton =
      new QPushButton(tr("Bake Drawing Swap..."), this);
  bakeSwapButton->setToolTip(
      tr("Write the cells of a column over the scene frames, choosing its "
         "drawing from the value of a master controller"));

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
    QHBoxLayout *button2Layout = new QHBoxLayout();
    button2Layout->setContentsMargins(0, 0, 0, 0);
    button2Layout->setSpacing(5);
    {
      button2Layout->addWidget(bindPosesButton);
      button2Layout->addWidget(bakeSwapButton);
      button2Layout->addStretch(1);
    }
    mainLayout->addLayout(button2Layout);
    mainLayout->addWidget(scrollArea, 1);
  }
  setLayout(mainLayout);

  connect(newButton, SIGNAL(clicked()), this, SLOT(onNewController()));
  connect(bindButton, SIGNAL(clicked()), this, SLOT(onBindChannel()));
  connect(bindPosesButton, SIGNAL(clicked()), this, SLOT(onBindPoses()));
  connect(bakeSwapButton, SIGNAL(clicked()), this, SLOT(onBakeDrawingSwap()));
}

//-----------------------------------------------------------------------------

MasterControllersPane::~MasterControllersPane() {
  // An unfinished drag still registers its undo.
  delete m_dragSetter;
  delete m_dragYSetter;
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
          SLOT(onSceneSwitched()), Qt::UniqueConnection);
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
             SLOT(onSceneSwitched()));
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

void MasterControllersPane::onSceneSwitched() {
  // The widget modes belong to the controllers of the previous scene.
  m_modes.clear();
  m_padY.clear();
  refresh();
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

    if (!m_modes.count(id))
      m_modes[id] = hasPoseBlendLayers(id) ? PoseBlendMode : SliderMode;

    QWidget *row           = new QWidget(this);
    QLabel *nameLabel      = new QLabel(shortName, row);
    QComboBox *modeCombo         = new QComboBox(row);
    UiTranslations::watchCombo(modeCombo);
    QSlider *slider        = new QSlider(Qt::Horizontal, row);
    QDoubleSpinBox *field  = new QDoubleSpinBox(row);
    QPushButton *keyButton = new QPushButton(tr("Key"), row);
    QPushButton *bindPosesButton = new QPushButton(tr("Bind Poses..."), row);
    MasterControllerPad *pad     = new MasterControllerPad(row);
    QWidget *padYBox             = new QWidget(row);
    QComboBox *padYCombo         = new QComboBox(padYBox);

    nameLabel->setToolTip(UiTranslations::text(tr("%1\nReference it in expressions as %2"))
                              .arg(name)
                              .arg(getControllerRef(id)));
    modeCombo->addItem(tr("Slider"));
    modeCombo->addItem(tr("2D Pad"));
    modeCombo->addItem(tr("Pose Blend"));
    modeCombo->setCurrentIndex(m_modes[id]);
    modeCombo->setToolTip(
        tr("Widget of the controller: a slider, a 2D pad also setting a "
           "second controller, or a slider blending two poses"));
    slider->setRange(0, sliderSteps);
    slider->setToolTip(tr("Controller value from 0 to 1"));
    field->setRange(-10000.0, 10000.0);
    field->setDecimals(3);
    field->setSingleStep(0.01);
    field->setKeyboardTracking(false);
    field->setToolTip(tr("Controller value (may exceed the slider range)"));
    keyButton->setCheckable(true);
    keyButton->setToolTip(tr("Set or remove a key at the current frame"));
    bindPosesButton->setToolTip(
        tr("Pick the poses at the values 0 and 1 of this controller"));

    // The controllers which can give the Y of the pad
    TStageObjectId padY = getPadY(id);
    for (int i = 0; i < (int)m_controllers.size(); ++i) {
      if (m_controllers[i].first == id) continue;
      padYCombo->addItem(QString::fromStdString(m_controllers[i].second), i);
      if (m_controllers[i].first == padY)
        padYCombo->setCurrentIndex(padYCombo->count() - 1);
    }
    padYCombo->setEnabled(padYCombo->count() > 0);
    padYCombo->setToolTip(tr("Controller set by the vertical position"));
    QHBoxLayout *padYLayout = new QHBoxLayout();
    padYLayout->setContentsMargins(0, 0, 0, 0);
    padYLayout->setSpacing(4);
    {
      padYLayout->addWidget(new QLabel(tr("Y:"), padYBox));
      padYLayout->addWidget(padYCombo, 1);
    }
    padYBox->setLayout(padYLayout);

    QVBoxLayout *rowLayout = new QVBoxLayout();
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(2);
    {
      QHBoxLayout *headLayout = new QHBoxLayout();
      headLayout->setContentsMargins(0, 0, 0, 0);
      headLayout->setSpacing(4);
      {
        headLayout->addWidget(nameLabel, 1);
        headLayout->addWidget(modeCombo);
        headLayout->addWidget(field);
        headLayout->addWidget(keyButton);
      }
      rowLayout->addLayout(headLayout);
      QHBoxLayout *bodyLayout = new QHBoxLayout();
      bodyLayout->setContentsMargins(0, 0, 0, 0);
      bodyLayout->setSpacing(4);
      {
        bodyLayout->addWidget(slider, 1);
        bodyLayout->addWidget(bindPosesButton);
        bodyLayout->addWidget(pad);
        bodyLayout->addWidget(padYBox, 1);
        bodyLayout->addStretch(0);
      }
      rowLayout->addLayout(bodyLayout);
    }
    row->setLayout(rowLayout);
    m_listLayout->addWidget(row);

    Row rowData = {id,        modeCombo,       slider, pad,      padYBox,
                   padYCombo, bindPosesButton, field,  keyButton};
    m_rows.push_back(rowData);
    applyMode(rowData, m_modes[id]);

    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, id](int index) {
              m_modes[id] = index;
              for (const Row &r : m_rows)
                if (r.m_id == id) applyMode(r, index);
              updateValues();
            });
    connect(padYCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, id, padYCombo](int) {
              int index = padYCombo->currentData().toInt();
              if (index < 0 || index >= (int)m_controllers.size()) return;
              m_padY[id] = m_controllers[index].first;
              updateValues();
            });
    connect(bindPosesButton, &QPushButton::clicked, this,
            [this, id]() { bindPoses(id); });
    pad->m_onPress   = [this, id]() { beginDrag(id, getPadY(id)); };
    pad->m_onMove    = [this](double x, double y) { dragTo(x, y); };
    pad->m_onRelease = [this]() { endDrag(); };
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

      if (getMode(row.m_id) == PadMode) {
        TDoubleParam *yCurve = getControllerCurve(getPadY(row.m_id));
        row.m_pad->setValues(value, yCurve ? yCurve->getValue(frame) : 0.0);
      }
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

  // The 2D pad keys its Y controller too.
  std::vector<TDoubleParam *> curves(1, curve);
  if (getMode(id) == PadMode)
    if (TDoubleParam *yCurve = getControllerCurve(getPadY(id)))
      curves.push_back(yCurve);
  const bool addKeys = !curve->isKeyframe(frame);

  QString undoName = tr("Master Controller Key");
  beginUndoBlock(undoName);
  for (TDoubleParam *c : curves) {
    if (!addKeys)
      KeyframeSetter::removeKeyframeAt(c, frame);
    else
      KeyframeSetter::setValue(c, frame, c->getValue(frame));
  }
  endUndoBlock(undoName);
}

//-----------------------------------------------------------------------------

void MasterControllersPane::beginDrag(const TStageObjectId &id,
                                      const TStageObjectId &yId) {
  if (m_dragging) endDrag();
  m_dragging = true;
  m_dragId   = id;
  m_dragYId  = yId;
  m_dragHasY = yId != TStageObjectId::NoneId;
}

//-----------------------------------------------------------------------------

void MasterControllersPane::dragTo(double value, double yValue) {
  if (!m_dragging) return;
  int frame = TApp::instance()->getCurrentFrame()->getFrame();
  if (!m_dragSetter) {
    TDoubleParam *curve = getControllerCurve(m_dragId);
    if (!curve) return;
    m_dragSetter = new KeyframeSetter(curve);
    m_dragSetter->createKeyframe(frame);
  }
  if (m_dragHasY && !m_dragYSetter) {
    if (TDoubleParam *yCurve = getControllerCurve(m_dragYId)) {
      m_dragYSetter = new KeyframeSetter(yCurve);
      m_dragYSetter->createKeyframe(frame);
    }
  }
  m_dragSetter->setValue(value);
  if (m_dragYSetter) m_dragYSetter->setValue(yValue);
  TApp::instance()->getCurrentObject()->notifyObjectIdChanged(true);
}

//-----------------------------------------------------------------------------

void MasterControllersPane::endDrag() {
  m_dragging = false;
  m_dragHasY = false;
  if (m_dragSetter || m_dragYSetter) {
    QString undoName = tr("Set Master Controller");
    beginUndoBlock(undoName);
    // The setters register the keyframe undos when deleted: a drag of a pad
    // is a single undo step.
    delete m_dragSetter;
    m_dragSetter = 0;
    delete m_dragYSetter;
    m_dragYSetter = 0;
    endUndoBlock(undoName);
  }
  refresh();
}

//-----------------------------------------------------------------------------

int MasterControllersPane::getMode(const TStageObjectId &id) const {
  auto it = m_modes.find(id);
  return it == m_modes.end() ? (int)SliderMode : it->second;
}

//-----------------------------------------------------------------------------

//! Returns the controller giving the Y of the pad of the controller: the one
//! chosen if it still exists, and the first other one otherwise.
TStageObjectId MasterControllersPane::getPadY(const TStageObjectId &id) const {
  auto it = m_padY.find(id);
  if (it != m_padY.end() && it->second != id) {
    for (const Controllers::value_type &controller : m_controllers)
      if (controller.first == it->second) return it->second;
  }
  for (const Controllers::value_type &controller : m_controllers)
    if (controller.first != id) return controller.first;
  return TStageObjectId::NoneId;
}

//-----------------------------------------------------------------------------

void MasterControllersPane::applyMode(const Row &row, int mode) const {
  row.m_slider->setVisible(mode != PadMode);
  row.m_bindPosesButton->setVisible(mode == PoseBlendMode);
  row.m_pad->setVisible(mode == PadMode);
  row.m_padYBox->setVisible(mode == PadMode);
}

//-----------------------------------------------------------------------------

//! Returns whether the controller has been bound to poses: some object has the
//! layers "<controller>_A" and "<controller>_B".
bool MasterControllersPane::hasPoseBlendLayers(const TStageObjectId &id) const {
  TXsheet *xsh = TApp::instance()->getCurrentXsheet()->getXsheet();
  if (!xsh) return false;
  TStageObjectTree *tree   = xsh->getStageObjectTree();
  TStageObject *controller = tree->getStageObject(id, false);
  if (!controller) return false;
  std::wstring name = ::to_wstring(controller->getName());
  for (int i = 0; i < tree->getStageObjectCount(); ++i) {
    TStageObject *obj = tree->getStageObject(i);
    if (obj->findOffsetLayer(name + L"_A") >= 0 &&
        obj->findOffsetLayer(name + L"_B") >= 0)
      return true;
  }
  return false;
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
          UiTranslations::text(tr("A master controller named %1 already exists.")).arg(name));
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
  QString undoName = UiTranslations::text(tr("New Master Controller  %1")).arg(name);
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
      UiTranslations::text(tr("The expression replaces the interpolation of every segment of the "
         "channel over the scene frames. The result is read in %1."))
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

  QString undoName =
      UiTranslations::text(tr("Bind to Master Controller  %1"))
          .arg(QString::fromStdString(controllers[index].second));
  beginUndoBlock(undoName);
  bindCurveToExpression(curve, text, unitName);
  endUndoBlock(undoName);
}

//-----------------------------------------------------------------------------

void MasterControllersPane::onBindPoses() { bindPoses(TStageObjectId::NoneId); }

//-----------------------------------------------------------------------------

//! Makes the controller blend two poses: the poses are applied to two offset
//! layers, "<controller>_A" and "<controller>_B", of every object they change,
//! and the weights of the layers are driven by the controller, as
//! (1 - value) and value.
void MasterControllersPane::bindPoses(const TStageObjectId &preselected) {
  TApp *app         = TApp::instance();
  ToonzScene *scene = app->getCurrentScene()->getScene();
  if (!scene) return;
  if (scene->getChildStack()->getAncestorCount() > 0) {
    DVGui::warning(tr("Poses can be used only in the top-level xsheet."));
    return;
  }
  Controllers controllers = getControllers();
  if (controllers.empty()) {
    DVGui::warning(
        tr("There are no master controllers in the current xsheet. Create "
           "one with New Controller first."));
    return;
  }
  PoseLibrary *poseLibrary = scene->getPoseLibrary();
  if (poseLibrary->getPoseCount() < 2) {
    DVGui::warning(
        tr("Two poses are needed. Add them in the Poses panel first."));
    return;
  }

  // Dialog
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Bind Poses to Master Controller"));

  QComboBox *controllerCombo = new QComboBox(&dialog);
  for (int i = 0; i < (int)controllers.size(); ++i) {
    controllerCombo->addItem(QString::fromStdString(controllers[i].second));
    if (controllers[i].first == preselected)
      controllerCombo->setCurrentIndex(i);
  }
  QComboBox *poseACombo = new QComboBox(&dialog);
  QComboBox *poseBCombo = new QComboBox(&dialog);
  for (int i = 0; i < poseLibrary->getPoseCount(); ++i) {
    poseACombo->addItem(poseLibrary->getPose(i).m_name);
    poseBCombo->addItem(poseLibrary->getPose(i).m_name);
  }
  poseBCombo->setCurrentIndex(1);

  QDialogButtonBox *buttonBox = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);

  QFormLayout *formLayout = new QFormLayout();
  formLayout->addRow(tr("Controller:"), controllerCombo);
  formLayout->addRow(tr("Pose A (value 0):"), poseACombo);
  formLayout->addRow(tr("Pose B (value 1):"), poseBCombo);

  QLabel *noteLabel = new QLabel(
      tr("The poses are applied to the offset layers <controller>_A and "
         "<controller>_B of the objects they change, which replace the layers "
         "with these names. Their weights are driven by the controller, "
         "over the scene frames."),
      &dialog);
  noteLabel->setWordWrap(true);

  QVBoxLayout *dialogLayout = new QVBoxLayout();
  dialogLayout->addLayout(formLayout);
  dialogLayout->addWidget(noteLabel);
  dialogLayout->addWidget(buttonBox);
  dialog.setLayout(dialogLayout);

  connect(buttonBox, SIGNAL(accepted()), &dialog, SLOT(accept()));
  connect(buttonBox, SIGNAL(rejected()), &dialog, SLOT(reject()));

  if (dialog.exec() != QDialog::Accepted) return;

  int index = controllerCombo->currentIndex();
  if (index < 0 || index >= (int)controllers.size()) return;
  int indexA = poseACombo->currentIndex();
  int indexB = poseBCombo->currentIndex();
  if (indexA == indexB) {
    DVGui::warning(tr("Choose two different poses."));
    return;
  }
  const TStageObjectId controllerId = controllers[index].first;
  const PoseLibrary::Pose &poseA    = poseLibrary->getPose(indexA);
  const PoseLibrary::Pose &poseB    = poseLibrary->getPose(indexB);

  TXsheet *xsh = scene->getTopXsheet();
  std::vector<TStageObjectId> ids;
  PosesPane::getLayerObjects(xsh, poseA, ids);
  PosesPane::getLayerObjects(xsh, poseB, ids);
  if (ids.empty()) {
    DVGui::warning(
        tr("None of the channels of the poses can be applied to an offset "
           "layer."));
    return;
  }
  if (std::find(ids.begin(), ids.end(), controllerId) != ids.end()) {
    DVGui::warning(
        tr("A pose changes the controller itself, which would make its "
           "weights depend on themselves."));
    return;
  }

  const QString controllerName =
      QString::fromStdString(controllers[index].second);
  const std::wstring layerNames[2] = {(controllerName + "_A").toStdWString(),
                                      (controllerName + "_B").toStdWString()};
  // Layer A has the weight (1 - value), layer B the weight value
  const std::string ref = getControllerRef(controllerId).toStdString();
  const std::string weightTexts[2] = {"1-" + ref, ref};

  TXsheetHandle *xshHandle = app->getCurrentXsheet();
  TStageObjectTree *tree   = xsh->getStageObjectTree();

  QString undoName =
      UiTranslations::text(tr("Bind Poses to Master Controller  %1")).arg(controllerName);
  beginUndoBlock(undoName);
  // Start from new layers, on every object any of the poses changes
  for (const TStageObjectId &id : ids) {
    TStageObject *obj = tree->getStageObject(id, false);
    for (const std::wstring &layerName : layerNames) {
      int layerIndex = obj->findOffsetLayer(layerName);
      if (layerIndex >= 0)
        TStageObjectCmd::removeOffsetLayer(id, layerIndex, xshHandle);
      TStageObjectCmd::addOffsetLayer(id, layerName, xshHandle);
    }
  }
  PosesPane::applyPoseKeys(poseA, 1.0, layerNames[0]);
  PosesPane::applyPoseKeys(poseB, 1.0, layerNames[1]);
  for (const TStageObjectId &id : ids) {
    TStageObject *obj = tree->getStageObject(id, false);
    for (int k = 0; k < 2; ++k) {
      int layerIndex = obj->findOffsetLayer(layerNames[k]);
      if (layerIndex < 0) continue;
      bindCurveToExpression(
          obj->getOffsetLayer(layerIndex).m_weight.getPointer(), weightTexts[k],
          std::string());
    }
  }
  endUndoBlock(undoName);

  m_modes[controllerId] = PoseBlendMode;
  rebuild();
}

//-----------------------------------------------------------------------------

//! Writes the cells of a column over the scene frames: the drawing of each
//! frame is chosen by the value of the controller at the frame. No animatable
//! channel selects the drawing of a column, so there is nothing to bind an
//! expression to, and the cells are written once ("baked").
void MasterControllersPane::onBakeDrawingSwap() {
  TApp *app    = TApp::instance();
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  if (!xsh) return;
  Controllers controllers = getControllers();
  if (controllers.empty()) {
    DVGui::warning(
        tr("There are no master controllers in the current xsheet. Create "
           "one with New Controller first."));
    return;
  }

  // The columns of drawings, with the level of their first cell
  struct ColumnInfo {
    int m_index;
    TXshLevelP m_level;
    std::vector<TFrameId> m_fids;
  };
  std::vector<ColumnInfo> columns;
  for (int c = 0; c < xsh->getColumnCount(); ++c) {
    TXshColumn *column = xsh->getColumn(c);
    if (!column || column->isEmpty() || !column->getLevelColumn()) continue;
    int r0, r1;
    column->getRange(r0, r1);
    if (r1 < r0) continue;
    TXshSimpleLevel *level = xsh->getCell(r0, c).getSimpleLevel();
    if (!level) continue;
    ColumnInfo info;
    info.m_index = c;
    info.m_level = xsh->getCell(r0, c).m_level;
    level->getFids(info.m_fids);
    if (info.m_fids.empty()) continue;
    columns.push_back(info);
  }
  if (columns.empty()) {
    DVGui::warning(tr("There are no columns of drawings in the xsheet."));
    return;
  }
  int frameCount = xsh->getFrameCount();

  // Dialog
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Bake Drawing Swap"));

  QComboBox *controllerCombo = new QComboBox(&dialog);
  for (const Controllers::value_type &controller : controllers)
    controllerCombo->addItem(QString::fromStdString(controller.second));
  QComboBox *columnCombo = new QComboBox(&dialog);
  for (const ColumnInfo &info : columns) {
    TStageObject *obj =
        xsh->getStageObject(TStageObjectId::ColumnId(info.m_index));
    columnCombo->addItem(
        UiTranslations::text(tr("%1 (%2, %3 drawings)"))
            .arg(QString::fromStdString(obj->getName()))
            .arg(QString::fromStdWString(info.m_level->getName()))
            .arg((int)info.m_fids.size()));
  }

  QDialogButtonBox *buttonBox = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);

  QFormLayout *formLayout = new QFormLayout();
  formLayout->addRow(tr("Controller:"), controllerCombo);
  formLayout->addRow(tr("Column:"), columnCombo);

  QLabel *noteLabel = new QLabel(
      UiTranslations::text(tr("No channel can select the drawing of a column, so the cells of the "
         "column are written once for the frames 1 to %1 of the scene: the "
         "drawing number is round(value * (n - 1)) of the n drawings of the "
         "level, in their order, with the value of the controller limited to "
         "0..1. The existing cells of these frames are replaced. Bake again "
         "after changing the controller."))
          .arg(frameCount),
      &dialog);
  noteLabel->setWordWrap(true);

  QVBoxLayout *dialogLayout = new QVBoxLayout();
  dialogLayout->addLayout(formLayout);
  dialogLayout->addWidget(noteLabel);
  dialogLayout->addWidget(buttonBox);
  dialog.setLayout(dialogLayout);

  connect(buttonBox, SIGNAL(accepted()), &dialog, SLOT(accept()));
  connect(buttonBox, SIGNAL(rejected()), &dialog, SLOT(reject()));

  if (dialog.exec() != QDialog::Accepted) return;

  int controllerIndex = controllerCombo->currentIndex();
  int columnIndex     = columnCombo->currentIndex();
  if (controllerIndex < 0 || controllerIndex >= (int)controllers.size() ||
      columnIndex < 0 || columnIndex >= (int)columns.size())
    return;
  TDoubleParam *curve = getControllerCurve(controllers[controllerIndex].first);
  const ColumnInfo &info = columns[columnIndex];
  if (!curve || frameCount <= 0) return;
  if (xsh->getColumn(info.m_index)->isLocked()) {
    DVGui::warning(tr("The column is locked."));
    return;
  }

  std::vector<TXshCell> before(frameCount), after(frameCount);
  bool changed = false;
  int n        = (int)info.m_fids.size();
  for (int r = 0; r < frameCount; ++r) {
    before[r]    = xsh->getCell(r, info.m_index);
    double value = std::min(std::max(curve->getValue(r), 0.0), 1.0);
    after[r]     = TXshCell(info.m_level, info.m_fids[tround(value * (n - 1))]);
    if (before[r].m_level.getPointer() != after[r].m_level.getPointer() ||
        before[r].m_frameId != after[r].m_frameId)
      changed = true;
  }
  if (!changed) return;

  QString undoName =
      UiTranslations::text(tr("Bake Drawing Swap  %1"))
          .arg(QString::fromStdString(controllers[controllerIndex].second));
  beginUndoBlock(undoName);
  BakeDrawingSwapUndo *undo =
      new BakeDrawingSwapUndo(info.m_index, before, after);
  undo->redo();
  TUndoManager::manager()->add(undo);
  endUndoBlock(undoName);
}
