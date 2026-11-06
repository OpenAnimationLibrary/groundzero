#pragma once

#ifndef WEIGHTPAINTER_H
#define WEIGHTPAINTER_H

#include "tgeometry.h"
#include "tfx.h"

#include "toonz/tdeformerstack.h"

#include <string>

//=============================================================================
// WeightPainter
//-----------------------------------------------------------------------------
/*! Paints the weights of a layer of the Deformer Stack FX, for the Animate
    tool's Paint Weights option. The weights are a TDeformerWeightPainting in
    the FX's space, kept in the layer's "<letter>_paint" parameter; painting
    raises them toward 1, erasing lowers them toward 0. A whole drag is one
    undo step.

    The painting is made on the first stroke, over the Source's bounds (or a
    default area when they are unknown), with 64 cells on its longer side.

    Positions and radii are in the FX's space.                              */

class WeightPainter {
  TFxP m_fx;
  std::string m_paramName;
  std::wstring m_before;
  TDeformerWeightPainting m_painting;
  TPointD m_lastPos;
  double m_radius;
  bool m_erase;
  bool m_active;

public:
  WeightPainter() : m_radius(0.0), m_erase(false), m_active(false) {}

  //! Returns whether fx has painted weights: it is a Deformer Stack FX.
  static bool accepts(TFx *fx);

  //! Returns the painting of the layer (0 to 2) of fx, empty when none.
  static TDeformerWeightPainting getPainting(TFx *fx, int layer);

  bool isActive() const { return m_active; }

  //! Starts painting the layer of fx at pos. Returns false if fx has no
  //! painted weights.
  bool begin(TFx *fx, int layer, const TPointD &pos, double radius,
             bool erase, double frame);
  //! Paints along the segment from the last position to pos.
  void drag(const TPointD &pos);
  //! Ends the drag, adding its undo if anything changed.
  void end();

  //! The painting being made, while active.
  const TDeformerWeightPainting &painting() const { return m_painting; }

  //! Draws the painting in the current OpenGL matrix (the FX's space), and
  //! the brush at cursor when its radius is above 0.
  static void draw(const TDeformerWeightPainting &painting,
                   const TPointD &cursor, double radius, double pixelSize);

private:
  void dab(const TPointD &pos);
  void store();
};

#endif  // WEIGHTPAINTER_H
