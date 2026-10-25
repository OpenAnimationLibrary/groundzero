#pragma once

#ifndef SUBSTITUTIONSLIDERPANE_H
#define SUBSTITUTIONSLIDERPANE_H

#include "toonz/txshlevel.h"
#include "tfilepath.h"

#include <QFrame>

#include <vector>

class QLabel;
class QSlider;
class SubstituteDrawingUndo;

//=============================================================================
// SubstitutionSliderPane
//-----------------------------------------------------------------------------
/*! A compact slider over the drawings of the level exposed in the current
    cell. Dragging the slider swaps the drawing in the current cell, or in
    every cell of the current cell selection which holds the same level, live;
    the whole drag is a single undo.                                      */

class SubstitutionSliderPane final : public QFrame {
  Q_OBJECT

  QLabel *m_label;
  QSlider *m_slider;

  TXshLevelP m_level;
  std::vector<TFrameId> m_fids;

  // Cells swapped by the drag in progress; null when the slider is not
  // being dragged.
  SubstituteDrawingUndo *m_dragUndo;
  bool m_dragChanged;

public:
  SubstitutionSliderPane(QWidget *parent       = 0,
                         Qt::WindowFlags flags = Qt::WindowFlags());
  ~SubstitutionSliderPane();

private:
  void updateLabel(const TFrameId &currentFid);
  void endDrag();

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

protected slots:
  void refresh();
  void onSliderPressed();
  void onSliderReleased();
  void onValueChanged(int value);
};

#endif  // SUBSTITUTIONSLIDERPANE_H
