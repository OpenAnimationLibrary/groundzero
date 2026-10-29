

#include "onionskinmaskgui.h"
#include "tapp.h"
#include "toonz/tonionskinmaskhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/txshsimplelevel.h"

#include "toonz/onionskinmask.h"

#include <QActionGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QSlider>
#include <QWidgetAction>

namespace {

void setGhostFade(const GhostFade::Settings &settings) {
  OnionSkinMask::setGhostFade(settings);
  // Redraw the viewers
  TApp::instance()->getCurrentOnionSkin()->notifyOnionSkinMaskChanged();
}

//------------------------------------------------------------------------------

// The Ghost Fade submenu: an opacity slider for all the ghosts, their tint and
// the step between the relative ghosts (doc/ghost_fade.md)
void addGhostFadeMenu(QMenu *menu) {
  QMenu *fadeMenu                    = menu->addMenu(QObject::tr("Ghost Fade"));
  const GhostFade::Settings &current = OnionSkinMask::getGhostFade();

  // Opacity slider, in percent
  QWidget *sliderBox  = new QWidget(fadeMenu);
  QHBoxLayout *layout = new QHBoxLayout(sliderBox);
  layout->setContentsMargins(8, 4, 8, 4);
  QLabel *label   = new QLabel(QObject::tr("Opacity:"), sliderBox);
  QSlider *slider = new QSlider(Qt::Horizontal, sliderBox);
  slider->setRange(0, int(GhostFade::maxOpacity * 100));
  slider->setValue(int(current.opacity * 100 + 0.5));
  slider->setMinimumWidth(120);
  QLabel *value = new QLabel(QString("%1%").arg(slider->value()), sliderBox);
  value->setMinimumWidth(value->fontMetrics().horizontalAdvance("200%"));
  layout->addWidget(label);
  layout->addWidget(slider, 1);
  layout->addWidget(value);
  QWidgetAction *sliderAction = new QWidgetAction(fadeMenu);
  sliderAction->setDefaultWidget(sliderBox);
  fadeMenu->addAction(sliderAction);
  QObject::connect(slider, &QSlider::valueChanged, [value](int percent) {
    value->setText(QString("%1%").arg(percent));
    GhostFade::Settings settings = OnionSkinMask::getGhostFade();
    settings.opacity             = percent / 100.0;
    setGhostFade(settings);
  });

  // Tint
  QAction *tint = fadeMenu->addAction(QObject::tr("Tint Ghosts"));
  tint->setCheckable(true);
  tint->setChecked(current.tint);
  QObject::connect(tint, &QAction::triggered, [](bool checked) {
    GhostFade::Settings settings = OnionSkinMask::getGhostFade();
    settings.tint                = checked;
    setGhostFade(settings);
  });

  // Step between the relative ghosts
  QMenu *stepMenu      = fadeMenu->addMenu(QObject::tr("Ghost Every"));
  QActionGroup *steps  = new QActionGroup(stepMenu);
  const int stepList[] = {1, 2, 3, 4, 6, 8, 12};
  for (int step : stepList) {
    QAction *action = stepMenu->addAction(
        step == 1 ? QObject::tr("Every Frame")
                  : QObject::tr("Every %1 Frames").arg(step));
    action->setCheckable(true);
    action->setChecked(step == current.step);
    action->setData(step);
    steps->addAction(action);
  }
  QObject::connect(steps, &QActionGroup::triggered, [](QAction *action) {
    GhostFade::Settings settings = OnionSkinMask::getGhostFade();
    settings.step                = action->data().toInt();
    setGhostFade(settings);
  });

  fadeMenu->addSeparator();
  QAction *reset = fadeMenu->addAction(QObject::tr("Reset Ghost Fade"));
  reset->setEnabled(!current.isDefault());
  QObject::connect(reset, &QAction::triggered,
                   [] { setGhostFade(GhostFade::Settings()); });
}

}  // namespace

//=============================================================================
// OnioniSkinMaskGUI::OnionSkinSwitcher
//-----------------------------------------------------------------------------

OnionSkinMask OnioniSkinMaskGUI::OnionSkinSwitcher::getMask() const {
  return TApp::instance()->getCurrentOnionSkin()->getOnionSkinMask();
}

//------------------------------------------------------------------------------

void OnioniSkinMaskGUI::OnionSkinSwitcher::setMask(const OnionSkinMask &mask) {
  TApp::instance()->getCurrentOnionSkin()->setOnionSkinMask(mask);
  TApp::instance()->getCurrentOnionSkin()->notifyOnionSkinMaskChanged();
}

//------------------------------------------------------------------------------

bool OnioniSkinMaskGUI::OnionSkinSwitcher::isActive() const {
  OnionSkinMask osm = getMask();
  return osm.isEnabled() && !osm.isEmpty();
}

//------------------------------------------------------------------------------

bool OnioniSkinMaskGUI::OnionSkinSwitcher::isWholeScene() const {
  OnionSkinMask osm = getMask();
  return osm.isWholeScene();
}

//------------------------------------------------------------------------------

void OnioniSkinMaskGUI::OnionSkinSwitcher::activate() {
  OnionSkinMask osm = getMask();
  if (osm.isEnabled() && !osm.isEmpty()) return;
  if (osm.isEmpty()) {
    osm.setMos(-1, true);
    osm.setMos(-2, true);
    osm.setMos(-3, true);
  }
  osm.enable(true);
  setMask(osm);
}

//------------------------------------------------------------------------------

void OnioniSkinMaskGUI::OnionSkinSwitcher::deactivate() {
  OnionSkinMask osm = getMask();
  if (!osm.isEnabled() || osm.isEmpty()) return;
  osm.enable(false);
  setMask(osm);
}

//------------------------------------------------------------------------------

void OnioniSkinMaskGUI::OnionSkinSwitcher::setWholeScene() {
  OnionSkinMask osm = getMask();
  osm.setIsWholeScene(true);
  setMask(osm);
}

//------------------------------------------------------------------------------

void OnioniSkinMaskGUI::OnionSkinSwitcher::setSingleLevel() {
  OnionSkinMask osm = getMask();
  osm.setIsWholeScene(false);
  setMask(osm);
}

void OnioniSkinMaskGUI::OnionSkinSwitcher::clearFOS() {
  OnionSkinMask osm = getMask();

  for (int i = (osm.getFosCount() - 1); i >= 0; i--)
    osm.setFos(osm.getFos(i), false);

  setMask(osm);
}

void OnioniSkinMaskGUI::OnionSkinSwitcher::clearMOS() {
  OnionSkinMask osm = getMask();

  for (int i = (osm.getMosCount() - 1); i >= 0; i--)
    osm.setMos(osm.getMos(i), false);

  setMask(osm);
}

void OnioniSkinMaskGUI::OnionSkinSwitcher::clearOS() {
  clearFOS();
  clearMOS();
}

//------------------------------------------------------------------------------

void OnioniSkinMaskGUI::addOnionSkinCommand(QMenu *menu, bool isFilmStrip) {
  static OnioniSkinMaskGUI::OnionSkinSwitcher switcher;
  if (switcher.isActive()) {
    QAction *dectivateOnionSkin =
        menu->addAction(QString(QObject::tr("Deactivate Onion Skin")));
    menu->connect(dectivateOnionSkin, SIGNAL(triggered()), &switcher,
                  SLOT(deactivate()));
    if (!isFilmStrip) {
      if (switcher.isWholeScene()) {
        QAction *limitOnionSkinToLevel =
            menu->addAction(QString(QObject::tr("Limit Onion Skin To Level")));
        menu->connect(limitOnionSkinToLevel, SIGNAL(triggered()), &switcher,
                      SLOT(setSingleLevel()));
      } else {
        QAction *extendOnionSkinToScene =
            menu->addAction(QString(QObject::tr("Extend Onion Skin To Scene")));
        menu->connect(extendOnionSkinToScene, SIGNAL(triggered()), &switcher,
                      SLOT(setWholeScene()));
      }
      OnionSkinMask osm = switcher.getMask();
      if (osm.getFosCount() || osm.getMosCount()) {
        QAction *clearAllOnionSkins = menu->addAction(
            QString(QObject::tr("Clear All Onion Skin Markers")));
        menu->connect(clearAllOnionSkins, SIGNAL(triggered()), &switcher,
                      SLOT(clearOS()));
      }
      if (osm.getFosCount() && osm.getMosCount()) {
        QAction *clearFixedOnionSkins = menu->addAction(
            QString(QObject::tr("Clear All Fixed Onion Skin Markers")));
        menu->connect(clearFixedOnionSkins, SIGNAL(triggered()), &switcher,
                      SLOT(clearFOS()));
        QAction *clearRelativeOnionSkins = menu->addAction(
            QString(QObject::tr("Clear All Relative Onion Skin Markers")));
        menu->connect(clearRelativeOnionSkins, SIGNAL(triggered()), &switcher,
                      SLOT(clearMOS()));
      }
    }
    addGhostFadeMenu(menu);
  } else {
    QAction *activateOnionSkin =
        menu->addAction(QString(QObject::tr("Activate Onion Skin")));
    menu->connect(activateOnionSkin, SIGNAL(triggered()), &switcher,
                  SLOT(activate()));
  }
}

//------------------------------------------------------------------------------

void OnioniSkinMaskGUI::resetShiftTraceFrameOffset() {
  auto setGhostOffset = [](int firstOffset, int secondOffset) {
    OnionSkinMask osm =
        TApp::instance()->getCurrentOnionSkin()->getOnionSkinMask();
    osm.setShiftTraceGhostFrameOffset(0, firstOffset);
    osm.setShiftTraceGhostFrameOffset(1, secondOffset);
    TApp::instance()->getCurrentOnionSkin()->setOnionSkinMask(osm);
  };

  TApp *app = TApp::instance();
  if (app->getCurrentFrame()->isEditingLevel()) {
    TXshSimpleLevel *level = app->getCurrentLevel()->getSimpleLevel();
    if (!level) {
      setGhostOffset(0, 0);
      return;
    }
    TFrameId fid     = app->getCurrentFrame()->getFid();
    int firstOffset  = (fid > level->getFirstFid()) ? -1 : 0;
    int secondOffset = (fid < level->getLastFid()) ? 1 : 0;

    setGhostOffset(firstOffset, secondOffset);
  } else {  // when scene frame is selected
    TXsheet *xsh       = app->getCurrentXsheet()->getXsheet();
    int col            = app->getCurrentColumn()->getColumnIndex();
    TXshColumn *column = xsh->getColumn(col);
    if (!column || column->isEmpty()) {
      setGhostOffset(0, 0);
      return;
    }
    int r0, r1;
    column->getRange(r0, r1);
    int row         = app->getCurrentFrame()->getFrame();
    TXshCell cell   = xsh->getCell(row, col);
    int firstOffset = -1;
    while (1) {
      int r = row + firstOffset;
      if (r < r0) {
        firstOffset = 0;
        break;
      }
      if (!xsh->getCell(r, col).isEmpty() && xsh->getCell(r, col) != cell) {
        break;
      }
      firstOffset--;
    }
    int secondOffset = 1;
    while (1) {
      int r = row + secondOffset;
      if (r > r1) {
        secondOffset = 0;
        break;
      }
      if (!xsh->getCell(r, col).isEmpty() && xsh->getCell(r, col) != cell) {
        break;
      }
      secondOffset++;
    }
    setGhostOffset(firstOffset, secondOffset);
  }
}
