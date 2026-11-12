#pragma once
#include <cmath>
#include <cstddef>

namespace RasterBrush {
namespace SourceGihV1 {
// Experimental V1 uses 64 phase buckets; whole/half-pixel captures establish
// the endpoint distinction, while finer grid granularity awaits independent
// fractional-origin heldouts. This helper does not grant source qualification.
// Retaining the upper phase endpoint is deliberate. An immediately preceding
// integer represented from below has phase 64, while the integer has phase 0;
// they rasterize identically but invalidate different cached transform keys.
inline int phase(double coordinate) {
  return int(std::floor((coordinate - std::floor(coordinate)) * 64 + .5));
}
inline double quantized(double coordinate) {
  return std::floor(coordinate) + phase(coordinate) / 64.;
}
struct GeometryKey {
  const void *resources = nullptr, *proposalResource = nullptr;
  std::size_t resourceCount = 0;
  int width = 0, height = 0, phaseX = 0, phaseY = 0;
  double diameter = 0, aspect = 1, rotation = 0;
  bool mirrorX = false, mirrorY = false, colorAsMask = false;
  bool operator==(const GeometryKey &other) const {
    return resources == other.resources && proposalResource == other.proposalResource &&
        resourceCount == other.resourceCount && width == other.width && height == other.height &&
        phaseX == other.phaseX && phaseY == other.phaseY && diameter == other.diameter &&
        aspect == other.aspect && rotation == other.rotation && mirrorX == other.mirrorX &&
        mirrorY == other.mirrorY && colorAsMask == other.colorAsMask;
  }
};
class Selection {
  bool initialized = false;
  GeometryKey cached;
  std::size_t proposal = 0, actual = 0;
public:
  template <class FreshSelection>
  std::size_t select(const GeometryKey &key, std::size_t proposed,
                     FreshSelection freshSelection) {
    if (!initialized || proposed != proposal || !(key == cached)) {
      actual = freshSelection();
      proposal = proposed;
      cached = key;
      initialized = true;
    }
    return actual;
  }
  bool valid() const { return initialized; }
  std::size_t proposedFrame() const { return proposal; }
  std::size_t actualFrame() const { return actual; }
};
} // namespace SourceGihV1
} // namespace RasterBrush
