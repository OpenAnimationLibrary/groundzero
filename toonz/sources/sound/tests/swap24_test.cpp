#include "tcommon.h"

#include "../tsioutils.h"

#include <iostream>
#include <vector>

namespace {
int failures = 0;

void check(bool value, const char* message) {
  if (value) return;
  ++failures;
  std::cerr << "FAILED: " << message << '\n';
}

void everySampleIsSwapped(int sampleCount) {
  std::vector<UCHAR> source(sampleCount * 3 + 3, 0);
  for (size_t i = 0; i < source.size(); ++i) source[i] = UCHAR(i + 1);
  std::vector<UCHAR> destination(source.size(), 0xEE);
  swapAndCopy24Bits(source.data(), destination.data(), sampleCount);
  for (int sample = 0; sample < sampleCount; ++sample) {
    const UCHAR* in  = &source[sample * 3];
    const UCHAR* out = &destination[sample * 3];
    check(out[0] == in[2] && out[1] == in[1] && out[2] == in[0],
          "each 24-bit sample is byte-swapped, including the last");
  }
  const UCHAR* after = &destination[sampleCount * 3];
  check(after[0] == 0xEE && after[1] == 0xEE && after[2] == 0xEE,
        "nothing is written past the requested samples");
}
}  // namespace

int main() {
  for (int sampleCount : {0, 1, 2, 7}) everySampleIsSwapped(sampleCount);
  if (failures == 0) std::cout << "swap24: PASS\n";
  return failures == 0 ? 0 : 1;
}
