#pragma once

#ifndef RIGLIBRARYPANE_H
#define RIGLIBRARYPANE_H

#include <QFrame>

class QFileSystemWatcher;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QLabel;
class QTimer;

//=============================================================================
// RigLibraryPane
//-----------------------------------------------------------------------------
/*! A library of cut-out characters, with two tabs:

    - "Rigs" shows the rig templates saved in the "templates" folder of the
      Library (see "Save Selection as Template") with their thumbnail. A
      template is inserted as a sub-xsheet at the current frame, like "Load as
      Sub-xsheet" does, by a double-click, with the Insert button, or by
      dragging it into the cells of the xsheet.
    - "Poses" shows the poses of the pose library of the scene (see PosesPane),
      which are applied at the current frame, to the base keys or to an offset
      layer.

    The lists follow the changes of the Library folder and of the scene.  */

class RigLibraryPane final : public QFrame {
  Q_OBJECT

  QListWidget *m_rigList;
  QListWidget *m_poseList;
  QLabel *m_rigEmptyLabel;
  QLabel *m_poseEmptyLabel;
  QPushButton *m_insertButton;
  QPushButton *m_applyButton;
  QPushButton *m_applyLayerButton;

  QFileSystemWatcher *m_watcher;
  QTimer *m_refreshTimer;

public:
  RigLibraryPane(QWidget *parent       = 0,
                 Qt::WindowFlags flags = Qt::WindowFlags());

private:
  void insertRig(QListWidgetItem *item);
  void applyPose(QListWidgetItem *item, bool asOffsetLayer);
  void updateWatchedFolders();

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

protected slots:
  void refresh();
  void refreshRigs();
  void refreshPoses();
  void updateButtons();
  void onFolderChanged();
  void onInsert();
  void onApply();
  void onApplyAsLayer();
};

#endif  // RIGLIBRARYPANE_H
