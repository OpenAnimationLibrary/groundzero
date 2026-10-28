#pragma once

#ifndef ASPECTGUIDES_INCLUDED
#define ASPECTGUIDES_INCLUDED

#include "tgeometry.h"

class QMenu;
class QWidget;

//=============================================================================
// AspectGuides
//-----------------------------------------------------------------------------
// The aspect ratio guides of the viewers: 4:3 (the Center-Cut Guide), 1.66:1,
// 16:9, 1.85:1, 2.39:1, 2.40:1 and a custom ratio, each a View toggle, with an
// optional letterbox that shades the frame outside each guide.

namespace AspectGuides {

// Whether a toggle stored as on in the user's settings; for the start status
// of the View commands.
bool storedStatus(const char *commandId);

bool anyActive();

// Draws the active guides centred in frame, given in the current GL
// coordinates (the camera rect of the scene viewer, the image of a flipbook).
void draw(const TRectD &frame);

// The "Aspect Ratio Guides" submenu: every guide, the letterbox toggle and
// the settings command.
QMenu *createMenu(QWidget *parent);

}  // namespace AspectGuides

#endif
