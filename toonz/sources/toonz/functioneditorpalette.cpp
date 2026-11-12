#include "tapp.h"

#include "floatingpanelcommand.h"
#include "pane.h"
#include "tsimplecolorstyles.h"
#include "toonz/levelset.h"
#include "toonz/palettecmd.h"
#include "toonz/tframehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/functionkeytrack.h"
#include "toonzqt/functionviewer.h"
#include "toonzqt/keyframenavigator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

FunctionViewer *openEditor() {
  auto *panel = OpenFloatingPanel::getOrOpenFloatingPanel("FunctionEditor");
  return panel ? qobject_cast<FunctionViewer *>(panel->widget()) : nullptr;
}

QString sourceId(const void *owner, qlonglong id) {
  return QString::number(quintptr(owner), 16) + ':' + QString::number(id);
}

bool validFrame(double frame) {
  return std::isfinite(frame) && frame >= 0 &&
         frame < (std::numeric_limits<int>::max)();
}

TPalette *levelPalette(TXshLevel *level) {
  if (!level) return nullptr;
  if (auto *simple = level->getSimpleLevel()) {
    if (simple->getType() == PLI_XSHLEVEL || simple->getType() == TZP_XSHLEVEL)
      return simple->getPalette();
  }
  return nullptr;
}

bool hasStyle(TPalette *palette, int styleId) {
  if (styleId <= 0 || styleId >= palette->getStyleCount() ||
      !palette->getStylePage(styleId))
    return false;
  auto *style = palette->getStyle(styleId);
  return style && typeid(*style) == typeid(TSolidColorStyle);
}

class PaletteKeyTrack final : public FunctionKeyTrack {
  TXshLevelP m_level;
  ToonzScene *m_scene;
  int m_styleId;
  TPalette *palette() const {
    if (TApp::instance()->getCurrentScene()->getScene() != m_scene ||
        m_scene->getLevelSet()->getLevel(m_level->getName()) !=
            m_level.getPointer())
      return nullptr;
    auto *owner = levelPalette(m_level.getPointer());
    return owner && hasStyle(owner, m_styleId) ? owner : nullptr;
  }

  static Value components(const TColorStyle &style) {
    const auto color = style.getMainColor();
    return {double(color.r), double(color.g), double(color.b), double(color.m)};
  }

public:
  PaletteKeyTrack(TXshLevel *level, ToonzScene *scene, int styleId)
      : m_level(level), m_scene(scene), m_styleId(styleId) {}

  Snapshot read() const override {
    Snapshot result;
    auto *owner      = palette();
    result.available = owner != nullptr;
    if (!owner) return result;
    const auto keys = owner->getStyleAnimation(m_styleId);
    for (const auto &[frame, key] : keys) {
      result.keys.emplace(frame, components(*key.style));
      result.interpolation.emplace(frame, key.interpolation);
    }
    // At keyed frames the visible style changes while scrubbing. The default
    // matters only for unkeyed tracks.
    if (keys.empty()) result.defaults = components(*owner->getStyle(m_styleId));
    return result;
  }

  std::optional<double> valueAt(int component, double frame) const override {
    auto *owner = palette();
    if (!owner) return {};
    return components(*owner->sampleStyle(m_styleId, frame))[component];
  }

  void addInterpolationActions(QMenu &menu, double frame) const override {
    if (!canEdit() || !validFrame(frame)) return;
    PaletteKeyframeNavigator::addInterpolationActions(
        menu, palette(), m_styleId, int(std::floor(frame)), notifier());
  }

  QColor swatch() const override {
    auto *owner = palette();
    if (!owner) return {};
    auto *frame      = TApp::instance()->getCurrentFrame();
    const auto style = owner->sampleStyle(m_styleId, frame->getFrame());
    const auto color = style->getMainColor();
    return QColor(color.r, color.g, color.b, color.m);
  }

  bool canEdit() const override {
    auto *owner = palette();
    if (!owner || owner->isLocked() ||
        TApp::instance()->getCurrentFrame()->isPlaying())
      return false;
    auto *levels = m_scene->getLevelSet();
    bool found   = false;
    for (int i = 0; i < levels->getLevelCount(); ++i) {
      auto *level = levels->getLevel(i);
      if (levelPalette(level) != owner) continue;
      if (auto *simple = level->getSimpleLevel())
        if (simple->isReadOnly()) return false;
      found = true;
    }
    return found;
  }

  std::function<void(TPalette *)> notifier() const {
    return [scene = m_scene](TPalette *palette) {
      auto *app = TApp::instance();
      if (app->getCurrentScene()->getScene() == scene)
        app->notifyPaletteKeyframesChanged(palette);
    };
  }

  bool edit(int component, const std::vector<Edit> &edits) override {
    if (!canEdit() || edits.empty()) return false;
    auto *owner = palette();
    if (!owner) return false;
    const auto before      = owner->getStyleAnimation(m_styleId);
    auto after             = before;
    const auto nativeFrame = [](double frame) {
      return int(std::round(frame));
    };

    // Vacate the whole selection first, so adjacent keys can move together.
    for (const auto &edit : edits) {
      if ((!edit.from && !edit.to) || (edit.from && !validFrame(*edit.from)) ||
          (edit.to && !validFrame(*edit.to)) ||
          (edit.value && !std::isfinite(*edit.value)))
        return false;
      if (edit.from && after.erase(nativeFrame(*edit.from)) != 1) return false;
    }
    for (const auto &edit : edits) {
      if (!edit.to) continue;
      const int frame = nativeFrame(*edit.to);
      if (after.count(frame)) return false;
      auto key = edit.from ? before.at(nativeFrame(*edit.from))
                           : TPalette::StyleKey{
                                 owner->sampleStyle(m_styleId, frame), {}};
      if (edit.value) {
        const auto existing = key.style->getMainColor();
        TPixel32 color      = existing;
        const std::array channels{&color.r, &color.g, &color.b, &color.m};
        *channels[component] =
            int(std::round(std::clamp(*edit.value, 0.0, 255.0)));
        if (color != existing) {
          key.style = key.style->clone();
          key.style->setMainColor(color);
        }
      }
      after.emplace(frame, std::move(key));
    }
    return PaletteCmd::editStyleAnimation(
        owner, m_styleId, after, QObject::tr("Edit Palette Keys"), notifier());
  }
};

}  // namespace

void TApp::showPaletteStyleInFunctionEditor(TPalette *palette, int styleId) {
  auto *app   = TApp::instance();
  auto *scene = app->getCurrentScene()->getScene();
  auto *level = app->getCurrentLevel()->getLevel();
  if (!scene || !palette || levelPalette(level) != palette ||
      !hasStyle(palette, styleId))
    return;
  if (auto *editor = openEditor()) {
    const QString label =
        QObject::tr("%1 · %2 [%3]")
            .arg(QString::fromStdWString(level->getName()))
            .arg(QString::fromStdWString(palette->getStyle(styleId)->getName()))
            .arg(styleId);
    editor->showKeyTrack(
        "palette:" + sourceId(level, styleId), QObject::tr("Palettes"), label,
        {QObject::tr("Red"), QObject::tr("Green"), QObject::tr("Blue"),
         QObject::tr("Alpha")},
        std::make_shared<PaletteKeyTrack>(level, scene, styleId));
  }
}
