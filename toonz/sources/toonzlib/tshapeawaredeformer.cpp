#include "toonz/tshapeawaredeformer.h"

#include <algorithm>
#include <cmath>

//****************************************************************************************
//    TShapeAwareDeformer::Weights  definition
//****************************************************************************************

struct TShapeAwareDeformer::Weights {
  int m_nx, m_ny;
  double m_x0, m_y0, m_h;   // Grid origin and cell size
  int m_count;              // Handles + 1; the anchor is the last one
  std::vector<double> m_w;  // Node-major: m_count weights per node

  Weights() : m_nx(0), m_ny(0), m_x0(0.0), m_y0(0.0), m_h(1.0), m_count(0) {}
};

//****************************************************************************************
//    Local namespace stuff
//****************************************************************************************

namespace {

typedef TShapeAwareDeformer::Weights Weights;

const int c_minCells = 8;  // Cells along the longest side of the coarsest grid

//---------------------------------------------------------------------------------

bool isFinite(const TPointD &p) {
  return std::isfinite(p.x) && std::isfinite(p.y);
}

bool isFinite(const TAffine &a) {
  return std::isfinite(a.a11) && std::isfinite(a.a12) && std::isfinite(a.a13) &&
         std::isfinite(a.a21) && std::isfinite(a.a22) && std::isfinite(a.a23);
}

//---------------------------------------------------------------------------------

// One grid of the pyramid. The nodes of the two outer rings of the grid are
// fixed to the anchor, and so are those the handles constrain: the weight of
// the handle k at a fixed node n is m_values[n * H + k], and 0 for the handles
// that are not named (the anchor's weight is what is left of 1).
struct Level {
  int m_nx, m_ny;
  double m_x0, m_y0, m_h;
  int m_handles;  // H
  std::vector<char> m_fixed;
  std::vector<double> m_values;

  Level(int nx, int ny, double x0, double y0, double h, int handles)
      : m_nx(nx)
      , m_ny(ny)
      , m_x0(x0)
      , m_y0(y0)
      , m_h(h)
      , m_handles(handles)
      , m_fixed((size_t)nx * ny, 0)
      , m_values((size_t)nx * ny * handles, 0.0) {
    for (int j = 0; j < ny; ++j)
      for (int i = 0; i < nx; ++i)
        if (i < 2 || j < 2 || i > nx - 3 || j > ny - 3)
          m_fixed[(size_t)j * nx + i] = 1;
  }

  TPointD nodePos(int i, int j) const {
    return TPointD(m_x0 + i * m_h, m_y0 + j * m_h);
  }

  // Fixes the node (i, j) to the weights va of the handle ka and vb of kb
  // (kb < 0: none), unless it is fixed already.
  void claim(int i, int j, int ka, double va, int kb, double vb) {
    if (i < 0 || j < 0 || i >= m_nx || j >= m_ny) return;

    size_t n = (size_t)j * m_nx + i;
    if (m_fixed[n]) return;

    m_fixed[n]                   = 1;
    m_values[n * m_handles + ka] = va;
    if (kb >= 0) m_values[n * m_handles + kb] += vb;
  }

  // Fixes the four nodes of the cell of q. With kb >= 0, the weights are
  // those of the handles ka and kb interpolated along the segment a to b, at
  // the projection of each node; else ka has the weight 1.
  void claimCell(const TPointD &q, const TPointD &a, const TPointD &b, int ka,
                 int kb) {
    int i = (int)std::floor((q.x - m_x0) / m_h);
    int j = (int)std::floor((q.y - m_y0) / m_h);

    TPointD ab(b - a);
    double len2 = ab * ab;

    for (int dj = 0; dj <= 1; ++dj)
      for (int di = 0; di <= 1; ++di) {
        double t = 0.0;
        if (kb >= 0 && len2 > 1e-18) {
          t = (nodePos(i + di, j + dj) - a) * ab / len2;
          t = std::min(1.0, std::max(0.0, t));
        }
        if (kb >= 0)
          claim(i + di, j + dj, ka, 1.0 - t, kb, t);
        else
          claim(i + di, j + dj, ka, 1.0, -1, 0.0);
      }
  }

  // Fixes the nodes around the segment a to b, see claimCell().
  void claimSegment(const TPointD &a, const TPointD &b, int ka, int kb) {
    double len = norm(b - a);
    int steps  = std::max(1, (int)std::ceil(len / (0.5 * m_h)));
    for (int s = 0; s <= steps; ++s)
      claimCell(a + (b - a) * ((double)s / steps), a, b, ka, kb);
  }
};

//---------------------------------------------------------------------------------

// Applies the biharmonic operator (the transposed discrete Laplacian applied
// to the discrete Laplacian), restricted to the free nodes, to x, which is zero
// on the fixed nodes. lap is a work vector of the size of the grid.
void applyOperator(const Level &lv, const std::vector<double> &x,
                   std::vector<double> &lap, std::vector<double> &out) {
  const int nx = lv.m_nx, ny = lv.m_ny;

  std::fill(lap.begin(), lap.end(), 0.0);
  for (int j = 1; j < ny - 1; ++j) {
    size_t n = (size_t)j * nx + 1;
    for (int i = 1; i < nx - 1; ++i, ++n)
      lap[n] = x[n - 1] + x[n + 1] + x[n - nx] + x[n + nx] - 4.0 * x[n];
  }

  for (int j = 0; j < ny; ++j) {
    size_t n = (size_t)j * nx;
    for (int i = 0; i < nx; ++i, ++n) {
      if (lv.m_fixed[n]) {
        out[n] = 0.0;
        continue;
      }
      // Free nodes are at least 2 nodes away from the border
      out[n] =
          lap[n - 1] + lap[n + 1] + lap[n - nx] + lap[n + nx] - 4.0 * lap[n];
    }
  }
}

//---------------------------------------------------------------------------------

double dotProduct(const std::vector<double> &a, const std::vector<double> &b) {
  double sum = 0.0;
  for (size_t i = 0; i < a.size(); ++i) sum += a[i] * b[i];
  return sum;
}

//---------------------------------------------------------------------------------

// Solves the weights of the handle k on the level, by conjugate gradients
// started from sol, which is replaced by the solution (including the fixed
// values).
void solveHandle(const Level &lv, int k, std::vector<double> &sol,
                 int maxIterations) {
  const size_t n = (size_t)lv.m_nx * lv.m_ny;

  std::vector<double> x(n, 0.0), w0(n, 0.0);
  for (size_t i = 0; i < n; ++i) {
    if (lv.m_fixed[i])
      w0[i] = lv.m_values[i * lv.m_handles + k];
    else
      x[i] = sol[i];
  }

  std::vector<double> lap(n), b(n), r(n), p(n), ap(n);

  // The system is A x = b, with b = -A w0 on the free nodes
  applyOperator(lv, w0, lap, b);
  double bNorm2 = 0.0;
  for (size_t i = 0; i < n; ++i) {
    b[i] = -b[i];
    bNorm2 += b[i] * b[i];
  }

  if (bNorm2 <= 1e-30) {
    // No constraint reaches this handle: its weight is 0
    sol = w0;
    return;
  }

  applyOperator(lv, x, lap, ap);
  for (size_t i = 0; i < n; ++i) r[i] = b[i] - ap[i];
  p = r;

  double rr = dotProduct(r, r);
  for (int it = 0; it < maxIterations && rr > 1e-10 * bNorm2; ++it) {
    applyOperator(lv, p, lap, ap);
    double pap = dotProduct(p, ap);
    if (!(pap > 0.0)) break;

    double alpha = rr / pap;
    for (size_t i = 0; i < n; ++i) {
      x[i] += alpha * p[i];
      r[i] -= alpha * ap[i];
    }

    double rrNew = dotProduct(r, r);
    double beta  = rrNew / rr;
    rr           = rrNew;
    for (size_t i = 0; i < n; ++i) p[i] = r[i] + beta * p[i];
  }

  for (size_t i = 0; i < n; ++i) sol[i] = x[i] + w0[i];
}

//---------------------------------------------------------------------------------

// Samples the (bilinear) field of the level at q, clamping outside the grid.
double sampleLevel(const Level &lv, const std::vector<double> &field,
                   const TPointD &q) {
  double fx = (q.x - lv.m_x0) / lv.m_h, fy = (q.y - lv.m_y0) / lv.m_h;
  fx = std::min((double)lv.m_nx - 1.0, std::max(0.0, fx));
  fy = std::min((double)lv.m_ny - 1.0, std::max(0.0, fy));

  int i = std::min(lv.m_nx - 2, (int)fx), j = std::min(lv.m_ny - 2, (int)fy);
  double u = fx - i, v = fy - j;

  const double *f00 = &field[(size_t)j * lv.m_nx + i];
  const double *f01 = f00 + lv.m_nx;
  return (1.0 - u) * (1.0 - v) * f00[0] + u * (1.0 - v) * f00[1] +
         (1.0 - u) * v * f01[0] + u * v * f01[1];
}

//---------------------------------------------------------------------------------

double distanceToSegment(const TPointD &p, const TPointD &a, const TPointD &b) {
  TPointD ab(b - a);
  double len2 = ab * ab, t = 0.0;
  if (len2 > 1e-18) t = std::min(1.0, std::max(0.0, ((p - a) * ab) / len2));
  return norm(p - (a + ab * t));
}

}  // namespace

//****************************************************************************************
//    TShapeAwareDeformer  implementation
//****************************************************************************************

std::vector<double> TShapeAwareDeformer::makeWeightsKey(
    const std::vector<Handle> &handles, const Settings &settings) {
  std::vector<double> key;
  key.reserve(3 + 5 * handles.size());
  key.push_back(settings.m_resolution);
  key.push_back(settings.m_margin);
  key.push_back((double)handles.size());
  for (const Handle &h : handles) {
    key.push_back((double)h.m_type);
    key.push_back(h.m_restHead.x);
    key.push_back(h.m_restHead.y);
    key.push_back(h.m_restTail.x);
    key.push_back(h.m_restTail.y);
  }
  return key;
}

//---------------------------------------------------------------------------------

std::shared_ptr<const TShapeAwareDeformer::Weights>
TShapeAwareDeformer::computeWeights(const std::vector<Handle> &handles,
                                    const Settings &settings) {
  const int H = (int)handles.size();
  if (H < 1 || H > MAX_HANDLES || !std::isfinite(settings.m_margin))
    return nullptr;

  // The extent of the rest geometry
  TPointD p0(handles[0].m_restHead), p1(p0);
  for (const Handle &h : handles) {
    if (!isFinite(h.m_restHead) || !isFinite(h.m_restTail)) return nullptr;

    const TPointD ends[2] = {h.m_restHead, h.m_restTail};
    for (const TPointD &p : ends) {
      p0.x = std::min(p0.x, p.x), p0.y = std::min(p0.y, p.y);
      p1.x = std::max(p1.x, p.x), p1.y = std::max(p1.y, p.y);
    }
  }
  double extent = std::max(p1.x - p0.x, p1.y - p0.y);
  double margin =
      std::max(std::max(0.0, settings.m_margin), 0.25 * std::max(extent, 1.0));

  // The grid is at least 0.6 times as wide as long, so that the coarse grids
  // have free nodes
  double sizeX = p1.x - p0.x + 2.0 * margin;
  double sizeY = p1.y - p0.y + 2.0 * margin;
  double x0 = p0.x - margin, y0 = p0.y - margin;
  if (sizeX < 0.6 * sizeY) {
    x0 -= 0.5 * (0.6 * sizeY - sizeX);
    sizeX = 0.6 * sizeY;
  } else if (sizeY < 0.6 * sizeX) {
    y0 -= 0.5 * (0.6 * sizeX - sizeY);
    sizeY = 0.6 * sizeX;
  }
  const double longSide = std::max(sizeX, sizeY);

  // The cells along the longest side of each level, from coarse to fine
  const int resolution = std::min(128, std::max(16, settings.m_resolution));
  std::vector<int> cells;
  for (int c = resolution; c >= c_minCells; c /= 2) cells.push_back(c);
  std::reverse(cells.begin(), cells.end());

  // The cage: its vertices, in order, are the corners of a closed polygon
  std::vector<int> cage;
  for (int i = 0; i < H; ++i)
    if (handles[i].m_type == Handle::CageVertex) cage.push_back(i);

  std::vector<double> prevSol[MAX_HANDLES];
  std::vector<Level> levels;
  levels.reserve(cells.size());

  for (size_t l = 0; l < cells.size(); ++l) {
    const double h = longSide / cells[l];
    const int nx   = (int)std::ceil(sizeX / h - 1e-9) + 1;
    const int ny   = (int)std::ceil(sizeY / h - 1e-9) + 1;

    levels.push_back(Level(nx, ny, x0, y0, h, H));
    Level &lv = levels.back();

    // Pins first, then bones and the cage: a node goes to the first claimer
    for (int k = 0; k < H; ++k) {
      const Handle &hd = handles[k];
      if (hd.m_type == Handle::Pin ||
          (hd.m_type == Handle::CageVertex && cage.size() < 3))
        lv.claimCell(hd.m_restHead, hd.m_restHead, hd.m_restHead, k, -1);
    }
    for (int k = 0; k < H; ++k)
      if (handles[k].m_type == Handle::Bone)
        lv.claimSegment(handles[k].m_restHead, handles[k].m_restTail, k, -1);
    if (cage.size() >= 3)
      for (size_t c = 0; c < cage.size(); ++c) {
        int ka = cage[c], kb = cage[(c + 1) % cage.size()];
        lv.claimSegment(handles[ka].m_restHead, handles[kb].m_restHead, ka, kb);
      }

    // The starting point of each weight: on the coarsest grid, an inverse
    // squared distance field to the handles and the border of the grid, on the
    // other ones the previous solution
    const size_t n = (size_t)nx * ny;
    std::vector<double> sol[MAX_HANDLES];
    for (int k = 0; k < H; ++k) sol[k].assign(n, 0.0);

    if (l == 0) {
      const double eps2 = 0.25 * h * h;
      std::vector<double> invD(H + 1);
      for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
          TPointD q(lv.nodePos(i, j));
          double total = 0.0;
          for (int k = 0; k < H; ++k) {
            const Handle &hd = handles[k];
            double d         = (hd.m_type == Handle::Bone)
                                   ? distanceToSegment(q, hd.m_restHead, hd.m_restTail)
                                   : norm(q - hd.m_restHead);
            total += (invD[k] = 1.0 / (d * d + eps2));
          }
          double db = std::min(std::min(q.x - x0, x0 + sizeX - q.x),
                               std::min(q.y - y0, y0 + sizeY - q.y));
          db        = std::max(0.0, db);
          total += (invD[H] = 1.0 / (db * db + eps2));

          for (int k = 0; k < H; ++k)
            sol[k][(size_t)j * nx + i] = invD[k] / total;
        }
    } else {
      const Level &coarse = levels[l - 1];
      for (int k = 0; k < H; ++k)
        for (int j = 0; j < ny; ++j)
          for (int i = 0; i < nx; ++i)
            sol[k][(size_t)j * nx + i] =
                sampleLevel(coarse, prevSol[k], lv.nodePos(i, j));
    }

    const int maxIterations = (l + 1 == cells.size()) ? 150 : 400;
    for (int k = 0; k < H; ++k) {
      solveHandle(lv, k, sol[k], maxIterations);
      prevSol[k].swap(sol[k]);
    }
  }

  // The weights of the finest grid, the anchor having what is left, clamped
  // to [0, 1] and normalised
  const Level &fine = levels.back();
  const int K       = H + 1;

  std::shared_ptr<Weights> weights = std::make_shared<Weights>();
  weights->m_nx                    = fine.m_nx;
  weights->m_ny                    = fine.m_ny;
  weights->m_x0                    = fine.m_x0;
  weights->m_y0                    = fine.m_y0;
  weights->m_h                     = fine.m_h;
  weights->m_count                 = K;
  weights->m_w.assign((size_t)fine.m_nx * fine.m_ny * K, 0.0);

  for (size_t n = 0; n < (size_t)fine.m_nx * fine.m_ny; ++n) {
    double *w  = &weights->m_w[n * K];
    double sum = 0.0;
    for (int k = 0; k < H; ++k) {
      double v = prevSol[k][n];
      if (!(v > 0.0)) v = 0.0;
      if (v > 1.0) v = 1.0;
      sum += (w[k] = v);
    }

    double anchor = 1.0;
    for (int k = 0; k < H; ++k) anchor -= prevSol[k][n];
    anchor = std::min(1.0, std::max(0.0, anchor));
    sum += (w[H] = anchor);

    if (sum <= 1e-12) {
      for (int k = 0; k < H; ++k) w[k] = 0.0;
      w[H] = 1.0;
    } else
      for (int k = 0; k < K; ++k) w[k] /= sum;
  }

  return weights;
}

//---------------------------------------------------------------------------------

TShapeAwareDeformer::TShapeAwareDeformer(
    const std::vector<Handle> &handles, const Settings &settings,
    const TAffine &placement, const std::shared_ptr<const Weights> &weights)
    : m_scale(1.0), m_valid(false) {
  // The negated test also rejects NaNs
  if (handles.empty() || handles.size() > MAX_HANDLES ||
      !(std::fabs(placement.det()) > 1e-18) || !isFinite(placement))
    return;

  for (const Handle &h : handles)
    if (!isFinite(h.m_transform) || !isFinite(h.m_restHead) ||
        !isFinite(h.m_restTail))
      return;

  m_weights = weights ? weights : computeWeights(handles, settings);
  if (!m_weights || m_weights->m_count != (int)handles.size() + 1) {
    m_weights.reset();
    return;
  }

  m_handles      = handles;
  m_placement    = placement;
  m_placementInv = placement.inv();

  m_inverses.reserve(handles.size());
  for (const Handle &h : m_handles)
    m_inverses.push_back(std::fabs(h.m_transform.det()) > 1e-12
                             ? h.m_transform.inv()
                             : TAffine());

  m_valid = true;

  // The domain and the bounds of its image, in the plane
  const Weights &w = *m_weights;
  const double gx1 = w.m_x0 + (w.m_nx - 1) * w.m_h;
  const double gy1 = w.m_y0 + (w.m_ny - 1) * w.m_h;
  m_scale          = std::max(gx1 - w.m_x0, gy1 - w.m_y0);

  const TPointD corners[4] = {
      m_placement * TPointD(w.m_x0, w.m_y0), m_placement * TPointD(gx1, w.m_y0),
      m_placement * TPointD(gx1, gy1), m_placement * TPointD(w.m_x0, gy1)};
  m_domain = TRectD(corners[0].x, corners[0].y, corners[0].x, corners[0].y);
  for (const TPointD &c : corners) {
    m_domain.x0 = std::min(m_domain.x0, c.x);
    m_domain.y0 = std::min(m_domain.y0, c.y);
    m_domain.x1 = std::max(m_domain.x1, c.x);
    m_domain.y1 = std::max(m_domain.y1, c.y);
  }

  // The deformed nodes, enlarged by the largest step between neighbours
  std::vector<TPointD> row(w.m_nx), prevRow(w.m_nx);
  double step = 0.0;
  for (int j = 0; j < w.m_ny; ++j) {
    for (int i = 0; i < w.m_nx; ++i) {
      TPointD q = m_placement *
                  evaluate(TPointD(w.m_x0 + i * w.m_h, w.m_y0 + j * w.m_h), 0);
      row[i] = q;

      if (i == 0 && j == 0)
        m_deformedDomain = TRectD(q.x, q.y, q.x, q.y);
      else {
        m_deformedDomain.x0 = std::min(m_deformedDomain.x0, q.x);
        m_deformedDomain.y0 = std::min(m_deformedDomain.y0, q.y);
        m_deformedDomain.x1 = std::max(m_deformedDomain.x1, q.x);
        m_deformedDomain.y1 = std::max(m_deformedDomain.y1, q.y);
      }
      if (i > 0) step = std::max(step, norm(q - row[i - 1]));
      if (j > 0) step = std::max(step, norm(q - prevRow[i]));
    }
    std::swap(row, prevRow);
  }
  m_deformedDomain = m_deformedDomain.enlarge(step);
}

//---------------------------------------------------------------------------------

bool TShapeAwareDeformer::isIdentity(double tolerance) const {
  if (!m_valid) return true;

  for (const Handle &h : m_handles)
    if (!h.isIdentity(tolerance)) return false;
  return true;
}

//---------------------------------------------------------------------------------

namespace {

// Fills w (one weight per handle, then the anchor's) with the weights at q,
// bilinearly interpolated, and gx and gy (if not null) with their gradients.
// Returns false outside the grid, where the anchor has the weight 1.
bool lookupWeights(const Weights &wt, const TPointD &q, double *w, double *gx,
                   double *gy) {
  const int K = wt.m_count;

  double fx = (q.x - wt.m_x0) / wt.m_h, fy = (q.y - wt.m_y0) / wt.m_h;
  // The negated test also rejects NaNs
  if (!(fx >= 0.0 && fy >= 0.0 && fx <= wt.m_nx - 1 && fy <= wt.m_ny - 1)) {
    for (int k = 0; k < K; ++k) w[k] = 0.0;
    w[K - 1] = 1.0;
    if (gx)
      for (int k = 0; k < K; ++k) gx[k] = gy[k] = 0.0;
    return false;
  }

  int i = std::min(wt.m_nx - 2, (int)fx), j = std::min(wt.m_ny - 2, (int)fy);
  double u = fx - i, v = fy - j;

  const double *w00 = &wt.m_w[((size_t)j * wt.m_nx + i) * K];
  const double *w10 = w00 + K;
  const double *w01 = w00 + (size_t)wt.m_nx * K;
  const double *w11 = w01 + K;

  const double c00 = (1.0 - u) * (1.0 - v), c10 = u * (1.0 - v),
               c01 = (1.0 - u) * v, c11 = u * v;
  for (int k = 0; k < K; ++k)
    w[k] = c00 * w00[k] + c10 * w10[k] + c01 * w01[k] + c11 * w11[k];

  if (gx) {
    const double invH = 1.0 / wt.m_h;
    for (int k = 0; k < K; ++k) {
      gx[k] = ((1.0 - v) * (w10[k] - w00[k]) + v * (w11[k] - w01[k])) * invH;
      gy[k] = ((1.0 - u) * (w01[k] - w00[k]) + u * (w11[k] - w10[k])) * invH;
    }
  }
  return true;
}

}  // namespace

//---------------------------------------------------------------------------------

// Deforms q, in the space of the handles. If jacobian is not null, it receives
// the derivatives (dx'/dx, dx'/dy, dy'/dx, dy'/dy).
TPointD TShapeAwareDeformer::evaluate(const TPointD &q,
                                      double *jacobian) const {
  const int H = (int)m_handles.size();
  double w[MAX_HANDLES + 1], gx[MAX_HANDLES + 1], gy[MAX_HANDLES + 1];

  bool inside = lookupWeights(*m_weights, q, w, jacobian ? gx : nullptr,
                              jacobian ? gy : nullptr);
  if (!inside) {
    if (jacobian)
      jacobian[0] = jacobian[3] = 1.0, jacobian[1] = jacobian[2] = 0.0;
    return q;
  }

  TPointD sum;
  double j[4] = {0.0, 0.0, 0.0, 0.0};

  // The anchor is the handle H, with the identity transform
  for (int k = 0; k <= H; ++k) {
    if (w[k] == 0.0 && (!jacobian || (gx[k] == 0.0 && gy[k] == 0.0))) continue;

    const TAffine *a = (k < H) ? &m_handles[k].m_transform : 0;
    TPointD t        = a ? (*a) * q : q;
    sum += t * w[k];

    if (jacobian) {
      j[0] += w[k] * (a ? a->a11 : 1.0) + t.x * gx[k];
      j[1] += w[k] * (a ? a->a12 : 0.0) + t.x * gy[k];
      j[2] += w[k] * (a ? a->a21 : 0.0) + t.y * gx[k];
      j[3] += w[k] * (a ? a->a22 : 1.0) + t.y * gy[k];
    }
  }

  if (jacobian) std::copy(j, j + 4, jacobian);
  return sum;
}

//---------------------------------------------------------------------------------

double TShapeAwareDeformer::weightsAt(const TPointD &p,
                                      std::vector<double> &weights) const {
  weights.assign(m_handles.size(), 0.0);
  if (!m_valid) return 1.0;

  double w[MAX_HANDLES + 1];
  lookupWeights(*m_weights, m_placementInv * p, w, 0, 0);

  const size_t H = m_handles.size();
  for (size_t k = 0; k < H; ++k) weights[k] = w[k];
  return w[H];
}

//---------------------------------------------------------------------------------

TPointD TShapeAwareDeformer::deform(const TPointD &p, double frame) const {
  if (!m_valid) return p;

  // Outside the grid the plane does not move
  TPointD q(m_placementInv * p);
  double w[MAX_HANDLES + 1];
  if (!lookupWeights(*m_weights, q, w, nullptr, nullptr)) return p;

  return m_placement * evaluate(q, nullptr);
}

//---------------------------------------------------------------------------------

bool TShapeAwareDeformer::undeform(const TPointD &p, double frame,
                                   TPointD &result) const {
  result = p;
  if (!m_valid) return true;

  // Solve deform(y) = t in the space of the handles
  const TPointD t(m_placementInv * p);

  double w[MAX_HANDLES + 1];
  if (!lookupWeights(*m_weights, t, w, 0, 0)) return true;

  // Start from the inverse transforms of the handles blended with the weights
  // at the target: exact where a single handle acts
  const int H = (int)m_handles.size();
  TPointD y   = t * w[H];
  for (int k = 0; k < H; ++k) y += (m_inverses[k] * t) * w[k];

  // Damped Newton steps, with the exact Jacobian of the blend
  const double tolerance = 1e-7 * m_scale;

  TPointD f(evaluate(y, 0) - t);
  double err = norm(f);

  for (int it = 0; it < 24 && err > tolerance; ++it) {
    double jac[4];
    evaluate(y, jac);

    double det = jac[0] * jac[3] - jac[1] * jac[2];
    if (!(std::fabs(det) > 1e-12)) break;  // Folded there

    TPointD step((jac[3] * f.x - jac[1] * f.y) / det,
                 (jac[0] * f.y - jac[2] * f.x) / det);

    bool improved = false;
    double lambda = 1.0;
    for (int k = 0; k < 5; ++k, lambda *= 0.5) {
      TPointD yNew(y - step * lambda);
      TPointD fNew(evaluate(yNew, 0) - t);
      double errNew = norm(fNew);
      if (errNew < err) {
        y = yNew, f = fNew, err = errNew;
        improved = true;
        break;
      }
    }
    if (!improved) break;  // No progress, e.g. near a fold
  }

  result = m_placement * y;
  return true;
}

//---------------------------------------------------------------------------------

namespace {

bool isInfiniteRect(const TRectD &rect) {
  const double big = 1e30;
  return rect.x0 <= -big || rect.y0 <= -big || rect.x1 >= big || rect.y1 >= big;
}

bool intersects(const TRectD &a, const TRectD &b) {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

TRectD unite(const TRectD &a, const TRectD &b) {
  return TRectD(std::min(a.x0, b.x0), std::min(a.y0, b.y0),
                std::max(a.x1, b.x1), std::max(a.y1, b.y1));
}

}  // namespace

// The plane does not move outside the domain, and what is in it ends in the
// bounds of its image: so the bounds are those of the rect and of the image of
// the domain, if the rect meets the domain.
TRectD TShapeAwareDeformer::deformedBounds(const TRectD &sourceRect,
                                           double frame) const {
  if (sourceRect.isEmpty()) return TRectD();
  if (isInfiniteRect(sourceRect) || !m_valid ||
      !intersects(sourceRect, m_domain))
    return sourceRect;

  return unite(sourceRect, m_deformedDomain);
}

//---------------------------------------------------------------------------------

TRectD TShapeAwareDeformer::sourceBounds(const TRectD &deformedRect,
                                         double frame) const {
  if (deformedRect.isEmpty()) return TRectD();
  if (isInfiniteRect(deformedRect) || !m_valid ||
      !intersects(deformedRect, m_deformedDomain))
    return deformedRect;

  return unite(deformedRect, m_domain);
}
