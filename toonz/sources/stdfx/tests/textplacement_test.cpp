// SPDX-License-Identifier: BSD-3-Clause

// The Text and Time Code effects used to place their content from the center
// of the tile they were computing, corrected by half the camera size. That is
// right only for a tile the size of the frame: an effect downstream that
// enlarges the area it asks for (a blur) moved the content by the enlargement,
// and a frame computed as several tiles drew it once per tile in a different
// place. The content now goes to its position in render coordinates whatever
// the tile. This test computes each effect on the frame as one tile, as
// several tiles, on enlarged and shifted tiles and under a blur, and checks
// that each gives the frame tile's pixels where they overlap and that the
// frame tile puts the content at its parameter position.

#include "tdoubleparam.h"
#include "tfx.h"
#include "tfxparam.h"
#include "tparamset.h"
#include "traster.h"
#include "trasterfx.h"
#include "trenderer.h"
#include "tthread.h"
#include "tnotanimatableparam.h"

#include <QGuiApplication>

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

DV_IMPORT_API void initStdFx();

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// A frame not centered on the render origin, so a placement that ignored the
// tile position could not pass by accident
const TPointD cFramePos(-90, -70);
const TDimension cFrameSize(192, 128);

TFxP create(const std::string& id) {
  TFxP fx = TFx::create(id);
  require(fx.getPointer(), "unknown FX identifier " + id);
  return fx;
}

template <typename P>
P* param(const TFxP& fx, const std::string& name) {
  P* p = dynamic_cast<P*>(fx->getParams()->getParam(name));
  require(p, fx->getFxType() + " has no parameter " + name);
  return p;
}

// Computes fx on one tile, with a renderer of its own so no tile is served
// from another's cached result
TRaster32P compute(const TFxP& fx, const TPointD& pos, const TDimension& size) {
  TRenderer renderer(1);
  unsigned long renderId = TRenderer::buildRenderId();
  renderer.install(renderId);
  renderer.declareRenderStart(renderId);
  renderer.declareFrameStart(0);

  TRenderSettings info;
  info.m_bpp = 32;
  info.m_cameraBox =
      TRectD(cFramePos, TDimensionD(cFrameSize.lx, cFrameSize.ly));
  TRaster32P ras(size);
  ras->clear();
  TTile tile(ras, pos);
  TRasterFxP(fx)->compute(tile, 0, info);

  renderer.declareFrameEnd(0);
  renderer.declareRenderEnd(renderId);
  renderer.uninstall();
  return tile.getRaster();
}

struct Tile {
  TPointD pos;
  TRaster32P ras;
};

// Compares a tile with the frame where they overlap
void requireSameAsFrame(const Tile& frame, const Tile& tile,
                        const std::string& label) {
  int dx = int(tile.pos.x - frame.pos.x), dy = int(tile.pos.y - frame.pos.y);
  int differing = 0, firstX = 0, firstY = 0;
  for (int y = 0; y < tile.ras->getLy(); ++y) {
    int fy = y + dy;
    if (fy < 0 || fy >= frame.ras->getLy()) continue;
    for (int x = 0; x < tile.ras->getLx(); ++x) {
      int fx = x + dx;
      if (fx < 0 || fx >= frame.ras->getLx()) continue;
      if (tile.ras->pixels(y)[x] != frame.ras->pixels(fy)[fx] &&
          differing++ == 0)
        firstX = fx, firstY = fy;
    }
  }
  require(differing == 0,
          label + ": " + std::to_string(differing) +
              " pixels differ from the frame tile, the first at frame pixel (" +
              std::to_string(firstX) + ", " + std::to_string(firstY) + ")");
}

// Alpha box of the content, in render coordinates
TRect alphaBox(const Tile& tile) {
  TRect box;
  bool empty = true;
  for (int y = 0; y < tile.ras->getLy(); ++y)
    for (int x = 0; x < tile.ras->getLx(); ++x)
      if (tile.ras->pixels(y)[x].m) {
        TPoint p(x + int(tile.pos.x), y + int(tile.pos.y));
        if (empty)
          box = TRect(p, p), empty = false;
        else
          box += TRect(p, p);
      }
  require(!empty, "the effect drew nothing");
  return box;
}

// Alpha-weighted center of the content, in render coordinates
TPointD alphaCenter(const Tile& tile) {
  double sum = 0, sx = 0, sy = 0;
  for (int y = 0; y < tile.ras->getLy(); ++y)
    for (int x = 0; x < tile.ras->getLx(); ++x) {
      double m = tile.ras->pixels(y)[x].m;
      sum += m, sx += m * (x + 0.5), sy += m * (y + 0.5);
    }
  require(sum > 0, "the effect drew nothing");
  return TPointD(sx / sum, sy / sum) + tile.pos;
}

std::string str(const TPointD& p) {
  return "(" + std::to_string(p.x) + ", " + std::to_string(p.y) + ")";
}

void checkTilings(const std::string& name, const TFxP& fx, const Tile& frame) {
  const int lx = cFrameSize.lx, ly = cFrameSize.ly;

  // Four quadrants, then three uneven strips as the renderer cuts them
  std::vector<std::pair<TPoint, TDimension>> parts = {
      {{0, 0}, {lx / 2, ly / 2}},
      {{lx / 2, 0}, {lx - lx / 2, ly / 2}},
      {{0, ly / 2}, {lx / 2, ly - ly / 2}},
      {{lx / 2, ly / 2}, {lx - lx / 2, ly - ly / 2}}};
  for (int s = 0; s < 3; ++s)
    parts.push_back({{0, ly * s / 3}, {lx, ly * (s + 1) / 3 - ly * s / 3}});
  for (const auto& part : parts) {
    TPointD pos = frame.pos + TPointD(part.first.x, part.first.y);
    requireSameAsFrame(frame, {pos, compute(fx, pos, part.second)},
                       name + " on a " + std::to_string(part.second.lx) + "x" +
                           std::to_string(part.second.ly) + " tile at " +
                           str(pos));
  }

  // The frame enlarged by 20 pixels on each side, as a blur of radius 20
  // asks for it, and the frame shifted by an odd offset
  TPointD enlarged = frame.pos - TPointD(20, 20);
  requireSameAsFrame(
      frame, {enlarged, compute(fx, enlarged, TDimension(lx + 40, ly + 40))},
      name + " on the frame enlarged by 20 pixels");
  TPointD shifted = frame.pos + TPointD(15, -9);
  requireSameAsFrame(frame, {shifted, compute(fx, shifted, cFrameSize)},
                     name + " on the frame shifted by (15, -9)");

  // Under a symmetric blur the content keeps its center
  TFxP blur = create("STD_blurFx");
  param<TDoubleParam>(blur, "value")->setDefaultValue(20);
  blur->getInputPort(0)->setFx(fx.getPointer());
  TPointD direct = alphaCenter(frame);
  TPointD blurred =
      alphaCenter({frame.pos, compute(blur, frame.pos, cFrameSize)});
  require(std::abs(blurred.x - direct.x) <= 0.5 &&
              std::abs(blurred.y - direct.y) <= 0.5,
          name + " under a blur of radius 20 is centered at " + str(blurred) +
              " instead of " + str(direct));
}

void checkText() {
  TFxP text = create("STD_iwa_TextFx");
  param<TStringParam>(text, "text")->setValue(L"GZ");
  param<TPointParam>(text, "center")->setDefaultValue(TPointD(13, -7));
  param<TPixelParam>(text, "boxColor")->setDefaultValue(TPixel32::Red);

  Tile frame{cFramePos, compute(text, cFramePos, cFrameSize)};
  // The box is centered on the parameter, within the rounding of half its
  // size
  TRect box = alphaBox(frame);
  TPointD center(0.5 * (box.x0 + box.x1 + 1), 0.5 * (box.y0 + box.y1 + 1));
  require(std::abs(center.x - 13) <= 1 && std::abs(center.y + 7) <= 1,
          "Text on the frame tile is centered at " + str(center) +
              " instead of (13, -7)");
  checkTilings("Text", text, frame);
  std::cout << "  Text: same pixels on every tiling and under a blur"
            << std::endl;
}

void checkTimeCode() {
  TFxP timeCode = create("STD_iwa_TimeCodeFx");
  param<TPointParam>(timeCode, "position")->setDefaultValue(TPointD(-50, 20));
  param<TDoubleParam>(timeCode, "size")->setDefaultValue(10);

  Tile frame{cFramePos, compute(timeCode, cFramePos, cFrameSize)};
  // The box's lower left corner is at the parameter
  TRect box = alphaBox(frame);
  require(box.x0 == -50 && box.y0 == 20,
          "Time Code on the frame tile starts at (" + std::to_string(box.x0) +
              ", " + std::to_string(box.y0) + ") instead of (-50, 20)");
  checkTilings("Time Code", timeCode, frame);
  std::cout << "  Time Code: same pixels on every tiling and under a blur"
            << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
  QGuiApplication application(argc, argv);
  TThread::init();
  initStdFx();
  try {
    checkText();
    checkTimeCode();
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
  }
  std::cout << "textplacement: PASS" << std::endl;
  return 0;
}
