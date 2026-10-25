#pragma once

#ifndef POSESPANE_H
#define POSESPANE_H

#include "toonz/tstageobjectid.h"

#include <QFrame>

#include <string>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;

//=============================================================================
// PoseLibraryNotifier
//-----------------------------------------------------------------------------
/*! Tells the panels showing the pose library of the scene (see PoseLibrary)
    that poses have been added or deleted.                                 */

class PoseLibraryNotifier final : public QObject {
  Q_OBJECT

  PoseLibraryNotifier() {}

public:
  static PoseLibraryNotifier *instance();
  void notifyChanged() { emit changed(); }

signals:
  void changed();
};

//=============================================================================
// PosesPane
//-----------------------------------------------------------------------------
/*! Stores rig poses as channel offsets from a base pose and applies them
    additively, with a weight, as keyframes at the current frame. They can
    also be applied to a named offset layer of the objects (see
    TStageObject::OffsetLayer), which keeps them apart from the base keys.  */

class PosesPane final : public QFrame {
  Q_OBJECT

  QLabel *m_baseLabel;
  QVBoxLayout *m_poseListLayout;
  QCheckBox *m_asLayerCheck;
  QLineEdit *m_layerNameEdit;

public:
  PosesPane(QWidget *parent = 0, Qt::WindowFlags flags = Qt::WindowFlags());

  //! Warns and returns false if poses cannot be used in the current xsheet
  static bool checkTopXsheet();
  //! Applies the pose to the base keys of the objects, or to the offset layer
  //! named \e offsetLayerName if it is not empty
  static void applyPose(int index, double weight,
                        const std::wstring &offsetLayerName = std::wstring());

private:
  void getTargetObjects(std::vector<TStageObjectId> &ids) const;
  void deletePose(int index);

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

protected slots:
  void refresh();
  void onSetBase();
  void onAddPose();
};

#endif  // POSESPANE_H
