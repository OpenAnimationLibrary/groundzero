#pragma once

#ifndef POSESPANE_H
#define POSESPANE_H

#include "toonz/tstageobjectid.h"
#include "toonz/poselibrary.h"

#include <QFrame>

#include <string>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;
class TXsheet;

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
  //! Appends the objects of the top-level xsheet that the pose changes in an
  //! offset layer, i.e. the ones having a channel which a layer can hold.
  static void getLayerObjects(TXsheet *xsh, const PoseLibrary::Pose &pose,
                              std::vector<TStageObjectId> &ids);
  //! Applies the weighted pose at the current frame to the top-level xsheet,
  //! to the base keys or, if \e offsetLayerName is not empty, to the offset
  //! layer with that name (added to the objects that don't have it). It opens
  //! no undo block and notifies nothing: the caller does. Returns false if no
  //! channel of the pose could be applied.
  static bool applyPoseKeys(const PoseLibrary::Pose &pose, double weight,
                            const std::wstring &offsetLayerName);

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
