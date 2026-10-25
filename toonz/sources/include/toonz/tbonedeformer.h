#pragma once

#ifndef TBONEDEFORMER_H
#define TBONEDEFORMER_H

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
// TBoneDeformer
//
//================================================================================================

//! The TBoneDeformer deforms the plane with bones: segments given in a rest
//! pose and in a deformed pose, each moving the part of the plane around it.
/*!
  No mesh is needed. Each bone has a region of influence, the capsule of the
  points within a distance of its rest segment: the bone has the full
  influence within the inner radius and a smoothstep falloff from there to 0
  at the outer radius.

  The bone i moves the plane rigidly, by the transform M_i that takes its rest
  segment onto the deformed one: the rotation about the rest head that turns
  the rest direction to the deformed direction, followed by the translation of
  the rest head to the deformed head. The lengths of the two segments are not
  compared: a stretched bone does not stretch the plane, and its tail does not
  reach the deformed tail (a chain stretched this way tears at the joints
  rather than scaling).

  The deformation is the linear blend skinning of these transforms:
  deform(p) = sum_i w_i(p) M_i p. The weight of a bone is its influence at p
  divided by the sum of the influences when that sum is more than 1, so that
  bones overlapping at a joint share the plane; when the sum s is less than 1,
  the point also keeps the weight 1 - s of staying where it is. So a point out
  of the regions of all the bones does not move, and one in the falloff of a
  single bone moves partly with it. The deformation is continuous, smooth where
  a single bone acts, and exactly M_i within the inner radius of a bone that
  alone reaches there.

  Linear blending shortens a segment of the plane across a strong bend (the
  usual "collapsing joint" of skinning): keep the bend moderate or the radii
  of the neighbours wide enough. Where the bones fold the plane (deform() is
  not invertible), undeform() returns the best estimate found.

  The bones may form a tree: m_parent is the index of the parent bone, which
  must come before it in the list, or -1 for a root. The deformer does not
  use the parent to place the bones (every bone is given in absolute
  coordinates, in both poses), so a client animating a hierarchy composes the
  poses before the call; the parents are kept for the clients that need the
  hierarchy (a stack, an export). A chain is the particular tree where each
  bone's parent is the previous one, see makeChain().

  undeform(), which the raster warp needs once per pixel, inverts deform()
  with a few damped Newton steps (the Jacobian is taken by finite
  differences) started from the inverse of the bones' transforms blended with
  the weights at the target.

  The bones may be placed in the plane by an affine map, so that a deformer can
  be built in a render's space (see TDeformer) from bones defined in an fx's
  space. It must be invertible, and a similarity for the capsules to stay
  round: the radii are scaled by the square root of its determinant.

  A TBoneDeformer is a plain TDeformer, which can be a layer of a
  TDeformerStack.
*/
class DVAPI TBoneDeformer final : public TDeformer {
public:
  struct Bone {
    TPointD m_restHead, m_restTail;  //!< The rest segment
    TPointD m_head, m_tail;          //!< The deformed segment
    double m_inner, m_outer;         //!< Radii of the region of influence
    int m_parent;                    //!< Index of the parent, or -1

    Bone() : m_inner(0.0), m_outer(0.0), m_parent(-1) {}
    Bone(const TPointD &restHead, const TPointD &restTail, const TPointD &head,
         const TPointD &tail, double inner, double outer, int parent = -1)
        : m_restHead(restHead)
        , m_restTail(restTail)
        , m_head(head)
        , m_tail(tail)
        , m_inner(inner)
        , m_outer(outer)
        , m_parent(parent) {}
  };

private:
  // What deform() needs of a bone, in the plane
  struct Cache {
    TPointD m_a, m_ab;  // Rest head, and rest tail - rest head
    double m_len2;      // Squared length of m_ab
    double m_c, m_s;    // Cosine and sine of the rotation
    TPointD m_t;        // Translation: M p = R p + m_t
    double m_inner, m_outer;
  };

  std::vector<Bone> m_bones;  // In the plane (placement applied)
  std::vector<Cache> m_cache;
  double m_scale;  // Size of the bones, for the tolerances
  bool m_valid;

  static double influence(const Cache &c, const TPointD &p);
  static TPointD apply(const Cache &c, const TPointD &p);
  static TPointD applyInverse(const Cache &c, const TPointD &p);

public:
  //! Builds the deformer from the bones, in the space of the bones;
  //! placement, the identity by default, is the affine map from this space
  //! to the plane of the deformer, through which the bones are mapped.
  TBoneDeformer(const std::vector<Bone> &bones,
                const TAffine &placement = TAffine());

  //! Builds the bones of a chain through the joints, in rest pose and
  //! deformed: bone i goes from joint i to joint i + 1, has bone i - 1 as its
  //! parent, and the passed radii. Returns no bone if the lists differ in
  //! size or have fewer than 2 joints.
  static std::vector<Bone> makeChain(const std::vector<TPointD> &restJoints,
                                     const std::vector<TPointD> &joints,
                                     double inner, double outer);

  //! Returns whether the deformation is defined: there is at least one bone,
  //! all the values are finite, each parent is -1 or the index of a previous
  //! bone, and the placement is invertible. A deformer that is not valid does
  //! not move any point.
  bool isValid() const { return m_valid; }

  int getBoneCount() const { return (int)m_bones.size(); }

  //! Returns the bone i, in the plane (placement applied, radii scaled).
  const Bone &getBone(int index) const { return m_bones[index]; }

  //! Returns whether the deformer is valid and no end of a bone is moved
  //! farther than tolerance from its rest position.
  bool isIdentity(double tolerance) const;

  //! Fills weights with the weight of each bone at p, as used by deform(),
  //! and returns the weight of staying in place. They sum to 1.
  double weightsAt(const TPointD &p, std::vector<double> &weights) const;

  TPointD deform(const TPointD &p, double frame) const override;

  //! Always succeeds, even where the bones fold the plane; see the class
  //! description.
  bool undeform(const TPointD &p, double frame, TPointD &result) const override;
};

#endif  // TBONEDEFORMER_H
