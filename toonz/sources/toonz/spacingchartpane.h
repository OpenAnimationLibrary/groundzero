#pragma once

#ifndef SPACINGCHARTPANE_H
#define SPACINGCHARTPANE_H

#include "toonz/motionarc.h"

#include <QFrame>
#include <QWidget>

#include <vector>

class QCheckBox;
class QLabel;
class QSpinBox;

//=============================================================================
// SpacingChart
//-----------------------------------------------------------------------------

//! The drawing of the chart: the ticks along the arc and the strip of the
//! distances.
class SpacingChart final : public QWidget {
  Q_OBJECT

  std::vector<MotionArc::Tick> m_ticks;
  int m_currentFrame;

public:
  SpacingChart(QWidget *parent = 0);

  void setTicks(const std::vector<MotionArc::Tick> &ticks, int currentFrame);

protected:
  void paintEvent(QPaintEvent *) override;
};

//=============================================================================
// SpacingChartPane
//-----------------------------------------------------------------------------

/*!
  The Spacing Chart panel (doc/spacing_chart.md): the classic spacing chart of
  the current column between two frames, from the path of its Motion Arc
  (the point tracked there, the centre by default). One tick per frame at its
  distance along the arc, keys as long ticks, and a strip of the distances
  frame by frame. Nothing is saved.
*/
class SpacingChartPane final : public QFrame {
  Q_OBJECT

  QLabel *m_columnLabel, *m_infoLabel;
  QCheckBox *m_keysAround;
  QSpinBox *m_from, *m_to;
  SpacingChart *m_chart;

public:
  SpacingChartPane(QWidget *parent       = 0,
                   Qt::WindowFlags flags = Qt::WindowFlags());

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

protected slots:
  void refresh();
};

#endif  // SPACINGCHARTPANE_H
