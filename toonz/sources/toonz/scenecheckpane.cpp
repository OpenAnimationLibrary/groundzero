#include "scenecheckpane.h"

// Tnz6 includes
#include "floatingpanelcommand.h"
#include "mainwindow.h"
#include "menubarcommandids.h"
#include "pane.h"
#include "tapp.h"

// TnzQt includes
#include "toonzqt/dvdialog.h"
#include "toonzqt/menubarcommand.h"

// TnzLib includes
#include "toonz/levelset.h"
#include "toonz/palettecontroller.h"
#include "toonz/preferences.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tpalettehandle.h"
#include "toonz/tscenehandle.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshcell.h"
#include "toonz/txshchildlevel.h"
#include "toonz/txshcolumn.h"
#include "toonz/txshlevel.h"
#include "toonz/txshlevelhandle.h"

// TnzCore includes
#include "tpalette.h"

// Qt includes
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <map>

namespace {

enum { IssueRole = Qt::UserRole + 1 };

QString fileSizeText(qint64 bytes) {
  if (bytes < 0) return SceneCheckPane::tr("not saved");
  if (bytes < 1024) return SceneCheckPane::tr("%1 bytes").arg(bytes);
  if (bytes < 1024 * 1024)
    return SceneCheckPane::tr("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
  return SceneCheckPane::tr("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

QIcon severityIcon(SceneChecker::Severity severity) {
  QStyle *style = QApplication::style();
  switch (severity) {
  case SceneChecker::Error:
    return style->standardIcon(QStyle::SP_MessageBoxCritical);
  case SceneChecker::Warning:
    return style->standardIcon(QStyle::SP_MessageBoxWarning);
  default:
    return style->standardIcon(QStyle::SP_MessageBoxInformation);
  }
}

// In the current xsheet, the first cell of a sub-xsheet column showing xsh
bool findSubXsheet(TXsheet *current, TXsheet *xsh, int &row, int &col) {
  for (int c = 0; c < current->getColumnCount(); ++c) {
    TXshColumn *column = current->getColumn(c);
    if (!column || column->getSoundColumn()) continue;
    TXshCellColumn *cells = column->getCellColumn();
    int r0, r1;
    if (!cells || !cells->getRange(r0, r1)) continue;
    for (int r = r0; r <= r1; ++r) {
      TXshChildLevel *cl = cells->getCell(r).getChildLevel();
      if (cl && cl->getXsheet() == xsh) {
        row = r, col = c;
        return true;
      }
    }
  }
  return false;
}

}  // namespace

//=============================================================================
// SceneCheckPane
//-----------------------------------------------------------------------------

SceneCheckPane::SceneCheckPane(QWidget *parent, Qt::WindowFlags flags)
    : QFrame(parent, flags) {
  setObjectName("SceneCheckPane");

  m_statistics = new QLabel(this);
  m_statistics->setWordWrap(true);
  m_statistics->setTextInteractionFlags(Qt::TextSelectableByMouse);

  QPushButton *checkButton = new QPushButton(tr("Check"), this);
  checkButton->setToolTip(tr("Check the current scene again"));
  m_unusedStyles = new QCheckBox(tr("Unused Styles"), this);
  m_unusedStyles->setToolTip(
      tr("Also look for palette styles no drawing uses. It loads every "
         "drawing, so it is slow on large scenes."));

  m_severityFilter = new QComboBox(this);
  m_severityFilter->addItem(tr("All"), SceneChecker::Info);
  m_severityFilter->addItem(tr("Warnings and Errors"), SceneChecker::Warning);
  m_severityFilter->addItem(tr("Errors"), SceneChecker::Error);
  m_severityFilter->setToolTip(tr("The issues to show"));
  m_textFilter = new QLineEdit(this);
  m_textFilter->setPlaceholderText(tr("Filter"));
  m_textFilter->setClearButtonEnabled(true);
  m_textFilter->setToolTip(tr("Show only the issues whose text contains this"));

  m_tree = new QTreeWidget(this);
  m_tree->setColumnCount(1);
  m_tree->header()->hide();
  m_tree->setUniformRowHeights(true);
  m_tree->setToolTip(
      tr("Double-click an issue to go to its frame, column, level or style"));

  QHBoxLayout *checkLayout = new QHBoxLayout();
  checkLayout->setContentsMargins(0, 0, 0, 0);
  checkLayout->setSpacing(5);
  checkLayout->addWidget(checkButton);
  checkLayout->addWidget(m_unusedStyles);
  checkLayout->addStretch(1);

  QHBoxLayout *filterLayout = new QHBoxLayout();
  filterLayout->setContentsMargins(0, 0, 0, 0);
  filterLayout->setSpacing(5);
  filterLayout->addWidget(m_severityFilter);
  filterLayout->addWidget(m_textFilter, 1);

  QVBoxLayout *mainLayout = new QVBoxLayout();
  mainLayout->setContentsMargins(5, 5, 5, 5);
  mainLayout->setSpacing(5);
  mainLayout->addWidget(m_statistics);
  mainLayout->addLayout(checkLayout);
  mainLayout->addLayout(filterLayout);
  mainLayout->addWidget(m_tree, 1);
  setLayout(mainLayout);

  connect(checkButton, SIGNAL(clicked()), this, SLOT(check()));
  connect(m_unusedStyles, SIGNAL(toggled(bool)), this, SLOT(check()));
  connect(m_severityFilter, SIGNAL(currentIndexChanged(int)), this,
          SLOT(updateTree()));
  connect(m_textFilter, SIGNAL(textChanged(const QString &)), this,
          SLOT(updateTree()));
  connect(m_tree, SIGNAL(itemActivated(QTreeWidgetItem *, int)), this,
          SLOT(onItemActivated(QTreeWidgetItem *, int)));
  connect(TApp::instance()->getCurrentScene(), SIGNAL(sceneSwitched()), this,
          SLOT(onSceneSwitched()));
}

//-----------------------------------------------------------------------------

void SceneCheckPane::showEvent(QShowEvent *) {
  // Later, so that a check run right after opening is not run twice
  QMetaObject::invokeMethod(this, "checkIfNeeded", Qt::QueuedConnection);
}

void SceneCheckPane::checkIfNeeded() {
  if (!m_checked && isVisible()) check();
}

//-----------------------------------------------------------------------------

void SceneCheckPane::onSceneSwitched() {
  if (isVisible()) {
    check();
    return;
  }
  // The issues point into the scene that was closed
  m_checked = false;
  m_issues.clear();
  updateTree();
}

//-----------------------------------------------------------------------------

void SceneCheckPane::check() {
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (!scene) return;
  SceneChecker::Options options;
  options.unusedStyles = m_unusedStyles->isChecked();
  QApplication::setOverrideCursor(Qt::WaitCursor);
  std::vector<SceneChecker::Issue> issues = SceneChecker::check(scene, options);
  QApplication::restoreOverrideCursor();
  setIssues(issues);
}

//-----------------------------------------------------------------------------

void SceneCheckPane::setIssues(const std::vector<SceneChecker::Issue> &issues) {
  m_issues  = issues;
  m_checked = true;

  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (scene) {
    SceneChecker::Statistics stats = SceneChecker::statistics(scene);
    m_statistics->setText(
        tr("Drawings: %1   Levels: %2   Columns: %3   Frames: %4   Styles: %5"
           "   Scene File: %6")
            .arg(stats.drawings)
            .arg(stats.levels)
            .arg(stats.columns)
            .arg(stats.frames)
            .arg(stats.styles)
            .arg(fileSizeText(stats.sceneFileSize)));
  } else
    m_statistics->clear();

  updateTree();
  emit titleChanged(title(m_issues));
}

//-----------------------------------------------------------------------------

QString SceneCheckPane::title(const std::vector<SceneChecker::Issue> &issues) {
  int counts[3] = {0, 0, 0};
  for (const SceneChecker::Issue &issue : issues) ++counts[issue.severity];
  if (issues.empty()) return tr("Scene Check: no issues");
  QStringList parts;
  if (counts[SceneChecker::Error])
    parts << tr("%n error(s)", "", counts[SceneChecker::Error]);
  if (counts[SceneChecker::Warning])
    parts << tr("%n warning(s)", "", counts[SceneChecker::Warning]);
  if (counts[SceneChecker::Info])
    parts << tr("%1 info").arg(counts[SceneChecker::Info]);
  return tr("Scene Check: %1").arg(parts.join(", "));
}

//-----------------------------------------------------------------------------

void SceneCheckPane::updateTree() {
  m_tree->clear();
  int minSeverity = m_severityFilter->currentData().toInt();
  QString text    = m_textFilter->text().trimmed();

  std::map<int, QTreeWidgetItem *> groups;
  std::map<int, int> counts;
  for (int i = 0; i < (int)m_issues.size(); ++i) {
    const SceneChecker::Issue &issue = m_issues[i];
    if (issue.severity < minSeverity) continue;
    if (!text.isEmpty() && !issue.text.contains(text, Qt::CaseInsensitive) &&
        !SceneChecker::kindTitle(issue.kind)
             .contains(text, Qt::CaseInsensitive))
      continue;
    QTreeWidgetItem *&group = groups[issue.kind];
    if (!group) {
      group = new QTreeWidgetItem();
      group->setIcon(0, severityIcon(issue.severity));
      group->setData(0, IssueRole, -1);
    }
    QTreeWidgetItem *item = new QTreeWidgetItem(group);
    item->setText(0, issue.text);
    item->setToolTip(0, issue.text);
    item->setIcon(0, severityIcon(issue.severity));
    item->setData(0, IssueRole, i);
    ++counts[issue.kind];
  }
  // The groups in the order of the kinds
  for (auto &group : groups) {
    group.second->setText(
        0, tr("%1 (%2)")
               .arg(SceneChecker::kindTitle(SceneChecker::Kind(group.first)))
               .arg(counts[group.first]));
    m_tree->addTopLevelItem(group.second);
    group.second->setExpanded(true);
  }
  if (groups.empty()) {
    QTreeWidgetItem *item = new QTreeWidgetItem();
    item->setText(0, m_issues.empty() ? tr("No issues found.")
                                      : tr("No issues match the filter."));
    item->setData(0, IssueRole, -1);
    item->setFlags(Qt::ItemIsEnabled);
    m_tree->addTopLevelItem(item);
  }
}

//-----------------------------------------------------------------------------

void SceneCheckPane::onItemActivated(QTreeWidgetItem *item, int) {
  if (!item) return;
  int index = item->data(0, IssueRole).toInt();
  if (index < 0 || index >= (int)m_issues.size()) return;
  const SceneChecker::Issue &issue = m_issues[index];

  TApp *app         = TApp::instance();
  ToonzScene *scene = app->getCurrentScene()->getScene();
  if (!scene) return;
  TXsheet *current = app->getCurrentXsheet()->getXsheet();

  // The cell: in the current xsheet, or the sub-xsheet's column showing it
  if (issue.xsheet && issue.column >= 0) {
    int row = issue.row, col = issue.column;
    if (issue.xsheet == current ||
        findSubXsheet(current, issue.xsheet, row, col)) {
      app->getCurrentColumn()->setColumnIndex(col);
      if (row >= 0) app->getCurrentFrame()->setFrame(row);
    }
  }

  // The level, and its style
  if (!issue.levelName.empty()) {
    TXshLevel *level = scene->getLevelSet()->getLevel(issue.levelName);
    if (level) {
      app->getCurrentLevel()->setLevel(level);
      if (issue.styleId >= 0) {
        TPaletteHandle *ph =
            app->getPaletteController()->getCurrentLevelPalette();
        if (ph->getPalette() && ph->getPalette()->getStyle(issue.styleId))
          ph->setStyleIndex(issue.styleId);
      }
    }
  }
}

//-----------------------------------------------------------------------------

SceneCheckPane *SceneCheckPane::open() {
  // A Scene Check panel already shown in the room, or a floating one
  TMainWindow *room = TApp::instance()->getCurrentRoom();
  if (room)
    for (SceneCheckPane *pane : room->findChildren<SceneCheckPane *>())
      if (pane->isVisible()) return pane;
  TPanel *panel = OpenFloatingPanel::getOrOpenFloatingPanel("SceneCheck");
  return panel ? panel->findChild<SceneCheckPane *>() : nullptr;
}

//=============================================================================

bool checkSceneBeforeRender() {
  if (!Preferences::instance()->getBoolValue(checkSceneBeforeRendering))
    return true;
  ToonzScene *scene = TApp::instance()->getCurrentScene()->getScene();
  if (!scene) return true;
  std::vector<SceneChecker::Issue> issues = SceneChecker::check(scene);
  int errors                              = 0;
  for (const SceneChecker::Issue &issue : issues)
    if (issue.severity == SceneChecker::Error) ++errors;
  if (errors == 0) return true;

  if (SceneCheckPane *pane = SceneCheckPane::open()) pane->setIssues(issues);
  int ret = DVGui::MsgBox(
      QObject::tr("The scene check found %n error(s): see the Scene Check "
                  "panel.\nRender anyway?",
                  "", errors),
      QObject::tr("Continue"), QObject::tr("Cancel"), 1);
  return ret == 1;
}

//=============================================================================
// Render > Check Scene
//-----------------------------------------------------------------------------

class CheckSceneCommand final : public MenuItemHandler {
public:
  CheckSceneCommand() : MenuItemHandler(MI_CheckScene) {}
  void execute() override {
    if (SceneCheckPane *pane = SceneCheckPane::open()) pane->check();
  }
} checkSceneCommand;
