#include "toonz/tdeformerstack.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

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

bool isValidPaintingGrid(const TRectD& rect, int lx, int ly) {
  if (lx <= 0 || ly <= 0) return false;
  const double cw = rect.getLx() / lx, ch = rect.getLy() / ly;
  for (double value : {rect.x0, rect.y0, cw, ch})
    if (!std::isfinite(value)) return false;
  return cw > 0.0 && ch > 0.0 && lx <= (std::numeric_limits<int>::max)() / ly;
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
//    TDeformerWeightPainting  implementation
//****************************************************************************************

TDeformerWeightPainting TDeformerWeightPainting::make(const TRectD &rect,
                                                      int lx, int ly) {
  TDeformerWeightPainting painting;
  if (!isValidPaintingGrid(rect, lx, ly)) return painting;
  painting.m_lx   = lx;
  painting.m_ly   = ly;
  painting.m_rect = rect;
  painting.m_values.assign(lx * ly, 0);
  return painting;
}

//---------------------------------------------------------------------------------

bool TDeformerWeightPainting::isEmpty() const {
  return !isValidPaintingGrid(m_rect, m_lx, m_ly) ||
         m_values.size() != static_cast<size_t>(m_lx * m_ly);
}

//---------------------------------------------------------------------------------

double TDeformerWeightPainting::weightAt(const TPointD &p) const {
  if (isEmpty()) return 0.0;
  if (!(p.x >= m_rect.x0 && p.x <= m_rect.x1 && p.y >= m_rect.y0 &&
        p.y <= m_rect.y1))
    return 0.0;

  // In cells, from the center of the first one
  const double u = (p.x - m_rect.x0) / m_rect.getLx() * m_lx - 0.5;
  const double v = (p.y - m_rect.y0) / m_rect.getLy() * m_ly - 0.5;
  const double cu = std::min(std::max(u, 0.0), double(m_lx - 1));
  const double cv = std::min(std::max(v, 0.0), double(m_ly - 1));
  const int i0 = std::min((int)cu, m_lx - 1), j0 = std::min((int)cv, m_ly - 1);
  const int i1 = std::min(i0 + 1, m_lx - 1), j1 = std::min(j0 + 1, m_ly - 1);
  const double fu = cu - i0, fv = cv - j0;

  auto at = [this](int i, int j) { return m_values[j * m_lx + i] / 255.0; };
  const double bottom = at(i0, j0) + (at(i1, j0) - at(i0, j0)) * fu;
  const double top    = at(i0, j1) + (at(i1, j1) - at(i0, j1)) * fu;
  return bottom + (top - bottom) * fv;
}

//---------------------------------------------------------------------------------

bool TDeformerWeightPainting::paint(const TPointD &center, double radius,
                                    double target, double strength) {
  if (isEmpty() || !(radius > 0.0) || !(strength > 0.0)) return false;
  if (!std::isfinite(center.x) || !std::isfinite(center.y) ||
      !std::isfinite(radius))
    return false;
  target   = clamp01(target);
  strength = clamp01(strength);

  const double cw = m_rect.getLx() / m_lx, ch = m_rect.getLy() / m_ly;
  // Clip in floating point before converting: a finite brush can extend
  // beyond the integer range, or its bounds can overflow to infinity.
  auto cellIndex = [](double value, int count) {
    return (int)std::floor(std::min(double(count), std::max(-1.0, value)));
  };
  const int i0 =
      std::max(0, cellIndex((center.x - radius - m_rect.x0) / cw, m_lx));
  const int i1 =
      std::min(m_lx - 1, cellIndex((center.x + radius - m_rect.x0) / cw, m_lx));
  const int j0 =
      std::max(0, cellIndex((center.y - radius - m_rect.y0) / ch, m_ly));
  const int j1 =
      std::min(m_ly - 1, cellIndex((center.y + radius - m_rect.y0) / ch, m_ly));

  bool changed = false;
  for (int j = j0; j <= j1; ++j)
    for (int i = i0; i <= i1; ++i) {
      const TPointD cell(m_rect.x0 + (i + 0.5) * cw, m_rect.y0 + (j + 0.5) * ch);
      const double d = norm(cell - center) / radius;
      if (!(d < 1.0)) continue;

      // Full strength at the center, smoothly none at the rim
      const double falloff = 1.0 - d * d * (3.0 - 2.0 * d);
      unsigned char &value = m_values[j * m_lx + i];
      const double delta   = (target * 255.0 - value) * strength * falloff;
      int next             = (int)std::lround(value + delta);
      // Small steps still reach the target
      if (next == value && std::fabs(delta) > 0.05) next += delta > 0 ? 1 : -1;
      next = std::min(255, std::max(0, next));
      if (next != value) value = (unsigned char)next, changed = true;
    }
  return changed;
}

//---------------------------------------------------------------------------------

std::string TDeformerWeightPainting::toString() const {
  if (isEmpty()) return std::string();
  QByteArray bytes((const char *)m_values.data(), (int)m_values.size());
  QStringList fields;
  fields << "1" << QString::number(m_lx) << QString::number(m_ly)
         << QString::number(m_rect.x0, 'g', 17)
         << QString::number(m_rect.y0, 'g', 17)
         << QString::number(m_rect.x1, 'g', 17)
         << QString::number(m_rect.y1, 'g', 17)
         << QString::fromLatin1(bytes.toBase64());
  return fields.join(' ').toStdString();
}

//---------------------------------------------------------------------------------

TDeformerWeightPainting TDeformerWeightPainting::fromString(
    const std::string &text) {
  const QStringList fields =
      QString::fromStdString(text).split(' ', Qt::SkipEmptyParts);
  if (fields.size() != 8 || fields[0] != "1") return TDeformerWeightPainting();

  bool ok[6];
  const int lx = fields[1].toInt(&ok[0]), ly = fields[2].toInt(&ok[1]);
  const TRectD rect(fields[3].toDouble(&ok[2]), fields[4].toDouble(&ok[3]),
                    fields[5].toDouble(&ok[4]), fields[6].toDouble(&ok[5]));
  for (bool b : ok)
    if (!b) return TDeformerWeightPainting();
  // A bound on the size keeps a damaged scene from allocating much
  if (lx <= 0 || ly <= 0 || lx > 1024 || ly > 1024)
    return TDeformerWeightPainting();

  const QByteArray bytes = QByteArray::fromBase64(fields[7].toLatin1());
  if (bytes.size() != lx * ly) return TDeformerWeightPainting();

  TDeformerWeightPainting painting = make(rect, lx, ly);
  if (painting.isEmpty()) return painting;
  std::copy(bytes.begin(), bytes.end(), painting.m_values.begin());
  return painting;
}

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

TDeformerWeightMap TDeformerWeightMap::withPainting(
    const std::shared_ptr<const TDeformerWeightPainting> &painting,
    const TAffine &planeToPainting) const {
  TDeformerWeightMap map(*this);
  if (painting && !painting->isEmpty()) {
    map.m_painting   = painting;
    map.m_toPainting = planeToPainting;
  }
  return map;
}

//---------------------------------------------------------------------------------

double TDeformerWeightMap::weightAt(const TPointD &p) const {
  double weight = m_weight;
  if (m_type != Uniform) {
    double d = norm(p - m_center);
    if (!(d < m_outer) && !(d <= m_inner))
      return 0.0;  // Also when the radii are equal
    if (d > m_inner) {
      double t = (d - m_inner) / (m_outer - m_inner);  // In (0, 1)
      weight *= 1.0 - t * t * (3.0 - 2.0 * t);
    }
  }
  if (m_painting && weight > 0.0)
    weight *= m_painting->weightAt(m_toPainting * p);
  return weight;
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
