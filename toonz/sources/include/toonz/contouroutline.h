#pragma once

#ifndef CONTOUROUTLINE_H
#define CONTOUROUTLINE_H

#include "tgeometry.h"

#include <vector>

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

class TStroke;

//================================================================================================
//
// ContourOutline
//
//================================================================================================

//! Conversion of a variable-width brush stroke (a centerline with a thickness
//! per control point) to the closed outline shape it draws, as a normal
//! zero-thickness closed stroke that can be edited control point by control
//! point (the "Contour Editor" workflow).
/*!
  Nothing here changes TStroke or the level formats: the result is a plain
  closed TStroke. The conversion is one way, the thickness profile cannot be
  recovered from the outline. See doc/contour_editor.md.
*/
namespace ContourOutline {

//! Quality settings of the conversion. Lengths are in stroke units.
struct Options {
  double m_sampleError;  //!< Max distance of the outline polygon from the
                         //!< exact offset curves.
  double m_fitError;     //!< Max distance of the fitted curve from the polygon.
  int m_maxControlPoints;  //!< The fit is relaxed until it has at most this
                           //!< many control points (when it can be).

  Options() : m_sampleError(0.05), m_fitError(0.1), m_maxControlPoints(400) {}
};

//! Returns true if \b stroke can be converted: it has some thickness, it is
//! not a centerline-only stroke and it is not a self loop (a closed brush
//! stroke is a ring and would need two outlines).
DVAPI bool canConvert(const TStroke &stroke);

//! Computes the outline of \b stroke as a closed polygon, counter-clockwise,
//! without repeating the first point at the end. The two sides are the
//! centerline offset by the thickness at each side, joined and capped as the
//! outline options of the stroke say (cap and join styles, miter limit).
//! Returns false if the stroke cannot be converted.
DVAPI bool computeOutlinePolygon(const TStroke &stroke,
                                 std::vector<TPointD> &polygon,
                                 double sampleError = 0.05);

//! Computes the control points of the closed zero-thickness stroke that fits
//! the outline of \b stroke (the first and the last control point are the
//! same). Use them with TStroke::reshape() followed by setSelfLoop().
//! Returns false if the stroke cannot be converted.
DVAPI bool makeOutlineControlPoints(const TStroke &stroke,
                                    std::vector<TThickPoint> &controlPoints,
                                    const Options &options = Options());

//! Returns a new closed zero-thickness stroke with the outline of \b stroke
//! and its style and outline options, or 0 if it cannot be converted.
DVAPI TStroke *makeOutlineStroke(const TStroke &stroke,
                                 const Options &options = Options());

//! Signed area of a polygon (positive when counter-clockwise).
DVAPI double polygonArea(const std::vector<TPointD> &polygon);

}  // namespace ContourOutline

#endif
