#pragma once

#ifndef FILMSTRIPCOMMAND_H
#define FILMSTRIPCOMMAND_H

#include "tcommon.h"
#include "tfilepath.h"
#include "tgeometry.h"

#include <set>
#include <utility>
#include <vector>

// Forward declarations
class TXshSimpleLevel;
class TXshLevel;
class TXshPaletteLevel;
class TXshSoundLevel;

//=============================================================================
// FilmStripCmd namespace
//-----------------------------------------------------------------------------

namespace FilmstripCmd {

// Add empty frames to the level
void addFrames(TXshSimpleLevel *sl, int start, int end, int step);

// Renumber frames in the level
void renumber(TXshSimpleLevel *sl, std::set<TFrameId> &frames, int startFrame,
              int stepFrame);
void renumber(TXshSimpleLevel *sl,
              const std::vector<std::pair<TFrameId, TFrameId>> &table,
              bool forceCallUpdateXSheet = false);
// Reassign drawing frame IDs without rewriting Xsheet cell references.
// Used by Level Strip drag reordering, where the drawing order itself is the
// intended edit.
void reorder(TXshSimpleLevel *sl,
             const std::vector<std::pair<TFrameId, TFrameId>> &table);

// Copy/paste operations for frames
void copy(TXshSimpleLevel *sl, std::set<TFrameId> &frames);
bool insertFramesFromLevel(TXshSimpleLevel *sourceLevel,
                           const std::set<TFrameId> &sourceFrames,
                           TXshSimpleLevel *targetLevel,
                           std::set<TFrameId> &targetFrames);
void paste(TXshSimpleLevel *sl, std::set<TFrameId> &frames);
void merge(TXshSimpleLevel *sl, std::set<TFrameId> &frames);
void pasteInto(TXshSimpleLevel *sl, std::set<TFrameId> &frames);
void cut(TXshSimpleLevel *sl, std::set<TFrameId> &frames);
void clear(TXshSimpleLevel *sl, std::set<TFrameId> &frames);
void remove(TXshSimpleLevel *sl, std::set<TFrameId> &frames);
void insert(TXshSimpleLevel *sl, const std::set<TFrameId> &frames,
            bool withUndo);

// Frame manipulation operations
void reverse(TXshSimpleLevel *sl, std::set<TFrameId> &frames);
void swing(TXshSimpleLevel *sl, std::set<TFrameId> &frames);
void step(TXshSimpleLevel *sl, std::set<TFrameId> &frames, int step);
void each(TXshSimpleLevel *sl, std::set<TFrameId> &frames, int each);

// Duplicate operations
void duplicateFrameWithoutUndo(TXshSimpleLevel *sl, TFrameId srcFrame,
                               TFrameId targetFrame);
void duplicate(TXshSimpleLevel *sl, std::set<TFrameId> &frames, bool withUndo);

// // TODO: Need to be moved to another location - Level to scene operations
void moveToScene(TXshLevel *sl, std::set<TFrameId> &frames);
void moveToScene(TXshSimpleLevel *sl);
void moveToScene(TXshPaletteLevel *pl);
void moveToScene(TXshSoundLevel *sl);

// Inbetween interpolation types and functions
enum InbetweenInterpolation {
  II_Linear,
  II_EaseIn,
  II_EaseOut,
  II_EaseInOut,
  II_Custom  // uses InbetweenOptions::m_slowStart / m_slowEnd
};

// Smarter morphing options; the default ones give the plain inbetween.
// See TInbetween::MatchOptions and TInbetween::customEaseInterpolation.
struct InbetweenOptions {
  bool m_autoMatch = false;  // match the strokes not paired by shape/position
  // (zone in the first drawing, zone in the last drawing)
  std::vector<std::pair<TRectD, TRectD>> m_zoneHints;
  double m_slowStart = 0;  // II_Custom ease strengths, 0..1
  double m_slowEnd   = 0;
  int m_unmatchedPolicy = 0;  // TInbetween::UnmatchedPolicy
};

// strokePairs optionally forces the (fid0 stroke index, fid1 stroke index)
// matching of the strokes; see TInbetween::StrokeCorrespondence
void inbetweenWithoutUndo(TXshSimpleLevel *sl, const TFrameId &fid0,
                          const TFrameId &fid1,
                          InbetweenInterpolation interpolation,
                          const std::vector<std::pair<int, int>> &strokePairs =
                              std::vector<std::pair<int, int>>(),
                          const InbetweenOptions &options = InbetweenOptions());
void inbetween(TXshSimpleLevel *sl, const TFrameId &fid0, const TFrameId &fid1,
               InbetweenInterpolation interpolation,
               const std::vector<std::pair<int, int>> &strokePairs =
                   std::vector<std::pair<int, int>>(),
               const InbetweenOptions &options = InbetweenOptions());

// Renumber a single drawing
void renumberDrawing(TXshSimpleLevel *sl, const TFrameId &oldFid,
                     const TFrameId &desiredNewFid);

}  // namespace FilmstripCmd

// Operator overload for TFrameId
TFrameId operator+(const TFrameId &fid, int d);

// Utility function to make space for frame IDs in a level
void makeSpaceForFids(TXshSimpleLevel *sl,
                      const std::set<TFrameId> &framesToInsert);

#endif  // FILMSTRIPCOMMAND_H
