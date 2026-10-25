#include <memory>

// TnzCore includes
#include "tthreadmessage.h"

// TnzExt includes
#include "ext/plasticskeleton.h"
#include "ext/plasticskeletondeformation.h"

// tcg includes
#include "tcg/tcg_point_ops.h"

// STD includes
#include <limits>
#include <map>
#include <algorithm>

// Boost includes
#include <boost/multi_index_container.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

// Qt includes
#include <QMutex>
#include <QMutexLocker>

#include "ext/plasticdeformerstorage.h"

//***********************************************************************************************
//    Storage multi-index map  definition
//***********************************************************************************************

namespace {

typedef PlasticDeformerDataGroup DataGroup;

//----------------------------------------------------------------------------------

typedef std::pair<const SkD *, int> DeformedSkeleton;

//----------------------------------------------------------------------------------

struct Key {
  const TMeshImage *m_mi;
  DeformedSkeleton m_ds;

  std::shared_ptr<DataGroup> m_dataGroup;

public:
  Key(const TMeshImage *mi, const SkD *sd, int skelId)
      : m_mi(mi), m_ds(sd, skelId), m_dataGroup() {}

  bool operator<(const Key &other) const {
    return (m_mi < other.m_mi) ||
           ((!(other.m_mi < m_mi)) && (m_ds < other.m_ds));
  }
};

//----------------------------------------------------------------------------------

using namespace boost::multi_index;

typedef boost::multi_index_container<
    Key, indexed_by<

             ordered_unique<identity<Key>>,
             ordered_non_unique<tag<TMeshImage>,
                                member<Key, const TMeshImage *, &Key::m_mi>>,
             ordered_non_unique<tag<DeformedSkeleton>,
                                member<Key, DeformedSkeleton, &Key::m_ds>>

             >>
    DeformersSet;

typedef DeformersSet::nth_index<0>::type DeformersByKey;
typedef DeformersSet::index<TMeshImage>::type DeformersByMeshImage;
typedef DeformersSet::index<DeformedSkeleton>::type DeformersByDeformedSkeleton;

}  // namespace

//***********************************************************************************************
//    Initialization stage  functions
//***********************************************************************************************

namespace {

void initializeSO(PlasticDeformerData &data, const TTextureMeshP &mesh) {
  data.m_so.reset(new double[mesh->facesCount()]);
}

//----------------------------------------------------------------------------------

void initializeDeformerData(PlasticDeformerData &data,
                            const TTextureMeshP &mesh) {
  initializeSO(data, mesh);  // Allocates SO data

  // Also, allocate suitable input-output arrays for the deformation
  data.m_output.reset(new double[2 * mesh->verticesCount()]);
}

//----------------------------------------------------------------------------------

void initializeDeformersData(DataGroup *group, const TMeshImage *meshImage) {
  group->m_datas.reset(new PlasticDeformerData[meshImage->meshes().size()]);

  // Push a PlasticDeformer for each mesh in the image
  const std::vector<TTextureMeshP> &meshes = meshImage->meshes();
  int fTotal                               = 0;  // Also count total # of faces

  int m, mCount = meshes.size();
  for (m = 0; m != mCount; ++m) {
    fTotal += meshes[m]->facesCount();
    initializeDeformerData(group->m_datas[m], meshes[m]);
  }

  // Initialize the vector of sorted faces
  std::vector<std::pair<int, int>> &sortedFaces = group->m_sortedFaces;

  sortedFaces.reserve(fTotal);
  for (m = 0; m != mCount; ++m) {
    const TTextureMesh &mesh = *meshes[m];

    int f, fCount = mesh.facesCount();
    for (f = 0; f != fCount; ++f) sortedFaces.push_back(std::make_pair(f, m));
  }
}

}  // namespace

//***********************************************************************************************
//    Handle processing  functions
//***********************************************************************************************

namespace {

void transformHandles(std::vector<PlasticHandle> &handles, const TAffine &aff) {
  // Transforms handles through deformAff AND applies mi's dpi scale inverse
  std::vector<PlasticHandle>::size_type h, hCount = handles.size();
  for (h = 0; h != hCount; ++h) handles[h].m_pos = aff * handles[h].m_pos;
}

//----------------------------------------------------------------------------------

void transformHandles(std::vector<TPointD> &handles, const TAffine &aff) {
  // Transforms handles through deformAff AND applies mi's dpi scale inverse
  std::vector<PlasticHandle>::size_type h, hCount = handles.size();
  for (h = 0; h != hCount; ++h) handles[h] = aff * handles[h];
}

//----------------------------------------------------------------------------------

/*
  Curve bending

  When curve bending is enabled, the deformation of each skeleton edge is
  distributed along a smooth curve instead of following the edge rigidly. The
  curve is a chord-length weighted Catmull-Rom spline through the skeleton's
  vertex chains: it is tangent-continuous through vertices with exactly one
  child, while it follows the edge direction at branch and end vertices.

  The same curve is built through both the original and the deformed skeleton.
  Points sampled at the same parameters on both become additional handles for
  the mesh deformers - so the original skeleton is still mapped exactly to
  the original mesh, and the existing deformer bends the mesh smoothly.
*/

const int l_curveSamplesPerEdge = 3;  //!< Curve handles built for each edge

//----------------------------------------------------------------------------------

//! Returns the only child of the specified vertex, or -1 if it has no or
//! multiple children.
int singleChild(const PlasticSkeleton &skeleton, int v) {
  const PlasticSkeleton::vertex_type &vx = skeleton.vertex(v);

  int child = -1;

  PlasticSkeleton::vertex_type::edges_const_iterator et, eEnd(vx.edgesEnd());
  for (et = vx.edgesBegin(); et != eEnd; ++et) {
    int vChild = skeleton.edge(*et).vertex(1);
    if (vChild == v) continue;

    if (child >= 0) return -1;
    child = vChild;
  }

  return child;
}

//----------------------------------------------------------------------------------

//! Returns the Hermite tangent at a vertex of a chord-length weighted
//! Catmull-Rom spline, for a curve segment of length len.
inline TPointD curveTangent(const TPointD &prev, const TPointD &next,
                            double prevLen, double len) {
  double den = prevLen + len;
  return (den > 1e-8) ? (len / den) * (next - prev) : TPointD();
}

//----------------------------------------------------------------------------------

//! Builds the curve handles of the specified skeleton - a fixed number per
//! edge, in edge order. Skeletons with the same topology (eg the original and
//! the deformed skeleton) produce corresponding handles.
void buildCurveHandles(const PlasticSkeleton &skeleton,
                       std::vector<PlasticHandle> &handles) {
  handles.clear();
  handles.reserve(l_curveSamplesPerEdge * skeleton.edgesCount());

  tcg::list<PlasticSkeleton::edge_type>::const_iterator et,
      eEnd(skeleton.edges().end());
  for (et = skeleton.edges().begin(); et != eEnd; ++et) {
    int v1 = et->vertex(0), v2 = et->vertex(1);  // v1 is v2's parent

    const PlasticSkeleton::vertex_type &vx1 = skeleton.vertex(v1);
    const PlasticSkeleton::vertex_type &vx2 = skeleton.vertex(v2);

    // Retrieve the chain's previous and next vertices, if any
    int v0 = (vx1.parent() >= 0 && singleChild(skeleton, v1) == v2)
                 ? vx1.parent()
                 : -1;
    int v3 = singleChild(skeleton, v2);

    const TPointD &p1 = vx1.P(), &p2 = vx2.P();
    const TPointD p0 = (v0 >= 0) ? skeleton.vertex(v0).P() : 2.0 * p1 - p2;
    const TPointD p3 = (v3 >= 0) ? skeleton.vertex(v3).P() : 2.0 * p2 - p1;

    double l01 = tcg::point_ops::dist(p0, p1),
           l12 = tcg::point_ops::dist(p1, p2),
           l23 = tcg::point_ops::dist(p2, p3);

    const TPointD m1 = ::curveTangent(p0, p2, l01, l12),
                  m2 = ::curveTangent(p1, p3, l23, l12);

    // Sample the curve segment (Hermite form), excluding its end points
    for (int s = 1; s <= l_curveSamplesPerEdge; ++s) {
      double t = s / double(l_curveSamplesPerEdge + 1), t2 = t * t, t3 = t2 * t;

      PlasticHandle handle((2.0 * t3 - 3.0 * t2 + 1.0) * p1 +
                           (t3 - 2.0 * t2 + t) * m1 +
                           (3.0 * t2 - 2.0 * t3) * p2 + (t3 - t2) * m2);
      handle.m_interpolate = vx1.m_interpolate && vx2.m_interpolate;

      handles.push_back(handle);
    }
  }
}

//----------------------------------------------------------------------------------

//! Selects the curve handles to be compiled in a mesh's deformer. Handles are
//! discarded if outside the mesh, or if their face already contains another
//! handle - as too many constraints on the same face may make the deformer
//! fail compilation.
void selectCurveHandles(const TTextureMesh &mesh,
                        const std::vector<PlasticHandle> &handles,
                        const std::vector<int> &faceHints,
                        const std::vector<PlasticHandle> &curveHandles,
                        std::vector<int> &selected,
                        std::vector<int> &selectedFaces) {
  selected.clear(), selectedFaces.clear();

  int fCount = mesh.facesCount();
  std::vector<char> usedFaces(fCount, 0);

  int h, hCount = int(handles.size());
  for (h = 0; h != hCount; ++h) {
    int f = (h < int(faceHints.size())) ? faceHints[h] : -1;
    if (f < 0 || f >= fCount || !mesh.faceContains(f, handles[h].m_pos))
      f = mesh.faceContaining(handles[h].m_pos);

    if (f >= 0) usedFaces[f] = 1;
  }

  int c, cCount = int(curveHandles.size());
  for (c = 0; c != cCount; ++c) {
    int f = mesh.faceContaining(curveHandles[c].m_pos);
    if (f < 0 || usedFaces[f]) continue;

    usedFaces[f] = 1;
    selected.push_back(c), selectedFaces.push_back(f);
  }
}

//----------------------------------------------------------------------------------

void processHandles(DataGroup *group, double frame, const TMeshImage *meshImage,
                    const SkD *sd, int skelId,
                    const TAffine &deformationAffine) {
  assert(sd);

  const PlasticSkeletonP &skeleton = sd->skeleton(skelId);

  if (!skeleton || skeleton->verticesCount() == 0) {
    group->m_handles.clear();
    group->m_dstHandles.clear();

    if (!group->m_curveHandles.empty()) {
      // Mesh deformers reference curve handles - they must be recompiled
      group->m_curveHandles.clear();
      group->m_compiled &= ~PlasticDeformerStorage::MESH;
    }
    group->m_curveDstHandles.clear();

    group->m_compiled |= PlasticDeformerStorage::HANDLES;
    group->m_upToDate |= PlasticDeformerStorage::HANDLES;

    return;
  }

  int mCount = meshImage->meshes().size();

  if (!(group->m_upToDate & PlasticDeformerStorage::HANDLES)) {
    // Compile handles if necessary
    if (!(group->m_compiled & PlasticDeformerStorage::HANDLES)) {
      // Build and transform handles
      group->m_handles = skeleton->verticesToHandles();
      ::transformHandles(group->m_handles, deformationAffine);

      // Prepare a vector for handles' face hints
      for (int m = 0; m != mCount; ++m)
        group->m_datas[m].m_faceHints.resize(group->m_handles.size(), -1);

      // Build curve handles, if required
      bool hadCurveHandles = !group->m_curveHandles.empty();

      if (sd->curveBending()) {
        ::buildCurveHandles(*skeleton, group->m_curveHandles);
        ::transformHandles(group->m_curveHandles, deformationAffine);
      } else
        group->m_curveHandles.clear();

      if (hadCurveHandles || !group->m_curveHandles.empty())
        group->m_compiled &=
            ~PlasticDeformerStorage::MESH;  // Mesh deformers reference curve
                                            // handles by index

      group->m_compiled |= PlasticDeformerStorage::HANDLES;
    }

    // Then, build destination handles
    PlasticSkeleton
        deformedSkeleton;  // NOTE: Could this be moved to the group as well?
    sd->storeDeformedSkeleton(skelId, frame, deformedSkeleton);

    // Copy deformed skeleton data into input deformation parameters
    group->m_dstHandles = std::vector<TPointD>(
        deformedSkeleton.vertices().begin(), deformedSkeleton.vertices().end());
    ::transformHandles(group->m_dstHandles, deformationAffine);

    // Build destination curve handles, sampling the deformed skeleton's curves
    group->m_curveDstHandles.clear();

    if (!group->m_curveHandles.empty()) {
      std::vector<PlasticHandle> curveDstHandles;
      ::buildCurveHandles(deformedSkeleton, curveDstHandles);

      assert(curveDstHandles.size() == group->m_curveHandles.size());
      if (curveDstHandles.size() == group->m_curveHandles.size()) {
        group->m_curveDstHandles.reserve(curveDstHandles.size());
        for (const PlasticHandle &handle : curveDstHandles)
          group->m_curveDstHandles.push_back(handle.m_pos);

        ::transformHandles(group->m_curveDstHandles, deformationAffine);
      } else {
        // Should not happen - anyway, just deform without curve handles
        group->m_curveHandles.clear();
        group->m_compiled &= ~PlasticDeformerStorage::MESH;
      }
    }

    group->m_upToDate |= PlasticDeformerStorage::HANDLES;
  }
}

}  // namespace

//***********************************************************************************************
//    Stacking Order processing  functions
//***********************************************************************************************

namespace {

bool updateHandlesSO(DataGroup *group, const SkD *sd, int skelId,
                     double frame) {
  assert(sd);

  const PlasticSkeletonP &skeleton = sd->skeleton(skelId);

  if (!skeleton || skeleton->verticesCount() == 0) {
    group->m_soMin = group->m_soMax = 0.0;
    return false;
  }

  // Copy SO values to data's handles
  // Return whether values changed with respect to previous ones
  bool changed = false;

  assert(group->m_handles.size() == skeleton->verticesCount());

  int h, hCount = group->m_handles.size();
  {
    tcg::list<PlasticSkeletonVertex>::iterator vt =
        skeleton->vertices().begin();

    for (h = 0; h != hCount; ++h, ++vt) {
      const SkVD *vd = sd->vertexDeformation(vt->name());
      if (!vd) continue;

      double so = vd->m_params[SkVD::SO]->getValue(frame);

      PlasticHandle &handle = group->m_handles[h];
      if (handle.m_so != so) {
        group->m_handles[h].m_so = so;
        changed                  = true;
      }
    }
  }

  if (changed) {
    // Rebuild SO minmax
    group->m_soMax = -(group->m_soMin = (std::numeric_limits<double>::max)());

    for (h = 0; h != hCount; ++h) {
      const double &so = group->m_handles[h].m_so;

      group->m_soMin = std::min(group->m_soMin, so);
      group->m_soMax = std::max(group->m_soMax, so);
    }
  }

  return changed;
}

//----------------------------------------------------------------------------------

void interpolateSO(DataGroup *group, const TMeshImage *meshImage) {
  int m, mCount = meshImage->meshes().size();

  if (group->m_handles.size() == 0) {
    // No handles case, fill in with 0s

    for (m = 0; m != mCount; ++m) {
      const TTextureMesh &mesh  = *meshImage->meshes()[m];
      PlasticDeformerData &data = group->m_datas[m];

      std::fill(data.m_so.get(), data.m_so.get() + mesh.facesCount(), 0.0);
    }

    return;
  }

  // Apply handles' SO values to each mesh
  for (m = 0; m != mCount; ++m) {
    const TTextureMesh &mesh  = *meshImage->meshes()[m];
    PlasticDeformerData &data = group->m_datas[m];

    // Interpolate so values
    std::unique_ptr<double[]> verticesSO(new double[mesh.verticesCount()]);

    ::buildSO(verticesSO.get(), mesh, group->m_handles,
              &data.m_faceHints.front());

    // Make the mean of each face's vertex values and store that
    int f, fCount = mesh.facesCount();
    for (f = 0; f != fCount; ++f) {
      int v0, v1, v2;
      mesh.faceVertices(f, v0, v1, v2);

      data.m_so[f] = (verticesSO[v0] + verticesSO[v1] + verticesSO[v2]) / 3.0;
    }
  }
}

//----------------------------------------------------------------------------------

struct FaceLess {
  const PlasticDeformerDataGroup *m_group;

public:
  FaceLess(const PlasticDeformerDataGroup *group) : m_group(group) {}

  bool operator()(const std::pair<int, int> &a, const std::pair<int, int> &b) {
    return (m_group->m_datas[a.second].m_so[a.first] <
            m_group->m_datas[b.second].m_so[b.first]);
  }
};

// Must be invoked after updateSO
void updateSortedFaces(PlasticDeformerDataGroup *group) {
  FaceLess comp(group);
  std::sort(group->m_sortedFaces.begin(), group->m_sortedFaces.end(), comp);
}

//----------------------------------------------------------------------------------

void processSO(DataGroup *group, double frame, const TMeshImage *meshImage,
               const SkD *sd, int skelId, const TAffine &deformationAffine) {
  // SO re-interpolate values along the mesh if either:
  //  1. Recompilation was requested (ie some vertex may have been
  //  added/removed)
  //  2. OR the value of one of the handle has changed

  bool interpolate = !(group->m_compiled & PlasticDeformerStorage::SO);

  if (!(group->m_upToDate &
        PlasticDeformerStorage::SO))  // implied by (interpolate == true)
  {
    interpolate = updateHandlesSO(group, sd, skelId, frame) ||
                  interpolate;  // Order is IMPORTANT

    if (interpolate) {
      interpolateSO(group, meshImage);
      updateSortedFaces(group);
    }

    group->m_compiled |= PlasticDeformerStorage::SO;
    group->m_upToDate |= PlasticDeformerStorage::SO;
  }
}

}  // namespace

//***********************************************************************************************
//    Mesh Deform processing  functions
//***********************************************************************************************

namespace {

void processMesh(DataGroup *group, double frame, const TMeshImage *meshImage,
                 const SkD *sd, int skelId, const TAffine &deformationAffine) {
  if (!(group->m_upToDate & PlasticDeformerStorage::MESH)) {
    int m, mCount = meshImage->meshes().size();

    if (!(group->m_compiled & PlasticDeformerStorage::MESH)) {
      for (m = 0; m != mCount; ++m) {
        const TTextureMeshP &mesh = meshImage->meshes()[m];
        PlasticDeformerData &data = group->m_datas[m];

        data.m_curveSamples.clear();
        data.m_deformer.initialize(mesh);

        if (!group->m_curveHandles.empty()) {
          // Curve bending - compile the curve handles after skeleton ones
          std::vector<int> curveFaces;
          ::selectCurveHandles(*mesh, group->m_handles, data.m_faceHints,
                               group->m_curveHandles, data.m_curveSamples,
                               curveFaces);

          if (!data.m_curveSamples.empty()) {
            std::vector<PlasticHandle> handles(group->m_handles);
            std::vector<int> faceHints(data.m_faceHints);

            faceHints.resize(handles.size(), -1);

            int s, sCount = int(data.m_curveSamples.size());
            for (s = 0; s != sCount; ++s) {
              handles.push_back(group->m_curveHandles[data.m_curveSamples[s]]);
              faceHints.push_back(curveFaces[s]);
            }

            if (data.m_deformer.compile(handles, &faceHints.front())) {
              // Keep the skeleton handles' updated face hints
              std::copy(faceHints.begin(),
                        faceHints.begin() + data.m_faceHints.size(),
                        data.m_faceHints.begin());
            } else {
              // Fall back to the standard deformation
              data.m_curveSamples.clear();
              data.m_deformer.initialize(mesh);
            }
          }
        }

        if (data.m_curveSamples.empty())
          data.m_deformer.compile(
              group->m_handles,
              data.m_faceHints.empty() ? 0 : &data.m_faceHints.front());

        data.m_deformer.releaseInitializedData();
      }

      group->m_compiled |= PlasticDeformerStorage::MESH;
    }

    const TPointD *dstHandlePos =
        group->m_dstHandles.empty() ? 0 : &group->m_dstHandles.front();

    for (m = 0; m != mCount; ++m) {
      PlasticDeformerData &data = group->m_datas[m];

      if (data.m_curveSamples.empty()) {
        data.m_deformer.deform(dstHandlePos, data.m_output.get());
        continue;
      }

      // Append the destination curve handles compiled in this deformer
      std::vector<TPointD> &dstHandles = data.m_dstHandlesBuffer;
      dstHandles.assign(group->m_dstHandles.begin(), group->m_dstHandles.end());

      for (int s : data.m_curveSamples) {
        assert(s < int(group->m_curveDstHandles.size()));
        dstHandles.push_back(group->m_curveDstHandles[s]);
      }

      data.m_deformer.deform(&dstHandles.front(), data.m_output.get());
    }

    group->m_upToDate |= PlasticDeformerStorage::MESH;
  }
}

}  // namespace

//***********************************************************************************************
//    PlasticDeformerData  implementation
//***********************************************************************************************

PlasticDeformerData::PlasticDeformerData() {}

//----------------------------------------------------------------------------------

PlasticDeformerData::~PlasticDeformerData() {}

//***********************************************************************************************
//    PlasticDeformerDataGroup  implementation
//***********************************************************************************************

PlasticDeformerDataGroup::PlasticDeformerDataGroup()
    : m_datas()
    , m_compiled(PlasticDeformerStorage::NONE)
    , m_upToDate(PlasticDeformerStorage::NONE)
    , m_outputFrame((std::numeric_limits<double>::max)())
    , m_soMin()
    , m_soMax() {}

//----------------------------------------------------------------------------------

PlasticDeformerDataGroup::~PlasticDeformerDataGroup() {}

//***********************************************************************************************
//    PlasticDeformerStorage::Imp  definition
//***********************************************************************************************

class PlasticDeformerStorage::Imp {
public:
  TThread::Mutex m_mutex;    //!< Access mutex - needed for thread-safety
  DeformersSet m_deformers;  //!< Set of deformers, ordered by mesh image,
                             //! deformation, and affine.

public:
  Imp() {}
};

//***********************************************************************************************
//    PlasticDeformerStorage  implementation
//***********************************************************************************************

PlasticDeformerStorage::PlasticDeformerStorage() : m_imp(new Imp) {}

//----------------------------------------------------------------------------------

PlasticDeformerStorage::~PlasticDeformerStorage() {}

//----------------------------------------------------------------------------------

PlasticDeformerStorage *PlasticDeformerStorage::instance() {
  static PlasticDeformerStorage theInstance;
  return &theInstance;
}

//----------------------------------------------------------------------------------

PlasticDeformerDataGroup *PlasticDeformerStorage::deformerData(
    const TMeshImage *meshImage, const PlasticSkeletonDeformation *deformation,
    int skelId) {
  QMutexLocker locker(&m_imp->m_mutex);

  // Search for the corresponding deformation in the storage
  Key key(meshImage, deformation, skelId);

  DeformersByKey::iterator dt = m_imp->m_deformers.find(key);
  if (dt == m_imp->m_deformers.end()) {
    // No deformer was found. Allocate it.
    key.m_dataGroup = std::make_shared<PlasticDeformerDataGroup>();
    initializeDeformersData(key.m_dataGroup.get(), meshImage);

    dt = m_imp->m_deformers.insert(key).first;
  }

  return dt->m_dataGroup.get();
}

//----------------------------------------------------------------------------------

const PlasticDeformerDataGroup *PlasticDeformerStorage::process(
    double frame, const TMeshImage *meshImage,
    const PlasticSkeletonDeformation *deformation, int skelId,
    const TAffine &skeletonAffine, DataType dataType) {
  QMutexLocker locker(&m_imp->m_mutex);

  PlasticDeformerDataGroup *group =
      deformerData(meshImage, deformation, skelId);

  // On-the-fly checks for data invalidation
  if (group->m_skeletonAffine != skeletonAffine) {
    group->m_upToDate       = NONE;
    group->m_compiled       = NONE;
    group->m_skeletonAffine = skeletonAffine;
  }

  if (group->m_outputFrame != frame) {
    group->m_upToDate    = NONE;
    group->m_outputFrame = frame;
  }

  bool doMesh    = (dataType & MESH);
  bool doSO      = (dataType & SO) || doMesh;
  bool doHandles = (bool)dataType;

  // Process data
  if (doHandles)
    processHandles(group, frame, meshImage, deformation, skelId,
                   skeletonAffine);

  if (doSO)
    processSO(group, frame, meshImage, deformation, skelId, skeletonAffine);

  if (doMesh)
    processMesh(group, frame, meshImage, deformation, skelId, skeletonAffine);

  return group;
}

//----------------------------------------------------------------------------------

const PlasticDeformerDataGroup *PlasticDeformerStorage::processOnce(
    double frame, const TMeshImage *meshImage,
    const PlasticSkeletonDeformation *deformation, int skelId,
    const TAffine &skeletonAffine, DataType dataType) {
  PlasticDeformerDataGroup *group = new PlasticDeformerDataGroup;
  initializeDeformersData(group, meshImage);

  bool doMesh    = (dataType & MESH);
  bool doSO      = (dataType & SO) || doMesh;
  bool doHandles = (bool)dataType;

  // Process data
  if (doHandles)
    processHandles(group, frame, meshImage, deformation, skelId,
                   skeletonAffine);

  if (doSO)
    processSO(group, frame, meshImage, deformation, skelId, skeletonAffine);

  if (doMesh)
    processMesh(group, frame, meshImage, deformation, skelId, skeletonAffine);

  return group;
}

//----------------------------------------------------------------------------------

void PlasticDeformerStorage::invalidateMeshImage(const TMeshImage *meshImage,
                                                 int recompiledData) {
  QMutexLocker locker(&m_imp->m_mutex);

  DeformersByMeshImage &deformers = m_imp->m_deformers.get<TMeshImage>();

  DeformersByMeshImage::iterator dBegin(deformers.lower_bound(meshImage));
  if (dBegin == deformers.end()) return;

  DeformersByMeshImage::iterator dt, dEnd(deformers.upper_bound(meshImage));
  for (dt = dBegin; dt != dEnd; ++dt) {
    dt->m_dataGroup->m_outputFrame =
        (std::numeric_limits<double>::max)();  // Schedule for redeformation
    if (recompiledData)
      dt->m_dataGroup->m_compiled &=
          ~recompiledData;  // Schedule for recompilation, too
  }
}

//----------------------------------------------------------------------------------

void PlasticDeformerStorage::invalidateSkeleton(
    const PlasticSkeletonDeformation *deformation, int skelId,
    int recompiledData) {
  QMutexLocker locker(&m_imp->m_mutex);

  DeformedSkeleton ds(deformation, skelId);

  DeformersByDeformedSkeleton &deformers =
      m_imp->m_deformers.get<DeformedSkeleton>();

  DeformersByDeformedSkeleton::iterator dBegin(deformers.lower_bound(ds));
  if (dBegin == deformers.end()) return;

  DeformersByDeformedSkeleton::iterator dt, dEnd(deformers.upper_bound(ds));
  for (dt = dBegin; dt != dEnd; ++dt) {
    dt->m_dataGroup->m_outputFrame =
        (std::numeric_limits<double>::max)();  // Schedule for redeformation
    if (recompiledData)
      dt->m_dataGroup->m_compiled &=
          ~recompiledData;  // Schedule for recompilation, too
  }
}

//----------------------------------------------------------------------------------

void PlasticDeformerStorage::invalidateDeformation(
    const PlasticSkeletonDeformation *deformation, int recompiledData) {
  QMutexLocker locker(&m_imp->m_mutex);

  DeformersByDeformedSkeleton &deformers =
      m_imp->m_deformers.get<DeformedSkeleton>();

  DeformedSkeleton dsBegin(deformation, -(std::numeric_limits<int>::max)()),
      dsEnd(deformation, (std::numeric_limits<int>::max)());

  DeformersByDeformedSkeleton::iterator dBegin(deformers.lower_bound(dsBegin));
  DeformersByDeformedSkeleton::iterator dEnd(deformers.upper_bound(dsEnd));

  if (dBegin == dEnd) return;

  for (DeformersByDeformedSkeleton::iterator dt = dBegin; dt != dEnd; ++dt) {
    dt->m_dataGroup->m_outputFrame =
        (std::numeric_limits<double>::max)();  // Schedule for redeformation
    if (recompiledData)
      dt->m_dataGroup->m_compiled &=
          ~recompiledData;  // Schedule for recompilation, too
  }
}

//----------------------------------------------------------------------------------

void PlasticDeformerStorage::releaseMeshData(const TMeshImage *meshImage) {
  QMutexLocker locker(&m_imp->m_mutex);

  DeformersByMeshImage &deformers = m_imp->m_deformers.get<TMeshImage>();

  DeformersByMeshImage::iterator dBegin(deformers.lower_bound(meshImage));
  if (dBegin == deformers.end()) return;

  deformers.erase(dBegin, deformers.upper_bound(meshImage));
}

//----------------------------------------------------------------------------------

void PlasticDeformerStorage::releaseSkeletonData(const SkD *deformation,
                                                 int skelId) {
  QMutexLocker locker(&m_imp->m_mutex);

  DeformedSkeleton ds(deformation, skelId);

  DeformersByDeformedSkeleton &deformers =
      m_imp->m_deformers.get<DeformedSkeleton>();

  DeformersByDeformedSkeleton::iterator dBegin(deformers.lower_bound(ds));
  if (dBegin == deformers.end()) return;

  deformers.erase(dBegin, deformers.upper_bound(ds));
}

//----------------------------------------------------------------------------------

void PlasticDeformerStorage::releaseDeformationData(const SkD *deformation) {
  QMutexLocker locker(&m_imp->m_mutex);

  DeformersByDeformedSkeleton &deformers =
      m_imp->m_deformers.get<DeformedSkeleton>();

  DeformedSkeleton dsBegin(deformation, -(std::numeric_limits<int>::max)()),
      dsEnd(deformation, (std::numeric_limits<int>::max)());

  DeformersByDeformedSkeleton::iterator dBegin(deformers.lower_bound(dsBegin));
  DeformersByDeformedSkeleton::iterator dEnd(deformers.upper_bound(dsEnd));

  if (dBegin == dEnd) return;

  deformers.erase(dBegin, dEnd);
}

//----------------------------------------------------------------------------------

void PlasticDeformerStorage::clear() {
  QMutexLocker locker(&m_imp->m_mutex);

  m_imp->m_deformers.clear();
}
