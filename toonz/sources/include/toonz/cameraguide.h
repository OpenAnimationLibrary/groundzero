#pragma once

#ifndef CAMERA_GUIDE_INCLUDED
#define CAMERA_GUIDE_INCLUDED

#include "tgeometry.h"

#include <cmath>
#include <string>
#include <vector>

namespace CameraGuide {

// frame is in physical, camera-local coordinates; resolution is applied later.
inline TRectD centeredRect(const TRectD &frame, double aspectRatio) {
  double width = frame.getLx(), height = frame.getLy();
  if (!std::isfinite(frame.x0) || !std::isfinite(frame.y0) ||
      !std::isfinite(width) || !std::isfinite(height) || width <= 0.0 ||
      height <= 0.0 || !std::isfinite(aspectRatio) || aspectRatio <= 0.0)
    return TRectD();

  if (width / height > aspectRatio)
    width = height * aspectRatio;
  else
    height = width / aspectRatio;

  const TPointD center =
      frame.getP00() + TPointD(frame.getLx() * 0.5, frame.getLy() * 0.5);
  return TRectD(center.x - width * 0.5, center.y - height * 0.5,
                center.x + width * 0.5, center.y + height * 0.5);
}

// Reads an aspect ratio typed as "1.85", "1.85:1", "16:9" or "4/3".
// Returns 0 when the text is not a positive, finite ratio.
inline double parseRatio(const std::string &text) {
  const std::string::size_type sep = text.find_first_of(":/");
  try {
    std::size_t used = 0;
    const std::string first =
        sep == std::string::npos ? text : text.substr(0, sep);
    double ratio = std::stod(first, &used);
    if (first.find_first_not_of(" \t", used) != std::string::npos) return 0.0;
    if (sep != std::string::npos) {
      const std::string second = text.substr(sep + 1);
      const double height      = std::stod(second, &used);
      if (second.find_first_not_of(" \t", used) != std::string::npos ||
          !(height > 0.0))
        return 0.0;
      ratio /= height;
    }
    return std::isfinite(ratio) && ratio > 0.0 ? ratio : 0.0;
  } catch (...) {
    return 0.0;
  }
}

// The parts of frame outside inner: up to four bars, top and bottom spanning
// the whole width, left and right between them.
inline std::vector<TRectD> letterboxBars(const TRectD &frame,
                                         const TRectD &inner) {
  std::vector<TRectD> bars;
  if (frame.isEmpty() || inner.isEmpty()) return bars;
  const TRectD in = inner * frame;
  if (in.isEmpty()) return bars;
  if (in.y1 < frame.y1)
    bars.push_back(TRectD(frame.x0, in.y1, frame.x1, frame.y1));
  if (frame.y0 < in.y0)
    bars.push_back(TRectD(frame.x0, frame.y0, frame.x1, in.y0));
  if (frame.x0 < in.x0) bars.push_back(TRectD(frame.x0, in.y0, in.x0, in.y1));
  if (in.x1 < frame.x1) bars.push_back(TRectD(in.x1, in.y0, frame.x1, in.y1));
  return bars;
}

}  // namespace CameraGuide

#endif
