#pragma once

#ifndef AUTOPATCHOUTLINE_H
#define AUTOPATCHOUTLINE_H

// Pure helper of the Auto-Patch Fx (autopatchfx.cpp), free of any toonz type
// so that stdfx/tests/autopatchoutline_test.cpp can compile it on its own.
//
// Outline Only limits the patch to the lines of Top that lie on its outline:
// the seam of a joint is Top's outline crossing Bottom's paint, while the
// detail lines drawn inside Top have Top's paint on both sides. A line pixel
// is on the outline when it is close to a pixel outside Top, which is found
// with an exact Euclidean distance transform (Felzenszwalb and Huttenlocher,
// "Distance Transforms of Sampled Functions", 2012).

#include <algorithm>
#include <cmath>
#include <vector>

namespace autopatch {

namespace detail {

// Far enough for any image, small enough to keep the parabolas' arithmetic
// finite
const double c_far = 1e20;

// The squared distance transform of the n samples of f, f[i] being 0 at the
// sites and c_far elsewhere. v and z are work buffers of n and n + 1 values.
inline void transform1d(const double *f, int n, double *d, int *v,
                        double *z) {
  // The lower envelope of the parabolas rooted at the samples: v holds their
  // roots, z the bounds of the ranges where each is the lowest. z[0] is below
  // any intersection, so the backtracking stops at the first parabola.
  auto intersection = [f](int q, int p) {
    return ((f[q] + double(q) * q) - (f[p] + double(p) * p)) / (2.0 * (q - p));
  };
  int k = 0;
  v[0]  = 0;
  z[0]  = -2.0 * c_far;
  z[1]  = 2.0 * c_far;
  for (int q = 1; q < n; ++q) {
    double s = intersection(q, v[k]);
    while (s <= z[k]) s = intersection(q, v[--k]);
    ++k;
    v[k]     = q;
    z[k]     = s;
    z[k + 1] = 2.0 * c_far;
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < q) ++k;
    const double dq = q - v[k];
    d[q]            = dq * dq + f[v[k]];
  }
}

}  // namespace detail

//! outside holds w x h flags, row by row: the pixels outside Top. Returns the
//! squared distance of each pixel from the nearest outside pixel, or a value
//! of at least 1e20 when there is none.
inline std::vector<double> squaredDistances(
    const std::vector<unsigned char> &outside, int w, int h) {
  std::vector<double> dist;
  if (w <= 0 || h <= 0 || (int)outside.size() != w * h) return dist;
  dist.resize(outside.size());
  for (size_t i = 0; i < outside.size(); ++i)
    dist[i] = outside[i] ? 0.0 : detail::c_far;

  const int n = std::max(w, h);
  std::vector<double> f(n), d(n), z(n + 1);
  std::vector<int> v(n);

  // Columns, then rows
  for (int x = 0; x < w; ++x) {
    for (int y = 0; y < h; ++y) f[y] = dist[y * w + x];
    detail::transform1d(f.data(), h, d.data(), v.data(), z.data());
    for (int y = 0; y < h; ++y) dist[y * w + x] = d[y];
  }
  for (int y = 0; y < h; ++y) {
    double *row = &dist[y * w];
    std::copy(row, row + w, f.begin());
    detail::transform1d(f.data(), w, d.data(), v.data(), z.data());
    std::copy(d.begin(), d.begin() + w, row);
  }
  return dist;
}

//! Returns, for each of the w x h pixels, how much it lies within radius
//! pixels of an outside pixel: 1 up to radius - 0.5, then down to 0 at
//! radius + 0.5, so the limit is antialiased. Without outside pixels every
//! weight is 0.
inline std::vector<float> outlineWeights(
    const std::vector<unsigned char> &outside, int w, int h, double radius) {
  const std::vector<double> dist = squaredDistances(outside, w, h);
  std::vector<float> weights(dist.size(), 0.0f);
  if (!(radius > 0.0)) return weights;
  const double reach = (radius + 0.5) * (radius + 0.5);
  for (size_t i = 0; i < dist.size(); ++i) {
    if (dist[i] >= reach) continue;
    const double t = radius + 0.5 - std::sqrt(dist[i]);
    weights[i]     = (float)std::min(1.0, std::max(0.0, t));
  }
  return weights;
}

}  // namespace autopatch

#endif  // AUTOPATCHOUTLINE_H
