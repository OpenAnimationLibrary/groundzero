#include "motionarcs.h"

#include "menubarcommandids.h"
#include "tapp.h"

#include "toonz/hook.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/colorfield.h"
#include "toonzqt/menubarcommand.h"

#include "tenv.h"
#include "tgl.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMenu>
#include <QSpinBox>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace {

TEnv::IntVar MotionArcsShowCurrent("MotionArcsShowCurrent", 1);
TEnv::IntVar MotionArcsSpacing("MotionArcsSpacing", MotionArcs::SpacingColor);
TEnv::IntVar MotionArcsLabelEvery("MotionArcsLabelEvery", 4);
TEnv::IntVar MotionArcsMaxFrames("MotionArcsMaxFrames", 240);
TEnv::IntVar MotionArcsRGB("MotionArcsRGB", 0x30B0FF);  // light blue

QString tr(const char *text) {
  return QCoreApplication::translate("MotionArcs", text);
}

//-----------------------------------------------------------------------------

// The runtime state of the columns: held by the column (which stays alive
// while it is here), and forgotten when the scene changes
struct ColumnState {
  bool pinned = false;
  MotionArc::Track track;
};

struct State {
  const ToonzScene *scene = nullptr;
  std::map<TXshColumnP, ColumnState> columns;
};

State &state() {
  static State s;
  const ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (s.scene != scene) {
    s.columns.clear();
    s.scene = scene;
  }
  return s;
}

const ColumnState *findState(TXshColumn *column) {
  if (!column) return nullptr;
  State &s = state();
  auto it  = s.columns.find(TXshColumnP(column));
  return it == s.columns.end() ? nullptr : &it->second;
}

void redraw() {
  // emit sceneChanged WITHOUT dirty flag, as the viewer toggles
  TApp::instance()->getCurrentScene()->notifySceneChanged(false);
}

TPixel32 arcColor(int rgb) {
  return TPixel32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

bool hasArc(TXsheet *xsh, int col) {
  TXshColumn *column = xsh->getColumn(col);
  return column && !column->isEmpty() && !column->getSoundColumn() &&
         !column->getSoundTextColumn() && column->isCamstandVisible();
}

// The transform of the column's local space to the camera-seen world at frame
// (as the stage places the column, see StageBuilder::addCell)
TAffine columnViewAff(TXsheet *xsh, const TStageObjectId &id, int frame) {
  const MotionArc::Camera camera = MotionArcs::currentCamera(xsh, frame);
  TAffine aff;
  if (!TStageObject::perspective(
          aff, camera.aff, camera.z, xsh->getPlacement(id, frame),
          xsh->getZ(id, frame), xsh->getStageObject(id)->getGlobalNoScaleZ()))
    aff = xsh->getPlacement(id, frame);
  return aff;
}

//-----------------------------------------------------------------------------

void drawArc(const std::vector<MotionArc::Sample> &samples,
             const TAffine &viewMatrix, int frame,
             const MotionArcs::Settings &settings, double px) {
  if (samples.size() < 2) return;
  const TPixel32 color = arcColor(settings.rgb);

  std::vector<TPointD> points;
  points.reserve(samples.size());
  for (const MotionArc::Sample &s : samples)
    points.push_back(viewMatrix * s.viewPos);

  // The path
  tglColor(color);
  glBegin(GL_LINE_STRIP);
  for (const TPointD &p : points) tglVertex(p);
  glEnd();

  // A dot per frame, its colour or size by the speed
  const std::vector<double> speed =
      MotionArc::normalized(MotionArc::speeds(samples));
  const bool byColor = settings.spacing & MotionArcs::SpacingColor;
  const bool bySize  = settings.spacing & MotionArcs::SpacingSize;
  for (size_t i = 0; i < samples.size(); ++i) {
    const TPointD &p = points[i];
    if (byColor) {
      int r, g, b;
      MotionArc::rampColor(speed[i], r, g, b);
      tglColor(TPixel32(r, g, b));
    } else
      tglColor(color);
    if (samples[i].key) {
      // Squares on the keys
      const double h = 4.0 * px;
      tglFillRect(p.x - h, p.y - h, p.x + h, p.y + h);
      tglColor(TPixel32::Black);
      tglDrawRect(p.x - h, p.y - h, p.x + h, p.y + h);
    } else {
      const double r = (bySize ? 1.5 + 3.5 * speed[i] : 2.5) * px;
      tglDrawDisk(p, r);
    }
    if (samples[i].frame == frame) {
      tglColor(TPixel32::White);
      tglDrawCircle(p, 7.0 * px);
    }
  }

  // The frame numbers, as the xsheet shows them, about 10 pixels high
  tglColor(color);
  for (size_t i = 0; i < samples.size(); ++i) {
    if (!MotionArc::isLabelled(samples[i].frame, samples[i].key,
                               settings.labelEvery))
      continue;
    const TPointD at = points[i] + TPointD(6.0 * px, 6.0 * px);
    glPushMatrix();
    glTranslated(at.x, at.y, 0);
    glScaled(1.2 * px, 1.2 * px, 1);
    tglDrawText(TPointD(), std::to_string(samples[i].frame + 1));
    glPopMatrix();
  }
}

//-----------------------------------------------------------------------------

class MotionArcsSettingsDialog final : public QDialog {
public:
  MotionArcsSettingsDialog() {
    setWindowTitle(tr("Motion Arcs Settings"));
    const MotionArcs::Settings current = MotionArcs::settings();

    QCheckBox *showCurrent = new QCheckBox(tr("Show the Current Column"), this);
    showCurrent->setChecked(current.showCurrent);

    QComboBox *spacing = new QComboBox(this);
    spacing->addItem(tr("Color by Speed"), MotionArcs::SpacingColor);
    spacing->addItem(tr("Size by Speed"), MotionArcs::SpacingSize);
    spacing->addItem(tr("Color and Size by Speed"), MotionArcs::SpacingBoth);
    spacing->addItem(tr("Plain Dots"), MotionArcs::SpacingNone);
    spacing->setCurrentIndex(std::max(0, spacing->findData(current.spacing)));

    QSpinBox *labelEvery = new QSpinBox(this);
    labelEvery->setRange(0, 100);
    labelEvery->setSpecialValueText(tr("None"));
    labelEvery->setValue(current.labelEvery);

    QSpinBox *maxFrames = new QSpinBox(this);
    maxFrames->setRange(10, 1000);
    maxFrames->setValue(current.maxFrames);

    DVGui::ColorField *color =
        new DVGui::ColorField(this, false, arcColor(current.rgb), 40, false);
    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    QFormLayout *layout = new QFormLayout(this);
    layout->addRow(showCurrent);
    layout->addRow(tr("Spacing:"), spacing);
    layout->addRow(tr("Frame Numbers Every:"), labelEvery);
    layout->addRow(tr("Frames Shown:"), maxFrames);
    layout->addRow(tr("Color:"), color);
    layout->addRow(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this,
            [this, showCurrent, spacing, labelEvery, maxFrames, color]() {
              MotionArcs::Settings settings;
              settings.showCurrent = showCurrent->isChecked();
              settings.spacing     = spacing->currentData().toInt();
              settings.labelEvery  = labelEvery->value();
              settings.maxFrames   = maxFrames->value();
              const TPixel32 value = color->getColor();
              settings.rgb         = (value.r << 16) | (value.g << 8) | value.b;
              MotionArcs::setSettings(settings);
              accept();
            });
  }
};

class MotionArcsSettingsCommand final : public MenuItemHandler {
public:
  MotionArcsSettingsCommand() : MenuItemHandler(MI_MotionArcsSettings) {}
  void execute() override {
    MotionArcsSettingsDialog dialog;
    dialog.exec();
  }
} motionArcsSettingsCommand;

}  // namespace

//=============================================================================

MotionArcs::Settings MotionArcs::settings() {
  Settings s;
  s.showCurrent = MotionArcsShowCurrent != 0;
  s.spacing     = std::max(0, std::min((int)MotionArcsSpacing, 3));
  s.labelEvery  = std::max(0, std::min((int)MotionArcsLabelEvery, 100));
  s.maxFrames   = std::max(10, std::min((int)MotionArcsMaxFrames, 1000));
  s.rgb         = MotionArcsRGB;
  return s;
}

void MotionArcs::setSettings(const Settings &s) {
  MotionArcsShowCurrent = s.showCurrent ? 1 : 0;
  MotionArcsSpacing     = s.spacing;
  MotionArcsLabelEvery  = s.labelEvery;
  MotionArcsMaxFrames   = s.maxFrames;
  MotionArcsRGB         = s.rgb;
  redraw();
}

//-----------------------------------------------------------------------------

bool MotionArcs::isPinned(TXshColumn *column) {
  const ColumnState *s = findState(column);
  return s && s->pinned;
}

void MotionArcs::setPinned(TXshColumn *column, bool pinned) {
  if (!column) return;
  state().columns[TXshColumnP(column)].pinned = pinned;
  redraw();
}

MotionArc::Track MotionArcs::track(TXshColumn *column) {
  const ColumnState *s = findState(column);
  return s ? s->track : MotionArc::Track();
}

void MotionArcs::setTrack(TXshColumn *column, const MotionArc::Track &track) {
  if (!column) return;
  state().columns[TXshColumnP(column)].track = track;
  redraw();
}

void MotionArcs::clear() {
  state().columns.clear();
  redraw();
}

//-----------------------------------------------------------------------------

MotionArc::Camera MotionArcs::currentCamera(TXsheet *xsh, int frame) {
  const TStageObjectId cameraId =
      xsh->getStageObjectTree()->getCurrentCameraId();
  MotionArc::Camera camera;
  camera.aff = xsh->getPlacement(cameraId, frame);
  camera.z   = xsh->getZ(cameraId, frame);
  return camera;
}

std::vector<MotionArc::Sample> MotionArcs::columnSamples(int col) {
  TApp *app       = TApp::instance();
  TXsheet *xsh    = app->getCurrentXsheet()->getXsheet();
  const int frame = app->getCurrentFrame()->getFrame();
  std::vector<MotionArc::Sample> samples;
  if (!xsh || col < 0 || !hasArc(xsh, col)) return samples;

  const TStageObjectId id      = TStageObjectId::ColumnId(col);
  const MotionArc::Track track = MotionArcs::track(xsh->getColumn(col));
  int r0, r1;
  if (!MotionArc::arcRange(xsh, id, track, frame, settings().maxFrames, r0, r1))
    return samples;
  return MotionArc::sample(xsh, id, track, r0, r1, currentCamera(xsh, frame),
                           frame);
}

//-----------------------------------------------------------------------------

void MotionArcs::draw(const TAffine &viewMatrix) {
  TApp *app = TApp::instance();
  if (app->getCurrentFrame()->isEditingLevel()) return;
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  if (!xsh) return;
  const int frame     = app->getCurrentFrame()->getFrame();
  const int current   = app->getCurrentColumn()->getColumnIndex();
  const Settings sets = settings();
  const double px     = std::sqrt(tglGetPixelSize2());

  glPushAttrib(GL_ALL_ATTRIB_BITS);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_LINE_SMOOTH);
  for (int col = 0; col < xsh->getColumnCount(); ++col) {
    const bool shown =
        (sets.showCurrent && col == current) || isPinned(xsh->getColumn(col));
    if (!shown) continue;
    drawArc(columnSamples(col), viewMatrix, frame, sets, px);
  }
  glPopAttrib();
}

//-----------------------------------------------------------------------------

void MotionArcs::addContextMenu(QMenu *menu, const TPointD &world) {
  TApp *app = TApp::instance();
  if (app->getCurrentFrame()->isEditingLevel()) return;
  TXsheet *xsh = app->getCurrentXsheet()->getXsheet();
  if (!xsh) return;

  QMenu *arcMenu = menu->addMenu(tr("Motion Arcs"));
  arcMenu->addAction(CommandManager::instance()->getAction(MI_MotionArcs));

  const int col      = app->getCurrentColumn()->getColumnIndex();
  const int frame    = app->getCurrentFrame()->getFrame();
  TXshColumn *column = col >= 0 && hasArc(xsh, col) ? xsh->getColumn(col) : 0;
  if (column) {
    TXshColumnP keep(column);
    const QString name = QString::fromStdString(
        xsh->getStageObject(TStageObjectId::ColumnId(col))->getName());

    QAction *pin = arcMenu->addAction(tr("Keep the Arc of %1").arg(name));
    pin->setCheckable(true);
    pin->setChecked(isPinned(column));
    QObject::connect(pin, &QAction::triggered,
                     [keep](bool on) { setPinned(keep.getPointer(), on); });

    // The tracked point
    QMenu *trackMenu               = arcMenu->addMenu(tr("Track"));
    QActionGroup *group            = new QActionGroup(trackMenu);
    const MotionArc::Track current = track(column);
    auto addTrack = [&](const QString &label, const MotionArc::Track &t,
                        bool checked) {
      QAction *action = trackMenu->addAction(label);
      action->setCheckable(true);
      action->setChecked(checked);
      group->addAction(action);
      QObject::connect(action, &QAction::triggered,
                       [keep, t]() { setTrack(keep.getPointer(), t); });
    };
    addTrack(tr("Center"), MotionArc::Track(),
             current.kind == MotionArc::Track::Center);

    // The hooks of the current drawing
    TXshSimpleLevel *sl = xsh->getCell(frame, col).getSimpleLevel();
    if (HookSet *hooks = sl ? sl->getHookSet() : 0) {
      for (int i = 0; i < hooks->getHookCount(); ++i) {
        Hook *hook = hooks->getHook(i);
        if (!hook || hook->isEmpty()) continue;
        MotionArc::Track t;
        t.kind = MotionArc::Track::Hook;
        t.hook = i + 1;
        addTrack(
            tr("Hook %1").arg(i + 1), t,
            current.kind == MotionArc::Track::Hook && current.hook == i + 1);
      }
    }

    // The clicked point, in the column's local space at the current frame
    const TStageObjectId id = TStageObjectId::ColumnId(col);
    MotionArc::Track picked;
    picked.kind  = MotionArc::Track::Point;
    picked.point = columnViewAff(xsh, id, frame).inv() * world;
    xsh->getPlacement(id, frame);  // keep the placement cache current
    addTrack(tr("This Point"), picked, current.kind == MotionArc::Track::Point);
  }

  QAction *clearAll = arcMenu->addAction(tr("Clear All Motion Arcs"));
  QObject::connect(clearAll, &QAction::triggered, [] { clear(); });
  arcMenu->addSeparator();
  arcMenu->addAction(
      CommandManager::instance()->getAction(MI_MotionArcsSettings));
}
