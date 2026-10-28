#pragma once

#include "cadmiumtypes.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace Cadmium {

inline QVector<int> matchingNativeRegions(const Frame &reference,
                                         const QColor &proxy) {
  QVector<int> regions;
  if (!proxy.isValid()) return regions;
  std::set<std::tuple<double, double, double, double, bool>> seen;
  const int count = std::min(reference.regionColors.size(),
                             reference.nativeRegionColors.size());
  for (int region = 0; region < count; ++region) {
    if (reference.regionColors[region] != proxy) continue;
    const NativeColor &candidate = reference.nativeRegionColors[region];
    if (!candidate.valid || !std::isfinite(candidate.r) ||
        !std::isfinite(candidate.g) || !std::isfinite(candidate.b) ||
        !std::isfinite(candidate.a) || candidate.a < 0.0 || candidate.a > 1.0)
      continue;
    if (seen.emplace(candidate.r, candidate.g, candidate.b, candidate.a,
                     candidate.linear).second)
      regions.append(region);
  }
  return regions;
}

}  // namespace Cadmium
