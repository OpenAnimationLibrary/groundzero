#pragma once

#ifndef MOTIONARCS_INCLUDED
#define MOTIONARCS_INCLUDED

#include "toonz/motionarc.h"

#include <vector>

class QMenu;
class TXsheet;
class TXshColumn;

//=============================================================================
// MotionArcs
//-----------------------------------------------------------------------------
// The Motion Arcs overlay of the viewer (doc/motion_arcs.md): the path of a
// point of a column across its frames, with a dot per frame, squares on the
// keys, the spacing shown by the colour or the size of the dots, and frame
// numbers. The columns shown and the point tracked in each are runtime state,
// not saved in the scene; the look is in the user's settings.

namespace MotionArcs {

enum Spacing {
  SpacingNone  = 0,
  SpacingColor = 1,
  SpacingSize  = 2,
  SpacingBoth  = 3
};

struct Settings {
  bool showCurrent = true;  // the current column's arc, besides the pinned
  int spacing      = SpacingColor;
  int labelEvery   = 4;    // frame numbers every N frames, 0 for none
  int maxFrames    = 240;  // the frames sampled around the current frame
  int rgb          = 0x30B0FF;
};

Settings settings();
void setSettings(const Settings &settings);

// Whether the column's arc is shown even when it is not the current column.
bool isPinned(TXshColumn *column);
void setPinned(TXshColumn *column, bool pinned);

// The point of the column the arc follows (the centre unless set).
MotionArc::Track track(TXshColumn *column);
void setTrack(TXshColumn *column, const MotionArc::Track &track);

// Forgets every pinned column and tracked point.
void clear();

// The camera the current frame is seen through.
MotionArc::Camera currentCamera(TXsheet *xsh, int frame);

// The samples of the arc of a column of the current xsheet at the current
// frame (empty when it does not move).
std::vector<MotionArc::Sample> columnSamples(int col);

// Draws the arcs, with viewMatrix from the camera-seen world to the viewer
// (as SceneViewer::drawSpline). Not for the 3D view.
void draw(const TAffine &viewMatrix);

// Adds the Motion Arcs submenu to the viewer's context menu; world is the
// clicked point in the viewer's world (SceneViewer::winToWorld), for "Track
// This Point".
void addContextMenu(QMenu *menu, const TPointD &world);

}  // namespace MotionArcs

#endif
