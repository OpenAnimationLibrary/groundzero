// SPDX-License-Identifier: BSD-3-Clause

// swapShort, swapUshort and swapTINT32 reverse the bytes of a value. They
// used to shift negative values left. Checked against a table built with
// explicit byte extraction, for every 16-bit value and for 32-bit values built
// from every combination of edge bytes plus a pseudo-random sequence.

#include "tcommon.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

uint32_t reversed32(uint32_t v) {
  const uint32_t b0 = v & 0xFF, b1 = (v >> 8) & 0xFF, b2 = (v >> 16) & 0xFF,
                 b3 = (v >> 24) & 0xFF;
  return b0 * 16777216u + b1 * 65536u + b2 * 256u + b3;
}

void sixteenBitValues() {
  for (int v = -32768; v <= 32767; ++v) {
    const unsigned bits     = unsigned(v) & 0xFFFFu;
    const unsigned expected = ((bits & 0xFFu) << 8) | (bits >> 8);
    require(unsigned(swapShort(short(v))) % 65536u == expected,
            "swapShort reverses the bytes of " + std::to_string(v));
    require(short(swapShort(swapShort(short(v)))) == v,
            "swapShort twice is the identity for " + std::to_string(v));
  }
  for (unsigned v = 0; v <= 0xFFFFu; ++v) {
    const unsigned expected = ((v & 0xFFu) << 8) | (v >> 8);
    require(swapUshort(USHORT(v)) == expected,
            "swapUshort reverses the bytes of " + std::to_string(v));
  }
}

void thirtyTwoBitValues() {
  const uint32_t edge[] = {0x00, 0x01, 0x7F, 0x80, 0x81,
                           0xFE, 0xFF, 0x55, 0xAA};
  for (uint32_t b3 : edge)
    for (uint32_t b2 : edge)
      for (uint32_t b1 : edge)
        for (uint32_t b0 : edge) {
          const uint32_t v = (b3 << 24) | (b2 << 16) | (b1 << 8) | b0;
          require(uint32_t(swapTINT32(TINT32(v))) == reversed32(v),
                  "swapTINT32 reverses the bytes of " + std::to_string(v));
        }
  uint32_t state = 12345u;
  for (int i = 0; i < 2000000; ++i) {
    state = state * 1664525u + 1013904223u;
    require(uint32_t(swapTINT32(TINT32(state))) == reversed32(state),
            "swapTINT32 reverses the bytes of " + std::to_string(state));
    require(uint32_t(swapTINT32(swapTINT32(TINT32(state)))) == state,
            "swapTINT32 twice is the identity for " + std::to_string(state));
  }
  require(swapTINT32(TINT32(0x12345678)) == TINT32(0x78563412),
          "a known value");
  require(swapTINT32(TINT32(0x32000000)) == TINT32(0x32),
          "a value whose low byte shifts into the sign bit");
  require(swapTINT32(TINT32(0x000000FF)) == TINT32(0xFF000000u),
          "the low byte moves to the sign byte");
}

}  // namespace

int main() {
  try {
    sixteenBitValues();
    thirtyTwoBitValues();
  } catch (const std::exception& error) {
    std::cerr << "byteswap: FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "byteswap: PASS\n";
  return 0;
}
