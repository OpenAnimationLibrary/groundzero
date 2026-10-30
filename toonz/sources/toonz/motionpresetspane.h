#pragma once

#ifndef MOTIONPRESETSPANE_H
#define MOTIONPRESETSPANE_H

#include "toonz/motionpreset.h"
#include "tfilepath.h"

#include <QFrame>

#include <vector>

class QCheckBox;
class QFileSystemWatcher;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

//=============================================================================
// MotionPresetsPane
//-----------------------------------------------------------------------------
/*! A library of motion presets (see MotionPreset): saved animations that are
    applied to the selected columns and pegbars in one click, starting at the
    current frame and relative to where the objects are there.

    The presets are the \c .tnzmotion files of the "motionpresets" folder of
    the Library (the stock ones) and of its "user" subfolder (the ones that
    "Save Selection as Preset" writes). The list follows the changes of those
    folders. A preset is applied by a double-click or with the Apply button, to
    the base keys of the objects or, as an option, to a new offset layer that
    is named after the preset. One apply is one undo.                      */

class MotionPresetsPane final : public QFrame {
  Q_OBJECT

  QListWidget *m_list;
  QLabel *m_emptyLabel;
  QLabel *m_infoLabel;
  QCheckBox *m_asLayerCheck;
  QPushButton *m_applyButton;
  QPushButton *m_saveButton;
  QPushButton *m_deleteButton;

  QFileSystemWatcher *m_watcher;
  QTimer *m_refreshTimer;

  std::vector<MotionPreset> m_presets;  //!< Shown by the list, by index

public:
  MotionPresetsPane(QWidget *parent       = 0,
                    Qt::WindowFlags flags = Qt::WindowFlags());

  //! The folder of the stock presets
  static TFilePath getStockFolder();
  //! The folder where the presets of the user are saved
  static TFilePath getUserFolder();

  //! Applies the preset at the current frame to the selected columns and
  //! pegbars, or to the current object if none is selected. One undo.
  static void applyPreset(const MotionPreset &preset, bool asOffsetLayer);

  //! Saves the selected keyframes of the current object as a preset of the
  //! user, asking for its name. If no keyframe of the object is selected, the
  //! whole animation of the object is saved.
  static void saveSelectionAsPreset();

private:
  void addPresets(const TFilePath &folder, bool userPresets,
                  const QString &title);
  void updateWatchedFolders();
  void retranslateUi();
  const MotionPreset *getCurrentPreset(bool *userPreset = 0,
                                       QString *path    = 0) const;
  void applyItem(QListWidgetItem *item);

protected:
  void showEvent(QShowEvent *) override;

protected slots:
  void refresh();
  void updateButtons();
  void onFolderChanged();
  void onApply();
  void onSave();
  void onDelete();
};

#endif  // MOTIONPRESETSPANE_H
