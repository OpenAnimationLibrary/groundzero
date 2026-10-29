// Regression test of the Colour Script (doc/colour_script.md): the hue,
// saturation and lightness shift (toonz/palettetransform.h), and the render
// data that applies it to a copy of a palette, keeping the styles named with
// the keep prefix.

#include "toonz/palettetransform.h"
#include "ttzpimagefx.h"
#include "tcolorstyles.h"
#include "tpalette.h"
#include "toonz/txshlevelcolumn.h"

#include <QCoreApplication>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
bool approx(double a, double b, double tolerance = 1e-9) {
  return std::abs(a - b) <= tolerance;
}

void testMath() {
  using namespace PaletteTransform;
  // HSL round trip over a grid of colours
  for (int r = 0; r <= 10; ++r)
    for (int g = 0; g <= 10; ++g)
      for (int b = 0; b <= 10; ++b) {
        const double rgb[3] = {r / 10.0, g / 10.0, b / 10.0};
        double h, s, l, back[3];
        rgbToHsl(rgb, h, s, l);
        require(h >= 0 && h < 360 && s >= 0 && s <= 1 && l >= 0 && l <= 1,
                "math: HSL out of range");
        hslToRgb(h, s, l, back);
        for (int i = 0; i < 3; ++i)
          require(approx(back[i], rgb[i], 1e-9),
                  "math: HSL does not round trip");
      }

  Shift none;
  require(none.isIdentity(), "math: the default shift does something");
  double c[3] = {0.3, 0.6, 0.9};
  applyStraight(none, c);
  require(c[0] == 0.3 && c[1] == 0.6 && c[2] == 0.9,
          "math: no shift changed a colour");
  Shift turn;
  turn.hue = 360;
  require(turn.isIdentity(), "math: a full turn is not the identity");

  // Red turned by 120 degrees is green, by -120 blue
  Shift green;
  green.hue     = 120;
  double red[3] = {1, 0, 0};
  applyStraight(green, red);
  require(approx(red[0], 0) && approx(red[1], 1) && approx(red[2], 0),
          "math: the hue does not turn red into green");
  Shift blue;
  blue.hue       = -120;
  double red2[3] = {1, 0, 0};
  applyStraight(blue, red2);
  require(approx(red2[2], 1) && approx(red2[0], 0), "math: -120 is not blue");

  // Saturation -1 is grey with the same lightness; lightness 1 is white,
  // -1 black
  Shift grey;
  grey.saturation = -1;
  double g[3]     = {0.8, 0.2, 0.4};
  applyStraight(grey, g);
  require(approx(g[0], g[1]) && approx(g[1], g[2]) && approx(g[0], 0.5),
          "math: no saturation is not grey");
  Shift white, black;
  white.lightness = 1, black.lightness = -1;
  double w[3] = {0.1, 0.5, 0.3}, k[3] = {0.1, 0.5, 0.3};
  applyStraight(white, w);
  applyStraight(black, k);
  require(approx(w[0], 1) && approx(w[1], 1) && approx(w[2], 1) &&
              approx(k[0], 0) && approx(k[1], 0) && approx(k[2], 0),
          "math: lightness does not reach white and black");
  // Half way: halfway in lightness
  Shift half;
  half.lightness = 0.5;
  double m[3]    = {0.4, 0.4, 0.4};
  applyStraight(half, m);
  require(approx(m[0], 0.7), "math: half the lightness is not half the way");

  // Premultiplied: same colour at any alpha, transparent stays so
  double p[3] = {0.5, 0, 0};
  applyPremultiplied(green, p, 0.5);
  require(approx(p[0], 0) && approx(p[1], 0.5), "math: premultiplied is wrong");
  double t[3] = {0, 0, 0};
  applyPremultiplied(white, t, 0);
  require(t[0] == 0 && t[1] == 0 && t[2] == 0,
          "math: a transparent pixel changed");

  int r8 = 255, g8 = 0, b8 = 0;
  apply8(green, r8, g8, b8);
  require(r8 == 0 && g8 == 255 && b8 == 0, "math: 8-bit shift is wrong");

  require(isKept(L"KEEP skin", L"KEEP") && !isKept(L"skin", L"KEEP") &&
              !isKept(L"KEEP skin", L""),
          "math: the keep prefix is wrong");

  Shift bad;
  bad.hue = 540, bad.saturation = 4, bad.lightness = NAN;
  bad.clamp();
  require(approx(std::abs(bad.hue), 180) && bad.saturation == 1 &&
              bad.lightness == 0,
          "math: clamp does not make the shift valid");
}

void testPalette() {
  TPaletteP palette    = new TPalette();
  TPalette::Page *page = palette->getPage(0);
  const int red        = page->addStyle(TPixel32(255, 0, 0));
  const int kept       = page->addStyle(TPixel32(255, 0, 0));
  const int other      = page->addStyle(TPixel32(0, 0, 255, 128));
  palette->getStyle(kept)->setName(L"KEEP skin");
  const TPixel32 none = palette->getStyle(0)->getMainColor();

  ColourOverrideFxRenderData data;
  require(data.isEmpty(), "palette: new data is not empty");
  const std::string plain = data.toString();
  data.m_shift.hue        = 120;
  data.m_keepPrefix       = L"KEEP";
  require(!data.isEmpty(), "palette: a shift is empty");
  require(data.toString() != plain,
          "palette: the shift is not in the cache key");

  TPaletteP copy = palette->clone();
  data.apply(copy.getPointer());
  require(copy->getStyle(red)->getMainColor() == TPixel32(0, 255, 0),
          "palette: a style was not shifted");
  require(copy->getStyle(kept)->getMainColor() == TPixel32(255, 0, 0),
          "palette: a kept style was shifted");
  require(copy->getStyle(other)->getMainColor() == TPixel32(255, 0, 0, 128),
          "palette: the matte of a style changed");
  require(copy->getStyle(0)->getMainColor() == none,
          "palette: style 0 was shifted");
  require(palette->getStyle(red)->getMainColor() == TPixel32(255, 0, 0),
          "palette: the original palette was modified");

  // Overrides come first, then the shift
  ColourOverrideFxRenderData both;  // not copyable
  both.m_shift      = data.m_shift;
  both.m_keepPrefix = data.m_keepPrefix;
  both.m_overrides.push_back(
      ColourOverrideFxRenderData::Entry(red, TPixel32(0, 0, 255)));
  TPaletteP copy2 = palette->clone();
  both.apply(copy2.getPointer());
  require(copy2->getStyle(red)->getMainColor() == TPixel32(255, 0, 0),
          "palette: the shift is not applied after the override");

  // Without the shift: only the overrides
  ColourOverrideFxRenderData *kept2 = both.withoutShift();
  TRasterFxRenderDataP keep(kept2);
  require(kept2->m_shift.isIdentity() && kept2->m_overrides == both.m_overrides,
          "palette: withoutShift() is wrong");
  require(!(both == *kept2) && both == both, "palette: equality is wrong");
}

void testColumnFlag() {
  // The Keep Colours bit is its own: it does not touch the others
  TXshLevelColumn column;
  column.setDepthExcluded(true);
  column.setOutlineView(true);
  require(!column.areColoursKept(), "column: colours kept by default");
  column.setColoursKept(true);
  require(column.areColoursKept() && column.isDepthExcluded() &&
              column.isOutlineView() && column.isCamstandVisible(),
          "column: Keep Colours changed another flag");
  column.setColoursKept(false);
  require(!column.areColoursKept() && column.isDepthExcluded(),
          "column: clearing Keep Colours changed another flag");
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  try {
    testMath();
    testPalette();
    testColumnFlag();
  } catch (const std::exception &e) {
    std::cerr << "colourscript_test: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "ok: colour script" << std::endl;
  return 0;
}
