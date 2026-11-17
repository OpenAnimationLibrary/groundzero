#include "timagecache.h"
#include "tpalette.h"
#include "trop.h"
#include "traster.h"

extern "C" {
#include "toonz4.6/raster.h"
}

#include <QCoreApplication>
#include <QTemporaryDir>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::string cacheId(const RASTER* raster) {
  return std::string(raster->cacheId, raster->cacheIdLength);
}

void rgbaRelease(bool unlocked) {
  TRaster32P raster(3, 2);
  raster->fill(TPixel32(17, 34, 51, 68));
  const int references = raster->getRefCount();
  RASTER* wrapper      = TRop::convertRaster50to46(raster, TPaletteP());
  const std::string id = cacheId(wrapper);
  require(wrapper->buffer == raster->getRawData() &&
              TImageCache::instance()->isCached(id),
          "Legacy wrapper must borrow the actual cached raster buffer");
  if (unlocked) TRop::unlockRaster(wrapper);
  TRop::releaseRaster46(wrapper);
  require(!wrapper && !TImageCache::instance()->isCached(id),
          "Normal release must clear the wrapper and its cache entry");
  require(raster->getRefCount() == references,
          "Normal release must balance its native raster references");
  require(raster->pixels()[0] == TPixel32(17, 34, 51, 68),
          "Normal release must preserve the borrowed native pixels");
}

void paletteRelease(bool unlocked) {
  TRasterCM32P raster(3, 2);
  raster->fill(TPixelCM32(1, 2, 127));
  TPaletteP palette           = new TPalette;
  const int style             = palette->addStyle(TPixel32::Red);
  const int references        = raster->getRefCount();
  const int paletteReferences = palette->getRefCount();
  RASTER* wrapper             = TRop::convertRaster50to46(raster, palette);
  const std::string id        = cacheId(wrapper);
  require(wrapper->cmap.buffer && wrapper->cmap.buffer[style].r == 255 &&
              wrapper->cmap.buffer[style].m == 255,
          "Legacy colormap must contain the copied native palette color");
  if (unlocked) TRop::unlockRaster(wrapper);
  TRop::releaseRaster46(wrapper, false);
  require(!wrapper && !TImageCache::instance()->isCached(id),
          "Colormap release must clear the wrapper and cache entry");
  require(raster->getRefCount() == references &&
              palette->getRefCount() == paletteReferences,
          "Colormap release must balance raster and palette references");
  require(raster->pixels()[0] == TPixelCM32(1, 2, 127) &&
              palette->getStyle(style)->getMainColor() == TPixel32::Red,
          "Colormap release must preserve the original pixels and palette");
}

void expectUnsupported(RASTER*& wrapper) {
  try {
    TRop::releaseRaster46(wrapper, true);
  } catch (const TRopException& error) {
    require(
        error.getMessage().find(L"doReleaseBuffer=false") != std::wstring::npos,
        "Unsupported release must identify the supported argument");
    return;
  }
  throw std::runtime_error(
      "Unsupported buffer release must throw TRopException");
}

void rejectedBorrowedBuffer(bool unlocked) {
  std::array<TPixel32, 6> pixels;
  pixels.fill(TPixel32(17, 34, 51, 68));
  TRaster32P raster(3, 2, 3, pixels.data());
  RASTER* wrapper = TRop::convertRaster50to46(raster, TPaletteP());
  if (unlocked) TRop::unlockRaster(wrapper);
  RASTER* const original = wrapper;
  void* const buffer     = wrapper->buffer;
  const std::string id   = cacheId(wrapper);
  const int references   = raster->getRefCount();
  expectUnsupported(wrapper);
  require(wrapper == original && wrapper->buffer == buffer &&
              cacheId(wrapper) == id && TImageCache::instance()->isCached(id),
          "Rejected release must leave its wrapper and cache unchanged");
  require(
      raster->getRefCount() == references &&
          pixels[0] == TPixel32(17, 34, 51, 68),
      "Rejected release must leave borrowed pixels and references unchanged");
  TRop::releaseRaster46(wrapper, false);
  require(
      !wrapper && raster->getRefCount() == 1 &&
          !TImageCache::instance()->isCached(id),
      "A rejected wrapper must remain releasable through the supported path");
}

void rejectedPalette() {
  TRasterCM32P raster(3, 2);
  raster->clear();
  TPaletteP palette    = new TPalette;
  const int style      = palette->addStyle(TPixel32::Red);
  RASTER* wrapper      = TRop::convertRaster50to46(raster, palette);
  LPIXEL* const colors = wrapper->cmap.buffer;
  const std::string id = cacheId(wrapper);
  expectUnsupported(wrapper);
  require(wrapper->cmap.buffer == colors && colors[style].r == 255 &&
              colors[style].m == 255 && TImageCache::instance()->isCached(id),
          "Rejected release must preserve its copied palette and cache entry");
  TRop::releaseRaster46(wrapper);
  require(!wrapper && raster->getRefCount() == 1 && palette->getRefCount() == 1,
          "Rejected colormap wrapper must still release its resources exactly "
          "once");
}

void controls() {
  rgbaRelease(false);
  rgbaRelease(true);
  paletteRelease(false);
  paletteRelease(true);
  std::cout << "rasterrelease-test: locked/unlocked RGBA/colormap release "
               "controls PASS"
            << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    QCoreApplication application(argc, argv);
    QTemporaryDir root;
    require(root.isValid(), "Create private image-cache directory");
    TImageCache::instance()->setRootDir(TFilePath(root.path().toStdWString()));
    controls();
    if (argc == 2 && std::string(argv[1]) == "--controls-only") return 0;
    require(argc == 1,
            "rasterrelease-test accepts only --controls-only or no arguments");
    rejectedBorrowedBuffer(false);
    rejectedBorrowedBuffer(true);
    rejectedPalette();
    TImageCache::instance()->clear(true);
    std::cout << "rasterrelease-test: unsupported requests reject before "
                 "mutation PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "rasterrelease-test: " << error.what() << '\n';
    return 1;
  }
}
