// SPDX-License-Identifier: BSD-3-Clause

// Centerline color sampling on alternating single-pixel inks. Each color
// change splits a skeleton sequence; a split placed on an existing node made
// a zero-length step, and the sampler then walked past the sequence tail into
// links the end node does not have (an access violation during native
// indexed-to-vector paste).

#include "toonz/tcenterlinevectorizer.h"
#include "toonz/vectorizerparameters.h"
#include "tpalette.h"
#include "trastercm.h"
#include "tstroke.h"
#include "ttoonzimage.h"
#include "tvectorimage.h"

#include <QCoreApplication>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

TPaletteP makePalette() {
  TPaletteP palette = new TPalette;
  palette->setStyle(1, TPixel32::Blue);
  require(palette->addStyle(TPixel32::Green) == 2,
          "fixture palette did not get a second style");
  return palette;
}

// The paste fixture uses size 16, modulus 3, row weight 2 and no inset.
TToonzImageP makeAlternatingInk(int size, int modulus, int rowWeight,
                                int inset) {
  TRasterCM32P raster(size, size);
  raster->fill(TPixelCM32());
  for (int y = inset; y < size - inset; ++y)
    for (int x = inset; x < size - inset; ++x)
      raster->pixels(y)[x] =
          TPixelCM32((x + rowWeight * y) % modulus == 0 ? 1 : 2, 0, 0);
  TToonzImageP image(new TToonzImage(raster, raster->getBounds()));
  image->setDpi(96, 96);
  return image;
}

void vectorizeCase(int size, int modulus, int rowWeight, int inset) {
  const std::string name = "size=" + std::to_string(size) +
                           " modulus=" + std::to_string(modulus) +
                           " rowWeight=" + std::to_string(rowWeight) +
                           " inset=" + std::to_string(inset);
  TPaletteP palette      = makePalette();
  TToonzImageP image     = makeAlternatingInk(size, modulus, rowWeight, inset);
  image->setPalette(palette.getPointer());

  VectorizerParameters parameters;
  const CenterlineConfiguration config =
      parameters.getCenterlineConfiguration(0.0);
  require(config.m_maxThickness > 0.0,
          "default centerline configuration skips color sampling");

  VectorizerCore vectorizer;
  TVectorImageP result =
      vectorizer.vectorize(image.getPointer(), config, palette.getPointer());
  require(bool(result), "centerline vectorization returned no image: " + name);
  for (UINT i = 0; i < result->getStrokeCount(); ++i) {
    const int style = result->getStroke(i)->getStyle();
    require(style >= 0 && style < palette->getStyleCount(),
            "stroke sampled a color outside the palette: " + name);
  }
}
}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  try {
    int cases = 0;
    for (int size : {8, 15, 16, 17, 32})
      for (int modulus : {2, 3, 4})
        for (int rowWeight : {1, 2})
          for (int inset : {0, 2}) {
            vectorizeCase(size, modulus, rowWeight, inset);
            ++cases;
          }
    std::cout << "centerline_colors: PASS " << cases
              << " alternating-ink centerline vectorizations" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "centerline_colors: " << error.what() << std::endl;
    return 1;
  }
}
