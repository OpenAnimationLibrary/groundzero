#pragma once

#ifndef STORYBOARDPANE_H
#define STORYBOARDPANE_H

#include "toonz/txshlevel.h"
#include "tfilepath.h"

#include <QFrame>
#include <QColor>
#include <QPixmap>
#include <QString>

#include <vector>

class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace DVGui {
class LineEdit;
}

//=============================================================================
// StoryboardPane
//-----------------------------------------------------------------------------
/*! A light storyboard view of the current xsheet. Each navigation tag of the
    xsheet starts a panel which lasts until the next tag (the last one until
    the end of the scene); the tag label is the panel caption. Clicking a
    panel moves the current frame to its start; the panel can add, delete
    and rename tags (undoably) and play the animatic from a panel start.  */

class StoryboardPane final : public QFrame {
  Q_OBJECT

  struct Panel {
    int m_r0, m_r1;  // first and last frame
    QString m_label;
    QColor m_color;
    TXshLevelP m_level;  // drawing shown as thumbnail, may be null
    TFrameId m_fid;
  };

  QLabel *m_infoLabel;
  QListWidget *m_panelList;
  DVGui::LineEdit *m_captionField;
  QPushButton *m_deleteButton, *m_playButton;

  std::vector<Panel> m_panels;
  int m_frameCount;
  int m_captionFrame;  // start of the panel shown in the caption field

public:
  StoryboardPane(QWidget *parent       = 0,
                 Qt::WindowFlags flags = Qt::WindowFlags());

private:
  void buildPanels(std::vector<Panel> &panels, int &frameCount) const;
  void rebuildList();
  void updateCurrentPanel();
  int currentPanelIndex() const;
  QString panelCaption(int index) const;
  QPixmap panelIcon(int index, bool onDemand) const;

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

protected slots:
  void refresh();
  void onFrameSwitched();
  void onIconGenerated();
  void onItemClicked(QListWidgetItem *item);
  void onItemDoubleClicked(QListWidgetItem *item);
  void onCaptionEdited();
  void onNewPanel();
  void onDeletePanel();
  void onPlayAnimatic();
};

#endif  // STORYBOARDPANE_H
