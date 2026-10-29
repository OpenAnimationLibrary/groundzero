#pragma once

#ifndef AUTOSAVERECOVERY_H
#define AUTOSAVERECOVERY_H

#include <QDialog>
#include <QString>
#include <QStringList>

class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
class QLabel;

//=============================================================================
// AutosaveRecovery
//-----------------------------------------------------------------------------
// Optional autosave mode (Preferences > Saving > "Save to a Recovery Folder
// instead of Overwriting the Files"). Autosave then leaves the user's files
// untouched and writes a recovery copy of the current scene, including an
// untitled one, to <user profile>/recovery/<date>_<time>_<scene>/:
//   - the scene file, referring to the saved files of the unchanged levels
//     with absolute paths, so that the copy opens from the recovery folder;
//   - a copy of each level with unsaved changes (drawings and the palette
//     stored with the level), and of each modified palette level, referred to
//     from the scene copy with the $scenefolder alias;
//   - scenes.xml, linking the copy to the project of the scene;
//   - recovery.ini, describing the copy.
// The open scene, its levels and palettes keep their paths and dirty flags,
// and the undo history is not changed.
//
// A session marker in the recovery folder tells whether the previous session
// exited normally. If it did not, the recovery copies it wrote that are newer
// than the saved scenes are offered at the next launch. Old copies are
// removed automatically.
//-----------------------------------------------------------------------------

namespace AutosaveRecovery {

// Call once at startup, after the Toonz environment is initialized. Checks
// whether the previous session exited normally, starts the session marker if
// the recovery mode is enabled and removes the expired recovery copies.
// Nothing is created while the recovery mode has never been enabled.
void initSession();

// Call on normal exit: removes the session marker
void endSession();

// Writes a recovery copy if the scene changed or the previous copy was partial.
// Returns true only for a complete copy; missing resources remain pending.
bool saveRecoveryCopy();

// Shows the "Recover Unsaved Work" dialog if the previous session did not
// exit normally and left recovery copies newer than the saved scenes. The
// dialog also gives access to the report of a crash in the previous session.
// Returns false if the dialog was not shown.
bool showRecoveryDialog();

}  // namespace AutosaveRecovery

//=============================================================================
// RecoveryDialog
//-----------------------------------------------------------------------------

class RecoveryDialog final : public QDialog {
  Q_OBJECT

  QTreeWidget *m_list;
  QLabel *m_introLabel;
  bool m_hasCrashReport;
  QPushButton *m_openButton, *m_folderButton, *m_discardButton,
      *m_crashReportButton;
  QString m_sceneToOpen;

  void retranslateUi();
  void retranslateItem(QTreeWidgetItem *item);

public:
  RecoveryDialog(QWidget *parent, bool hasCrashReport);

  // sceneFile is the recovery copy to open, folders the recovery folders
  // removed by Discard; the first one is shown by Show Folder
  void addItem(const QString &sceneName, const QString &savedAt,
               const QString &originalScene, const QString &sceneFile,
               const QStringList &folders, const QStringList &uncopiedLevels);

  // The recovery copy chosen with Open, if any
  QString getSceneToOpen() const { return m_sceneToOpen; }

protected slots:
  void onCurrentItemChanged();
  void onOpen();
  void onShowFolder();
  void onDiscard();
  void onCrashReport();
};

#endif  // AUTOSAVERECOVERY_H
