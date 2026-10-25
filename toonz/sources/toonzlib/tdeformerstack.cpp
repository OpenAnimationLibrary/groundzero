#include "toonz/tdeformerstack.h"

#include <algorithm>
#include <cmath>

//****************************************************************************************
//    Local namespace stuff
//****************************************************************************************

namespace {

inline double clamp01(double v) {
  // The negated test also maps NaNs to 0
  if (!(v > 0.0)) return 0.0;
  return (v > 1.0) ? 1.0 : v;
}

//---------------------------------------------------------------------------------

// Moves p by the layer, scaled by the layer's weight at p.
TPointD blendLayer(const TDeformerStack::Layer &layer, const TPointD &p,
                   double frame) {
  double w = layer.m_weights.weightAt(p);
  if (!(w > 0.0)) return p;

  TPointD q(layer.m_deformer->deform(p, frame));
  return (w >= 1.0) ? q : p + (q - p) * w;
}

//---------------------------------------------------------------------------------

// Finds the point x such that blendLayer(layer, x) = y.
bool unblendLayer(const TDeformerStack::Layer &layer, const TPointD &y,
                  double frame, double scale, TPointD &result) {
  const TDeformerWeightMap &map = layer.m_weights;

  // Where the layer applies in full or not at all, the inverse is direct
  if (map.isUniform()) {
    double w = map.getWeight();
    if (!(w > 0.0)) {
      result = y;
      return true;
    }
    if (w >= 1.0) return layer.m_deformer->undeform(y, frame, result);
  }

  // Start from the inverse of the full layer, blended with the weight at y,
  // which is exact for a uniform weight and a translation. Then refine with
  // Newton steps on blendLayer(x) = y, with the Jacobian by finite
  // differences.
  TPointD x(y), g;
  if (layer.m_deformer->undeform(y, frame, g))
    x = y + (g - y) * map.weightAt(y);

  TPointD f(blendLayer(layer, x, frame));

  const double h = 1e-5 * scale, tolerance = 1e-6 * scale;
  double err = norm(f - y);

  for (int it = 0; it < 8 && err > tolerance; ++it) {
    TPointD fx(blendLayer(layer, x + TPointD(h, 0.0), frame));
    TPointD fy(blendLayer(layer, x + TPointD(0.0, h), frame));

    TPointD jx((fx - f) * (1.0 / h)), jy((fy - f) * (1.0 / h));
    double det = cross(jx, jy);
    if (std::fabs(det) < 1e-12) break;  // Folded there

    TPointD r(y - f);
    TPointD xNew(x + TPointD(cross(r, jy), cross(jx, r)) * (1.0 / det));

    TPointD fNew(blendLayer(layer, xNew, frame));
    double errNew = norm(fNew - y);
    if (!(errNew < err)) break;  // No progress, e.g. near a fold

    x = xNew, f = fNew, err = errNew;
  }

  result = x;
  return std::isfinite(x.x) && std::isfinite(x.y);
}

}  // namespace

//****************************************************************************************
//    TDeformerWeightMap  implementation
//****************************************************************************************

TDeformerWeightMap TDeformerWeightMap::makeUniform(double weight) {
  TDeformerWeightMap map;
  map.m_weight = clamp01(weight);
  return map;
}

//---------------------------------------------------------------------------------

TDeformerWeightMap TDeformerWeightMap::makeDistance(double weight,
                                                    const TPointD &center,
                                                    double inner,
                                                    double outer) {
  TDeformerWeightMap map;
  map.m_type   = Distance;
  map.m_weight = clamp01(weight);
  map.m_center = center;
  map.m_inner  = std::max(inner, 0.0);
  map.m_outer  = std::max(outer, map.m_inner);
  return map;
}

//---------------------------------------------------------------------------------

double TDeformerWeightMap::weightAt(const TPointD &p) const {
  if (m_type == Uniform) return m_weight;

  double d = norm(p - m_center);
  if (d <= m_inner) return m_weight;
  if (!(d < m_outer)) return 0.0;  // Also when the radii are equal

  double t = (d - m_inner) / (m_outer - m_inner);  // In (0, 1)
  return m_weight * (1.0 - t * t * (3.0 - 2.0 * t));
}

//****************************************************************************************
//    TDeformerStack  implementation
//****************************************************************************************

TDeformerStack::TDeformerStack() : m_scale(100.0) {}

//---------------------------------------------------------------------------------

void TDeformerStack::addLayer(const std::shared_ptr<const TDeformer> &deformer,
                              const TDeformerWeightMap &weights) {
  if (!deformer) return;

  Layer layer;
  layer.m_deformer = deformer;
  layer.m_weights  = weights;
  m_layers.push_back(layer);
}

//---------------------------------------------------------------------------------

TPointD TDeformerStack::deform(const TPointD &p, double frame) const {
  TPointD result(p);
  for (const Layer &layer : m_layers) result = blendLayer(layer, result, frame);
  return result;
}

//---------------------------------------------------------------------------------

bool TDeformerStack::undeform(const TPointD &p, double frame,
                              TPointD &result) const {
  TPointD q(p);
  for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) {
    TPointD x;
    if (!unblendLayer(*it, q, frame, m_scale, x)) return false;
    q = x;
  }

  result = q;
  return true;
}
