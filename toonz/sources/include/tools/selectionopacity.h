#pragma once

#include "tvectorimage.h"
#include "tstroke.h"
#include <cmath>
#include <set>

namespace SelectionOpacity {
struct Summary {
  int count = 0;
  double value = 1.0;
  bool mixed = false;
};

// Readback never changes strokes, tool properties, dirty flags or undo state.
inline Summary summarize(const TVectorImageP &image,
                         const std::set<int> &selection) {
  Summary result;
  if (!image) return result;
  for (int index : selection) {
    if (index < 0 || index >= int(image->getStrokeCount())) continue;
    const double value = image->getStroke(index)->getOpacity();
    if (!result.count)
      result.value = value;
    else if (std::abs(value - result.value) > 1e-9)
      result.mixed = true;
    ++result.count;
  }
  return result;
}
}  // namespace SelectionOpacity
