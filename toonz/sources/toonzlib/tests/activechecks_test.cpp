// Source-resolution checks must retain the Viewer's diagnostic meaning while
// leaving production resampling, the scene palette and source pixels alone.
#include "tcheckrenderdata.h"
#include "tcolorstyles.h"
#include "tpalette.h"
#include "tpixelutils.h"
#include "trop.h"
#include "tropcm.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

TPaletteP palette() {
  TPaletteP result = new TPalette;
  result->getStyle(1)->setMainColor(TPixel32(30, 60, 90));
  result->addStyle(TPixel32(90, 150, 210));
  const int autopaint = result->addStyle(TPixel32(180, 120, 60, 128));
  result->getStyle(autopaint)->setFlags(1);
  return result;
}

TRenderCheckSettings checks(int mask, int selected = 2) {
  TRenderCheckSettings result;
  result.m_enabled                = true;
  result.m_checks                 = mask;
  result.m_colorIndex             = selected;
  result.m_inkColor               = TPixel32(255, 0, 0);
  result.m_ink1Color              = TPixel32(0, 255, 0);
  result.m_paintColor             = TPixel32(0, 0, 255);
  result.m_transparencyInkOnWhite = TPixel32(12, 24, 36);
  result.m_transparencyInkOnBlack = TPixel32(240, 228, 216);
  result.m_transparencyPaint      = TPixel32(96, 96, 96);
  return result;
}

TPixel32 convertPixel(const TPixelCM32 &pixel, TPalette *plt,
                      const TRenderCheckSettings &settings) {
  TRasterCM32P source(1, 1);
  TRaster32P output(1, 1);
  source->pixels(0)[0] = pixel;
  output->fill(TPixel32::Yellow);
  TRop::convertWithChecks(output, source, plt, settings);
  require(source->pixels(0)[0] == pixel, "conversion changed source pixel");
  return output->pixels(0)[0];
}

void samePixels(const TRaster32P &expected, const TRaster32P &actual,
                const char *message, int tolerance = 0) {
  for (int y = 0; y < expected->getLy(); ++y)
    for (int x = 0; x < expected->getLx(); ++x) {
      const TPixel32 a = expected->pixels(y)[x], b = actual->pixels(y)[x];
      if (std::max(
              {std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)),
               std::abs(int(a.b) - int(b.b)), std::abs(int(a.m) - int(b.m))}) >
          tolerance) {
        std::ostringstream error;
        error << message << " at (" << x << ',' << y << ')';
        throw std::runtime_error(error.str());
      }
    }
}

void testPixelMeaning() {
  TPaletteP plt = palette();
  auto state = checks(TRenderCheckSettings::Ink | TRenderCheckSettings::Paint |
                      TRenderCheckSettings::Transparency);
  require(convertPixel(TPixelCM32(2, 0, 0), plt.getPointer(), state) ==
              state.m_inkColor,
          "selected ink did not override transparency color");
  require(convertPixel(TPixelCM32(1, 2, 255), plt.getPointer(), state) ==
              state.m_paintColor,
          "selected paint did not override transparency color");
  require(convertPixel(TPixelCM32(1, 1, 0), plt.getPointer(), state) ==
              state.m_transparencyInkOnWhite,
          "unselected ink was not transparency checked");
  require(convertPixel(TPixelCM32(1, 1, 255), plt.getPointer(), state) ==
              state.m_transparencyPaint,
          "unselected paint was not transparency checked");
  require(convertPixel(TPixelCM32(2, 2, 128), plt.getPointer(), state) ==
              TPixel32(191, 0, 64),
          "combined checks lost the Viewer's antialiased ink/paint transition");

  state.m_checks |= TRenderCheckSettings::InksOnly;
  require(convertPixel(TPixelCM32(1, 2, 255), plt.getPointer(), state) ==
              TPixel32::Transparent,
          "Inks Only retained checked paint");
  require(convertPixel(TPixelCM32(2, 2, 128), plt.getPointer(), state) ==
              TPixel32(127, 0, 0, 127),
          "Inks Only lost partial ink coverage");

  state = checks(TRenderCheckSettings::Transparency);
  require(convertPixel(TPixelCM32(2, 0, 80), plt.getPointer(), state) ==
              TPixel32::Transparent,
          "high-tone empty edge was not cleared");
  state.m_checks |= TRenderCheckSettings::InksOnly;
  require(convertPixel(TPixelCM32(2, 0, 80), plt.getPointer(), state).m > 0,
          "Inks Only incorrectly applied empty-edge suppression");
  state.m_checks =
      TRenderCheckSettings::Transparency | TRenderCheckSettings::BlackBg;
  require(convertPixel(TPixelCM32(2, 0, 0), plt.getPointer(), state) ==
              state.m_transparencyInkOnBlack,
          "Black BG did not select the matching diagnostic ink color");
  require(convertPixel(TPixelCM32(3, 3, 0), plt.getPointer(), state) ==
              premultiply(plt->getStyle(3)->getAverageColor()),
          "Transparency Check changed autopaint marker color");
  require(convertPixel(TPixelCM32(3, 3, 255), plt.getPointer(), state) ==
              premultiply(plt->getStyle(3)->getAverageColor()),
          "Transparency Check changed autopaint paint color");

  state = checks(TRenderCheckSettings::Ink1);
  require(convertPixel(TPixelCM32(1, 0, 0), plt.getPointer(), state) ==
              state.m_ink1Color,
          "Ink#1 depended on the selected style");
  state = checks(TRenderCheckSettings::Fill);
  require(convertPixel(TPixelCM32(1, 1, 255), plt.getPointer(), state) ==
              state.m_transparencyPaint,
          "Fill Check did not color the prepared filled region");
  state.m_checks |= TRenderCheckSettings::InksOnly;
  require(convertPixel(TPixelCM32(1, 1, 255), plt.getPointer(), state) ==
              TPixel32::Transparent,
          "Inks Only retained Fill Check regions");

  state = checks(TRenderCheckSettings::Ink | TRenderCheckSettings::Paint);
  state.m_inkColor   = TPixel32(255, 0, 128, 128);
  state.m_paintColor = TPixel32(0, 255, 128, 128);
  require(convertPixel(TPixelCM32(2, 0, 0), plt.getPointer(), state) ==
              TPixel32(128, 0, 64, 128),
          "transparent check ink was not premultiplied");
  require(convertPixel(TPixelCM32(1, 2, 255), plt.getPointer(), state) ==
              TPixel32(0, 128, 64, 128),
          "transparent check paint was not premultiplied");
  require(convertPixel(TPixelCM32(4095, 4095, 128), plt.getPointer(), state) ==
              TPixel32::Transparent,
          "invalid palette indices did not remain transparent");
}

void testViewerCombinationsAndStride() {
  TPaletteP plt = palette();
  TRasterCM32P storage(13, 5);
  TRasterCM32P source       = storage->extract(TRect(2, 1, 10, 3));
  const TPixelCM32 pixels[] = {
      TPixelCM32(0, 0, 255), TPixelCM32(1, 0, 0),   TPixelCM32(2, 0, 0),
      TPixelCM32(1, 1, 255), TPixelCM32(1, 2, 255), TPixelCM32(2, 1, 128),
      TPixelCM32(2, 0, 64),  TPixelCM32(2, 0, 128), TPixelCM32(3, 3, 128)};
  for (int y = 0; y < source->getLy(); ++y)
    for (int x = 0; x < source->getLx(); ++x) source->pixels(y)[x] = pixels[x];

  TRaster32P outputStorage(14, 5);
  TRaster32P output = outputStorage->extract(TRect(3, 1, 11, 3));
  TRaster32P expected(source->getSize());
  for (int selected : {1, 2}) {
    for (int mask = 0; mask < 256; ++mask) {
      if (mask & TRenderCheckSettings::Gap) continue;
      const auto state        = checks(mask, selected);
      const bool transparency = (mask & (TRenderCheckSettings::Transparency |
                                         TRenderCheckSettings::Fill)) != 0;
      const bool inksOnly     = (mask & TRenderCheckSettings::InksOnly) != 0;

      // Existing Viewer conversion is an independent behavioral reference.
      // Its caller applies this empty-edge correction before quickPut.
      TRasterCM32P viewerSource = source->clone();
      if (transparency && !inksOnly)
        for (int y = 0; y < viewerSource->getLy(); ++y)
          for (int x = 0; x < viewerSource->getLx(); ++x) {
            auto &pixel = viewerSource->pixels(y)[x];
            if (pixel.getPaint() == 0 && pixel.getTone() >= 80)
              pixel.setTone(255);
          }
      TRop::CmappedQuickputSettings viewer;
      viewer.m_inkIndex   = selected;
      viewer.m_paintIndex = mask & TRenderCheckSettings::Paint ? selected : -1;
      viewer.m_inkCheckEnabled   = (mask & TRenderCheckSettings::Ink) != 0;
      viewer.m_ink1CheckEnabled  = (mask & TRenderCheckSettings::Ink1) != 0;
      viewer.m_paintCheckEnabled = (mask & TRenderCheckSettings::Paint) != 0;
      viewer.m_transparencyCheck = transparency;
      viewer.m_blackBgCheck      = (mask & TRenderCheckSettings::BlackBg) != 0;
      viewer.m_inksOnly          = inksOnly;
      viewer.m_inkCheckColor     = state.m_inkColor;
      viewer.m_ink1CheckColor    = state.m_ink1Color;
      viewer.m_paintCheckColor   = state.m_paintColor;
      viewer.m_transpCheckInk    = state.m_transparencyInkOnWhite;
      viewer.m_transpCheckBg     = state.m_transparencyInkOnBlack;
      viewer.m_transpCheckPaint  = state.m_transparencyPaint;
      expected->clear();
      TRop::quickPut(expected, viewerSource, plt, TAffine(), viewer);

      outputStorage->fill(TPixel32::Yellow);
      TRop::convertWithChecks(output, source, plt.getPointer(), state);
      samePixels(expected, output,
                 "source conversion differs from Viewer checks");
      require(outputStorage->pixels(0)[0] == TPixel32::Yellow,
              "conversion wrote outside the extracted destination");
      for (int y = 0; y < source->getLy(); ++y)
        for (int x = 0; x < source->getLx(); ++x)
          require(source->pixels(y)[x] == pixels[x],
                  "checks changed the source raster");
    }
  }

  auto disabled      = checks(TRenderCheckSettings::SupportedChecks);
  disabled.m_enabled = false;
  TRop::convert(expected, source, plt);
  TRop::convertWithChecks(output, source, plt.getPointer(), disabled);
  // Windows SSE conversion rounds blends; the scalar Viewer truncates them.
  samePixels(expected, output, "disabled checks changed normal conversion", 1);
  require(plt->getStyle(2)->getAverageColor() == TPixel32(90, 150, 210) &&
              plt->getStyle(3)->getFlags() == 1,
          "diagnostics changed palette colors or autopaint flags");
}

std::string replaceToken(const std::string &text, size_t index,
                         const std::string &replacement) {
  std::istringstream input(text);
  std::ostringstream output;
  std::string token;
  size_t current = 0;
  while (input >> token) {
    if (current) output << ' ';
    output << (current == index ? replacement : token);
    ++current;
  }
  return output.str();
}

void testSnapshotPersistenceAndBounds() {
  auto state = checks(TRenderCheckSettings::Ink | TRenderCheckSettings::Paint |
                      TRenderCheckSettings::Fill);
  state.m_columnPath        = {2, 18, 4};
  state.m_fillOnlySavebox   = false;
  state.m_targetFxId        = 42;
  const std::string encoded = state.serialize();
  TRenderCheckSettings restored;
  require(restored.deserialize(encoded), "valid render snapshot did not load");
  require(restored.serialize() == encoded && restored.m_enabled &&
              restored.m_columnPath == state.m_columnPath &&
              !restored.m_fillOnlySavebox,
          "render snapshot lost settings during round trip");
  require(restored.m_targetFxId == 0,
          "scene serialization retained a session-local FX identity");
  require(state.appliesTo(42) && !state.appliesTo(43),
          "checks applied to a different column identity");
  restored.m_targetFxId = state.m_targetFxId;
  require(restored.cacheKey() == state.cacheKey(),
          "snapshot cache identity changed");
  restored.m_colorIndex = 1;
  require(restored.cacheKey() != state.cacheKey(),
          "selected style missing from cache identity");
  restored            = state;
  restored.m_inkColor = TPixel32::Yellow;
  require(restored.cacheKey() != state.cacheKey(),
          "check color missing from cache identity");

  const std::vector<std::string> invalid = {
      "not a snapshot",
      "1 4 2 1 1 0",
      encoded + " trailing",
      replaceToken(encoded, 0, "2"),
      replaceToken(encoded, 1, "256"),
      replaceToken(encoded, 1, "-1"),
      replaceToken(encoded, 2, "4096"),
      replaceToken(encoded, 2, "-2"),
      replaceToken(encoded, 3, "2"),
      replaceToken(encoded, 4, "66"),
      replaceToken(encoded, 4, "2"),
      replaceToken(encoded, 4, "999999999999999999999999999999999"),
      replaceToken(encoded, 5, "-1"),
      replaceToken(encoded, 8, "256"),
      replaceToken(encoded, 8, "-1")};
  for (const std::string &bad : invalid) {
    restored = state;
    require(!restored.deserialize(bad),
            "malformed render snapshot was accepted");
    require(!restored.m_enabled && restored.activeChecks() == 0 &&
                restored.m_columnPath.empty(),
            "malformed snapshot left stale render checks active");
  }
  restored = state;
  require(restored.deserialize("") && !restored.m_enabled,
          "legacy scene without check settings did not disable checks");
  state.m_enabled = false;
  require(state.serialize().empty() && state.cacheKey().empty() &&
              !state.appliesTo(42),
          "disabled checks changed serialization or render cache identity");
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  try {
    testPixelMeaning();
    testViewerCombinationsAndStride();
    testSnapshotPersistenceAndBounds();
  } catch (const std::exception &error) {
    std::cerr << "activechecks_test: " << error.what() << std::endl;
    return 1;
  }
  std::cout << "ok: render check pixels, combinations and snapshot persistence"
            << std::endl;
  return 0;
}
