#include "toonz/tfreeformdeformer.h"

#include <algorithm>
#include <cmath>

//****************************************************************************************
//    Local namespace stuff
//****************************************************************************************

namespace {

// Weights of the 4 control points of a span of a uniform cubic B-spline at
// the parameter t in [0, 1], and their derivatives with respect to t.
void splineBasis(double t, double b[4], double db[4]) {
  double t2 = t * t, t3 = t2 * t, s = 1.0 - t;

  b[0] = s * s * s / 6.0;
  b[1] = (3.0 * t3 - 6.0 * t2 + 4.0) / 6.0;
  b[2] = (-3.0 * t3 + 3.0 * t2 + 3.0 * t + 1.0) / 6.0;
  b[3] = t3 / 6.0;

  db[0] = -0.5 * s * s;
  db[1] = (3.0 * t2 - 4.0 * t) / 2.0;
  db[2] = (-3.0 * t2 + 2.0 * t + 1.0) / 2.0;
  db[3] = 0.5 * t2;
}

//---------------------------------------------------------------------------------

// Clamps the grid coordinate u to [0, n], and tells whether it was inside
// (the NaNs are brought to 0). Returns the index of the span of u and the
// parameter t of u in the span.
bool locate(double &u, int n, int &span, double &t) {
  bool inside = true;
  // The negated test also catches NaNs
  if (!(u >= 0.0))
    u = 0.0, inside = false;
  else if (u > n)
    u = n, inside = false;

  span = std::min((int)std::floor(u), n - 1);
  t    = u - span;
  return inside;
}

}  // namespace

//****************************************************************************************
//    TFreeFormDeformer  implementation
//****************************************************************************************

TFreeFormDeformer::TFreeFormDeformer(const TRectD &restRect, int nx, int ny,
                                     const std::vector<TPointD> &deformedPoints,
                                     const TAffine &placement)
    : m_nx(nx), m_ny(ny), m_valid(false) {
  if (nx < 1 || ny < 1 || deformedPoints.size() != (size_t)(nx + 1) * (ny + 1))
    return;

  double lx = restRect.x1 - restRect.x0, ly = restRect.y1 - restRect.y0;
  // The negated tests also reject NaNs
  if (!(lx > 0.0 && ly > 0.0) || !(std::fabs(placement.det()) > 1e-18)) return;

  // The grid of the lattice's own space; the placement maps it to the plane.
  const TAffine gridToLattice =
      TTranslation(restRect.x0, restRect.y0) * TScale(lx / nx, ly / ny);
  m_fromGrid = placement * gridToLattice;
  m_toGrid   = m_fromGrid.inv();

  // Displacements of the control points, in grid units. The deformed points
  // are in the lattice's space, like the rect: they go to the grid without
  // the placement, which the grid space already includes.
  const TAffine latticeToGrid = gridToLattice.inv();
  const int w                 = nx + 3;
  m_coefs.assign((size_t)w * (ny + 3), TPointD());
  auto coef = [&](int i, int j) -> TPointD & {
    return m_coefs[(size_t)(j + 1) * w + (i + 1)];
  };

  for (int j = 0; j <= ny; ++j)
    for (int i = 0; i <= nx; ++i)
      coef(i, j) = latticeToGrid * deformedPoints[(size_t)j * (nx + 1) + i] -
                   TPointD(i, j);

  // Pad with a linear extrapolation, which makes the spline reproduce linear
  // displacements and interpolate the corners.
  for (int j = 0; j <= ny; ++j) {
    coef(-1, j)     = coef(0, j) * 2.0 - coef(1, j);
    coef(nx + 1, j) = coef(nx, j) * 2.0 - coef(nx - 1, j);
  }
  for (int i = -1; i <= nx + 1; ++i) {
    coef(i, -1)     = coef(i, 0) * 2.0 - coef(i, 1);
    coef(i, ny + 1) = coef(i, ny) * 2.0 - coef(i, ny - 1);
  }

  m_valid = true;
}

//---------------------------------------------------------------------------------

TPointD TFreeFormDeformer::getRestPoint(int i, int j) const {
  return m_fromGrid * TPointD(i, j);
}

//---------------------------------------------------------------------------------

TPointD TFreeFormDeformer::getDeformedPoint(int i, int j) const {
  if (!m_valid) return TPointD(i, j);
  return m_fromGrid *
         (TPointD(i, j) + m_coefs[(size_t)(j + 1) * (m_nx + 3) + (i + 1)]);
}

//---------------------------------------------------------------------------------

bool TFreeFormDeformer::isIdentity(double tolerance) const {
  if (!m_valid) return false;

  for (int j = 0; j <= m_ny; ++j)
    for (int i = 0; i <= m_nx; ++i) {
      TPointD d(m_coefs[(size_t)(j + 1) * (m_nx + 3) + (i + 1)]);
      if (norm(m_fromGrid.transformDirection(d)) > tolerance) return false;
    }
  return true;
}

//---------------------------------------------------------------------------------

void TFreeFormDeformer::evalDisplacement(const TPointD &g, TPointD &d,
                                         TPointD &dU, TPointD &dV) const {
  double u = g.x, v = g.y;
  int i0, j0;
  double tu, tv;
  bool insideU = locate(u, m_nx, i0, tu);
  bool insideV = locate(v, m_ny, j0, tv);

  double bu[4], dbu[4], bv[4], dbv[4];
  splineBasis(tu, bu, dbu);
  splineBasis(tv, bv, dbv);

  d  = TPointD();
  dU = TPointD();
  dV = TPointD();

  // The span of parameter t has the control points span - 1 to span + 2,
  // which are at the padded indices span to span + 3.
  const int w = m_nx + 3;
  for (int b = 0; b < 4; ++b) {
    const TPointD *row = &m_coefs[(size_t)(j0 + b) * w + i0];
    for (int a = 0; a < 4; ++a) {
      d += row[a] * (bu[a] * bv[b]);
      dU += row[a] * (dbu[a] * bv[b]);
      dV += row[a] * (bu[a] * dbv[b]);
    }
  }

  if (!insideU) dU = TPointD();
  if (!insideV) dV = TPointD();
}

//---------------------------------------------------------------------------------

TPointD TFreeFormDeformer::deform(const TPointD &p, double frame) const {
  if (!m_valid) return p;

  TPointD g(m_toGrid * p), d, dU, dV;
  evalDisplacement(g, d, dU, dV);
  return m_fromGrid * (g + d);
}

//---------------------------------------------------------------------------------

bool TFreeFormDeformer::undeform(const TPointD &p, double frame,
                                 TPointD &result) const {
  if (!m_valid) {
    result = p;
    return true;
  }

  // Solve g + D(g) = target in grid space, where one unit is one cell of the
  // lattice, by Newton's method started from the displacement taken at the
  // target. The displacement is smooth and, unless the lattice is folded, a
  // small change of the identity, so a few steps are enough.
  TPointD target(m_toGrid * p);
  const double tolerance =
      1e-9 * (1.0 + std::fabs(target.x) + std::fabs(target.y));

  TPointD d, dU, dV;
  evalDisplacement(target, d, dU, dV);

  TPointD g(target - d);
  evalDisplacement(g, d, dU, dV);
  TPointD f(g + d - target);
  double err = norm(f);

  for (int it = 0; it < 16 && err > tolerance; ++it) {
    // Jacobian of g + D(g), whose columns are 1 + dD/du and dD/dv
    double j00 = 1.0 + dU.x, j01 = dV.x, j10 = dU.y, j11 = 1.0 + dV.y;
    double det = j00 * j11 - j01 * j10;
    if (!(std::fabs(det) > 1e-12)) break;  // Folded there

    TPointD step(j11 * f.x - j01 * f.y, j00 * f.y - j10 * f.x);
    step = step * (1.0 / det);

    // Damp the step until it reduces the error
    bool improved = false;
    double lambda = 1.0;
    for (int k = 0; k < 5; ++k, lambda *= 0.5) {
      TPointD gNew(g - step * lambda), dNew, dUNew, dVNew;
      evalDisplacement(gNew, dNew, dUNew, dVNew);

      TPointD fNew(gNew + dNew - target);
      double errNew = norm(fNew);
      if (errNew < err) {
        g = gNew, d = dNew, dU = dUNew, dV = dVNew, f = fNew, err = errNew;
        improved = true;
        break;
      }
    }
    if (!improved) break;  // No progress, e.g. near a fold
  }

  result = m_fromGrid * g;
  return true;
}
