

// TnzLib includes
#include "toonz/txsheet.h"
#include "toonz/txshcell.h"
#include "toonz/txshlevelcolumn.h"
#include "toonz/hook.h"
#include "toonz/dpiscale.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/stage.h"

#include "xshhandlemanager.h"

//*******************************************************************************
//    Local namespace
//*******************************************************************************

namespace {

// Returns the sum of each pass hook between fid and precFid.
TPointD forwardPass(const TFrameId &fid, const TFrameId &precFid,
                    TXshSimpleLevel *sl, Hook *hook) {
  TPointD delta;
  std::vector<TFrameId> levelFids;
  sl->getFids(levelFids);

  int f, fCount = levelFids.size();
  for (f = 0; f != fCount; ++f) {
    const TFrameId &levelFid = levelFids[f];

    if (levelFid < precFid) continue;

    if (levelFid >= fid) break;

    if (hook->isKeyframe(levelFid))
      delta += hook->getAPos(levelFid) - hook->getBPos(levelFid);
  }

  return delta;
}

//-----------------------------------------------------------------------------
// Returns subtraction of each pass hook between fid and precFid.
TPointD backwardPass(const TFrameId &fid, const TFrameId &precFid,
                     TXshSimpleLevel *sl, Hook *hook) {
  TPointD delta;
  std::vector<TFrameId> levelFids;
  sl->getFids(levelFids);

  int f, fCount = levelFids.size();
  for (f = 0; f != fCount; ++f) {
    const TFrameId &levelFid = levelFids[f];

    if (levelFid < fid) continue;

    if (levelFid >= precFid) break;

    if (hook->isKeyframe(levelFid))
      delta -= hook->getAPos(levelFid) - hook->getBPos(levelFid);
  }

  return delta;
}

//-----------------------------------------------------------------------------
// Compute pass hook between fid and precFid
TPointD computePassHook(const TFrameId &fid, const TFrameId &precFid,
                        TXshSimpleLevel *sl, Hook *hook) {
  if (fid < precFid && (precFid.getNumber() - fid.getNumber()) <= 2)
    return backwardPass(fid, precFid, sl, hook);

  return forwardPass(fid, precFid, sl, hook);
}

}  // namespace

//*******************************************************************************
//    XshHandleManager  implementation
//*******************************************************************************

TPointD XshHandleManager::getHandlePos(const TStageObjectId &id,
                                       const std::string &handle,
                                       int row) const {
  static const double unit = 8.0;

  assert(m_xsh->getScene());

  if (handle == "")
    return TPointD();

  else if (TStageObject::isSkeletonVertexHandle(handle))
    // Plastic vertex handles move their children through getHandleAffine()
    return TPointD();

  else if (handle[0] == 'H' && handle.length() > 1) {
    // Hook port case

    if (!id.isColumn()) return TPointD();

    int hookIndex = 0;
    {
      int h, hLength = handle.length();
      for (h = 1; h != hLength; ++h)
        hookIndex = hookIndex * 10 + (handle[h] - '0');
    }

    TStageObject *obj = m_xsh->getStageObject(id);
    if (const PlasticSkeletonDeformationP &def =
            obj->getPlasticSkeletonDeformation()) {
      int skelId = def->skeletonId(row);

      PlasticSkeleton skel;
      def->storeDeformedSkeleton(skelId, row, skel);

      int v = def->vertexIndex(hookIndex, skelId);
      return (v >= 0) ? TScale(1.0 / Stage::inch) * skel.vertex(v).P()
                      : TPointD();
    }

    --hookIndex;

    int col = id.getIndex();
    // The drawings the column shows: with a time remap, not the xsheet cells
    // (as the render, see TLevelColumnFx), or the hook would follow another
    // drawing than the one shown
    TXshColumn *column           = m_xsh->getColumn(col);
    TXshLevelColumn *levelColumn = column ? column->getLevelColumn() : 0;
    const bool remapped = levelColumn && levelColumn->isTimeRemapActive();
    auto cellAt         = [&](int r) {
      return remapped ? levelColumn->getRemappedCell(r)
                              : m_xsh->getCell(r, col);
    };
    TXshCell cell(cellAt(row));

    TXshLevel *xl = cell.m_level.getPointer();
    if (!xl) return TPointD();

    TXshSimpleLevel *sl = xl->getSimpleLevel();
    if (!sl) return TPointD();

    Hook *hook = sl->getHookSet()->getHook(hookIndex);
    if (!hook) return TPointD();

    TFrameId fid(cell.m_frameId);

    TPointD pos = hook->getAPos(fid) * (1.0 / Stage::inch);
    TPointD delta;

    for (int r = row - 1; r >= 0; --r) {
      cell = cellAt(r);
      if (cell.m_level.getPointer() != xl) break;

      const TFrameId &precFid = cell.m_frameId;
      delta += computePassHook(fid, precFid, sl, hook);
      fid = precFid;
    }

    pos += delta * (1.0 / Stage::inch);
    pos = getDpiAffine(sl, cell.m_frameId, true) * pos;

    return pos;
  } else if ('A' <= handle[0] && handle[0] <= 'Z')
    return TPointD(unit * (handle[0] - 'B'), 0);
  else if ('a' <= handle[0] && handle[0] <= 'z')
    return TPointD(0.5 * unit * (handle[0] - 'b'), 0);
  else
    return TPointD();
}

//-----------------------------------------------------------------------------

TAffine XshHandleManager::getHandleAffine(const TStageObjectId &id,
                                          const std::string &handle,
                                          double frame) const {
  // Kinematic output: a child linked to a Plastic skeleton vertex follows the
  // bone ending at that vertex (from its parent vertex), in position and
  // rotation. The returned transform maps the bone's rest pose (the original
  // skeleton) to its deformed pose at frame:
  //
  //   T(deformedPos) * R(deformedAngle - restAngle) * T(-restPos)
  //
  // which is the identity at rest, so linking a child does not move it.
  // Skeletons are expressed in the column's local stage coordinates (the
  // ones the column placement maps to the world, see the existing skeleton
  // hooks above and PlasticDeformerFx), independently of the mesh level's
  // dpi: no further conversion is needed.

  if (!id.isColumn() || !TStageObject::isSkeletonVertexHandle(handle))
    return TAffine();

  TStageObject *obj = m_xsh->getStageObject(id);

  const PlasticSkeletonDeformationP &sd = obj->getPlasticSkeletonDeformation();
  if (!sd) return TAffine();

  // The deformation is evaluated at the column's params time, as when it is
  // rendered (see PlasticDeformerFx)
  double sdFrame = obj->paramsTime(frame);
  int skelId     = sd->skeletonId(sdFrame);

  const PlasticSkeletonP &origSkel = sd->skeleton(skelId);
  if (!origSkel) return TAffine();

  // Search the vertex by name - names are persistent, indices are not
  const QString &vxName =
      QString::fromStdString(TStageObject::getSkeletonVertexName(handle));

  int v = -1;
  {
    const PlasticSkeleton::vertices_container &vertices = origSkel->vertices();

    PlasticSkeleton::vertices_container::const_iterator vt,
        vEnd = vertices.end();
    for (vt = vertices.begin(); vt != vEnd; ++vt)
      if (vt->name() == vxName) {
        v = int(vt.index());
        break;
      }
  }
  if (v < 0) return TAffine();

  // The deformed skeleton is a copy of the original one: same vertex indices
  PlasticSkeleton deformedSkel;
  sd->storeDeformedSkeleton(skelId, sdFrame, deformedSkel);

  const PlasticSkeletonVertex &restVx     = origSkel->vertex(v);
  const PlasticSkeletonVertex &deformedVx = deformedSkel.vertex(v);

  const TPointD &restPos = restVx.P(), &deformedPos = deformedVx.P();

  double angle = 0.0;  // Degrees

  int vParent = restVx.parent();
  if (vParent >= 0) {
    const TPointD &restBone = restPos - origSkel->vertex(vParent).P();
    const TPointD &deformedBone =
        deformedPos - deformedSkel.vertex(vParent).P();

    // Degenerate bones have no direction: follow the vertex position only
    const double minLength = 1e-4;
    if (norm(restBone) > minLength && norm(deformedBone) > minLength)
      angle = (std::atan2(deformedBone.y, deformedBone.x) -
               std::atan2(restBone.y, restBone.x)) *
              M_180_PI;
  }
  // else, the root vertex: translation only

  return TTranslation(deformedPos) * TRotation(angle) * TTranslation(-restPos);
}
