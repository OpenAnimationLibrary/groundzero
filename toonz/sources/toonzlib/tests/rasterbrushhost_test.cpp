#include "../../tnztools/nativerasterbrush.h"
#include "toonz/ttileset.h"
#include "toonz/ttilesaver.h"
#include "tools/track.h"
#include "tools/inputmanager.h"
#include "timagecache.h"
#include "timage_io.h"
#include "tnzimage.h"
#include "tthread.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool v, const char *message) {
  if (!v) throw std::runtime_error(message);
}
bool same(const TRaster32P &a, const TRaster32P &b) {
  if (!a || !b || a->getSize() != b->getSize()) return false;
  for (int y = 0; y < a->getLy(); ++y)
    for (int x = 0; x < a->getLx(); ++x)
      if (a->pixels(y)[x] != b->pixels(y)[x]) return false;
  return true;
}
struct Transaction : RasterController {
  TTileSetFullColor tiles;
  TTileSaverFullColor saver;
  bool accept = true;
  explicit Transaction(const TRaster32P &r)
      : tiles(r->getSize()), saver(r, &tiles) {}
  bool askWrite(const TRect &r) override {
    if (!accept) return false;
    saver.save(r);
    return true;
  }
  void undo(const TRaster32P &r) {
    for (int i = 0; i < tiles.getTileCount(); ++i) {
      auto tile = tiles.getTile(i);
      TRasterP saved;
      tile->getRaster(saved);
      r->copy(saved, tile->m_rasterBounds.getP00());
    }
  }
};
void testRaster(const QString &directory) {
  TRaster32P raster(128, 128);
  raster->fill(TPixel32(16, 32, 64, 255));
  TRaster32P before = raster->clone();
  Transaction transaction(raster);
  auto backing =
      std::make_shared<NativeRasterBrushSurface>(raster, transaction);
  auto budget = std::make_shared<RasterBrush::MemoryBudget>(4 * 1024 * 1024);
  {
    RasterBrush::BufferedSurface surface(backing, budget);
    RasterBrush::Preset preset;
    preset.size.value = 12;
    preset.color      = {1, 0, 0, 1};
    preset.flow.value = .2;
    RasterBrush::Stroke stroke(surface, preset, 4 * 1024 * 1024, budget);
    for (int i = 0; i < 100; ++i) {
      RasterBrush::Sample s;
      s.x    = i;
      s.y    = 64;
      s.time = i / 240.0;
      require(stroke.add(s), stroke.error().c_str());
    }
    stroke.finish();
  }
  require(!same(before, raster), "host raster did not change");
  require(transaction.tiles.getTileCount() > 0, "host undo was not captured");
  TRaster32P after = raster->clone();
  TFilePath path(QDir(directory).filePath("painted.png"));
  TImageWriter::save(path, TRasterP(raster));
  TRasterP reopened;
  require(TImageReader::load(path, reopened),
          "cannot reopen native-painted raster");
  require(same(after, TRaster32P(reopened)),
          "saved native-painted raster changed");
  transaction.undo(raster);
  require(same(before, raster), "tile undo did not restore exact pixels");
  raster->copy(after);
  require(same(after, raster), "baked redo pixels changed");
  Transaction orientation(raster);
  NativeRasterBrushSurface oriented(raster, orientation);
  require(oriented.prepare(0, 0, 1, 1), "orientation preparation failed");
  oriented.write(0, 0, {1, 0, 0, 1});
  require(raster->pixels(raster->getLy() - 1)[0] == TPixel32(255, 0, 0, 255),
          "top-down resource was flipped in host raster");
  orientation.undo(raster);
  require(same(after, raster), "oriented tile undo failed");
  Transaction denied(raster);
  denied.accept = false;
  NativeRasterBrushSurface refused(raster, denied);
  RasterBrush::Preset preset;
  RasterBrush::Stroke invalid(refused, preset);
  RasterBrush::Sample s;
  s.x = 64;
  s.y = 64;
  require(!invalid.add(s), "host transaction denial ignored");
  require(same(after, raster), "denied transaction painted");
}
void testRotation() {
  TInputManager manager;
  auto ticks = TToolTimer::ticks();
  manager.trackEvent(1, 0, TPointD(10, 20), .4, TPointD(.2, .3), true, true,
                     false, ticks, .75, true);
  auto track    = manager.getInputTracks().front();
  double before = track->back().time;
  manager.finishTracks();
  require(track->back().time >= before && track->back().time - before < .01,
          "generated pen-up used a different time origin");
  require(track->back().hasRotation && track->back().rotation == .75,
          "generated pen-up lost rotation");
  const double pi = 3.14159265358979323846;
  TTrackPoint a, b;
  a.rotation    = 350 * pi / 180;
  b.rotation    = 10 * pi / 180;
  a.hasRotation = b.hasRotation = true;
  auto mid                      = TTrack::interpolationLinear(a, b, .5);
  require(
      mid.hasRotation && std::abs(std::remainder(mid.rotation, 2 * pi)) < 1e-10,
      "barrel rotation crossed long arc");
  a.tilt     = TPointD(.5, .25);
  a.rotation = 0;
  // TAffine::rotation takes radians; TRotation takes degrees.
  auto transformed = TTrackTransform(TAffine::rotation(pi / 2)).apply(a);
  require(std::abs(transformed.rotation - pi / 2) < 1e-10,
          "barrel rotation not transformed");
  require(std::abs(transformed.tilt.x + .25) < 1e-10 &&
              std::abs(transformed.tilt.y - .5) < 1e-10,
          "tilt not transformed");
  transformed = TTrackTransform(TAffine(-2, 0, 0, 0, 1, 0)).apply(a);
  require(std::abs(std::abs(transformed.rotation) - pi) < 1e-10 &&
              std::abs(transformed.tilt.x + .5) < 1e-10 &&
              std::abs(transformed.tilt.y - .125) < 1e-10,
          "mirrored anisotropic transform lost tablet orientation");
  transformed = TTrackTransform(TAffine(0, 0, 0, 0, 0, 0)).apply(a);
  require(std::isfinite(transformed.rotation) && transformed.tilt == TPointD(),
          "degenerate transform generated invalid tablet values");
}
}  // namespace
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory unavailable");
    QDir().mkpath(directory.path() + "/cache");
    TImageCache::instance()->setRootDir(TFilePath(directory.path() + "/cache"));
    TThread::init();
    initImageIo();
    testRaster(directory.path());
    testRotation();
    std::cout << "Native host raster, tile undo, image persistence and input "
                 "tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
