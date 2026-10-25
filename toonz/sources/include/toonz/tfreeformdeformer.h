#pragma once

#ifndef TFREEFORMDEFORMER_H
#define TFREEFORMDEFORMER_H

#include "toonz/tdeformer.h"

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
// TFreeFormDeformer
//
//================================================================================================

//! The TFreeFormDeformer deforms the plane with a lattice of control points:
//! a regular grid of (NX + 1) x (NY + 1) points over a rectangle (the rest
//! lattice), each of which is moved to a deformed position.
/*!
  The displacement of the plane is the tensor product cubic B-spline of the
  displacements of the control points (free-form deformation, Sederberg and
  Parry 1986): deform(p) = p + D(p), where D is the spline of the vectors
  deformed - rest. The spline is C2, so the deformation is smooth and moves
  the plane locally, a control point mostly affecting the cells around it.
  Beyond the lattice's border, the spline continues the control points'
  displacement linearly, so that it reproduces exactly any translation and
  any affine map of the lattice, and it interpolates the four corner control
  points. The other control points are not interpolated: as with any
  B-spline, the art near one follows it by a fraction of its displacement
  (2/3 of it along a side, 4/9 of it inside), so the lattice must be moved a
  bit farther to place the art.

  Outside the lattice's rectangle the displacement is held at its value on
  the nearest point of the border, so the rest of the plane is translated
  along with the border and the deformation is continuous (but not smooth)
  across the border. Keep the art inside the rectangle for the whole smooth
  deformation.

  The deformation is evaluated directly by deform(). undeform(), which the
  raster warp needs once per pixel, inverts it with a few damped Newton steps
  using the spline's exact derivative. Where the lattice is folded (the
  deformation is not invertible) it returns the best estimate found, which
  does not map exactly to the passed point.

  The lattice may be placed in the plane by an affine map, so that a deformer
  can be built in a render's space (see TDeformer) from a lattice defined in
  an fx's space: the deformation is then the same as the unplaced one, seen
  through the map. It must be invertible.

  A TFreeFormDeformer is a plain TDeformer, which can be a layer of a
  TDeformerStack.
*/
class DVAPI TFreeFormDeformer final : public TDeformer {
  int m_nx, m_ny;  //!< Cells of the lattice along each axis
  //! Maps the plane to the grid space, where the rest control point (i, j)
  //! is at (i, j), and back.
  TAffine m_toGrid, m_fromGrid;
  //! Displacements, in grid units, of the control points (i, j) with i in
  //! [-1, NX + 1] and j in [-1, NY + 1]: the lattice's ones padded with a
  //! linear extrapolation on every side.
  std::vector<TPointD> m_coefs;
  bool m_valid;

  // Evaluates the displacement at the grid point g, clamped to the lattice,
  // and the derivatives of the displacement with respect to the coordinates
  // of g (both null outside the lattice along the clamped coordinate).
  void evalDisplacement(const TPointD &g, TPointD &d, TPointD &dU,
                        TPointD &dV) const;

public:
  //! Builds the deformer from the rest lattice, which is the regular grid of
  //! nx x ny cells over restRect, and the deformed position of its control
  //! points, listed row by row from the bottom row to the top one, each from
  //! left to right: (nx + 1) * (ny + 1) points, the point (i, j) at index
  //! j * (nx + 1) + i. The rect and the deformed points are in the same
  //! space; placement, the identity by default, is the affine map from this
  //! space to the plane of the deformer: the rest lattice and the deformed
  //! points are mapped through it.
  TFreeFormDeformer(const TRectD &restRect, int nx, int ny,
                    const std::vector<TPointD> &deformedPoints,
                    const TAffine &placement = TAffine());

  //! Returns whether the deformation is defined: nx and ny are at least 1,
  //! there are (nx + 1) * (ny + 1) deformed points, the rect has a positive
  //! area, and the placement is invertible. A deformer that is not valid does
  //! not move any point.
  bool isValid() const { return m_valid; }

  int getNX() const { return m_nx; }
  int getNY() const { return m_ny; }

  //! Returns the rest position, in the plane, of the control point (i, j).
  TPointD getRestPoint(int i, int j) const;

  //! Returns the deformed position, in the plane, of the control point
  //! (i, j).
  TPointD getDeformedPoint(int i, int j) const;

  //! Returns whether the deformer is valid and no control point is moved
  //! farther than tolerance from its rest position.
  bool isIdentity(double tolerance) const;

  TPointD deform(const TPointD &p, double frame) const override;

  //! Always succeeds, even where the deformation is folded; see the class
  //! description.
  bool undeform(const TPointD &p, double frame, TPointD &result) const override;
};

#endif  // TFREEFORMDEFORMER_H
