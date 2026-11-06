#pragma once
#include "tstroke.h"
#include "tsimplecolorstyles.h"
#include <cmath>

namespace StrokeOpacity {
// The legacy style bbox is not a render-bounds guarantee: procedural styles
// can draw ornaments outside it. Crop only styles whose extent is bounded by
// their implementation. Other styles must use the complete output viewport.
inline bool renderBounds(const TColorStyle &style, const TStroke &stroke,
                         TRectD &box) {
  double thickness = 0.;
  for (int i = 0; i < stroke.getControlPointCount(); ++i)
    thickness = std::max(thickness, std::abs(stroke.getControlPoint(i).thick));
  if (style.getTagId() == 3) {
    const double miter = std::max(1., std::abs(stroke.outlineOptions().m_miterUpper));
    if (!std::isfinite(miter) || !std::isfinite(thickness)) return false;
    box = stroke.getBBox().enlarge(thickness * miter);
    return true;
  }
  if (dynamic_cast<const TRasterImagePatternStrokeStyle *>(&style) ||
      dynamic_cast<const TVectorImagePatternStrokeStyle *>(&style)) {
    box = style.getStrokeBBox(&stroke);
    return true;
  }
  if (style.getTagId() == 106) { // Circlets: +/-2 tangent jitter + radius.
    const double radius = std::abs(style.getParamValue(TColorStyle::double_tag(), 2));
    if (!std::isfinite(radius) || !std::isfinite(thickness)) return false;
    box = stroke.getBBox().enlarge(2. + radius * std::max(1., thickness));
    return true;
  }
  return false;
}
} // namespace StrokeOpacity
