#pragma once

#ifndef MOTIONARC_INCLUDED
#define MOTIONARC_INCLUDED

#include "tgeometry.h"

#include <algorithm>
#include <cmath>
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

class TXsheet;
class TStageObject;
class TStageObjectId;

//================================================================================================
//
// MotionArc
//
//================================================================================================

//! The path of a point of a column across frames, for the Motion Arcs overlay
//! of the viewer and the Spacing Chart (doc/motion_arcs.md), and for the
//! trajectory of the Animate tool.
/*!
  The first part samples the stage (it needs an xsheet); the rest is plain
  math on the samples (spacing, speed, chart ticks, labels) and is inline.
  Positions are in stage units, as the placements of the stage objects.
*/
namespace MotionArc {

//! The point of the column that the arc follows.
struct Track {
  enum Kind {
    Center,  //!< The centre of the object (the Animate tool's centre).
    Hook,    //!< A hook of the column's drawings, H<hook>.
    Point    //!< A point fixed in the column's local space.
  };
  Kind kind = Center;
  int hook  = 1;  //!< The hook number, from 1, for Hook
  TPointD point;  //!< In the column's local space, in stage units, for Point

  bool operator==(const Track &other) const {
    return kind == other.kind && hook == other.hook && point == other.point;
  }
  bool operator!=(const Track &other) const { return !(*this == other); }
};

//! The camera that the arc is seen through, for the perspective of the Z.
struct Camera {
  TAffine aff;  //!< The camera placement
  double z = 0;
};

//! The tracked point at one frame.
struct Sample {
  int frame = 0;
  TPointD pos;       //!< In the world (the table), stage units
  TPointD viewPos;   //!< As seen through the camera (with the Z perspective)
  bool key = false;  //!< A key of the object or of one of its parents
};

//------------------------------------------------------------------------------
// Sampling

//! Brings [r0, r1] into the xsheet's frames [0, frameCount - 1] and, when it is
//! longer than maxFrames, to a window of maxFrames frames around current.
//! Returns false when nothing is left.
inline bool clampRange(int &r0, int &r1, int frameCount, int current,
                       int maxFrames) {
  r0 = std::max(r0, 0);
  r1 = std::min(r1, frameCount - 1);
  if (r0 > r1) return false;
  if (maxFrames > 0 && r1 - r0 + 1 > maxFrames) {
    int first = std::min(current - maxFrames / 2, r1 - maxFrames + 1);
    r0        = std::max(r0, first);
    r1        = r0 + maxFrames - 1;
  }
  return true;
}

//! The range of the Animate tool's trajectory: the keys of the object itself,
//! clamped as in clampRange().
DVAPI bool keyRange(TXsheet *xsh, const TStageObjectId &id, int current,
                    int maxFrames, int &r0, int &r1);

//! The range of a motion arc: the keys of the object and of its parents (any
//! of them moves it) and, when a hook is tracked, the cells of the column
//! (the hooks move with the drawings), clamped as in clampRange().
DVAPI bool arcRange(TXsheet *xsh, const TStageObjectId &id, const Track &track,
                    int current, int maxFrames, int &r0, int &r1);

//! The tracked point at frame, in the object's local space (stage units).
DVAPI TPointD localPos(TXsheet *xsh, const TStageObjectId &id,
                       const Track &track, int frame);

//! The tracked point at frame, in the world. It leaves the placement cache of
//! the stage objects at frame: see sample().
DVAPI TPointD worldPos(TXsheet *xsh, const TStageObjectId &id,
                       const Track &track, int frame);

//! The tracked point at each frame of [r0, r1]. The placements of the stage
//! objects are cached for a single frame: the cache is put back to
//! restoreFrame at the end, so that the rest of the viewer and the tools see
//! the current frame as before.
DVAPI std::vector<Sample> sample(TXsheet *xsh, const TStageObjectId &id,
                                 const Track &track, int r0, int r1,
                                 const Camera &camera, int restoreFrame);

//------------------------------------------------------------------------------
// Math on the samples

//! The distance of each sample from the previous one (0 for the first).
inline std::vector<double> spacing(const std::vector<Sample> &samples) {
  std::vector<double> result(samples.size(), 0.0);
  for (size_t i = 1; i < samples.size(); ++i)
    result[i] = tdistance(samples[i].pos, samples[i - 1].pos);
  return result;
}

//! The speed at each sample: the mean of the spacing before and after it (the
//! one there is, at the ends), in stage units per frame.
inline std::vector<double> speeds(const std::vector<Sample> &samples) {
  const std::vector<double> d = spacing(samples);
  const size_t n              = samples.size();
  std::vector<double> result(n, 0.0);
  for (size_t i = 0; i < n; ++i) {
    const bool before = i > 0, after = i + 1 < n;
    if (before && after)
      result[i] = 0.5 * (d[i] + d[i + 1]);
    else if (before)
      result[i] = d[i];
    else if (after)
      result[i] = d[i + 1];
  }
  return result;
}

//! The values divided by their largest one, in [0, 1] (all 0 when they are).
inline std::vector<double> normalized(const std::vector<double> &values) {
  double top = 0.0;
  for (double v : values) top = std::max(top, v);
  std::vector<double> result(values.size(), 0.0);
  if (top > 0.0)
    for (size_t i = 0; i < values.size(); ++i) result[i] = values[i] / top;
  return result;
}

//! The length of the arc from the first sample to the last.
inline double length(const std::vector<Sample> &samples) {
  double total = 0.0;
  for (double d : spacing(samples)) total += d;
  return total;
}

//! Whether the frame number is written next to the sample: every every
//! frames of the xsheet (frame numbers counted from 1, as the xsheet shows
//! them) and on the keys; none when every is 0.
inline bool isLabelled(int frame, bool key, int every) {
  if (every <= 0) return false;
  return key || (frame + 1) % every == 0;
}

//! The colour of the speed ramp at t in [0, 1]: blue for slow, through green,
//! to red for fast.
inline void rampColor(double t, int &r, int &g, int &b) {
  t = std::max(0.0, std::min(1.0, t));
  // blue (40, 110, 255) -> green (40, 210, 80) -> red (255, 60, 40)
  const double from[3][3] = {{40, 110, 255}, {40, 210, 80}, {255, 60, 40}};
  const int seg           = t < 0.5 ? 0 : 1;
  const double u          = t < 0.5 ? t * 2.0 : (t - 0.5) * 2.0;
  r = int(std::lround(from[seg][0] + (from[seg + 1][0] - from[seg][0]) * u));
  g = int(std::lround(from[seg][1] + (from[seg + 1][1] - from[seg][1]) * u));
  b = int(std::lround(from[seg][2] + (from[seg + 1][2] - from[seg][2]) * u));
}

//! A tick of the spacing chart.
struct Tick {
  int frame = 0;
  double t  = 0;  //!< Along the arc from the first frame (0) to the last (1)
  double spacing = 0;  //!< The distance from the previous frame
  bool key       = false;
};

//! The spacing chart of the frames [f0, f1] of the samples: one tick per
//! frame at its distance along the arc, as a fraction of the arc's length
//! between f0 and f1. With no motion (a hold) every tick is at 0.
inline std::vector<Tick> chart(const std::vector<Sample> &samples, int f0,
                               int f1) {
  std::vector<Tick> ticks;
  if (f0 > f1) std::swap(f0, f1);
  std::vector<Sample> part;
  for (const Sample &s : samples)
    if (s.frame >= f0 && s.frame <= f1) part.push_back(s);
  if (part.empty()) return ticks;
  const std::vector<double> d = spacing(part);
  double total                = 0.0;
  for (double v : d) total += v;
  double along = 0.0;
  for (size_t i = 0; i < part.size(); ++i) {
    along += d[i];
    Tick tick;
    tick.frame   = part[i].frame;
    tick.t       = total > 0.0 ? along / total : 0.0;
    tick.spacing = d[i];
    tick.key     = part[i].key;
    ticks.push_back(tick);
  }
  // The last tick exactly at the end, whatever the rounding
  if (total > 0.0) ticks.back().t = 1.0;
  return ticks;
}

//! The keys of the samples around frame, for the default range of the chart:
//! the last key at or before frame and the first key after it (the first and
//! last samples where there is none). Returns false without samples.
inline bool keysAround(const std::vector<Sample> &samples, int frame, int &f0,
                       int &f1) {
  if (samples.empty()) return false;
  f0 = samples.front().frame, f1 = samples.back().frame;
  for (const Sample &s : samples)
    if (s.key && s.frame <= frame) f0 = s.frame;
  for (const Sample &s : samples)
    if (s.key && s.frame > f0) {
      f1 = s.frame;
      break;
    }
  return true;
}

}  // namespace MotionArc

#endif  // MOTIONARC_INCLUDED
