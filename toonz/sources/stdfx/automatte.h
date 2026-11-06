#pragma once

#ifndef AUTOMATTE_H
#define AUTOMATTE_H

// Pure helper of the Auto Matte Fx (automattefx.cpp), free of any toonz type
// so that stdfx/tests/automatte_test.cpp can compile it on its own.
//
// A drawing's silhouette: everything its lines enclose. Pixels at least as
// opaque as the threshold are lines; a flood fill from the image border
// through the other pixels finds the outside. Outside pixels keep their own
// alpha, so the outer edge stays antialiased; every other pixel, the lines
// and what they enclose, becomes opaque. A gap in the outline lets the
// outside in, as with a fill.

#include <vector>

namespace automatte {

//! alpha holds w x h values in 0..1, row by row. Returns the matte alpha, or
//! an empty vector when cancel is set.
inline std::vector<float> silhouette(const std::vector<float> &alpha, int w,
                                     int h, double threshold,
                                     const int *cancel = nullptr) {
  std::vector<float> result(alpha);
  if (w <= 0 || h <= 0 || (int)alpha.size() != w * h) return result;
  std::vector<unsigned char> outside(alpha.size(), 0);
  std::vector<int> stack;
  auto push = [&](int x, int y) {
    const int i = y * w + x;
    if (outside[i] || alpha[i] >= threshold) return;
    outside[i] = 1;
    stack.push_back(i);
  };
  for (int x = 0; x < w; ++x) push(x, 0), push(x, h - 1);
  for (int y = 0; y < h; ++y) push(0, y), push(w - 1, y);
  long long steps = 0;
  while (!stack.empty()) {
    if ((++steps & 0xFFFF) == 0 && cancel && *cancel) return {};
    const int i = stack.back();
    stack.pop_back();
    const int x = i % w, y = i / w;
    if (x > 0) push(x - 1, y);
    if (x < w - 1) push(x + 1, y);
    if (y > 0) push(x, y - 1);
    if (y < h - 1) push(x, y + 1);
  }
  for (std::size_t i = 0; i < result.size(); ++i)
    if (!outside[i]) result[i] = 1.0f;
  return result;
}

}  // namespace automatte

#endif  // AUTOMATTE_H
