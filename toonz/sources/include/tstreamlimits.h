#pragma once

#include <cstdint>

// The TABc format stores signed 32-bit sizes. Keep writer admission, scene
// publication and reader allocation limits identical so a successful scene
// save cannot produce a file that its reader rejects as too large.
namespace TStreamLimits {
constexpr std::uint64_t maxCompressedBytes = 100000000;
constexpr std::uint64_t maxUncompressedBytes = std::uint64_t(1) << 28;

inline bool validCompressedSizes(std::uint64_t compressed,
                                 std::uint64_t uncompressed) {
  return compressed > 0 && compressed <= maxCompressedBytes &&
         uncompressed <= maxUncompressedBytes &&
         uncompressed <= compressed * 256 + 1024;
}
}  // namespace TStreamLimits
