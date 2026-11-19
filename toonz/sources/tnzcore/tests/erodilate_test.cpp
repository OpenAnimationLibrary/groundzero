// SPDX-License-Identifier: BSD-3-Clause

// TRop::erodilate on rasters with transparent pixels. Eroding a transparent
// pixel scaled its color by 0 / 0 and stored the NaN in a channel; the result
// must be a transparent pixel, and every other pixel must keep the value the
// scaling by (new matte) / (old matte) has always given.

#include "trop.h"
#include "traster.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

template <class Pix>
bool equals(const Pix& a, const Pix& b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.m == b.m;
}

// One row of nine pixels, repeated on three rows: an opaque run, a half
// transparent pixel, then transparent pixels, one of which keeps a color.
template <class Pix, class Chan>
void makeSource(TRasterPT<Pix>& raster, Chan r, Chan g, Chan b, Chan m,
                Chan halfScale) {
  for (int y = 0; y < raster->getLy(); ++y)
    for (int x = 0; x < raster->getLx(); ++x) {
      Pix& pixel = raster->pixels(y)[x];
      if (x < 4)
        pixel = Pix(r, g, b, m);
      else if (x == 4)
        pixel = Pix(r / 2, g / 2, b / 2, halfScale);
      else if (x == 5)
        pixel = Pix(9, 0, 0, 0);
      else
        pixel = Pix(0, 0, 0, 0);
    }
}

template <class Pix, class Chan>
void erodeRectangular(Chan r, Chan g, Chan b, Chan m, Chan half,
                      Pix expectedEdge, const char* kind) {
  TRasterPT<Pix> src(9, 3), dst(9, 3);
  makeSource<Pix, Chan>(src, r, g, b, m, half);
  TRop::erodilate(src, dst, -1.0, TRop::ED_rectangular);
  const std::string name(kind);
  // Row 1 has the same pixel above and below, so only the horizontal
  // neighbours change it. The first pixel is next to the raster edge, which
  // erodes like transparency.
  const Pix* row = dst->pixels(1);
  require(equals(row[0], Pix(0, 0, 0, 0)),
          name + ": a pixel at the raster edge is eroded away");
  require(equals(row[1], Pix(r, g, b, m)),
          name + ": an interior pixel is unchanged");
  require(equals(row[2], Pix(r, g, b, m)),
          name + ": an interior pixel is unchanged");
  require(equals(row[3], expectedEdge),
          name +
              ": a pixel beside a half transparent one is scaled by new / old "
              "matte");
  require(equals(row[4], Pix(0, 0, 0, 0)),
          name + ": a pixel beside a transparent one is eroded away");
  for (int x = 5; x < 9; ++x)
    require(
        equals(row[x], Pix(0, 0, 0, 0)),
        name +
            ": a transparent pixel stays transparent, whatever color it had");
  for (int y = 0; y < 3; y += 2)
    for (int x = 0; x < 9; ++x)
      require(dst->pixels(y)[x].m <= src->pixels(y)[x].m,
              name + ": erosion never raises the matte");
}

template <class Pix, class Chan>
void erodeCircular(Chan r, Chan g, Chan b, Chan m, Chan half,
                   const char* kind) {
  TRasterPT<Pix> src(9, 3), dst(9, 3);
  makeSource<Pix, Chan>(src, r, g, b, m, half);
  TRop::erodilate(src, dst, -1.0, TRop::ED_circular);
  const std::string name(kind);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 9; ++x) {
      const Pix &in = src->pixels(y)[x], &out = dst->pixels(y)[x];
      require(out.m <= in.m,
              name + ": circular erosion never raises the matte");
      if (in.m == 0)
        require(
            equals(out, Pix(0, 0, 0, 0)),
            name +
                ": a transparent pixel stays transparent in circular erosion");
    }
}

template <class Pix, class Chan>
void dilateKeepsColors(Chan r, Chan g, Chan b, Chan m, Chan half,
                       const char* kind) {
  TRasterPT<Pix> src(9, 3), dst(9, 3);
  makeSource<Pix, Chan>(src, r, g, b, m, half);
  TRop::erodilate(src, dst, 1.0, TRop::ED_rectangular);
  const std::string name(kind);
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 9; ++x) {
      const Pix &in = src->pixels(y)[x], &out = dst->pixels(y)[x];
      require(out.r == in.r && out.g == in.g && out.b == in.b,
              name + ": dilation keeps the colors");
      require(out.m >= in.m, name + ": dilation never lowers the matte");
    }
}

}  // namespace

int main() {
  try {
    erodeRectangular<TPixel32, UCHAR>(100, 50, 21, 200, 100,
                                      TPixel32(50, 25, 10, 100), "8-bit");
    erodeRectangular<TPixel64, USHORT>(30000, 20000, 10001, 40000, 20000,
                                       TPixel64(15000, 10000, 5000, 20000),
                                       "16-bit");
    erodeCircular<TPixel32, UCHAR>(100, 50, 21, 200, 100, "8-bit");
    erodeCircular<TPixel64, USHORT>(30000, 20000, 10001, 40000, 20000,
                                    "16-bit");
    dilateKeepsColors<TPixel32, UCHAR>(100, 50, 21, 200, 100, "8-bit");
    dilateKeepsColors<TPixel64, USHORT>(30000, 20000, 10001, 40000, 20000,
                                        "16-bit");
  } catch (const std::exception& error) {
    std::cerr << "erodilate: FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "erodilate: PASS\n";
  return 0;
}
