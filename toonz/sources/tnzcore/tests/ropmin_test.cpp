#include "trop.h"
#include "traster.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <class Pixel>
void checkPixel(const Pixel& up, const Pixel& down, const Pixel& expected,
                bool matte) {
  TRasterPT<Pixel> upper(1, 1), lower(1, 1), output(1, 1);
  upper->pixels()[0] = up;
  lower->pixels()[0] = down;
  TRop::ropmin(upper, lower, output, matte);
  require(output->pixels()[0] == expected,
          "ropmin returned an unexpected native pixel");
  require(upper->pixels()[0] == up && lower->pixels()[0] == down,
          "ropmin changed an input raster");
}

void controls() {
  checkPixel(TPixel32(200, 10, 30, 255), TPixel32(100, 50, 20, 128),
             TPixel32(100, 10, 20, 128), false);
  checkPixel(TPixel32(20, 60, 10, 128), TPixel32(100, 50, 20, 128),
             TPixel32(59, 50, 14, 191), false);
  checkPixel(TPixel32(200, 10, 30, 0), TPixel32(100, 50, 20, 128),
             TPixel32(100, 50, 20, 128), false);
  checkPixel(TPixel32(20, 60, 10, 128), TPixel32(100, 50, 20, 64),
             TPixel32(20, 50, 10, 64), true);
  checkPixel(TPixel64(50000, 1000, 30000, 32768),
             TPixel64(40000, 2000, 60000, 40000),
             TPixel64(40000, 1000, 30000, 32768), true);
  checkPixel(TPixelF(0.25f, 0.75f, 0.125f, 0.5f),
             TPixelF(0.75f, 0.5f, 0.25f, 0.5f),
             TPixelF(0.5f, 0.5f, 0.1875f, 0.75f), false);
  checkPixel(TPixelF(0.25f, 0.75f, 0.125f, 0.5f),
             TPixelF(0.75f, 0.5f, 0.25f, 0.25f),
             TPixelF(0.25f, 0.5f, 0.125f, 0.25f), true);
  std::cout << "ropmin-test: unchanged 8-bit, float, and matte controls PASS"
            << std::endl;
}

void rgba64Edges() {
  checkPixel(TPixel64(65535, 1000, 50000, 0),
             TPixel64(40000, 2000, 60000, 65535),
             TPixel64(40000, 2000, 60000, 65535), false);
  checkPixel(TPixel64(50000, 1000, 30000, 65535),
             TPixel64(40000, 2000, 40000, 50000),
             TPixel64(40000, 1000, 30000, 50000), false);
  checkPixel(TPixel64(8000, 12000, 16000, 32768),
             TPixel64(10000, 10000, 5000, 16384),
             TPixel64(8999, 10000, 5000, 40959), false);
  checkPixel(TPixel64(0, 0, 0, 1), TPixel64(65535, 65535, 65535, 65535),
             TPixel64(65534, 65534, 65534, 65535), false);
  checkPixel(TPixel64(0, 65534, 32767, 65534),
             TPixel64(65535, 65535, 65535, 65535),
             TPixel64(1, 65534, 32767, 65535), false);
  checkPixel(TPixel64(0, 0, 0, 65534), TPixel64(0, 0, 0, 0),
             TPixel64(0, 0, 0, 65534), false);
  std::cout
      << "ropmin-test: 16-bit opacity, precision, and product boundaries PASS"
      << std::endl;
}

void rgba64StrideAndAlias() {
  const TPixel64 sentinel(123, 456, 789, 987);
  std::array<TPixel64, 8> upper, lower, output;
  upper.fill(sentinel);
  lower.fill(sentinel);
  output.fill(sentinel);
  const std::array<TPixel64, 4> source{
      TPixel64(65535, 1000, 50000, 0), TPixel64(50000, 1000, 30000, 65535),
      TPixel64(8000, 12000, 16000, 32768), TPixel64(0, 65534, 32767, 65534)};
  const std::array<TPixel64, 4> background{
      TPixel64(40000, 2000, 60000, 65535), TPixel64(40000, 2000, 40000, 50000),
      TPixel64(10000, 10000, 5000, 16384),
      TPixel64(65535, 65535, 65535, 65535)};
  const std::array<TPixel64, 4> expected{
      background[0], TPixel64(40000, 1000, 30000, 50000),
      TPixel64(8999, 10000, 5000, 40959), TPixel64(1, 65534, 32767, 65535)};
  for (int index = 0; index < 4; ++index) {
    upper[(index / 2) * 4 + index % 2] = source[index];
    lower[(index / 2) * 4 + index % 2] = background[index];
  }
  TRaster64P up(2, 2, 4, upper.data()), down(2, 2, 4, lower.data());
  TRaster64P out(2, 2, 4, output.data());
  TRop::ropmin(up, down, out, false);
  TRop::ropmin(up, down, down, false);
  for (int index = 0; index < 4; ++index) {
    const int offset = (index / 2) * 4 + index % 2;
    require(
        output[offset] == expected[index] && lower[offset] == expected[index],
        "16-bit ropmin changed stride or output-alias behavior");
    require(upper[offset] == source[index],
            "16-bit ropmin modified the foreground");
  }
  for (int offset : {2, 3, 6, 7})
    require(output[offset] == sentinel && upper[offset] == sentinel &&
                lower[offset] == sentinel,
            "16-bit ropmin wrote into row padding");
  std::cout << "ropmin-test: native 16-bit stride and alias handling PASS"
            << std::endl;
}

void rejectedCombination() {
  TRaster32P up(1, 1);
  TRaster64P down(1, 1), out(1, 1);
  const TPixel64 sentinel(123, 456, 789, 987);
  out->pixels()[0] = sentinel;
  try {
    TRop::ropmin(up, down, out, false);
  } catch (const TRopException&) {
    require(out->pixels()[0] == sentinel,
            "Rejected ropmin request changed its output");
    std::cout << "ropmin-test: incompatible rasters reject without writing PASS"
              << std::endl;
    return;
  }
  throw std::runtime_error("ropmin accepted incompatible native raster types");
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    controls();
    rejectedCombination();
    if (argc == 2 && std::string(argv[1]) == "--controls-only") return 0;
    require(argc == 1,
            "ropmin-test accepts only --controls-only or no arguments");
    rgba64Edges();
    rgba64StrideAndAlias();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ropmin-test: " << error.what() << '\n';
    return 1;
  }
}
