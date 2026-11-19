// SPDX-License-Identifier: BSD-3-Clause

// TRasterFx::getBBox rounds the box of an fx outwards to whole pixels. A box
// that is infinite, not a number or beyond the range of int cannot be rounded
// through int and is returned as the fx gave it.

#include "trasterfx.h"
#include "tgeometry.h"

#include <cmath>
#include <cfloat>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

class BoxFx final : public TRasterFx {
public:
  TRectD box;

  std::string getPluginId() const override { return "bbox-test"; }
  const TPersistDeclaration* getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings&, double) override { return true; }
  bool doGetBBox(double, TRectD& out, const TRenderSettings&) override {
    out = box;
    return true;
  }
  void doCompute(TTile&, double, const TRenderSettings&) override {}
};

TRectD bboxOf(const TRectD& box, const TAffine& affine = TAffine()) {
  TRasterFxP fx                             = new BoxFx;
  static_cast<BoxFx*>(fx.getPointer())->box = box;
  TRenderSettings info;
  info.m_affine = affine;
  TRectD result;
  require(fx->getBBox(0, result, info), "the fixture fx reports a box");
  return result;
}

bool same(const TRectD& a, const TRectD& b) {
  return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

void ordinaryBoxesAreRoundedOutwards() {
  require(same(bboxOf(TRectD(0.25, -1.5, 3.75, 4.0)), TRectD(0, -2, 4, 4)),
          "a fractional box is rounded outwards");
  require(same(bboxOf(TRectD(2, 3, 9, 10)), TRectD(2, 3, 9, 10)),
          "a whole-pixel box is unchanged");
  require(same(bboxOf(TRectD(-0.5, -0.5, 0.5, 0.5)), TRectD(-1, -1, 1, 1)),
          "a box around the origin is rounded outwards");
  require(
      same(bboxOf(TRectD(0.3, 0.3, 1.2, 1.2), TScale(2)), TRectD(0, 0, 3, 3)),
      "the render affine is applied before rounding");
  require(same(bboxOf(TRectD(-2e9 + 0.5, 0, 2e9 + 0.5, 1.5)),
               TRectD(-2e9, 0, 2e9 + 1, 2)),
          "a box close to the range of int is still rounded");
}

void unroundableBoxesAreReturnedAsGiven() {
  const TRectD infinite = TConsts::infiniteRectD;
  require(same(bboxOf(infinite), infinite), "an infinite box stays infinite");
  require(same(bboxOf(TRectD(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX)),
               TRectD(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX)),
          "a box of the largest doubles is unchanged");
  const TRectD half(-DBL_MAX, 0.25, 10.5, 3.5);
  require(same(bboxOf(half), half), "a box open on one side is unchanged");
  const TRectD wide(-3e9, 0.5, 3e9, 1.5);
  require(same(bboxOf(wide), wide), "a box beyond int is unchanged");
  const TRectD edge(0.5, 0.5, 2147483647.5, 1.5);
  require(same(bboxOf(edge), edge), "a box just beyond int is unchanged");
  // A box that is not a number has no defined rounding; it only has to be
  // accepted without converting NaN to int.
  bboxOf(TRectD(std::nan(""), 0, 4, 4));
}

}  // namespace

int main() {
  try {
    ordinaryBoxesAreRoundedOutwards();
    unroundableBoxesAreReturnedAsGiven();
  } catch (const std::exception& error) {
    std::cerr << "rasterfx_bbox: FAILED: " << error.what() << '\n';
    return 1;
  }
  std::cout << "rasterfx_bbox: PASS\n";
  return 0;
}
