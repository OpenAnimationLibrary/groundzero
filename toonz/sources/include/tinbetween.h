#pragma once

#ifndef TINBETWEEN_H
#define TINBETWEEN_H

#include <memory>
#include <utility>
#include <vector>
#include "tcommon.h"

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
    EaseInOutInterpolation
  };

  //! Stroke correspondence hints: each pair forces the stroke at index
  //! \b first of the first image to be inbetweened with the stroke at index
  //! \b second of the last image.
  typedef std::vector<std::pair<int, int>> StrokeCorrespondence;

  static double interpolation(double t, enum TweenAlgorithm);

  TInbetween(const TVectorImageP firstImage, const TVectorImageP lastImage);

  //! Same as above, but the strokes listed in \b correspondence are paired as
  //! specified. The other strokes keep the default index order matching where
  //! possible; out of range or conflicting pairs are ignored. An empty
  //! correspondence gives exactly the same result as the constructor above.
  TInbetween(const TVectorImageP firstImage, const TVectorImageP lastImage,
             const StrokeCorrespondence &correspondence);

  virtual ~TInbetween();

  TVectorImageP tween(double t) const;
};

#endif
