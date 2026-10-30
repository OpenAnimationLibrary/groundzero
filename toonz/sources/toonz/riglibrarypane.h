#pragma once

#ifndef RIGLIBRARYPANE_H
#define RIGLIBRARYPANE_H

#include <QFrame>

class QFileSystemWatcher;
class QListWidget;
class QListWidgetItem;
class QPoint;
class QPushButton;
class QLabel;
class QTimer;
class QTabWidget;

//=============================================================================
// RigLibraryPane
//-----------------------------------------------------------------------------
/*! The Library panel: the items stored outside the scenes (see
    librarycommand.h), shared by all of them, in three tabs:

    - "Rigs & Symbols" shows the templates saved in the "templates" folder of
      the Library (by "Save Selection as Template" and "Save Symbol to
      Library") with their thumbnail. A template is inserted as a sub-xsheet
      at the current frame, like "Load as Sub-xsheet" does, by a
      double-click, with the Insert button, or by dragging it into the cells
      of the xsheet; the insertion can be undone.
    - "Pose Sets" shows the pose sets saved in the "poses" folder of the
      Library, which are merged into the poses of the current scene.
    - "Scene Poses" shows the poses of the pose library of the scene (see
      PosesPane), which are applied at the current frame, to the base keys or
      to an offset layer.

    The items can be renamed and deleted. The lists follow the changes of the
    Library folder and of the scene. The panel keeps its "RigLibrary" id, so
    the rooms saved with the Rig Library panel keep it.  */

class RigLibraryPane final : public QFrame {
  Q_OBJECT

  QListWidget *m_rigList;
  QListWidget *m_poseSetList;
  QListWidget *m_poseList;
  QLabel *m_rigEmptyLabel;
  QLabel *m_poseSetEmptyLabel;
  QLabel *m_poseEmptyLabel;
  QPushButton *m_insertButton;
  QPushButton *m_renameRigButton;
  QPushButton *m_deleteRigButton;
  QPushButton *m_loadPoseSetButton;
  QPushButton *m_renamePoseSetButton;
  QPushButton *m_deletePoseSetButton;
  QPushButton *m_applyButton;
  QPushButton *m_applyLayerButton;
  QTabWidget *m_tabs;

  QFileSystemWatcher *m_watcher;
  QTimer *m_refreshTimer;

public:
  RigLibraryPane(QWidget *parent       = 0,
                 Qt::WindowFlags flags = Qt::WindowFlags());

private:
  void insertRig(QListWidgetItem *item);
  void loadPoseSet(QListWidgetItem *item);
  void applyPose(QListWidgetItem *item, bool asOffsetLayer);
  void renameItem(QListWidget *list);
  void deleteItem(QListWidget *list);
  void updateWatchedFolders();
  void retranslateUi();

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

protected slots:
  void refresh();
  void refreshLibrary();
  void refreshRigs();
  void refreshPoseSets();
  void refreshPoses();
  void updateButtons();
  void onFolderChanged();
  void onInsert();
  void onRenameRig();
  void onDeleteRig();
  void onLoadPoseSet();
  void onSavePoseSet();
  void onRenamePoseSet();
  void onDeletePoseSet();
  void onApply();
  void onApplyAsLayer();
  void onRigContextMenu(const QPoint &pos);
  void onPoseSetContextMenu(const QPoint &pos);
};

#endif  // RIGLIBRARYPANE_H
