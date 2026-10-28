#pragma once

#ifndef ART_LAYER_INCLUDED
#define ART_LAYER_INCLUDED

// Line Art / Colour Art editing for Toonz Vector levels.
//
// The role of a stroke is not stored anywhere: it is derived from the stroke
// thickness. A stroke whose control points all have thickness 0 is Colour Art
// (a fill boundary that the viewer shows but the renderer skips, exactly like
// the existing "Size 0" strokes); any other stroke is Line Art. Files are
// therefore never changed by this feature.
//
// Underlay and Overlay are Line Art strokes classified by their place in the
// stacking order of the drawing: those below the first Colour Art stroke are
// Underlay, those above the last one are Overlay. They exist only if some Line
// Art lies between the Colour Art strokes; otherwise every stroke is Line Art
// or Colour Art, as before. Create Colour Art adds its strokes below the Line
// Art, and the Lasso Cutter adds its closing strokes above it: neither turns
// the existing Line Art into Overlay or Underlay.
//
// The inline functions below are pure and independent of the tool framework,
// so that they can be checked by tnztools/tests/artlayer_test.cpp.

#include "tcommon.h"
#include "tstroke.h"

#include <string>

#undef DVAPI
#undef DVVAR
#ifdef TNZTOOLS_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//====================================================

//  Forward declarations

class TStroke;
class TEnumProperty;
class TPropertyGroup;
class TVectorImage;

//====================================================

namespace ArtLayer {

//! The four art layers. UNDERLAY and OVERLAY are Line Art strokes below the
//! first and above the last Colour Art stroke of the drawing.
enum Role { LINE_ART, COLOUR_ART, UNDERLAY, OVERLAY };
const int RoleCount = 4;

//! Value of the "Art Layer:" tool option. TARGET_OFF disables the feature.
//! Underlay and Overlay behave like Line Art if the drawing has no Colour Art.
enum Target {
  TARGET_OFF,
  TARGET_LINE_ART,
  TARGET_COLOUR_ART,
  TARGET_UNDERLAY,
  TARGET_OVERLAY
};

//! Value of the "Show Art:" tool option.
enum ShowMode { SHOW_BOTH, SHOW_LINE_ART, SHOW_COLOUR_ART };

//! Bit of a role in the masks of hidden roles.
inline int roleBit(Role role) { return 1 << int(role); }

// Mirrors isOThick() in tglregions.cpp, which hides these strokes on render.
template <class Stroke>
bool hasZeroThickness(const Stroke &stroke) {
  for (int i = 0; i < stroke.getControlPointCount(); ++i)
    if (stroke.getControlPoint(i).thick != 0) return false;
  return true;
}

template <class Stroke>
Role roleOf(const Stroke &stroke) {
  return hasZeroThickness(stroke) ? COLOUR_ART : LINE_ART;
}

//! The first and last Colour Art strokes of a drawing, in stacking order, and
//! whether Line Art lies between them: without, there are no Underlay and
//! Overlay strokes (a block of Colour Art below or above the Line Art does
//! not make it Overlay or Underlay).
struct ColourRange {
  int first = -1, last = -1;
  bool lineArtBetween = false;
  bool isEmpty() const { return first < 0; }
  bool isLayered() const { return first >= 0 && lineArtBetween; }
};

//! Finds the Colour Art strokes of an image, which has getStrokeCount() and
//! getStroke(index) returning a pointer to a stroke.
template <class Image>
ColourRange colourRange(const Image &image) {
  ColourRange range;
  int count = int(image.getStrokeCount());
  for (int i = 0; i < count; ++i) {
    if (!hasZeroThickness(*image.getStroke(i))) continue;
    if (range.first < 0) range.first = i;
    range.last = i;
  }
  for (int i = range.first + 1; i < range.last && !range.lineArtBetween; ++i)
    range.lineArtBetween = !hasZeroThickness(*image.getStroke(i));
  return range;
}

//! The role of the stroke at the given place of the stacking order. A Line Art
//! stroke between the Colour Art strokes is plain Line Art.
inline Role roleInStack(const ColourRange &range, int index, bool isColour) {
  if (isColour) return COLOUR_ART;
  if (!range.isLayered()) return LINE_ART;
  if (index < range.first) return UNDERLAY;
  if (index > range.last) return OVERLAY;
  return LINE_ART;
}

//! Counts the strokes of an image for each role (indexed by Role).
template <class Image>
void countRoles(const Image &image, int (&counts)[RoleCount]) {
  for (int &count : counts) count = 0;
  ColourRange range = colourRange(image);
  int strokeCount   = int(image.getStrokeCount());
  for (int i = 0; i < strokeCount; ++i)
    ++counts[roleInStack(range, i, hasZeroThickness(*image.getStroke(i)))];
}

//! Underlay and Overlay targets act as Line Art if the drawing has none.
inline Target effectiveTarget(Target target, const ColourRange &range) {
  if ((target == TARGET_UNDERLAY || target == TARGET_OVERLAY) &&
      !range.isLayered())
    return TARGET_LINE_ART;
  return target;
}

inline bool isEditable(Target target, Role role) {
  switch (target) {
  case TARGET_LINE_ART:
    return role != COLOUR_ART;
  case TARGET_COLOUR_ART:
    return role == COLOUR_ART;
  case TARGET_UNDERLAY:
    return role == UNDERLAY;
  case TARGET_OVERLAY:
    return role == OVERLAY;
  default:
    return true;
  }
}

//! Underlay and Overlay are Line Art for the "Show Art:" option.
inline bool isFaded(Target target, ShowMode display, Role role) {
  if (target == TARGET_OFF) return false;
  if (display == SHOW_LINE_ART) return role == COLOUR_ART;
  if (display == SHOW_COLOUR_ART) return role != COLOUR_ART;
  return false;
}

//! Makes a newly drawn stroke match the target role.
template <class Stroke>
void applyTarget(Target target, Stroke &stroke) {
  if (target != TARGET_COLOUR_ART) return;
  for (int i = 0; i < stroke.getControlPointCount(); ++i) {
    auto point = stroke.getControlPoint(i);
    if (point.thick == 0) continue;
    point.thick = 0;
    stroke.setControlPoint(i, point);
  }
}

//----------------------------------------------------

//  Current settings, shared by the vector Brush, Geometric, Eraser and
//  Selection tools.

DVAPI Target target();
DVAPI ShowMode display();

//! Tells which strokes of an image the current target allows to edit. Build it
//! once for a loop over the strokes: the Underlay and Overlay targets need the
//! Colour Art range of the image.
class DVAPI Filter {
  const TVectorImage *m_image;
  Target m_target;
  ColourRange m_range;

public:
  explicit Filter(const TVectorImage &image);

  //! Returns false if the stroke at \b index has the role of another target.
  bool isEditable(int index) const;
};

DVAPI bool isFaded(Role role);
DVAPI void applyTarget(TStroke &stroke);

//! Roles hidden in the viewer by the Art Layer Manager, as a mask of
//! roleBit(). It only changes the display of the current column in the viewer.
DVAPI int hiddenRoles();
DVAPI void setRoleHidden(Role role, bool hidden);
//! Whether the viewer shows Colour Art strokes, as the Art Layer Manager asks
//! for, even if the "Art Layer:" option is Off.
DVAPI bool colourArtShown();
DVAPI void setColourArtShown(bool shown);

//! Appends to \b image a Colour Art copy, with all thicknesses 0, of every
//! Line Art stroke that has none yet. The copies go below the other strokes,
//! and the fills of the existing regions are kept. Returns the number of
//! strokes added.
DVAPI int createColourArt(TVectorImage &image);

//! Adds the shared "Art Layer:" and "Show Art:" options to a tool.
DVAPI void bindProperties(TPropertyGroup &group);
//! Returns true if \b propertyName is one of the shared options.
DVAPI bool onPropertyChanged(const std::string &propertyName);
DVAPI void updateTranslation();
//! Loads the saved options; call it when a tool using them is activated.
DVAPI void loadSettings();

}  // namespace ArtLayer

#endif
