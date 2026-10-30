#include "commandpalette.h"

#include "columnselection.h"
#include "floatingpanelcommand.h"
#include "menubarcommandids.h"
#include "pane.h"
#include "tapp.h"

#include "tools/tool.h"
#include "tools/toolhandle.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshcolumn.h"
#include "toonzqt/menubarcommand.h"
#include "toonzqt/tselectionhandle.h"

#include <QApplication>
#include <QCoreApplication>
#include <QMenu>
#include <QPointer>

#include <vector>

namespace {
TPanel *panelOf(QWidget *widget) {
  for (QWidget *parent = widget; parent; parent = parent->parentWidget())
    if (auto *panel = qobject_cast<TPanel *>(parent)) return panel;
  return nullptr;
}

void revealDockedPanel(TPanel *panel) {
  DockLayout *layout = panel ? panel->parentLayout() : nullptr;
  if (!layout || panel->isFloating()) return;
  if (DockWidget *maximized = layout->getMaximized())
    if (maximized != panel) maximized->maximizeDock();
  Region *region = layout->find(panel);
  if (!region) return;
  if (region->hasTabGroup()) {
    const auto &tabs = region->tabItems();
    for (int i = 0; i < static_cast<int>(tabs.size()); ++i)
      if (tabs[i] == panel) {
        layout->setActiveTab(region, i);
        break;
      }
  }
}

class PaletteContext final : public QObject {
  QPointer<QWidget> m_widget;
  QPointer<TPanel> m_panel;
  QPointer<TMainWindow> m_room;

public:
  PaletteContext() : QObject(qApp) {
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget *old, QWidget *now) {
              remember(old);
              remember(now);
            });
    // A closed/replaced scene must never revive an old UI target. Selection
    // handles remain entirely owned by the application throughout a search.
    TApp *app = TApp::instance();
    connect(app->getCurrentScene(), &TSceneHandle::sceneSwitched, this,
            [this]() { clear(); });
    connect(app->getCurrentXsheet(), &TXsheetHandle::xsheetSwitched, this,
            [this]() { clear(); });
  }

  void clear() {
    m_widget.clear();
    m_panel.clear();
    m_room.clear();
  }

  void remember(QWidget *widget) {
    TPanel *panel = panelOf(widget);
    if (!panel || panel->getPanelType() == "CommandPalette") return;
    TMainWindow *room = TApp::instance()->getCurrentRoom();
    if (!room || !room->isAncestorOf(panel)) return;
    m_widget = widget;
    m_panel = panel;
    m_room = room;
  }

  TPanel *panel() const {
    if (!m_widget || !m_panel ||
        m_room != TApp::instance()->getCurrentRoom() || !m_room ||
        !m_room->isAncestorOf(m_panel))
      return nullptr;
    if (m_panel->isFloating()) {
      if (!m_panel->isVisible()) return nullptr;
    } else if (!m_panel->parentLayout() ||
               !m_panel->parentLayout()->find(m_panel)) {
      return nullptr;
    }
    return m_panel.data();
  }

  void restoreFocus() {
    if (!panel() || QApplication::activeModalWidget()) return;
    revealDockedPanel(m_panel);
    m_widget->window()->activateWindow();
    m_widget->setFocus(Qt::OtherFocusReason);
  }
};

PaletteContext &context() {
  static QPointer<PaletteContext> instance;
  if (!instance) instance = new PaletteContext();
  return *instance;
}

// These right-click commands have explicit native targets which can be
// resolved independently of a temporary popup. Other right-click actions can
// refer to a menu's captured item and must not be exposed as global commands.
bool hasOutlineTarget() {
  TApp *app = TApp::instance();
  TXsheet *xsheet = app->getCurrentXsheet()->getXsheet();
  if (!xsheet) return false;
  auto *selection = dynamic_cast<TColumnSelection *>(
      app->getCurrentSelection()->getSelection());
  const bool selectedColumns = selection && !selection->isEmpty();
  const int current = app->getCurrentColumn()->getColumnIndex();
  for (int i = 0; i < xsheet->getColumnCount(); ++i) {
    if (selectedColumns ? !selection->isColumnSelected(i) : current != i)
      continue;
    if (xsheet->isColumnEmpty(i)) continue;
    TXshColumn *column = xsheet->getColumn(i);
    if (column && !column->getSoundColumn() && !column->getSoundTextColumn() &&
        !column->getPaletteColumn())
      return true;
  }
  return false;
}

bool hasUniqueStyleTarget(QAction *action) {
  TTool *tool = TApp::instance()->getCurrentTool()->getTool();
  if (!tool || tool->getName() != "T_GradientTransform" || !tool->isEnabled())
    return false;
  // The tool already knows whether its picked region is still in the current
  // drawing and shares a style. Ask that native eligibility path; do not guess
  // a region from the mouse position or duplicate private tool state.
  QMenu menu;
  tool->addContextMenuItems(&menu);
  return menu.actions().contains(action);
}

bool contextAllows(const QString &id, QAction *action) {
  if (id == QLatin1String(MI_MakeUniqueStyle)) return hasUniqueStyleTarget(action);
  if (id == QLatin1String(MI_ToggleColumnOutline)) return hasOutlineTarget();
  if (id == QLatin1String(MI_MaximizePanel)) {
    TMainWindow *room = TApp::instance()->getCurrentRoom();
    TPanel *origin = context().panel();
    return room && (room->dockLayout()->getMaximized() ||
                    (origin && !origin->isFloating() && origin->isMaximizable()));
  }
  if (id == QLatin1String("MI_ZoomInAndFitPanel")) {
    TPanel *origin = context().panel();
    return origin && origin->isFloating();
  }
  return true;
}

struct Category {
  CommandType type;
  const char *label;
};

const Category categories[] = {
    {MenuFileCommandType, QT_TRANSLATE_NOOP("CommandPalette", "File")},
    {MenuEditCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Edit")},
    {MenuScanCleanupCommandType,
     QT_TRANSLATE_NOOP("CommandPalette", "Scan and Cleanup")},
    {MenuLevelCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Level")},
    {MenuXsheetCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Xsheet")},
    {MenuCellsCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Cells")},
    {MenuPlayCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Play")},
    {MenuRenderCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Render")},
    {MenuViewCommandType, QT_TRANSLATE_NOOP("CommandPalette", "View")},
    {MenuWindowsCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Windows")},
    {CustomPanelCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Custom Panels")},
    {RoomCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Rooms")},
    {MenuHelpCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Help")},
    {ToolCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Tools")},
    {ToolModifierCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Tool Options")},
    {BrushPresetCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Brush Presets")},
    {BrushSizeCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Brush Sizes")},
    {FillCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Fill")},
    {PlaybackCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Playback")},
    {RGBACommandType, QT_TRANSLATE_NOOP("CommandPalette", "Channels")},
    {ZoomCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Navigation")},
    {MiscCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Other")},
    {StopMotionCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Stop Motion")},
    {CellMarkCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Cell Marks")},
    {MenuCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Commands")},
    {VisualizationButtonCommandType, QT_TRANSLATE_NOOP("CommandPalette", "View")},
    {RightClickMenuCommandType, QT_TRANSLATE_NOOP("CommandPalette", "Context")}};

QVector<CommandPaletteEntry> nativeCommands() {
  QVector<CommandPaletteEntry> entries;
  CommandManager *manager = CommandManager::instance();
  for (const Category &category : categories) {
    std::vector<QAction *> actions;
    manager->getActions(category.type, actions);
    for (QAction *action : actions) {
      if (!action) continue;
      const QString id = QString::fromStdString(manager->getIdFromAction(action));
      if (id == QLatin1String(MI_OpenCommandPalette) ||
          id.startsWith(QLatin1String("A_ToolOption_AssistantType")) ||
          id == QLatin1String("T_ZoomView") ||
          id == QLatin1String("T_RotateView") ||
          id == QLatin1String("T_HandView"))
        continue;
      if (category.type == RightClickMenuCommandType &&
          id != QLatin1String(MI_MakeUniqueStyle) &&
          id != QLatin1String(MI_ToggleColumnOutline))
        continue;
      const bool available = contextAllows(id, action);
      // A menu-only command is absent when its native target cannot be found,
      // even when its globally registered QAction happens to be enabled.
      if (category.type == RightClickMenuCommandType && !available) continue;
      entries.push_back({id, QCoreApplication::translate("CommandPalette", category.label),
                         QPointer<QAction>(action), available});
    }
  }
  return entries;
}

void dismissPalette(CommandPalette *palette) {
  TPanel *panel = panelOf(palette);
  if (panel && panel->isFloating()) panel->dismissFloatingPanel();
}

bool invokeNative(CommandPalette *palette, const CommandPaletteEntry &entry) {
  QPointer<QAction> action = entry.action;
  if (!action || !action->isEnabled() || !action->isVisible() ||
      !contextAllows(entry.id, action))
    return false;
  dismissPalette(palette);
  if (entry.id == QLatin1String(MI_MaximizePanel)) {
    TMainWindow *room = TApp::instance()->getCurrentRoom();
    DockWidget *maximized = room ? room->dockLayout()->getMaximized() : nullptr;
    if (maximized) {
      maximized->maximizeDock();
      context().restoreFocus();
      return true;
    }
    context().restoreFocus();
    TPanel *origin = context().panel();
    if (!origin || origin->isFloating() || !origin->isMaximizable()) return false;
    origin->maximizeDock();
    return true;
  }
  context().restoreFocus();
  if (!action || !action->isEnabled() || !action->isVisible() ||
      !contextAllows(entry.id, action))
    return false;
  if (entry.id == QLatin1String("MI_ZoomInAndFitPanel")) {
    TPanel *origin = context().panel();
    if (!origin || !origin->isFloating()) return false;
    origin->zoomContentsAndFitGeometry(true);
    return true;
  }
  // This is menu-style activation, not a synthetic shortcut or direct handler
  // call. QAction owns exclusive/checkable behavior and native confirmations.
  CommandManager::instance()->setPendingShortcutAction(nullptr);
  action->trigger();
  return true;
}

class OpenCommandPalette final : public MenuItemHandler {
public:
  OpenCommandPalette() : MenuItemHandler(MI_OpenCommandPalette) {}
  void execute() override {
    context().remember(QApplication::focusWidget());
    TMainWindow *room = TApp::instance()->getCurrentRoom();
    if (!room) return;
    // Focus an existing instance, including an inactive dock tab, instead of
    // creating a second palette. A closed docked panel can be reopened floating.
    for (TPanel *panel : room->findChildren<TPanel *>()) {
      if (panel->getPanelType() != "CommandPalette") continue;
      if (auto *palette = panel->findChild<CommandPalette *>()) {
        if (!panel->isFloating() && panel->parentLayout() &&
            panel->parentLayout()->find(panel)) {
          revealDockedPanel(panel);
        } else if (!panel->isVisible()) {
          panel->reset();
          room->addDockWidget(panel);
          panel->setFloating(true);
          panel->show();
        }
        panel->raise();
        panel->activateWindow();
        palette->beginSearch();
        return;
      }
    }
    TPanel *panel = OpenFloatingPanel::getOrOpenFloatingPanel("CommandPalette");
    if (panel)
      if (auto *palette = panel->findChild<CommandPalette *>())
        palette->beginSearch();
  }
} openCommandPalette;
}  // namespace

CommandPalette *createNativeCommandPalette(QWidget *parent) {
  context().remember(QApplication::focusWidget());
  auto *palette = new CommandPalette(parent);
  palette->setSource(nativeCommands);
  palette->setInvoker([palette](const CommandPaletteEntry &entry) {
    return invokeNative(palette, entry);
  });
  QObject::connect(palette, &CommandPalette::cancelled, palette, [palette]() {
    dismissPalette(palette);
    context().restoreFocus();
  });
  return palette;
}
