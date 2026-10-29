

// TnzLib includes
#include "toonz/txsheet.h"
#include "toonz/txshcolumn.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"
#include "toonz/stage.h"

#include "toonz/motionarc.h"

#include <string>

//********************************************************************************
//    Local namespace
//********************************************************************************

namespace {

// The object and its parents, up to the root of the stage
template <typename F>
void forEachInChain(TXsheet *xsh, TStageObjectId id, F f) {
  // The tree has no cycles; the bound only guards against a broken one
  for (int depth = 0; id != TStageObjectId::NoneId && depth < 1000; ++depth) {
    TStageObject *obj = xsh->getStageObject(id);
    if (!obj) return;
    f(obj);
    id = obj->getParent();
  }
}

bool isKeyInChain(TXsheet *xsh, const TStageObjectId &id, int frame) {
  bool key = false;
  forEachInChain(xsh, id, [&](TStageObject *obj) {
    if (obj->isKeyframe(frame)) key = true;
  });
  return key;
}

}  // namespace

//********************************************************************************
//    MotionArc  implementation
//********************************************************************************

bool MotionArc::keyRange(TXsheet *xsh, const TStageObjectId &id, int current,
                         int maxFrames, int &r0, int &r1) {
  TStageObject *obj = xsh->getStageObject(id);
  if (!obj || !obj->getKeyframeRange(r0, r1)) return false;
  return clampRange(r0, r1, xsh->getFrameCount(), current, maxFrames);
}

//------------------------------------------------------------------------------

bool MotionArc::arcRange(TXsheet *xsh, const TStageObjectId &id,
                         const Track &track, int current, int maxFrames,
                         int &r0, int &r1) {
  bool found = false;
  auto add   = [&](int a, int b) {
    if (a > b) return;
    if (!found)
      r0 = a, r1 = b, found = true;
    else
      r0 = std::min(r0, a), r1 = std::max(r1, b);
  };

  forEachInChain(xsh, id, [&](TStageObject *obj) {
    int a, b;
    if (obj->getKeyframeRange(a, b)) add(a, b);
  });

  // The hooks move with the drawings, even without keys
  if (track.kind == Track::Hook && id.isColumn()) {
    TXshColumn *column = xsh->getColumn(id.getIndex());
    int a, b;
    if (column && !column->isEmpty() && column->getRange(a, b) > 0) add(a, b);
  }

  if (!found) return false;
  return clampRange(r0, r1, xsh->getFrameCount(), current, maxFrames);
}

//------------------------------------------------------------------------------

TPointD MotionArc::localPos(TXsheet *xsh, const TStageObjectId &id,
                            const Track &track, int frame) {
  switch (track.kind) {
  case Track::Hook:
    if (id.isColumn() && track.hook >= 1) {
      const std::string handle = "H" + std::to_string(track.hook);
      return Stage::inch *
             xsh->getStageObjectTree()->getHandlePos(id, handle, frame);
    }
    break;
  case Track::Point:
    return track.point;
  case Track::Center:
    break;
  }
  return Stage::inch * xsh->getCenter(id, frame);
}

//------------------------------------------------------------------------------

TPointD MotionArc::worldPos(TXsheet *xsh, const TStageObjectId &id,
                            const Track &track, int frame) {
  return xsh->getPlacement(id, frame) * localPos(xsh, id, track, frame);
}

//------------------------------------------------------------------------------

std::vector<MotionArc::Sample> MotionArc::sample(TXsheet *xsh,
                                                 const TStageObjectId &id,
                                                 const Track &track, int r0,
                                                 int r1, const Camera &camera,
                                                 int restoreFrame) {
  std::vector<Sample> samples;
  TStageObject *obj = xsh->getStageObject(id);
  if (!obj || r0 > r1) return samples;

  const double noScaleZ = obj->getGlobalNoScaleZ();
  samples.reserve(r1 - r0 + 1);
  for (int f = r0; f <= r1; ++f) {
    const TPointD local     = localPos(xsh, id, track, f);
    const TAffine placement = xsh->getPlacement(id, f);

    Sample s;
    s.frame = f;
    s.pos   = placement * local;
    // As the stage places the column for the camera (see
    // StageBuilder::addCell)
    TAffine viewAff;
    if (TStageObject::perspective(viewAff, camera.aff, camera.z, placement,
                                  xsh->getZ(id, f), noScaleZ))
      s.viewPos = viewAff * local;
    else
      s.viewPos = s.pos;  // behind the camera
    s.key = isKeyInChain(xsh, id, f);
    samples.push_back(s);
  }

  // The placement cache holds a single frame: put the current one back
  xsh->getPlacement(id, restoreFrame);
  return samples;
}
