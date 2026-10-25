#include "toonz/tdeformer.h"

#include "traster.h"
#include "tpixel.h"

#include <algorithm>
#include <cassert>
#include <cmath>

//****************************************************************************************
//    Local namespace stuff
//****************************************************************************************

namespace {

// Samples per side of the grids used to bound the deformed rects.
const int c_boundsSamples = 16;

//---------------------------------------------------------------------------------

bool isInfinite(const TRectD &rect) {
  const double big = 1e30;
  return rect.x0 <= -big || rect.y0 <= -big || rect.x1 >= big || rect.y1 >= big;
}

//---------------------------------------------------------------------------------

// Bounds the images of a grid of points of rect, through map. The bounds are
// enlarged by the largest distance between the images of neighbouring grid
// points, which accounts for what the grid does not see.
template <typename MAP>
TRectD sampleBounds(const TRectD &rect, const MAP &map) {
  if (rect.isEmpty()) return TRectD();
  if (isInfinite(rect)) return TConsts::infiniteRectD;

  const int n = c_boundsSamples;
  std::vector<TPointD> row(n + 1), prevRow(n + 1);
  std::vector<char> ok(n + 1, 0), prevOk(n + 1, 0);

  bool hasBounds = false;
  double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0, step = 0.0;

  for (int j = 0; j <= n; ++j) {
    double y = rect.y0 + rect.getLy() * j / n;
    for (int i = 0; i <= n; ++i) {
      double x = rect.x0 + rect.getLx() * i / n;
      ok[i]    = map(TPointD(x, y), row[i]);
      if (!ok[i]) continue;

      const TPointD &q = row[i];
      if (!hasBounds) {
        x0 = x1 = q.x, y0 = y1 = q.y;
        hasBounds = true;
      } else {
        x0 = std::min(x0, q.x), y0 = std::min(y0, q.y);
        x1 = std::max(x1, q.x), y1 = std::max(y1, q.y);
      }

      if (i > 0 && ok[i - 1]) step = std::max(step, norm(q - row[i - 1]));
      if (j > 0 && prevOk[i]) step = std::max(step, norm(q - prevRow[i]));
    }
    std::swap(row, prevRow);
    std::swap(ok, prevOk);
  }

  if (!hasBounds) return TRectD();
  return TRectD(x0 - step, y0 - step, x1 + step, y1 + step);
}

//---------------------------------------------------------------------------------

template <typename CHANNEL>
inline CHANNEL toChannel(double v) {
  return (CHANNEL)(v + 0.5);
}

template <>
inline float toChannel<float>(double v) {
  return (float)v;
}

//---------------------------------------------------------------------------------

// Bilinear sample of ras at (x, y), in pixel units with the center of pixel
// (i, j) at (i, j). Pixels outside ras count as transparent.
template <typename PIXEL>
PIXEL sampleBilinear(const TRasterPT<PIXEL> &ras, double x, double y) {
  int lx = ras->getLx(), ly = ras->getLy();
  // The negated test also rejects NaNs
  if (!(x > -1.0 && y > -1.0 && x < lx && y < ly)) return PIXEL::Transparent;

  double fx = std::floor(x), fy = std::floor(y);
  int px = (int)fx, py = (int)fy;
  double u = x - fx, v = y - fy;

  double r = 0.0, g = 0.0, b = 0.0, m = 0.0;
  auto add = [&](int xx, int yy, double w) {
    if (w <= 0.0 || xx < 0 || yy < 0 || xx >= lx || yy >= ly) return;
    const PIXEL &pix = ras->pixels(yy)[xx];
    r += w * pix.r;
    g += w * pix.g;
    b += w * pix.b;
    m += w * pix.m;
  };
  add(px, py, (1.0 - u) * (1.0 - v));
  add(px + 1, py, u * (1.0 - v));
  add(px, py + 1, (1.0 - u) * v);
  add(px + 1, py + 1, u * v);

  typedef typename PIXEL::Channel Channel;
  PIXEL out;
  out.r = toChannel<Channel>(r);
  out.g = toChannel<Channel>(g);
  out.b = toChannel<Channel>(b);
  out.m = toChannel<Channel>(m);
  return out;
}

//---------------------------------------------------------------------------------

template <typename PIXEL>
void warpRasterT(const TDeformer &deformer, const TRasterPT<PIXEL> &outRas,
                 const TPointD &outPos, const TRasterPT<PIXEL> &inRas,
                 const TPointD &inPos, double frame) {
  // Pixel centers lie half a pixel from the corners
  TPointD inCenter(inPos + TPointD(0.5, 0.5));
  TPointD outCenter(outPos + TPointD(0.5, 0.5));

  outRas->lock();
  inRas->lock();

  int lx = outRas->getLx(), ly = outRas->getLy();
  for (int y = 0; y < ly; ++y) {
    PIXEL *pix = outRas->pixels(y);
    for (int x = 0; x < lx; ++x, ++pix) {
      TPointD src;
      if (deformer.undeform(outCenter + TPointD(x, y), frame, src))
        *pix = sampleBilinear(inRas, src.x - inCenter.x, src.y - inCenter.y);
      else
        *pix = PIXEL::Transparent;
    }
  }

  inRas->unlock();
  outRas->unlock();
}

//---------------------------------------------------------------------------------

// Computes tan(alpha / 2) = sin(alpha) / (1 + cos(alpha)), where alpha is the
// signed angle at the origin from the vector a to the vector b, of norms ra
// and rb. Returns false when the origin lies on the segment from a to b, where
// alpha is +-pi (or a or b is the origin).
inline bool halfAngleTan(const TPointD &a, double ra, const TPointD &b,
                         double rb, double &result) {
  double rr  = ra * rb;
  double den = rr + a * b;  // rr * (1 + cos(alpha))
  if (den <= 1e-12 * rr) return false;

  result = cross(a, b) / den;  // rr * sin(alpha) / den
  return true;
}

//---------------------------------------------------------------------------------

// The point of the segment from a to b at the same ratio as the origin lies
// on the segment between two vectors of norms ra and rb.
inline TPointD pointOnEdge(const TPointD &a, const TPointD &b, double ra,
                           double rb) {
  double r = ra + rb;
  return (r > 0.0) ? a + (b - a) * (ra / r) : a;
}

//---------------------------------------------------------------------------------

double polygonArea(const std::vector<TPointD> &polygon) {
  double area = 0.0;
  int n       = (int)polygon.size();
  for (int i = 0; i < n; ++i) area += cross(polygon[i], polygon[(i + 1) % n]);
  return 0.5 * area;
}

//---------------------------------------------------------------------------------

double polygonDiagonal(const std::vector<TPointD> &polygon) {
  if (polygon.empty()) return 0.0;

  TPointD p0(polygon[0]), p1(polygon[0]);
  for (const TPointD &p : polygon) {
    p0.x = std::min(p0.x, p.x), p0.y = std::min(p0.y, p.y);
    p1.x = std::max(p1.x, p.x), p1.y = std::max(p1.y, p.y);
  }
  return norm(p1 - p0);
}

}  // namespace

//****************************************************************************************
//    TDeformer  implementation
//****************************************************************************************

TRectD TDeformer::deformedBounds(const TRectD &sourceRect, double frame) const {
  return sampleBounds(sourceRect, [this, frame](const TPointD &p, TPointD &q) {
    q = deform(p, frame);
    return true;
  });
}

//---------------------------------------------------------------------------------

TRectD TDeformer::sourceBounds(const TRectD &deformedRect, double frame) const {
  return sampleBounds(deformedRect,
                      [this, frame](const TPointD &p, TPointD &q) {
                        return undeform(p, frame, q);
                      });
}

//---------------------------------------------------------------------------------

void TDeformer::warpRaster(const TRasterP &outRas, const TPointD &outPos,
                           const TRasterP &inRas, const TPointD &inPos,
                           double frame) const {
  if (!outRas) return;

  TRaster32P out32 = outRas, in32 = inRas;
  TRaster64P out64 = outRas, in64 = inRas;
  TRasterFP outF = outRas, inF = inRas;

  if (out32 && in32)
    ::warpRasterT<TPixel32>(*this, out32, outPos, in32, inPos, frame);
  else if (out64 && in64)
    ::warpRasterT<TPixel64>(*this, out64, outPos, in64, inPos, frame);
  else if (outF && inF)
    ::warpRasterT<TPixelF>(*this, outF, outPos, inF, inPos, frame);
  else {
    assert(!"TDeformer::warpRaster: unsupported or mismatched rasters");
    outRas->clear();
  }
}

//****************************************************************************************
//    TEnvelopeDeformer  implementation
//****************************************************************************************

TEnvelopeDeformer::TEnvelopeDeformer(
    const std::vector<TPointD> &restEnvelope,
    const std::vector<TPointD> &deformedEnvelope)
    : m_rest(restEnvelope), m_deformed(deformedEnvelope), m_scale(1.0) {
  double scale = std::max(polygonDiagonal(m_rest), polygonDiagonal(m_deformed));
  if (scale > 0.0) m_scale = scale;
}

//---------------------------------------------------------------------------------

bool TEnvelopeDeformer::isValid() const {
  if (m_rest.size() < 3 || m_rest.size() != m_deformed.size()) return false;

  double diagonal = polygonDiagonal(m_rest);
  return diagonal > 0.0 &&
         std::fabs(polygonArea(m_rest)) > 1e-6 * diagonal * diagonal;
}

//---------------------------------------------------------------------------------

bool TEnvelopeDeformer::isIdentity(double tolerance) const {
  if (m_rest.size() != m_deformed.size()) return false;

  for (size_t i = 0; i < m_rest.size(); ++i)
    if (norm(m_deformed[i] - m_rest[i]) > tolerance) return false;
  return true;
}

//---------------------------------------------------------------------------------

bool TEnvelopeDeformer::mapThrough(const std::vector<TPointD> &from,
                                   const std::vector<TPointD> &to,
                                   const TPointD &p, TPointD &result) {
  int n = (int)from.size();
  if (n < 3 || (int)to.size() != n) return false;

  // Mean value weights: w_i = (tan(alpha_(i-1) / 2) + tan(alpha_i / 2)) / r_i,
  // where alpha_i is the signed angle at p of the edge from vertex i to
  // vertex i + 1, and r_i the distance of vertex i from p. On an edge (or a
  // vertex) the coordinates are the linear interpolation along it.
  TPointD a(from[n - 1] - p), b(from[0] - p);
  double ra = norm(a), rb = norm(b);

  double tPrev;
  if (!halfAngleTan(a, ra, b, rb, tPrev)) {
    result = pointOnEdge(to[n - 1], to[0], ra, rb);
    return true;
  }

  TPointD sum;
  double wSum = 0.0, wAbsSum = 0.0;

  for (int i = 0; i < n; ++i) {
    int j = (i + 1) % n;

    a = b, ra = rb;  // Vertex i, which is not p
    b = from[j] - p, rb = norm(b);

    double t;
    if (!halfAngleTan(a, ra, b, rb, t)) {
      result = pointOnEdge(to[i], to[j], ra, rb);
      return true;
    }

    double w = (tPrev + t) / ra;
    sum += to[i] * w;
    wSum += w;
    wAbsSum += std::fabs(w);

    tPrev = t;
  }

  // The negated test also rejects NaNs
  if (!(std::fabs(wSum) > 1e-9 * wAbsSum)) return false;

  result = sum * (1.0 / wSum);
  return true;
}

//---------------------------------------------------------------------------------

bool TEnvelopeDeformer::undeform(const TPointD &p, double frame,
                                 TPointD &result) const {
  if (!isValid()) {
    result = p;
    return true;
  }

  return mapThrough(m_deformed, m_rest, p, result);
}

//---------------------------------------------------------------------------------

TPointD TEnvelopeDeformer::deform(const TPointD &p, double frame) const {
  if (!isValid()) return p;

  // Start from the mean value coordinates of the rest envelope, which agree
  // with the deformation on the envelope's edges, then refine with Newton
  // steps on undeform(q) = p, with the Jacobian by finite differences.
  TPointD q;
  if (!mapThrough(m_rest, m_deformed, p, q)) return p;

  TPointD g;
  if (!mapThrough(m_deformed, m_rest, q, g)) return q;

  const double h = 1e-5 * m_scale, tolerance = 1e-7 * m_scale;
  double err = norm(g - p);

  for (int it = 0; it < 8 && err > tolerance; ++it) {
    TPointD gx, gy;
    if (!mapThrough(m_deformed, m_rest, q + TPointD(h, 0.0), gx) ||
        !mapThrough(m_deformed, m_rest, q + TPointD(0.0, h), gy))
      break;

    TPointD jx((gx - g) * (1.0 / h)), jy((gy - g) * (1.0 / h));
    double det = cross(jx, jy);
    if (std::fabs(det) < 1e-12) break;  // Folded there

    TPointD r(p - g);
    TPointD qNew(q + TPointD(cross(r, jy), cross(jx, r)) * (1.0 / det));

    TPointD gNew;
    if (!mapThrough(m_deformed, m_rest, qNew, gNew)) break;

    double errNew = norm(gNew - p);
    if (!(errNew < err)) break;  // No progress, e.g. near a fold

    q = qNew, g = gNew, err = errNew;
  }

  return q;
}
