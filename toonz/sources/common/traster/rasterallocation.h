#pragma once

#include <limits>
#include <new>

namespace TRasterInternal {

// Raster offsets and several pixel operations still use signed int byte
// counts. Enforce that limit before multiplying, including attached buffers
// whose row stride is wider than the visible image.
inline unsigned int checkedByteCount(int lx, int ly, int pixelSize, int wrap) {
  const int limit = (std::numeric_limits<int>::max)();
  if (lx <= 0 || ly <= 0 || pixelSize <= 0 || wrap < lx ||
      wrap > limit / pixelSize)
    throw std::bad_alloc();
  const int rowBytes = wrap * pixelSize;
  if (ly > limit / rowBytes) throw std::bad_alloc();
  return static_cast<unsigned int>(rowBytes * ly);
}

}  // namespace TRasterInternal
