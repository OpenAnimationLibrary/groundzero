

#include "toolbar.h"
#include "tapp.h"
#include "pane.h"
#include "floatingpanelcommand.h"
#include "tools/toolhandle.h"
#include "tools/tool.h"
#include "tools/toolcommandids.h"
#include "toonzqt/menubarcommand.h"
#include "menubarcommandids.h"

#include "toonz/txshleveltypes.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/preferences.h"
#include "toonz/tscenehandle.h"

// TnzBase includes
#include "tenv.h"

#include <QPainter>
#include <QAction>
#include <QToolButton>
#include <QVBoxLayout>
#include <QObject>

TEnv::IntVar ShowAllToolsToggle("ShowAllToolsToggle", 1);

namespace {
const struct {
  const char *toolName;
  bool collapsible;
} buttonLayout[] = {{T_Edit, false},
                    {T_Selection, false},
                    {"Separator_1", false},
                    {T_Brush, false},
                    {T_Geometric, false},
                    {T_Type, true},
                    {T_Fill, false},
                    {T_PaintBrush, false},
                    {"Separator_2", false},
                    {T_Eraser, false},
                    {T_Tape, false},
                    {T_Finger, false},
                    {T_GradientTransform, true},
                    {"Separator_3", false},
                    {T_StylePicker, false},
                    {T_RGBPicker, false},
                    {T_Ruler, false},
                    {T_EditAssistants, false},
                    {"Separator_4", false},
                    {T_ControlPointEditor, false},
                    {T_Pinch, true},
                    {T_Pump, true},
                    {T_Magnet, true},
                    {T_Bender, true},
                    {T_Iron, true},
                    {T_Cutter, true},
                    {T_LassoCutter, true},
                    {"Separator_5", true},
                    {T_Skeleton, true},
                    {T_Tracker, true},
                    {T_Hook, true},
                    {T_Plastic, true},
                    {"Separator_6", false},
                    {T_Zoom, false},
                    {T_Rotate, true},
                    {T_Hand, false},
                    {0, false}};
}  // namespace
//=============================================================================
// Toolbar
//-----------------------------------------------------------------------------

Toolbar::Toolbar(QWidget *parent, bool isVertical)
    : QToolBar(parent), m_isExpanded(ShowAllToolsToggle != 0) {
  // Fondamentale per lo style sheet
  setObjectName("toolBar");

  setMovable(false);
  if (isVertical)
    setOrientation(Qt::Vertical);
  else
    setOrientation(Qt::Horizontal);

  setIconSize(QSize(20, 20));
  setToolButtonStyle(Qt::ToolButtonIconOnly);

  m_expandButton = new QToolButton(this);
  m_expandButton->setObjectName("expandButton");
  m_expandButton->setCheckable(true);
  m_expandButton->setChecked(m_isExpanded);
  m_expandButton->setArrowType((isVertical) ? Qt::DownArrow : Qt::RightArrow);

  m_expandAction = addWidget(m_expandButton);

  connect(m_expandButton, SIGNAL(toggled(bool)), this,
          SLOT(setIsExpanded(bool)));

  updateToolbar();
}

//-----------------------------------------------------------------------------
/*! Layout the tool buttons according to the state of the expandButton
 */
void Toolbar::updateToolbar() {
  TApp *app                 = TApp::instance();
  TFrameHandle *frameHandle = app->getCurrentFrame();

  if (frameHandle->isPlaying()) return;

  TXshLevelHandle *currlevel = app->getCurrentLevel();
  TXshLevel *level           = currlevel ? currlevel->getLevel() : 0;
  int levelType              = level ? level->getType() : NO_XSHLEVEL;

  TColumnHandle *colHandle = app->getCurrentColumn();
  int colIndex             = colHandle->getColumnIndex();

  int rowIndex = frameHandle->getFrameIndex();

  if (Preferences::instance()->isAutoCreateEnabled() &&
      Preferences::instance()->isAnimationSheetEnabled()) {
    // If in an empty cell, find most recent level
    if (levelType == NO_XSHLEVEL) {
      TXsheetHandle *xshHandle = app->getCurrentXsheet();
      TXsheet *xsh             = xshHandle->getXsheet();

      if (colIndex >= 0 && !xsh->isColumnEmpty(colIndex)) {
        int r0, r1;
        xsh->getCellRange(colIndex, r0, r1);
        if (0 <= r0 && r0 <= r1) {
          // level type depends on previous occupied cell
          for (int r = std::min(r1, rowIndex); r >= r0; r--) {
            TXshCell cell = xsh->getCell(r, colIndex);
            if (cell.isEmpty()) continue;
            levelType = cell.m_level->getType();
            rowIndex  = r;
            break;
          }

          if (levelType == NO_XSHLEVEL) {
            TXshCell cell = xsh->getCell(r0, colIndex);
            levelType     = cell.m_level->getType();
            rowIndex      = r0;
          }
        }
      }
    }
  }

  m_toolbarLevel = levelType;

  TTool::ToolTargetType targetType = TTool::NoTarget;

  switch (m_toolbarLevel) {
  case OVL_XSHLEVEL:
    targetType = TTool::RasterImage;
    break;
  case TZP_XSHLEVEL:
    targetType = TTool::ToonzImage;
    break;
  case PLI_XSHLEVEL:
  default:
    targetType = TTool::VectorImage;
    break;
  case MESH_XSHLEVEL:
    targetType = TTool::MeshImage;
    break;
  }

  // Hide action for now
  for (int idx = 0; buttonLayout[idx].toolName; idx++) {
    if (QAction *action = m_toolbarList[buttonLayout[idx].toolName])
      removeAction(action);
  }

  removeAction(m_expandAction);

  int levelBasedDisplay = Preferences::instance()->getLevelBasedToolsDisplay();

  bool actionEnabled     = false;
  ToolHandle *toolHandle = TApp::instance()->getCurrentTool();

  for (int idx = 0; buttonLayout[idx].toolName; idx++) {
    TTool *tool = TTool::getTool(buttonLayout[idx].toolName, targetType);
    if (tool) tool->updateEnabled(rowIndex, colIndex);
    bool isSeparator = !strncmp(buttonLayout[idx].toolName, "Separator", 9);
    bool enable =
        !levelBasedDisplay ? true : (!tool ? actionEnabled : tool->isEnabled());

    // Plastic tool should always be available so you can create a mesh
    if (!enable && !strncmp(buttonLayout[idx].toolName, T_Plastic, 9) &&
        (m_toolbarLevel & LEVELCOLUMN_XSHLEVEL))
      enable = true;

    if (!m_isExpanded && buttonLayout[idx].collapsible) continue;

    // addSeparator() creates an action owned by this toolbar. Sharing that
    // pointer across rooms left a dangling action when a workflow retired it.
    QAction *&action = m_toolbarList[buttonLayout[idx].toolName];
    if (!action) {
      if (isSeparator)
        action = addSeparator();
      else
        action =
            CommandManager::instance()->getAction(buttonLayout[idx].toolName);
    }

    if (levelBasedDisplay != 2)
      action->setEnabled(enable);
    else if (!enable)
      continue;

    actionEnabled = addAction(action) || actionEnabled;

    if (isSeparator) actionEnabled = false;
  }

  addAction(m_expandAction);

  if (m_isExpanded) {
    m_expandButton->setArrowType(
        (orientation() == Qt::Vertical) ? Qt::UpArrow : Qt::LeftArrow);
    m_expandButton->setToolTip(tr("Collapse toolbar"));
  } else {
    m_expandButton->setArrowType(
        (orientation() == Qt::Vertical) ? Qt::DownArrow : Qt::RightArrow);
    m_expandButton->setToolTip(tr("Expand toolbar"));
  }

  update();
}

//----------------------------------------------------------------------------

void Toolbar::setIsExpanded(bool expand) {
  m_isExpanded       = expand;
  ShowAllToolsToggle = (expand) ? 1 : 0;
  updateToolbar();
}

//-----------------------------------------------------------------------------

Toolbar::~Toolbar() {}

//-----------------------------------------------------------------------------

bool Toolbar::addAction(QAction *act) {
  if (!act) return false;
  QToolBar::addAction(act);
  return true;
}

//-----------------------------------------------------------------------------

void Toolbar::showEvent(QShowEvent *e) {
  TColumnHandle *columnHandle = TApp::instance()->getCurrentColumn();
  connect(columnHandle, SIGNAL(columnIndexSwitched()), this,
          SLOT(updateToolbar()));

  TFrameHandle *frameHandle = TApp::instance()->getCurrentFrame();
  connect(frameHandle, SIGNAL(frameSwitched()), this, SLOT(updateToolbar()));
  connect(frameHandle, SIGNAL(frameTypeChanged()), this, SLOT(updateToolbar()));

  TXsheetHandle *xsheetHandle = TApp::instance()->getCurrentXsheet();
  connect(xsheetHandle, SIGNAL(xsheetChanged()), this, SLOT(updateToolbar()));

  connect(TApp::instance()->getCurrentTool(), SIGNAL(toolSwitched()),
          SLOT(onToolChanged()));

  TXshLevelHandle *levelHandle = TApp::instance()->getCurrentLevel();
  connect(levelHandle, SIGNAL(xshLevelSwitched(TXshLevel *)), this,
          SLOT(updateToolbar()));

  connect(TApp::instance()->getCurrentScene(),
          SIGNAL(preferenceChanged(const QString &)), this,
          SLOT(onPreferenceChanged(const QString &)));
}

//-----------------------------------------------------------------------------

void Toolbar::hideEvent(QHideEvent *e) {
  disconnect(TApp::instance()->getCurrentLevel(), 0, this, 0);
  disconnect(TApp::instance()->getCurrentTool(), SIGNAL(toolSwitched()), this,
             SLOT(onToolChanged()));

  disconnect(TApp::instance()->getCurrentColumn(),
             SIGNAL(columnIndexSwitched()), this, SLOT(updateToolbar()));

  disconnect(TApp::instance()->getCurrentFrame(), SIGNAL(frameSwitched()), this,
             SLOT(updateToolbar()));
  disconnect(TApp::instance()->getCurrentFrame(), SIGNAL(frameTypeChanged()),
             this, SLOT(updateToolbar()));

  disconnect(TApp::instance()->getCurrentXsheet(), SIGNAL(xsheetChanged()),
             this, SLOT(updateToolbar()));

  disconnect(TApp::instance()->getCurrentScene(),
             SIGNAL(preferenceChanged(const QString &)), this,
             SLOT(onPreferenceChanged(const QString &)));
}

//-----------------------------------------------------------------------------

void Toolbar::onToolChanged() {
  ToolHandle *toolHandle = TApp::instance()->getCurrentTool();
  std::string toolName   = toolHandle->getRequestedToolName().toStdString();
  QAction *act = CommandManager::instance()->getAction(toolName.c_str());
  if (!act || act->isChecked()) return;
  act->setChecked(true);
}

//-----------------------------------------------------------------------------

void Toolbar::onPreferenceChanged(const QString &prefName) {
  if (prefName == "ToolbarDisplay" || prefName.isEmpty()) updateToolbar();
}

//=============================================================================

OpenFloatingPanel openToolbarPane(MI_OpenToolbar, "ToolBar", "");
