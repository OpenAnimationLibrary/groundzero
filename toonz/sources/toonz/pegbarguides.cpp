#include "toonzqt/uitranslations.h"
#include "pegbarguides.h"

#include "menubarcommandids.h"
#include "tapp.h"

#include "toonz/pegbarguide.h"
#include "toonz/sceneproperties.h"
#include "toonz/stage.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonzqt/colorfield.h"
#include "toonzqt/menubarcommand.h"

#include "tenv.h"
#include "tgl.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

TEnv::StringVar PegbarGuideFdg("PegbarGuideFdg", "fld_12_acme");
TEnv::IntVar PegbarGuideSide("PegbarGuideSide", PegbarGuide::Top);
TEnv::IntVar PegbarGuideRGB("PegbarGuideRGB", 0xFF8C00);  // orange

QString tr(const char *text) {
  return QCoreApplication::translate("PegbarGuides", text);
}

TPixel32 guideColor() {
  const int rgb = PegbarGuideRGB;
  return TPixel32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
}

// The field guide file to draw: the chosen one, else the first there is.
const CleanupTypes::FDG_INFO &currentFdg() {
  static std::string name;
  static CleanupTypes::FDG_INFO info;
  const std::string wanted = PegbarGuideFdg;
  if (wanted != name || info.m_name.empty()) {
    name = wanted;
    CleanupParameters params;
    if (!params.setFdgByName(wanted)) {
      std::vector<std::string> names;
      CleanupParameters::getFdgNames(names);
      if (!names.empty()) params.setFdgByName(names.front());
    }
    info = params.getFdgInfo();
  }
  return info;
}

//-----------------------------------------------------------------------------

// A slot with round ends, or a circle when it is as wide as high.
void drawHole(const PegbarGuide::Hole &hole, double mm) {
  const TPointD c = hole.center * mm;
  const double w = hole.width * mm, h = hole.height * mm;
  if (hole.round || std::abs(w - h) < 1e-9) {
    tglDrawCircle(c, 0.5 * std::max(w, h));
    return;
  }
  const bool horizontal = w > h;
  const double r        = 0.5 * (horizontal ? h : w);
  const double half     = 0.5 * (horizontal ? w : h) - r;
  const TPointD axis    = horizontal ? TPointD(1, 0) : TPointD(0, 1);
  const TPointD normal  = TPointD(-axis.y, axis.x);
  const int steps       = 12;
  const double pi       = 3.14159265358979323846;
  glBegin(GL_LINE_LOOP);
  for (int end = 0; end < 2; end++) {
    const double sign    = end == 0 ? 1.0 : -1.0;
    const TPointD center = c + axis * (sign * half);
    for (int i = 0; i <= steps; i++) {
      const double a  = pi * i / steps - 0.5 * pi;
      const TPointD p = center + axis * (sign * r * std::cos(a)) +
                        normal * (sign * r * std::sin(a));
      glVertex2d(p.x, p.y);
    }
  }
  glEnd();
}

//-----------------------------------------------------------------------------

class PegbarGuideSettings final : public QDialog {
public:
  PegbarGuideSettings() {
    setWindowTitle(tr("Pegbar Guide Settings"));

    QComboBox *fdg = new QComboBox(this);
    std::vector<std::string> names;
    CleanupParameters::getFdgNames(names);
    for (const std::string &name : names)
      fdg->addItem(QString::fromStdString(name));
    const int index =
        fdg->findText(QString::fromStdString(std::string(PegbarGuideFdg)));
    if (index >= 0) fdg->setCurrentIndex(index);

    QComboBox *side = new QComboBox(this);
    UiTranslations::watchCombo(side);
    side->addItem(tr("Top"), PegbarGuide::Top);
    side->addItem(tr("Bottom"), PegbarGuide::Bottom);
    side->addItem(tr("Left"), PegbarGuide::Left);
    side->addItem(tr("Right"), PegbarGuide::Right);
    side->setCurrentIndex(std::max(0, side->findData((int)PegbarGuideSide)));

    DVGui::ColorField *color =
        new DVGui::ColorField(this, false, guideColor(), 40, false);
    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    QFormLayout *layout = new QFormLayout(this);
    layout->addRow(tr("Field Guide:"), fdg);
    layout->addRow(tr("Pegs:"), side);
    layout->addRow(tr("Color:"), color);
    layout->addRow(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this,
            [this, fdg, side, color]() {
              if (fdg->count() > 0)
                PegbarGuideFdg = fdg->currentText().toStdString();
              PegbarGuideSide      = side->currentData().toInt();
              const TPixel32 value = color->getColor();
              PegbarGuideRGB       = (value.r << 16) | (value.g << 8) | value.b;
              TApp::instance()->getCurrentScene()->notifySceneChanged(false);
              accept();
            });
  }
};

class PegbarGuideSettingsCommand final : public MenuItemHandler {
public:
  PegbarGuideSettingsCommand() : MenuItemHandler(MI_PegbarGuideSettings) {}
  void execute() override {
    PegbarGuideSettings dialog;
    dialog.exec();
  }
} pegbarGuideSettingsCommand;

}  // namespace

//=============================================================================

void PegbarGuides::draw() {
  const CleanupTypes::FDG_INFO &info = currentFdg();
  const int stored                   = PegbarGuideSide;
  const PegbarGuide::Side side =
      stored >= PegbarGuide::Top && stored <= PegbarGuide::Right
          ? (PegbarGuide::Side)stored
          : PegbarGuide::Top;
  const std::vector<PegbarGuide::Hole> holes = PegbarGuide::holes(info, side);
  if (holes.empty()) return;

  const double mm = Stage::inch / 25.4;
  TSceneProperties *props =
      TApp::instance()->getCurrentScene()->getScene()->getProperties();
  int field = PegbarGuide::fieldFromName(info.m_name);
  if (field <= 0) field = std::max(props->getFieldGuideSize(), 4);
  const double ar    = props->getFieldGuideAspectRatio() > 0.0
                           ? props->getFieldGuideAspectRatio()
                           : 4.0 / 3.0;
  const double halfW = 0.5 * field * Stage::inch, halfH = halfW / ar;

  glPushAttrib(GL_CURRENT_BIT | GL_LINE_BIT | GL_ENABLE_BIT);
  tglColor(guideColor());
  glLineWidth(1.0);

  // the field boundary
  tglDrawRect(TRectD(-halfW, -halfH, halfW, halfH));

  // the registration holes
  for (const PegbarGuide::Hole &hole : holes) drawHole(hole, mm);

  // the paper edge behind the pegs, dashed
  const double edge = PegbarGuide::paperEdge(info) * mm;
  glLineStipple(1, 0xF0F0);
  glEnable(GL_LINE_STIPPLE);
  switch (side) {
  case PegbarGuide::Top:
    tglDrawSegment(TPointD(-halfW, edge), TPointD(halfW, edge));
    break;
  case PegbarGuide::Bottom:
    tglDrawSegment(TPointD(-halfW, -edge), TPointD(halfW, -edge));
    break;
  case PegbarGuide::Left:
    tglDrawSegment(TPointD(-edge, -halfH), TPointD(-edge, halfH));
    break;
  case PegbarGuide::Right:
    tglDrawSegment(TPointD(edge, -halfH), TPointD(edge, halfH));
    break;
  }
  glDisable(GL_LINE_STIPPLE);

  // the name of the field guide by the centre hole, about 12 pixels high
  const double pixelSize = std::sqrt(tglGetPixelSize2());
  const TPointD center   = holes[holes.size() / 2].center * mm;
  glPushMatrix();
  glTranslated(center.x + 8 * pixelSize, center.y + 8 * pixelSize, 0);
  glScaled(1.4 * pixelSize, 1.4 * pixelSize, 1);
  tglDrawText(TPointD(), info.m_name);
  glPopMatrix();

  glPopAttrib();
}
