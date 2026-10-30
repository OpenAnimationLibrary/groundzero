#pragma once

#ifndef TINBETWEEN_H
#define TINBETWEEN_H

#include <memory>
#include <utility>
#include <vector>
#include "tcommon.h"
#include "tgeometry.h"

class TVectorImageP;

#undef DVAPI
#undef DVVAR

#ifdef TVRENDER_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

class DVAPI TInbetween {
  class Imp;
  std::unique_ptr<Imp> m_imp;

public:
  enum TweenAlgorithm {
    LinearInterpolation,
    EaseInInterpolation,
    EaseOutInterpolation,
    EaseInOutInterpolation,
    //! Ease curve with a separate strength for the start and the end of the
    //! motion; see customEaseInterpolation(). \b interpolation() cannot
    //! evaluate it (no parameters) and returns t.
    CustomEaseInterpolation
  };

  //! Stroke correspondence hints: each pair forces the stroke at index
  //! \b first of the first image to be inbetweened with the stroke at index
  //! \b second of the last image.
  typedef std::vector<std::pair<int, int>> StrokeCorrespondence;

  enum UnmatchedPolicy { OmitUnmatched = 0, HoldUnmatched, CollapseUnmatched };
  enum MatchSource { ManualMatch, ZoneMatch, AutomaticMatch, IndexMatch };
  struct ResolvedPair {
    int first, last;
    MatchSource source;
  };
  struct CorrespondenceReview {
    std::vector<ResolvedPair> pairs;
    std::vector<int> unmatchedFirst, unmatchedLast;
  };

  //! A pair of zones: the strokes of the first image whose centroid is inside
  //! \b m_first are matched with the strokes of the last image whose centroid
  //! is inside \b m_last, nearest first.
  struct ZoneHint {
    TRectD m_first, m_last;
    ZoneHint() {}
    ZoneHint(const TRectD &first, const TRectD &last)
        : m_first(first), m_last(last) {}
  };

  //! Optional automatic matching, applied after the stroke pairs. Strokes are
  //! matched, in this order of priority, by: the stroke pairs, the zone hints,
  //! the automatic matching, the index order. The default options give the
  //! same result as no options.
  struct MatchOptions {
    bool m_autoMatch = false;  //!< see autoCorrespondence()
    std::vector<ZoneHint> m_zoneHints;
    UnmatchedPolicy m_unmatchedPolicy = OmitUnmatched;
  };

  //! The exact assignment consumed by tween(), also used by the native review.
  static CorrespondenceReview reviewCorrespondence(
      const TVectorImageP &first, const TVectorImageP &last,
      const StrokeCorrespondence &manual, const MatchOptions &options);

  static double interpolation(double t, enum TweenAlgorithm);

  //! The velocity curve of \b CustomEaseInterpolation: a cubic Bezier ease with
  //! control points (0,0) (slowStart,0) (1-slowEnd,1) (1,1), the strengths
  //! being in [0,1]. 0 / 0 is the linear interpolation.
  static double customEaseInterpolation(double t, double slowStart,
                                        double slowEnd);

  //! Matches the strokes of the two images by minimising a cost made of the
  //! centroid distance (relative to the bounding box diagonal of the images),
  //! the bounding box size difference, the length difference and a penalty
  //! for different styles. The assignment is optimal (Hungarian method), so
  //! the min(stroke counts) strokes are all matched, the surplus strokes of
  //! the larger image are left out like with the default matching.
  static StrokeCorrespondence autoCorrespondence(const TVectorImageP &first,
                                                 const TVectorImageP &last);

  //! Returns the pairs resulting from the zone hints, in the order of the
  //! hints; a stroke belongs to the first hint that matches it.
  static StrokeCorrespondence zoneCorrespondence(
      const TVectorImageP &first, const TVectorImageP &last,
      const std::vector<ZoneHint> &zoneHints);

  TInbetween(const TVectorImageP firstImage, const TVectorImageP lastImage);

  //! Same as above, but the strokes listed in \b correspondence are paired as
  //! specified. The other strokes keep the default index order matching where
  //! possible; out of range or conflicting pairs are ignored. An empty
  //! correspondence gives exactly the same result as the constructor above.
  TInbetween(const TVectorImageP firstImage, const TVectorImageP lastImage,
             const StrokeCorrespondence &correspondence);

  //! Same as above, with the zone hints and the automatic matching of
  //! \b options completing the \b correspondence.
  TInbetween(const TVectorImageP firstImage, const TVectorImageP lastImage,
             const StrokeCorrespondence &correspondence,
             const MatchOptions &options);

  virtual ~TInbetween();

  TVectorImageP tween(double t) const;
};

#endif
