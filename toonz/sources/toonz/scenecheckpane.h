#pragma once

#ifndef SCENECHECKPANE_H
#define SCENECHECKPANE_H

#include "toonz/scenechecker.h"

#include <QFrame>

#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

//=============================================================================
// SceneCheckPane
//-----------------------------------------------------------------------------
/*! The Scene Check panel (doc/scene_checker.md): the statistics of the current
    scene, and the issues SceneChecker finds in it, grouped by kind, with a
    filter. A double-click on an issue makes its frame, column, level and
    style current.

    The check runs when the panel opens, when the scene is switched, and with
    the Check button (or Render > Check Scene). It reads the scene only.
 */

class SceneCheckPane final : public QFrame {
  Q_OBJECT

  QLabel *m_statistics;
  QCheckBox *m_unusedStyles;
  QComboBox *m_severityFilter;
  QLineEdit *m_textFilter;
  QTreeWidget *m_tree;

  std::vector<SceneChecker::Issue> m_issues;
  bool m_checked = false;

public:
  SceneCheckPane(QWidget *parent       = 0,
                 Qt::WindowFlags flags = Qt::WindowFlags());

  //! Shows these issues (already found) and the statistics
  void setIssues(const std::vector<SceneChecker::Issue> &issues);

  //! The title with the counts: "Scene Check: 2 errors, 1 warning, 3 info"
  static QString title(const std::vector<SceneChecker::Issue> &issues);

  //! Opens the panel, floating if it is not in the room, and returns it
  static SceneCheckPane *open();

protected:
  void showEvent(QShowEvent *) override;

signals:
  void titleChanged(const QString &title);

public slots:
  void check();

protected slots:
  void checkIfNeeded();
  void onSceneSwitched();
  void updateTree();
  void onItemActivated(QTreeWidgetItem *item, int column);
};

//! The "Check Scene Before Rendering" step: when the preference is on and the
//! scene has errors, shows them in the panel and asks to continue. Returns
//! false to cancel the render.
bool checkSceneBeforeRender();

#endif  // SCENECHECKPANE_H
