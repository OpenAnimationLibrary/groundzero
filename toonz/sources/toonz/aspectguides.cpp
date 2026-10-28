#include "aspectguides.h"

#include "menubarcommandids.h"
#include "sceneviewer.h"
#include "tapp.h"

#include "toonz/cameraguide.h"
#include "toonz/tscenehandle.h"
#include "toonzqt/colorfield.h"
#include "toonzqt/menubarcommand.h"

#include "tenv.h"
#include "tgl.h"

#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMenu>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

extern ToggleCommandHandler centerCutGuideToggle;

namespace {

TEnv::IntVar AspectGuide166Toggle("AspectGuide166Toggle", 0);
TEnv::IntVar AspectGuide178Toggle("AspectGuide178Toggle", 0);
TEnv::IntVar AspectGuide185Toggle("AspectGuide185Toggle", 0);
TEnv::IntVar AspectGuide239Toggle("AspectGuide239Toggle", 0);
TEnv::IntVar AspectGuide240Toggle("AspectGuide240Toggle", 0);
TEnv::IntVar AspectGuideCustomToggle("AspectGuideCustomToggle", 0);
TEnv::IntVar AspectGuideLetterboxToggle("AspectGuideLetterboxToggle", 0);
TEnv::StringVar AspectGuideCustomRatio("AspectGuideCustomRatio", "2.20");
TEnv::IntVar AspectGuideLetterboxRGB("AspectGuideLetterboxRGB", 0x000000);
TEnv::IntVar AspectGuideLetterboxAlpha("AspectGuideLetterboxAlpha", 180);

//-----------------------------------------------------------------------------

// A View toggle whose status is kept in the user's settings.
class StoredToggle final : public MenuItemHandler {
  TEnv::IntVar &m_stored;
  bool m_status = false;

public:
  StoredToggle(CommandId id, TEnv::IntVar &stored)
      : MenuItemHandler(id), m_stored(stored) {}

  bool status() const { return m_status; }

  void execute() override {
    m_status = !m_status;
    m_stored = m_status ? 1 : 0;
    TApp::instance()->getCurrentScene()->notifySceneChanged(false);
  }
};

StoredToggle guide166(MI_AspectGuide166, AspectGuide166Toggle);
StoredToggle guide178(MI_AspectGuide178, AspectGuide178Toggle);
StoredToggle guide185(MI_AspectGuide185, AspectGuide185Toggle);
StoredToggle guide239(MI_AspectGuide239, AspectGuide239Toggle);
StoredToggle guide240(MI_AspectGuide240, AspectGuide240Toggle);
StoredToggle guideCustom(MI_AspectGuideCustom, AspectGuideCustomToggle);
StoredToggle letterbox(MI_AspectGuideLetterbox, AspectGuideLetterboxToggle);

//-----------------------------------------------------------------------------

struct Guide {
  const char *commandId;
  double ratio;  // 0: the custom ratio
  TPixel32 color;
  const char *label;
};

// Widest first, so the letterbox of a wide guide is not hidden by a narrow one
const Guide guides[] = {
    {MI_AspectGuide240, 2.40, TPixel32(255, 96, 96), "2.40:1"},
    {MI_AspectGuide239, 2.39, TPixel32(255, 160, 64), "2.39:1"},
    {MI_AspectGuide185, 1.85, TPixel32(255, 224, 64), "1.85:1"},
    {MI_AspectGuide178, 16.0 / 9.0, TPixel32(128, 224, 96), "16:9"},
    {MI_AspectGuide166, 1.66, TPixel32(224, 128, 255), "1.66:1"},
    {MI_CenterCutGuide, 4.0 / 3.0, TPixel32(0, 204, 255), "4:3"},
    {MI_AspectGuideCustom, 0.0, TPixel32(255, 255, 255), nullptr},
};

bool isActive(const Guide &guide) {
  const std::string id = guide.commandId;
  if (id == MI_CenterCutGuide) return centerCutGuideToggle.getStatus();
  if (id == MI_AspectGuide166) return guide166.status();
  if (id == MI_AspectGuide178) return guide178.status();
  if (id == MI_AspectGuide185) return guide185.status();
  if (id == MI_AspectGuide239) return guide239.status();
  if (id == MI_AspectGuide240) return guide240.status();
  return guideCustom.status();
}

double customRatio() {
  return CameraGuide::parseRatio(std::string(AspectGuideCustomRatio));
}

TPixel32 letterboxColor() {
  const int rgb   = AspectGuideLetterboxRGB;
  const int alpha = AspectGuideLetterboxAlpha;
  return TPixel32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff,
                  std::min(std::max(alpha, 0), 255));
}

void setLetterboxColor(const TPixel32 &color) {
  AspectGuideLetterboxRGB   = (color.r << 16) | (color.g << 8) | color.b;
  AspectGuideLetterboxAlpha = color.m;
}

//-----------------------------------------------------------------------------

class AspectGuideSettings final : public QDialog {
public:
  AspectGuideSettings() {
    setWindowTitle(QCoreApplication::translate("AspectGuides",
                                               "Aspect Ratio Guide Settings"));
    QLineEdit *ratio = new QLineEdit(
        QString::fromStdString(std::string(AspectGuideCustomRatio)), this);
    ratio->setToolTip(QCoreApplication::translate(
        "AspectGuides", "A ratio such as 2.2, 2.2:1 or 21:9"));
    DVGui::ColorField *color =
        new DVGui::ColorField(this, true, letterboxColor(), 40, false);
    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);

    QFormLayout *layout = new QFormLayout(this);
    layout->addRow(
        QCoreApplication::translate("AspectGuides", "Custom Aspect Ratio:"),
        ratio);
    layout->addRow(
        QCoreApplication::translate("AspectGuides", "Letterbox Color:"), color);
    layout->addRow(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, ratio, color]() {
      const std::string text = ratio->text().trimmed().toStdString();
      if (CameraGuide::parseRatio(text) <= 0.0) {
        ratio->setFocus();
        ratio->selectAll();
        return;
      }
      AspectGuideCustomRatio = text;
      setLetterboxColor(color->getColor());
      TApp::instance()->getCurrentScene()->notifySceneChanged(false);
      accept();
    });
  }
};

class AspectGuideSettingsCommand final : public MenuItemHandler {
public:
  AspectGuideSettingsCommand() : MenuItemHandler(MI_AspectGuideSettings) {}
  void execute() override {
    AspectGuideSettings dialog;
    dialog.exec();
  }
} aspectGuideSettingsCommand;

}  // namespace

//=============================================================================

bool AspectGuides::storedStatus(const char *commandId) {
  const std::string id = commandId;
  if (id == MI_AspectGuide166) return AspectGuide166Toggle != 0;
  if (id == MI_AspectGuide178) return AspectGuide178Toggle != 0;
  if (id == MI_AspectGuide185) return AspectGuide185Toggle != 0;
  if (id == MI_AspectGuide239) return AspectGuide239Toggle != 0;
  if (id == MI_AspectGuide240) return AspectGuide240Toggle != 0;
  if (id == MI_AspectGuideCustom) return AspectGuideCustomToggle != 0;
  if (id == MI_AspectGuideLetterbox) return AspectGuideLetterboxToggle != 0;
  return false;
}

//-----------------------------------------------------------------------------

bool AspectGuides::anyActive() {
  for (const Guide &guide : guides)
    if (isActive(guide)) return true;
  return false;
}

//-----------------------------------------------------------------------------

void AspectGuides::draw(const TRectD &frame) {
  if (frame.isEmpty()) return;

  struct Active {
    TRectD rect;
    TPixel32 color;
    std::string label;
  };
  std::vector<Active> active;
  for (const Guide &guide : guides) {
    if (!isActive(guide)) continue;
    double ratio      = guide.ratio;
    std::string label = guide.label ? guide.label : "";
    if (ratio <= 0.0) {
      ratio = customRatio();
      char text[32];
      std::snprintf(text, sizeof(text), "%.2f:1", ratio);
      label = text;
    }
    const TRectD rect = CameraGuide::centeredRect(frame, ratio);
    if (!rect.isEmpty()) active.push_back({rect, guide.color, label});
  }
  if (active.empty()) return;

  glPushAttrib(GL_CURRENT_BIT | GL_LINE_BIT | GL_ENABLE_BIT |
               GL_COLOR_BUFFER_BIT);

  if (letterbox.status()) {
    const TPixel32 color = letterboxColor();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4ub(color.r, color.g, color.b, color.m);
    for (const Active &guide : active)
      for (const TRectD &bar : CameraGuide::letterboxBars(frame, guide.rect))
        tglFillRect(bar);
    glDisable(GL_BLEND);
  }

  const double pixelSize = std::sqrt(tglGetPixelSize2());
  glLineStipple(1, 0xF0F0);
  glEnable(GL_LINE_STIPPLE);
  for (const Active &guide : active) {
    tglColor(guide.color);
    tglDrawRect(guide.rect);
  }
  glDisable(GL_LINE_STIPPLE);

  // The ratio inside the top left corner of each guide, about 12 pixels high
  // at any zoom; guides of the same height (the narrower ones) are told apart
  // by their left edge.
  for (const Active &guide : active) {
    tglColor(guide.color);
    glPushMatrix();
    glTranslated(guide.rect.x0 + 4 * pixelSize, guide.rect.y1 - 16 * pixelSize,
                 0);
    glScaled(1.4 * pixelSize, 1.4 * pixelSize, 1);
    tglDrawText(TPointD(), guide.label);
    glPopMatrix();
  }

  glPopAttrib();
}

//-----------------------------------------------------------------------------

QMenu *AspectGuides::createMenu(QWidget *parent) {
  QMenu *menu = new QMenu(
      QCoreApplication::translate("AspectGuides", "Aspect Ratio Guides"),
      parent);
  CommandManager *cm = CommandManager::instance();
  const char *ids[]  = {MI_CenterCutGuide,   MI_AspectGuide166,
                        MI_AspectGuide178,   MI_AspectGuide185,
                        MI_AspectGuide239,   MI_AspectGuide240,
                        MI_AspectGuideCustom};
  for (const char *id : ids)
    if (QAction *action = cm->getAction(id)) menu->addAction(action);
  menu->addSeparator();
  if (QAction *action = cm->getAction(MI_AspectGuideLetterbox))
    menu->addAction(action);
  if (QAction *action = cm->getAction(MI_AspectGuideSettings))
    menu->addAction(action);
  return menu;
}
