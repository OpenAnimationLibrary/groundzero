#pragma once

#ifndef STORYBOARDPANE_H
#define STORYBOARDPANE_H

#include "toonz/txshlevel.h"
#include "tfilepath.h"
#include "tgeometry.h"

#include <QFrame>
#include <QColor>
#include <QPixmap>
#include <QPoint>
#include <QString>

#include <vector>

class QCheckBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTimer;

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
    and rename tags (undoably) and play the animatic from a panel start up to
    its end. Panels can be reordered by dragging a card onto another one (the
    cells and the tags of the panel move together, undoably) and carry notes,
    shown in the card tooltip. The thumbnail is a camera render of the first
    frame of the panel.                                                   */

class StoryboardPane final : public QFrame {
  Q_OBJECT

  struct Panel {
    int m_r0, m_r1;  // first and last frame
    QString m_label;
    QString m_notes;
    QColor m_color;
    TXshLevelP m_level;  // drawing shown as thumbnail, may be null
    TFrameId m_fid;
  };

  QLabel *m_infoLabel;
  QListWidget *m_panelList;
  DVGui::LineEdit *m_captionField, *m_notesField;
  QCheckBox *m_cameraCheck;
  QTimer *m_thumbTimer;  // debounces the regeneration of the camera thumbnails
  QPushButton *m_deleteButton, *m_playButton;

  std::vector<Panel> m_panels;
  int m_frameCount;
  int m_captionFrame;  // start of the panel shown in the caption field
  int m_notesFrame;    // start of the panel shown in the notes field
  QPoint m_dragStart;  // card drag and drop
  int m_dragIndex;

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
  QPixmap placeholderIcon(int index) const;
  TDimension cameraIconSize() const;
  void updateInfoLabel();
  void movePanel(int from, int to, bool after);
  bool dropTarget(const QPoint &pos, int &index, bool &after) const;

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;
  bool eventFilter(QObject *obj, QEvent *e) override;

protected slots:
  void refresh();
  void onFrameSwitched();
  void onIconGenerated();
  void onItemClicked(QListWidgetItem *item);
  void onItemDoubleClicked(QListWidgetItem *item);
  void onCaptionEdited();
  void onNotesEdited();
  void onCameraThumbsToggled();
  void scheduleThumbnails();
  void regenerateThumbnails();
  void onNewPanel();
  void onDeletePanel();
  void onPlayAnimatic();
};

#endif  // STORYBOARDPANE_H
