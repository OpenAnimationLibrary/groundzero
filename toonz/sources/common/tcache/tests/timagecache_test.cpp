// SPDX-License-Identifier: BSD-3-Clause

// Compression must remove the raster-address index before releasing a raster.
// Force address reuse so this regression is independent of the system allocator.
#include "timagecache.h"
#include "tfilepath.h"
#include "trasterimage.h"
#include "trastercm.h"
#include "ttoonzimage.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstddef>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

template <class Pixel>
class ReusedRaster final : public TRasterT<Pixel> {
  inline static bool inUse = false;

public:
  ReusedRaster() : TRasterT<Pixel>(32, 32) {}

  static void *operator new(std::size_t size) {
    require(size == sizeof(ReusedRaster), "Unexpected test raster size");
    require(!inUse, "Compression did not release the original raster");
    alignas(ReusedRaster) static unsigned char storage[sizeof(ReusedRaster)];
    inUse = true;
    return storage;
  }

  static void operator delete(void *) noexcept { inUse = false; }
};

TImageP wrap(const TRasterPT<TPixel32> &raster) {
  return TRasterImageP(raster);
}

TImageP wrap(const TRasterPT<TPixelCM32> &raster) {
  return TToonzImageP(raster, raster->getBounds());
}

TRasterP imageRaster(const TImageP &image) {
  if (TRasterImageP rasterImage = image) return rasterImage->getRaster();
  if (TToonzImageP toonzImage = image) return toonzImage->getRaster();
  return TRasterP();
}

template <class Pixel>
TImageP makeImage(const Pixel &pixel) {
  TRasterP storage(new ReusedRaster<Pixel>());
  TRasterPT<Pixel> raster(storage);
  raster->lock();
  for (int y = 0; y < raster->getLy(); ++y)
    for (int x = 0; x < raster->getLx(); ++x) raster->pixels(y)[x] = pixel;
  raster->unlock();
  return wrap(raster);
}

TImageP ordinaryImage(const TPixel32 &pixel) {
  TRaster32P raster(8, 8);
  raster->fill(pixel);
  return TRasterImageP(raster);
}

template <class Pixel>
void expectImage(TImageCache *cache, const std::string &id,
                 const Pixel &expected) {
  TImageP image = cache->get(id, false);
  require(image.getPointer(), "A new cache entry became an empty alias");
  TRasterPT<Pixel> raster(imageRaster(image));
  require(raster.getPointer(), "Cache returned the wrong image type");
  bool matches = true;
  raster->lock();
  for (int y = 0; y < raster->getLy(); ++y)
    for (int x = 0; x < raster->getLx(); ++x)
      matches = matches && raster->pixels(y)[x] == expected;
  raster->unlock();
  require(matches, "Cache aliased a new drawing to a compressed old drawing");
}

template <class Pixel>
void testAddressReuse(TImageCache *cache, const Pixel &oldPixel,
                      const Pixel &newPixel, bool removeCompressed) {
  const std::string oldId = "compression-old";
  const std::string newId = "compression-new";
  const std::string aliasId = "compression-alias";
  cache->add(oldId, makeImage(oldPixel));
  cache->compress(oldId);
  if (removeCompressed) cache->remove(oldId);

  // This raster occupies exactly the address released by compress().
  cache->add(newId, makeImage(newPixel));
  expectImage(cache, newId, newPixel);
  if (!removeCompressed) expectImage(cache, oldId, oldPixel);

  // Genuine aliases must still survive removal of their original ID.
  cache->add(aliasId, cache->get(newId, false));
  cache->remove(newId);
  expectImage(cache, aliasId, newPixel);
  cache->remove(aliasId);
  cache->remove(oldId);
}

void testRefreshedAndRemappedHistory(TImageCache *cache) {
  const TPixel32 original(12, 34, 56, 255);
  const TPixel32 replacement(65, 43, 21, 255);
  cache->add(std::string("history-first"), makeImage(original));
  cache->add(std::string("history-second"), ordinaryImage(replacement));
  expectImage(cache, "history-first", original);  // Refresh the history key.
  cache->remap("history-renamed", "history-first");
  cache->compress("history-renamed");
  // Reuse fails immediately unless compression released the correct raster,
  // even after both its history key and its canonical ID changed.
  cache->add(std::string("history-new"), makeImage(replacement));
  expectImage(cache, "history-renamed", original);
  expectImage(cache, "history-new", replacement);
  expectImage(cache, "history-second", replacement);
  cache->clear();
}

void testIconPrefixRemap(TImageCache *cache) {
  const TPixel32 selected(17, 37, 57, 255);
  const TPixel32 unrelated(71, 73, 75, 255);
  const char *matching[] = {"source:", "source:1", "source:2:thumbnail"};
  const char *other[] = {"source", "source0:1", "source;1", "sourc:1"};
  for (const char *id : matching)
    cache->add(std::string(id), ordinaryImage(selected));
  for (const char *id : other)
    cache->add(std::string(id), ordinaryImage(unrelated));
  cache->add(std::string("unrelated-alias"), cache->get(std::string("source:1"), false));
  cache->add(std::string("source:compressed"), ordinaryImage(selected));
  cache->compress("source:compressed");

  cache->remapIcons("destination", "source");
  for (const char *id : matching) {
    require(!cache->isCached(id), "icon remap retained an old canonical ID");
    expectImage(cache, std::string("destination") + std::string(id).substr(6),
                selected);
  }
  for (const char *id : other) expectImage(cache, id, unrelated);
  expectImage(cache, "unrelated-alias", selected);
  // Keep the existing contract: remapIcons visits resident icons only.
  require(!cache->isCached("destination:compressed"),
          "icon remap changed the compressed-entry contract");
  expectImage(cache, "source:compressed", selected);
  cache->remapIcons("destination", "destination");
  cache->compress("destination:1");
  expectImage(cache, "unrelated-alias", selected);
  cache->clear();
}

}  // namespace

int main(int argc, char *argv[]) {
  QCoreApplication application(argc, argv);
  QTemporaryDir directory;
  TImageCache *cache = TImageCache::instance();
  try {
    require(directory.isValid(), "Cannot create the cache test directory");
    cache->setRootDir(TFilePath(directory.path().toStdWString()));
    // Exercise compression with unrelated resident history entries present.
    for (int i = 0; i < 64; ++i)
      cache->add("resident-" + std::to_string(i),
                 ordinaryImage(TPixel32(i, 80, 120, 255)));
    for (bool removeCompressed : {false, true}) {
      testAddressReuse(cache, TPixel32(10, 20, 30, 255),
                       TPixel32(200, 150, 100, 255), removeCompressed);
      testAddressReuse(cache, TPixelCM32(1, 2, 255), TPixelCM32(3, 4, 127),
                       removeCompressed);
    }
    cache->clear();
    testRefreshedAndRemappedHistory(cache);
    testIconPrefixRemap(cache);
    cache->clear(true);
    std::cout << "Image cache compression tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    cache->clear(true);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
