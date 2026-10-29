#include "traster.h"
#include "trastercm.h"

#include <iostream>
#include <limits>
#include <new>

namespace {
int failures = 0;

void check(bool value, const char *message) {
  if (value) return;
  ++failures;
  std::cerr << "FAILED: " << message << '\n';
}

template <class F>
void rejects(F create, const char *message) {
  try {
    create();
    check(false, message);
  } catch (const std::bad_alloc &) {
  } catch (...) {
    check(false, "allocation failure uses std::bad_alloc");
  }
}

void invalidAllocations() {
  rejects([] { TRaster32P raster(0, 10); }, "zero width is rejected");
  rejects([] { TRaster32P raster(10, 0); }, "zero height is rejected");
  rejects([] { TRaster32P raster(-1, 10); }, "negative width is rejected");
  rejects([] { TRaster32P raster(10, -1); }, "negative height is rejected");
  // These used to overflow signed intermediate sizes (and in one case wrap
  // the requested buffer to zero). They must fail before allocating pixels.
  rejects([] { TRaster32P raster(32768, 32768); }, "4 GiB raster is rejected");
  rejects([] { TRasterFP raster(16384, 16384); }, "4 GiB float raster is rejected");
  rejects([] { TRaster32P raster((std::numeric_limits<int>::max)(), 1); },
          "row byte overflow is rejected");
  rejects([] { TRaster64P raster(32768, 8192); },
          "signed total byte overflow is rejected");
}

void attachedBuffers() {
  TPixel32 pixels[12];
  rejects([&] { TRaster32P raster(3, 2, 2, pixels); },
          "stride narrower than image is rejected");
  rejects([] { TRaster32P raster(1, 1, 1, nullptr); },
          "null attached buffer is rejected");
  rejects([&] { TRaster32P raster(1, 2, 1 << 29, pixels); },
          "overflowing attached stride is rejected");
  for (TPixel32 &pixel : pixels) pixel = TPixel32::Red;
  {
    TRaster32P raster(3, 3, 4, pixels);
    raster->clear();
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col)
        check(pixels[row * 4 + col] == TPixel32(),
              "clear respects attached image rows");
      check(pixels[row * 4 + 3] == TPixel32::Red,
            "clear preserves attached row padding");
    }
  }
  check(pixels[3] == TPixel32::Red, "non-owned buffer survives raster destruction");
}

void failedReplacementAndSubrasters() {
  TRaster32P image(8, 6);
  image->fill(TPixel32::Red);
  auto *original = image.getPointer();
  rejects([&] { image.create(32768, 32768); },
          "failed replacement throws before changing smart pointer");
  check(image.getPointer() == original && image->pixels(0)[0] == TPixel32::Red,
        "failed replacement preserves the original image");

  TRaster32P child(image->extract(2, 1, 5, 4));
  TRaster32P nested(child->extract(1, 1, 2, 2));
  image = TRaster32P();
  child = TRaster32P();
  nested->fill(TPixel32::Blue);
  TRaster32P copy(nested->clone());
  check(copy->getSize() == TDimension(2, 2), "nested clone has the correct size");
  check(copy->pixels(0)[0] == TPixel32::Blue &&
            copy->pixels(1)[1] == TPixel32::Blue,
        "nested view retains its root and copies correct pixels");

  TRasterCM32P toonz(4, 4);
  toonz->clear();
  check(toonz->getRawData() != nullptr, "Toonz Raster storage remains supported");
}
}  // namespace

int main() {
  try {
    invalidAllocations();
    attachedBuffers();
    failedReplacementAndSubrasters();
  } catch (...) {
    check(false, "unexpected exception in valid raster operations");
  }
  return failures ? 1 : 0;
}
