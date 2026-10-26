#pragma once

#ifndef ARTLAYERMANAGERPANE_H
#define ARTLAYERMANAGERPANE_H

#include <QFrame>

class QCheckBox;
class QLabel;

//=============================================================================
// ArtLayerManagerPane
//-----------------------------------------------------------------------------
/*! Shows, for the current Toonz Vector drawing, how many strokes each art
    layer has (see ArtLayer in tools/artlayer.h): Overlay, Line Art, Colour Art
    and Underlay, from the top of the stacking order to the bottom. A check box
    per layer shows or hides its strokes in the viewer, and a button creates the
    Colour Art from the Line Art.

    Nothing is saved: the layers are derived from the thickness and the
    stacking order of the strokes, and the visibility only changes the viewer.
 */

class ArtLayerManagerPane final : public QFrame {
  Q_OBJECT

  enum { RoleCount = 4 };

  struct Row {
    int m_role;
    QCheckBox *m_visible;
    QLabel *m_count;
  };

  QLabel *m_drawingLabel;
  Row m_rows[RoleCount];

public:
  ArtLayerManagerPane(QWidget *parent       = 0,
                      Qt::WindowFlags flags = Qt::WindowFlags());

private:
  void updateVisibility();
  void repaintViewer();

protected:
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

protected slots:
  void refresh();
  void onVisibilityToggled(int role, bool visible);
  void onCreateColourArt();
};

#endif  // ARTLAYERMANAGERPANE_H
