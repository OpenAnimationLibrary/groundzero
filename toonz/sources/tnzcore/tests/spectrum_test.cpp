#include "tspectrum.h"

#include <iostream>
#include <limits>

namespace {
int failures = 0;

void check(bool value, const char* message) {
  if (value) return;
  ++failures;
  std::cerr << "FAILED: " << message << '\n';
}

template <class Pixel>
void lookupsStayInsideTheTable(const Pixel& first, const Pixel& last) {
  const TSpectrumT<Pixel> spectrum(first, last);
  const double nan      = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  check(spectrum.getValue(nan) == first, "NaN reads the first sample");
  check(spectrum.getPremultipliedValue(nan) == premultiply(first),
        "NaN reads the first premultiplied sample");
  check(spectrum.getValue(-infinity) == first,
        "-infinity reads the first sample");
  check(spectrum.getValue(infinity) == last, "infinity reads the last sample");
  check(spectrum.getPremultipliedValue(infinity) == premultiply(last),
        "infinity reads the last premultiplied sample");
  check(spectrum.getValue(0) == first && spectrum.getValue(1) == last,
        "the end positions are unchanged");
}
}  // namespace

int main() {
  lookupsStayInsideTheTable(TPixel32(200, 40, 16, 255),
                            TPixel32(24, 72, 192, 128));
  lookupsStayInsideTheTable(TPixel64(51400, 10280, 4112, 65535),
                            TPixel64(6168, 18504, 49344, 32896));
  if (failures) return 1;
  std::cout << "spectrum lookups passed\n";
  return 0;
}
