#include "columncoloreffectpopup.h"

// Tnz6 includes
#include "tapp.h"

// TnzQt includes
#include "toonzqt/colorfield.h"
#include "toonzqt/doublefield.h"
#include "toonzqt/uitranslations.h"

// TnzLib includes
#include "toonz/columncoloreffect.h"
#include "toonz/tframehandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjectid.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevelcolumn.h"

// TnzBase includes
#include "tdoubleparam.h"
#include "tfx.h"
#include "tparamcontainer.h"
#include "tparamset.h"

// TnzCore includes
#include "historytypes.h"
#include "tundo.h"

// Qt includes
#include <QCheckBox>
#include <QDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

// STD includes
#include <vector>

namespace {

//=============================================================================
// The parameters of the Color Effect of a level column fx
//-----------------------------------------------------------------------------

TDoubleParam *effectParam(TFx *fx, int p) {
  if (!fx) return 0;
  return dynamic_cast<TDoubleParam *>(
      fx->getParams()->getParam(ColumnColorEffect::paramName(p)));
}

TPixelParam *tintParam(TFx *fx) {
  if (!fx) return 0;
  return dynamic_cast<TPixelParam *>(
      fx->getParams()->getParam(ColumnColorEffect::tintColorParamName()));
}

//! All of them: the numeric ones in the order of ColumnColorEffect::Param,
//! then the tint color
std::vector<TParam *> effectParams(TFx *fx) {
  std::vector<TParam *> params;
  for (int p = 0; p < ColumnColorEffect::ParamCount; ++p)
    if (TDoubleParam *param = effectParam(fx, p)) params.push_back(param);
  if (TPixelParam *tint = tintParam(fx)) params.push_back(tint);
  return params;
}

//=============================================================================
// Undo: the parameters of the effect before and after a change
//-----------------------------------------------------------------------------

typedef std::vector<TParamP> EffectState;

EffectState saveState(TFx *fx) {
  EffectState state;
  for (TParam *param : effectParams(fx))
    state.push_back(TParamP(param->clone()));
  return state;
}

void restoreState(TFx *fx, const EffectState &state) {
  std::vector<TParam *> params = effectParams(fx);
  for (size_t i = 0; i < params.size() && i < state.size(); ++i)
    params[i]->copy(state[i].getPointer());
}

void notifyChanged() {
  TApp *app = TApp::instance();
  app->getCurrentScene()->setDirtyFlag(true);
  app->getCurrentXsheet()->notifyXsheetChanged();
}

class ColorEffectUndo final : public TUndo {
  TFxP m_fx;
  EffectState m_before, m_after;

public:
  ColorEffectUndo(const TFxP &fx, const EffectState &before,
                  const EffectState &after)
      : m_fx(fx), m_before(before), m_after(after) {}

  void undo() const override {
    restoreState(m_fx.getPointer(), m_before);
    notifyChanged();
  }
  void redo() const override {
    restoreState(m_fx.getPointer(), m_after);
    notifyChanged();
  }
  int getSize() const override {
    return sizeof(*this) + 256 * (int)(m_before.size() + m_after.size());
  }
  QString getHistoryString() override {
    return QObject::tr("Column Color Effect");
  }
  int getHistoryType() override { return HistoryType::Xsheet; }
};

//=============================================================================
// The dialog
//-----------------------------------------------------------------------------

class ColorEffectDialog final : public QDialog {
  TFxP m_fx;  //!< The column fx (kept alive, as the undos do)

  DVGui::DoubleField *m_fields[ColumnColorEffect::ParamCount];
  DVGui::ColorField *m_tintField;
  QCheckBox *m_keyCheck;
  QPushButton *m_advancedButton;
  QGroupBox *m_advancedBox;
  QString m_columnName;

  EffectState m_before;  //!< The parameters when the current edit started
  bool m_editing  = false;
  bool m_updating = false;

public:
  ColorEffectDialog(QWidget *parent);

  void setColumn(TXshColumn *column);

private:
  int currentFrame() const {
    return TApp::instance()->getCurrentFrame()->getFrame();
  }

  void refresh();
  void beginEdit();
  void endEdit(bool isDragging);
  void onValueChanged(int p, bool isDragging);
  void onTintChanged(const TPixel32 &color, bool isDragging);
  void reset();
};

//-----------------------------------------------------------------------------

ColorEffectDialog::ColorEffectDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Color Effect"));
  setModal(false);

  for (int p = 0; p < ColumnColorEffect::ParamCount; ++p) {
    double minValue = 0.0, maxValue = 0.0;
    ColumnColorEffect::valueRange(p, minValue, maxValue);
    m_fields[p] = new DVGui::DoubleField(this, true, 1);
    m_fields[p]->setValues(ColumnColorEffect::defaultValue(p), minValue,
                           maxValue);
    // The advanced values are typed
    if (p >= ColumnColorEffect::RedMul) m_fields[p]->enableSlider(false);
  }
  m_tintField = new DVGui::ColorField(this, false, TPixel32::Black, 40, false);

  m_keyCheck = new QCheckBox(tr("Set Key at the Current Frame"), this);
  m_keyCheck->setToolTip(
      tr("When on, a change sets a key at the current frame. Without keys a "
         "value applies to the whole column; the values that have keys are "
         "always keyed."));

  m_advancedButton = new QPushButton(tr("Advanced..."), this);
  m_advancedButton->setCheckable(true);
  QPushButton *resetButton = new QPushButton(tr("Reset"), this);
  resetButton->setToolTip(tr("Removes the Color Effect of the column"));
  QPushButton *closeButton = new QPushButton(tr("Close"), this);

  QLabel *note = new QLabel(
      tr("The values can be animated in the Function Editor, where they are "
         "listed under the column once used."),
      this);
  note->setWordWrap(true);

  // Layout
  QVBoxLayout *mainLayout = new QVBoxLayout(this);
  QGridLayout *grid       = new QGridLayout();
  grid->setHorizontalSpacing(6);
  grid->setVerticalSpacing(6);
  {
    int row = 0;
    grid->addWidget(new QLabel(tr("Brightness:"), this), row, 0,
                    Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(m_fields[ColumnColorEffect::Brightness], row, 1);
    grid->addWidget(new QLabel("%", this), row++, 2);

    grid->addWidget(new QLabel(tr("Tint:"), this), row, 0,
                    Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(m_tintField, row++, 1);

    grid->addWidget(new QLabel(tr("Tint Amount:"), this), row, 0,
                    Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(m_fields[ColumnColorEffect::TintAmount], row, 1);
    grid->addWidget(new QLabel("%", this), row++, 2);

    grid->addWidget(new QLabel(tr("Alpha:"), this), row, 0,
                    Qt::AlignRight | Qt::AlignVCenter);
    grid->addWidget(m_fields[ColumnColorEffect::Alpha], row, 1);
    grid->addWidget(new QLabel("%", this), row++, 2);
  }
  mainLayout->addLayout(grid);

  m_advancedBox          = new QGroupBox(tr("Advanced"), this);
  QGridLayout *advLayout = new QGridLayout(m_advancedBox);
  {
    advLayout->addWidget(new QLabel(tr("Multiplier (%)"), this), 0, 1,
                         Qt::AlignCenter);
    advLayout->addWidget(new QLabel(tr("Offset"), this), 0, 2, Qt::AlignCenter);
    const QString channels[4] = {tr("Red:"), tr("Green:"), tr("Blue:"),
                                 tr("Alpha:")};
    for (int i = 0; i < 4; ++i) {
      advLayout->addWidget(new QLabel(channels[i], this), i + 1, 0,
                           Qt::AlignRight | Qt::AlignVCenter);
      advLayout->addWidget(m_fields[ColumnColorEffect::RedMul + i], i + 1, 1);
      advLayout->addWidget(m_fields[ColumnColorEffect::RedAdd + i], i + 1, 2);
    }
  }
  mainLayout->addWidget(m_advancedBox);
  m_advancedBox->hide();

  mainLayout->addWidget(m_keyCheck);
  mainLayout->addWidget(note);

  QHBoxLayout *buttons = new QHBoxLayout();
  buttons->addWidget(m_advancedButton);
  buttons->addStretch(1);
  buttons->addWidget(resetButton);
  buttons->addWidget(closeButton);
  mainLayout->addLayout(buttons);

  // Connections
  for (int p = 0; p < ColumnColorEffect::ParamCount; ++p)
    connect(m_fields[p], &DVGui::DoubleValueField::valueChanged, this,
            [this, p](bool isDragging) { onValueChanged(p, isDragging); });
  connect(m_tintField, &DVGui::ColorField::colorChanged, this,
          [this](const TPixel32 &color, bool isDragging) {
            onTintChanged(color, isDragging);
          });
  connect(m_advancedButton, &QPushButton::toggled, this, [this](bool on) {
    m_advancedBox->setVisible(on);
    adjustSize();
  });
  connect(resetButton, &QPushButton::clicked, this, [this]() { reset(); });
  connect(closeButton, &QPushButton::clicked, this, [this]() { hide(); });

  TApp *app = TApp::instance();
  connect(app->getCurrentFrame(), &TFrameHandle::frameSwitched, this,
          [this]() { refresh(); });
  connect(app->getCurrentXsheet(), &TXsheetHandle::xsheetChanged, this,
          [this]() { refresh(); });
  // Another scene: the column is gone
  connect(app->getCurrentScene(), &TSceneHandle::sceneSwitched, this, [this]() {
    m_fx      = TFxP();
    m_editing = false;
    m_before.clear();
    hide();
  });
  // Do not call refresh() here: it writes numeric controls and could replace
  // an in-progress value edit. Static captions are tracked independently.
  UiTranslations::onLanguageChanged(this, [this]() {
    setWindowTitle(m_columnName.isEmpty() ? tr("Color Effect")
        : tr("Color Effect: %1").arg(m_columnName));
  });
}

//-----------------------------------------------------------------------------

void ColorEffectDialog::setColumn(TXshColumn *column) {
  TXshLevelColumn *levelColumn = column ? column->getLevelColumn() : 0;
  m_fx      = levelColumn ? TFxP(levelColumn->getFx()) : TFxP();
  m_editing = false;
  m_before.clear();

  QString name;
  if (levelColumn && levelColumn->getXsheet()) {
    TStageObject *object = levelColumn->getXsheet()->getStageObject(
        TStageObjectId::ColumnId(levelColumn->getIndex()));
    if (object) name = QString::fromStdString(object->getName());
  }
  setWindowTitle(name.isEmpty() ? tr("Color Effect")
                                : tr("Color Effect: %1").arg(name));
  m_columnName = name;

  // Show the advanced values when they are used
  bool advanced = false;
  for (int p = ColumnColorEffect::RedMul; p < ColumnColorEffect::ParamCount;
       ++p) {
    TDoubleParam *param = effectParam(m_fx.getPointer(), p);
    if (param &&
        (param->hasKeyframes() ||
         param->getDefaultValue() != ColumnColorEffect::defaultValue(p)))
      advanced = true;
  }
  if (advanced) m_advancedButton->setChecked(true);

  refresh();
}

//-----------------------------------------------------------------------------

void ColorEffectDialog::refresh() {
  if (!m_fx) return;

  m_updating      = true;
  const int frame = currentFrame();
  for (int p = 0; p < ColumnColorEffect::ParamCount; ++p)
    if (TDoubleParam *param = effectParam(m_fx.getPointer(), p))
      m_fields[p]->setValue(param->getValue(frame));
  if (TPixelParam *tint = tintParam(m_fx.getPointer())) {
    TPixel32 color = tint->getValue(frame);
    color.m        = 255;
    m_tintField->setColor(color);
  }
  m_updating = false;
}

//-----------------------------------------------------------------------------

void ColorEffectDialog::beginEdit() {
  if (m_editing) return;
  m_before  = saveState(m_fx.getPointer());
  m_editing = true;
}

//-----------------------------------------------------------------------------

void ColorEffectDialog::endEdit(bool isDragging) {
  notifyChanged();
  if (isDragging) return;

  // One undo for the whole drag
  m_editing = false;
  TUndoManager::manager()->add(
      new ColorEffectUndo(m_fx, m_before, saveState(m_fx.getPointer())));
  m_before.clear();
}

//-----------------------------------------------------------------------------

void ColorEffectDialog::onValueChanged(int p, bool isDragging) {
  if (m_updating || !m_fx) return;
  TDoubleParam *param = effectParam(m_fx.getPointer(), p);
  if (!param) return;

  beginEdit();
  const double value =
      ColumnColorEffect::clampValue(p, m_fields[p]->getValue());
  // A value that has keys is keyed; otherwise it is the value of the column
  if (m_keyCheck->isChecked() || param->hasKeyframes())
    param->setValue(currentFrame(), value);
  else
    param->setDefaultValue(value);
  endEdit(isDragging);
}

//-----------------------------------------------------------------------------

void ColorEffectDialog::onTintChanged(const TPixel32 &color, bool isDragging) {
  if (m_updating || !m_fx) return;
  TPixelParam *tint = tintParam(m_fx.getPointer());
  if (!tint) return;

  beginEdit();
  TPixel32 value(color.r, color.g, color.b, 255);
  if (m_keyCheck->isChecked() || tint->hasKeyframes())
    tint->setValue(currentFrame(), value);
  else
    tint->setDefaultValue(value);
  endEdit(isDragging);
}

//-----------------------------------------------------------------------------

void ColorEffectDialog::reset() {
  if (!m_fx) return;

  m_editing                = false;
  const EffectState before = saveState(m_fx.getPointer());
  for (int p = 0; p < ColumnColorEffect::ParamCount; ++p)
    if (TDoubleParam *param = effectParam(m_fx.getPointer(), p)) {
      param->clearKeyframes();
      param->enableCycle(false);
      param->setDefaultValue(ColumnColorEffect::defaultValue(p));
    }
  if (TPixelParam *tint = tintParam(m_fx.getPointer())) {
    tint->clearKeyframes();
    tint->setDefaultValue(TPixel32::Black);
  }
  TUndoManager::manager()->add(
      new ColorEffectUndo(m_fx, before, saveState(m_fx.getPointer())));
  notifyChanged();
  refresh();
}

}  // namespace

//=============================================================================

void ColumnColorEffectPopup::open(TXshColumn *column) {
  if (!column || !column->getLevelColumn()) return;

  static QPointer<ColorEffectDialog> dialog;
  if (!dialog)
    dialog = new ColorEffectDialog(TApp::instance()->getMainWindow());

  dialog->setColumn(column);
  dialog->show();
  dialog->raise();
  dialog->activateWindow();
}
