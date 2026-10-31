#pragma once

#include <array>
#include <cstdint>

// Private, bounded exchange format between the installed thumbnail provider
// and tconverter. Pixels are premultiplied BGRA, bottom row first, with no
// padding. It is not a document format and does not depend on C++ struct ABI.
namespace ShellThumbnail {
constexpr unsigned MaxSide = 1024;
constexpr unsigned HeaderSize = 16;
constexpr unsigned TimeoutMs = 20000;
constexpr char Command[] = "--shell-thumbnail";
constexpr std::array<unsigned char, 8> Magic{{'G','Z','T','H','U','M','B','1'}};
inline void put32(unsigned char *out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) out[i] = static_cast<unsigned char>(value >> (i * 8));
}
inline std::uint32_t get32(const unsigned char *in) {
  return std::uint32_t(in[0]) | (std::uint32_t(in[1]) << 8) |
      (std::uint32_t(in[2]) << 16) | (std::uint32_t(in[3]) << 24);
}
inline std::array<unsigned char, HeaderSize> header(unsigned width, unsigned height) {
  std::array<unsigned char, HeaderSize> result{};
  for (unsigned i = 0; i < Magic.size(); ++i) result[i] = Magic[i];
  put32(result.data() + 8, width); put32(result.data() + 12, height);
  return result;
}
inline bool dimensions(const unsigned char *bytes, unsigned &width, unsigned &height) {
  for (unsigned i = 0; i < Magic.size(); ++i) if (bytes[i] != Magic[i]) return false;
  width = get32(bytes + 8); height = get32(bytes + 12);
  return width && height && width <= MaxSide && height <= MaxSide;
}
} // namespace ShellThumbnail
