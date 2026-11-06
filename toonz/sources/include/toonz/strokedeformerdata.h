#pragma once

#ifndef STROKEDEFORMERDATA_H
#define STROKEDEFORMERDATA_H

#include "tcommon.h"
#include "tgeometry.h"
#include "tsmartpointer.h"
#include "tvectorimage.h"
#include "trasterfxrenderdata.h"

#include <memory>
#include <string>
#include <vector>

class TDeformer;
class TFx;

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//================================================================================================
//
// Deform Strokes
//
//================================================================================================

/*!
  With Deform Strokes on, a deformer fx whose Source draws a Toonz Vector
  drawing does not warp the rendered raster: it adds its deformer to the
  render settings, as a TStrokeDeformerRenderData, and the level column fx
  deforms a copy of the drawing's strokes before drawing them. The lines keep
  their thickness and stay sharp at any zoom, as in a baked drawing, but the
  deformation stays live.

  The deformer is in the fx's space, which is the space of the column's
  drawings (see TDeformerSource), so the column must be reached without a
  change of space: directly, through the column's own colour fxs, or through
  other deformer fxs that deform strokes. rendersVectorStrokes() checks it.
*/

//! The deformer of a deformer fx, for the level column fx to apply on the
//! strokes of its drawing.
class DVAPI TStrokeDeformerRenderData final : public TRasterFxRenderData {
  std::shared_ptr<const TDeformer> m_deformer;
  std::string m_key;

public:
  //! The key identifies the deformer in the render cache: two data with the
  //! same key must deform alike.
  TStrokeDeformerRenderData(const std::shared_ptr<const TDeformer> &deformer,
                            const std::string &key);

  const TDeformer &deformer() const { return *m_deformer; }

  float typeIndex() const override { return 0.75f; }
  bool operator==(const TRasterFxRenderData &data) const override;
  std::string toString() const override;
};

//! Returns whether the render of fx at frame draws the strokes of a Toonz
//! Vector drawing in fx's own space: fx is a level column fx showing a Toonz
//! Vector drawing at frame, reached through the column's colour fxs and
//! through deformer fxs whose deformsStrokes() is true.
DVAPI bool rendersVectorStrokes(TFx *fx, double frame);

//! Returns whether data holds a TStrokeDeformerRenderData.
DVAPI bool hasStrokeDeformers(const std::vector<TRasterFxRenderDataP> &data);

//! Returns a copy of vi deformed by the stroke deformers of data, or vi
//! itself if there are none. The deformers are applied from the last one
//! added, the fx nearest to the column, to the first one.
DVAPI TVectorImageP applyStrokeDeformers(
    const TVectorImageP &vi, const std::vector<TRasterFxRenderDataP> &data,
    double frame);

//! Returns a rect bounding the drawing bounded by bbox, whose thickest stroke
//! has thickness maxThickness, once deformed by the stroke deformers of data.
//! The strokes keep their thickness, so the bounds of their deformed
//! centerlines are enlarged by it.
DVAPI TRectD strokeDeformedBounds(
    const TRectD &bbox, double maxThickness,
    const std::vector<TRasterFxRenderDataP> &data, double frame);

#endif  // STROKEDEFORMERDATA_H
