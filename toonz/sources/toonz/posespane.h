#pragma once

#ifndef POSESPANE_H
#define POSESPANE_H

#include "toonz/tstageobjectid.h"

#include <QFrame>

#include <vector>

class QLabel;
class QVBoxLayout;

//=============================================================================
// PosesPane
//-----------------------------------------------------------------------------
/*! Stores rig poses as channel offsets from a base pose and applies them
    additively, with a weight, as keyframes at the current frame.        */

class PosesPane final : public QFrame {
  Q_OBJECT

  QLabel *m_baseLabel;
  QVBoxLayout *m_poseListLayout;

public:
  PosesPane(QWidget *parent = 0, Qt::WindowFlags flags = Qt::WindowFlags());

private:
  bool checkTopXsheet() const;
  void getTargetObjects(std::vector<TStageObjectId> &ids) const;
  void applyPose(int index, double weight);
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
