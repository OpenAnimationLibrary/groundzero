#pragma once

#ifndef TDEFORMERSTACK_H
#define TDEFORMERSTACK_H

#include "toonz/tdeformer.h"

#include <memory>
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

//================================================================================================
//
// TDeformerWeightMap
//
//================================================================================================

//! The TDeformerWeightMap tells how much a layer of a TDeformerStack applies
//! at each point of the plane: a weight between 0 (the layer does not move
//! the point) and 1 (the point is moved by the whole layer).
/*!
  A map is either uniform, with the same weight everywhere, or distance based:
  the weight is the map's weight within the inner radius of a center, falls
  smoothly (smoothstep) to 0 at the outer radius, and is 0 farther away.
*/
class DVAPI TDeformerWeightMap {
public:
  enum Type { Uniform, Distance };

private:
  Type m_type;
  double m_weight;  //!< In [0, 1]
  TPointD m_center;
  double m_inner, m_outer;  //!< 0 <= inner <= outer

public:
  //! Builds a uniform map of weight 1.
  TDeformerWeightMap()
      : m_type(Uniform), m_weight(1.0), m_inner(0.0), m_outer(0.0) {}

  //! Returns a map with the passed weight everywhere. The weight is clamped
  //! to [0, 1].
  static TDeformerWeightMap makeUniform(double weight);

  //! Returns a map with the passed weight (clamped to [0, 1]) within the inner
  //! radius of center, falling to 0 at the outer radius. Negative radii count
  //! as 0, and an outer radius smaller than the inner one as equal to it, which
  //! makes the weight drop sharply.
  static TDeformerWeightMap makeDistance(double weight, const TPointD &center,
                                         double inner, double outer);

  Type getType() const { return m_type; }
  bool isUniform() const { return m_type == Uniform; }
  double getWeight() const { return m_weight; }
  const TPointD &getCenter() const { return m_center; }
  double getInnerRadius() const { return m_inner; }
  double getOuterRadius() const { return m_outer; }

  //! Returns whether the weight is 0 everywhere.
  bool isZero() const { return !(m_weight > 0.0); }

  //! Returns the weight at p, in [0, 1].
  double weightAt(const TPointD &p) const;
};

//================================================================================================
//
// TDeformerStack
//
//================================================================================================

//! The TDeformerStack composes an ordered list of deformers, each with a
//! weight map, into a single deformer.
/*!
  deform() applies the layers in order. A layer moves the point p to
  p + w * (layer.deform(p) - p), where w is the layer's weight at p, so a
  layer of weight 0 does nothing and one of weight 1 is the layer itself. Each
  layer works on the output of the previous one, and the weight is read at the
  point the layer receives.

  undeform() is the inverse chain, from the last layer to the first. Where the
  weight of a layer is 0 or 1 everywhere, its inverse is the layer's own
  undeform(). Otherwise the blended map is not analytically invertible, and
  the inverse is found with a few Newton steps (as TEnvelopeDeformer::deform()
  does), which costs several layer evaluations. Such a layer is invertible
  where its weight changes slowly compared to how far the layer moves points:
  a weight that drops sharply across a large displacement folds the plane.

  Being a TDeformer, a stack gets warpRaster() and the bounds helpers
  unchanged. Like the other deformers, it works in a single plane: all its
  layers must be built in the same space.
*/
class DVAPI TDeformerStack final : public TDeformer {
public:
  struct Layer {
    std::shared_ptr<const TDeformer> m_deformer;
    TDeformerWeightMap m_weights;
  };

private:
  std::vector<Layer> m_layers;
  double m_scale;  //!< Size of the deformed art, for the tolerances

public:
  TDeformerStack();

  //! Adds a layer on top of the stack. A null deformer is ignored.
  void addLayer(const std::shared_ptr<const TDeformer> &deformer,
                const TDeformerWeightMap &weights = TDeformerWeightMap());

  void clear() { m_layers.clear(); }
  int getLayerCount() const { return (int)m_layers.size(); }
  const Layer &getLayer(int index) const { return m_layers[index]; }

  //! Sets the typical size of the deformed plane area (in the plane's units),
  //! to which the tolerances of the inverse are relative. The default is 100.
  void setScale(double scale) {
    if (scale > 0.0) m_scale = scale;
  }
  double getScale() const { return m_scale; }

  TPointD deform(const TPointD &p, double frame) const override;
  bool undeform(const TPointD &p, double frame, TPointD &result) const override;
};

#endif  // TDEFORMERSTACK_H
