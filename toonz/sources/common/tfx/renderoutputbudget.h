#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

namespace RenderOutputBudget {

// Bounds output rasters and their retained clones, excluding FX working memory,
// caches, and encoder buffers. A zero budget is unlimited; a finite budget admits
// at least one frame even when that single frame exceeds the budget.
inline std::size_t framesPerWindow(std::uint64_t budgetBytes, int lx, int ly,
                                  int bpp, bool stereo, bool field) {
  if (budgetBytes == 0) return (std::numeric_limits<std::size_t>::max)();
  if (lx <= 0 || ly <= 0) throw std::bad_alloc();

  const auto multiply = [](std::uint64_t a, std::uint64_t b) {
    if (a > (std::numeric_limits<std::uint64_t>::max)() / b)
      throw std::bad_alloc();
    return a * b;
  };
  // The raster pool uses wrap == lx. Stereo retains two outputs and two clones;
  // field rendering retains two outputs and one interlaced clone.
  const std::uint64_t pixelBytes = bpp == 128 ? 16 : bpp == 64 ? 8 : 4;
  const std::uint64_t copies     = stereo ? 4 : field ? 3 : 2;
  const std::uint64_t pixels =
      multiply(static_cast<std::uint64_t>(lx), static_cast<std::uint64_t>(ly));
  const std::uint64_t frameBytes = multiply(multiply(pixels, pixelBytes), copies);
  const std::uint64_t count      = budgetBytes / frameBytes;
  if (count == 0) return 1;
  if (count > (std::numeric_limits<std::size_t>::max)())
    return (std::numeric_limits<std::size_t>::max)();
  return static_cast<std::size_t>(count);
}

}  // namespace RenderOutputBudget
