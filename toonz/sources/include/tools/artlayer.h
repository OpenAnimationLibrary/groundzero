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
// The inline functions below are pure and independent of the tool framework,
// so that they can be checked by tnztools/tests/artlayer_test.cpp.

#include "tcommon.h"

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

//====================================================

namespace ArtLayer {

enum Role { LINE_ART, COLOUR_ART };

//! Value of the "Art Layer:" tool option. TARGET_OFF disables the feature.
enum Target { TARGET_OFF, TARGET_LINE_ART, TARGET_COLOUR_ART };

//! Value of the "Show Art:" tool option.
enum ShowMode { SHOW_BOTH, SHOW_LINE_ART, SHOW_COLOUR_ART };

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

inline bool isEditable(Target target, Role role) {
  if (target == TARGET_OFF) return true;
  return (target == TARGET_COLOUR_ART) == (role == COLOUR_ART);
}

inline bool isFaded(Target target, ShowMode display, Role role) {
  if (target == TARGET_OFF) return false;
  if (display == SHOW_LINE_ART) return role == COLOUR_ART;
  if (display == SHOW_COLOUR_ART) return role == LINE_ART;
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

DVAPI bool isEditable(const TStroke &stroke);
DVAPI bool isFaded(Role role);
DVAPI void applyTarget(TStroke &stroke);

//! Adds the shared "Art Layer:" and "Show Art:" options to a tool.
DVAPI void bindProperties(TPropertyGroup &group);
//! Returns true if \b propertyName is one of the shared options.
DVAPI bool onPropertyChanged(const std::string &propertyName);
DVAPI void updateTranslation();
//! Loads the saved options; call it when a tool using them is activated.
DVAPI void loadSettings();

}  // namespace ArtLayer

#endif
