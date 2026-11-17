#include <array>
#include <cstring>
#include <iostream>

#include "tpixel.h"

int main() {
#if TNZ_LITTLE_ENDIAN
  const std::array<TPixel32, 3> pixels = {TPixel32(0x12, 0x34, 0x56, 0x78),
                                          TPixel32(0xff, 0x80, 0x01, 0x00),
                                          TPixel32(0x00, 0x01, 0x80, 0xff)};
  const std::array<unsigned char, 12> expected = {0x56, 0x34, 0x12, 0x78, 0x01, 0x80,
                                                  0xff, 0x00, 0x80, 0x01, 0x00, 0xff};
  static_assert(sizeof(pixels) == sizeof(expected), "Pixels must retain four-byte storage");
  std::array<unsigned char, sizeof(pixels)> actual;
  std::memcpy(actual.data(), pixels.data(), sizeof(pixels));
  if (actual != expected) {
    std::cerr << "Little-endian macOS pixels must preserve native BGRM bytes\n";
    return 1;
  }
#endif
  return 0;
}
