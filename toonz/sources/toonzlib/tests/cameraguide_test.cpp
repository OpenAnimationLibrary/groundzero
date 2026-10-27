// Regression test of the aspect ratio guide math (toonz/cameraguide.h): the
// guide rect centred in a frame, the typed ratios, and the letterbox bars.

#include "toonz/cameraguide.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

bool near(const TRectD &a, const TRectD &b) {
  return near(a.x0, b.x0) && near(a.y0, b.y0) && near(a.x1, b.x1) &&
         near(a.y1, b.y1);
}

//------------------------------------------------------------------

void testCenteredRect() {
  const TRectD hd(-960, -540, 960, 540);
  // Narrower than the frame: full height, pillarboxed
  require(near(CameraGuide::centeredRect(hd, 4.0 / 3.0),
               TRectD(-720, -540, 720, 540)),
          "centered: 4:3 in 16:9");
  // Wider than the frame: full width, letterboxed
  const TRectD scope = CameraGuide::centeredRect(hd, 2.40);
  require(near(scope, TRectD(-960, -400, 960, 400)), "centered: 2.40 in 16:9");
  require(near(scope.getLx() / scope.getLy(), 2.40), "centered: ratio kept");
  // The same ratio as the frame
  require(near(CameraGuide::centeredRect(hd, 16.0 / 9.0), hd),
          "centered: same ratio");
  // An off-centre frame stays centred on itself
  require(near(CameraGuide::centeredRect(TRectD(0, 0, 200, 100), 1.0),
               TRectD(50, 0, 150, 100)),
          "centered: off-centre frame");
  // Bad input
  require(CameraGuide::centeredRect(hd, 0.0).isEmpty(), "centered: zero");
  require(CameraGuide::centeredRect(hd, -1.0).isEmpty(), "centered: negative");
  require(
      CameraGuide::centeredRect(hd, std::numeric_limits<double>::quiet_NaN())
          .isEmpty(),
      "centered: NaN");
  require(CameraGuide::centeredRect(TRectD(), 1.85).isEmpty(),
          "centered: empty frame");
}

//------------------------------------------------------------------

void testParseRatio() {
  require(near(CameraGuide::parseRatio("1.85"), 1.85), "parse: decimal");
  require(near(CameraGuide::parseRatio("2.39:1"), 2.39), "parse: n:1");
  require(near(CameraGuide::parseRatio("16:9"), 16.0 / 9.0), "parse: w:h");
  require(near(CameraGuide::parseRatio("4/3"), 4.0 / 3.0), "parse: w/h");
  require(near(CameraGuide::parseRatio(" 21 : 9 "), 21.0 / 9.0),
          "parse: spaces");
  require(CameraGuide::parseRatio("") == 0.0, "parse: empty");
  require(CameraGuide::parseRatio("wide") == 0.0, "parse: text");
  require(CameraGuide::parseRatio("1.85x") == 0.0, "parse: trailing text");
  require(CameraGuide::parseRatio("16:0") == 0.0, "parse: zero height");
  require(CameraGuide::parseRatio("-2") == 0.0, "parse: negative");
  require(CameraGuide::parseRatio("0") == 0.0, "parse: zero");
  require(CameraGuide::parseRatio("1e999") == 0.0, "parse: overflow");
}

//------------------------------------------------------------------

double area(const std::vector<TRectD> &bars) {
  double sum = 0;
  for (const TRectD &bar : bars) sum += bar.getLx() * bar.getLy();
  return sum;
}

void testLetterboxBars() {
  const TRectD hd(-960, -540, 960, 540);
  const double frameArea = hd.getLx() * hd.getLy();

  // Letterbox: a top and a bottom bar
  const TRectD scope       = CameraGuide::centeredRect(hd, 2.40);
  std::vector<TRectD> bars = CameraGuide::letterboxBars(hd, scope);
  require(bars.size() == 2, "bars: letterbox count");
  require(near(area(bars) + scope.getLx() * scope.getLy(), frameArea),
          "bars: letterbox covers the rest");

  // Pillarbox: a left and a right bar
  const TRectD academy = CameraGuide::centeredRect(hd, 4.0 / 3.0);
  bars                 = CameraGuide::letterboxBars(hd, academy);
  require(bars.size() == 2, "bars: pillarbox count");
  require(near(bars[0].getLy(), hd.getLy()), "bars: pillarbox full height");

  // Windowbox: four bars that do not overlap
  const TRectD window(-100, -50, 100, 50);
  bars = CameraGuide::letterboxBars(hd, window);
  require(bars.size() == 4, "bars: windowbox count");
  require(near(area(bars) + window.getLx() * window.getLy(), frameArea),
          "bars: windowbox covers the rest once");

  // No bars when the guide fills the frame or is missing
  require(CameraGuide::letterboxBars(hd, hd).empty(), "bars: same rect");
  require(CameraGuide::letterboxBars(hd, TRectD()).empty(), "bars: no guide");
  require(CameraGuide::letterboxBars(TRectD(), scope).empty(),
          "bars: no frame");
}

//------------------------------------------------------------------

void testAspectPixelRect() {
  const TDimension hd(1920, 1080);
  // Letterbox: centred rows, exact size
  TRect r = CameraGuide::aspectPixelRect(hd, 2.39, false);
  require(r.getLx() == 1920 && r.getLy() == 803, "pixels: 2.39 size");
  require(r.y0 == 138 && r.y1 == 940, "pixels: 2.39 centred");
  // Crop: even sizes
  r = CameraGuide::aspectPixelRect(hd, 2.39, true);
  require(r.getLy() == 802, "pixels: 2.39 even");
  r = CameraGuide::aspectPixelRect(hd, 1.85, true);
  require(r.getLx() == 1920 && r.getLy() == 1038, "pixels: 1.85");
  // Pillarbox
  r = CameraGuide::aspectPixelRect(hd, 4.0 / 3.0, true);
  require(r.getLx() == 1440 && r.x0 == 240 && r.x1 == 1679,
          "pixels: 4:3 centred columns");
  // Same ratio and bad ratios: the whole frame
  require(CameraGuide::aspectPixelRect(hd, 16.0 / 9.0, true) ==
              TRect(0, 0, 1919, 1079),
          "pixels: same ratio");
  require(
      CameraGuide::aspectPixelRect(hd, 0.0, true) == TRect(0, 0, 1919, 1079),
      "pixels: zero ratio");
  // Tiny frames keep at least a pixel
  r = CameraGuide::aspectPixelRect(TDimension(3, 1), 100.0, true);
  require(r.getLx() == 2 && r.getLy() == 1, "pixels: tiny frame");
}

}  // namespace

int main() {
  try {
    testCenteredRect();
    testParseRatio();
    testLetterboxBars();
    testAspectPixelRect();
  } catch (const std::exception &e) {
    std::cerr << "cameraguide_test failed: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "cameraguide_test passed" << std::endl;
  return 0;
}
