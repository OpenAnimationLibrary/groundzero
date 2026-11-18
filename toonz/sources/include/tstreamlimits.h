#pragma once

#include <cstddef>
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

// TIStream refuses tags nested deeper than this. The loaders recurse once per
// nested object: a nested sub-xsheet is 6 tags and an FX input written inline
// is 3, at about 300 bytes of stack per tag in a release build, so 1024 tags
// stay well within the 1 MB main thread stack of Windows. Scenes the editor
// writes nest about 30 tags; a macro of 64 chained FX nests 199.
constexpr std::size_t maxTagDepth = 1024;
}  // namespace TStreamLimits
