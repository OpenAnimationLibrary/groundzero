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

private:
  bool checkTopXsheet() const;
  void getTargetObjects(std::vector<TStageObjectId> &ids) const;
  //! Applies the pose to the base keys of the objects, or to the offset layer
  //! named \e offsetLayerName if it is not empty
  void applyPose(int index, double weight,
                 const std::wstring &offsetLayerName = std::wstring());
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
