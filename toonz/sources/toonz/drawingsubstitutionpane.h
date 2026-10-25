#pragma once

#ifndef DRAWINGSUBSTITUTIONPANE_H
#define DRAWINGSUBSTITUTIONPANE_H

#include "toonz/txshlevel.h"
#include "tfilepath.h"

#include <QFrame>

#include <vector>

class QLabel;
class QListWidget;
class QListWidgetItem;

//=============================================================================
// DrawingSubstitutionPane
//-----------------------------------------------------------------------------
/*! Shows the drawings of the level exposed in the current cell. Clicking a
    drawing exposes it in the current cell, or in every cell of the current
    cell selection which holds the same level, as a single undo.         */

class DrawingSubstitutionPane final : public QFrame {
  Q_OBJECT

  QLabel *m_levelLabel;
  QListWidget *m_drawingList;

  TXshLevelP m_level;
  std::vector<TFrameId> m_fids;

public:
  DrawingSubstitutionPane(QWidget *parent       = 0,
                          Qt::WindowFlags flags = Qt::WindowFlags());

private:
  void rebuildList();
  void substitute(const TFrameId &fid);

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

protected slots:
  void refresh();
  void onIconGenerated();
  void onItemClicked(QListWidgetItem *item);
  void onPrevious();
  void onNext();
};

#endif  // DRAWINGSUBSTITUTIONPANE_H
