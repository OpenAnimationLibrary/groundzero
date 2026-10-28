#include "cadmiumsource.h"

#include "tenv.h"
#include "texception.h"
#include "timage_io.h"
#include "timagecache.h"
#include "timageinfo.h"
#include "tnzimage.h"
#include "tpalette.h"
#include "tproperty.h"
#include "trasterimage.h"
#include "tthread.h"
#include "toonz/imagemanager.h"
#include "toonz/levelproperties.h"
#include "toonz/levelset.h"
#include "toonz/levelupdater.h"
#include "toonz/preferences.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonz/txshexrlevel.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfInputFile.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
namespace Exr = OPENEXR_IMF_NAMESPACE;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

void requireNear(float actual, float expected) {
  require(std::isfinite(actual) && std::fabs(actual - expected) <= 0.000001f,
          "float resource sample changed on save/reopen");
}

TFilePath pathOf(const QString &path) {
  return TFilePath(path.toStdWString());
}

TXshSimpleLevel *addLevel(ToonzScene &scene, const TFilePath &path,
                         const std::wstring &name, bool floating) {
  TXshSimpleLevel *level = new TXshSimpleLevel(name);
  level->setScene(&scene);
  level->setType(OVL_XSHLEVEL);
  level->setPath(path, true);
  level->setPalette(new TPalette);
  level->getPalette()->setDirtyFlag(false);
  level->set16BitChannelLevel(!floating);
  level->setFloatChannelLevel(floating);
  level->getProperties()->setBpp(floating ? 128 : 64);
  level->getProperties()->setHasAlpha(true);
  level->getProperties()->setImageRes(TDimension(2, 2));
  // Float rasters are written as linear values; use the same space on reload.
  level->getProperties()->setColorSpaceGamma(1.0);
  require(scene.getLevelSet()->insertLevel(level), "cannot insert test level");
  return level;
}

TRasterImageP drawing(bool floating) {
  TRasterP raster;
  if (floating) {
    TRasterFP pixels(2, 2);
    pixels->pixels(0)[0] = TPixelF(0.1234567f, 0.2345678f, 0.3456789f, 1.f);
    pixels->pixels(0)[1] = TPixelF(-0.1251234f, 1.234567f, 2.345678f, 0.456789f);
    pixels->pixels(1)[0] = TPixelF(0.000123456f, 0.2654321f, 0.3123456f, 0.5f);
    pixels->pixels(1)[1] = TPixelF(4.321987f, 0.8765432f, 0.f, 0.f);
    raster = pixels;
  } else {
    TRaster64P pixels(2, 2);
    pixels->pixels(0)[0] = TPixel64(12345, 23456, 34567, 65535);
    pixels->pixels(0)[1] = TPixel64(1, 257, 32769, 65535);
    pixels->pixels(1)[0] = TPixel64(54321, 43210, 32109, 65535);
    pixels->pixels(1)[1] = TPixel64(65534, 45678, 123, 65535);
    raster = pixels;
  }
  TRasterImageP image(raster);
  image->setDpi(96, 96);
  return image;
}

void verifySamples(const TRasterImageP &original, const TRasterImageP &decoded,
                   bool floating) {
  require(decoded && decoded->getRaster()->getSize() == TDimension(2, 2),
          "resource dimensions changed on save/reopen");
  if (floating) {
    TRasterFP expected = original->getRaster();
    TRasterFP actual = decoded->getRaster();
    require(actual, "float resource was reopened at lower precision");
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 2; ++x) {
        const TPixelF a = actual->pixels(y)[x];
        const TPixelF e = expected->pixels(y)[x];
        requireNear(a.r, e.r);
        requireNear(a.g, e.g);
        requireNear(a.b, e.b);
        requireNear(a.m, e.m);
      }
  } else {
    TRaster64P expected = original->getRaster();
    TRaster64P actual = decoded->getRaster();
    require(actual, "16-bit resource was reopened at lower precision");
    for (int y = 0; y < 2; ++y)
      for (int x = 0; x < 2; ++x)
        require(actual->pixels(y)[x] == expected->pixels(y)[x],
                "16-bit resource sample changed on save/reopen");
  }
}

void roundTrip(const QDir &dir, bool floating, double gamma = 1.0) {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  const QString extension = floating ? "exr" : "tif";
  const QString name = gamma == 1.0 ? "paint" : "paint-gamma";
  const TFilePath path = pathOf(dir.filePath(name + ".." + extension));
  TXshSimpleLevel *source = addLevel(
      scene, pathOf(dir.filePath("source-" + name + ".." + extension)),
      L"Source", floating);
  TXshSimpleLevel *paint = addLevel(scene, path, L"Paint", floating);
  source->getProperties()->setColorSpaceGamma(gamma);
  paint->getProperties()->setColorSpaceGamma(gamma);
  const TRasterImageP expected = drawing(floating);
  const TRasterImageP beforeSave = drawing(floating);
  const TFrameId fid(1);
  source->setFrame(fid, drawing(floating));
  paint->setCadmiumSourceLevelId(source->cadmiumLevelId());
  paint->setFrame(fid, TImageP(expected.getPointer()));
  paint->getProperties()->setDirtyFlag(true);
  // Exercise TXshSimpleLevel's real resource save path through LevelUpdater.
  paint->save();
  verifySamples(beforeSave, expected, floating);

  // The same inverse transfer is needed when saving an existing generated
  // resource; exporting must never modify the source or cached native raster.
  paint->setFrame(fid, TImageP(expected.getPointer()));
  paint->getProperties()->setDirtyFlag(true);
  paint->save();
  verifySamples(beforeSave, expected, floating);
  TRasterImageP unchangedSource = source->getFrame(
      fid, floating ? ImageManager::isFloatEnabled : ImageManager::is64bitEnabled,
      1);
  verifySamples(beforeSave, unchangedSource, floating);

  const TFilePath framePath = path.withFrame(fid);
  TImageReaderP reader(framePath);
  const TImageInfo *info = reader->getImageInfo();
  require(info && info->m_bitsPerSample == (floating ? 32 : 16) &&
              info->m_samplePerPixel == 4,
          "saved resource channel format is wrong");
  reader->enable16BitRead(!floating);
  reader->enableFloatRead(floating);
  reader->setColorSpaceGamma(gamma);
  verifySamples(expected, TRasterImageP(reader->load()), floating);

  if (floating) {
    Exr::InputFile inspection(framePath.getQString().toUtf8().constData());
    for (const char *name : {"R", "G", "B", "A"}) {
      const Exr::Channel *channel =
          inspection.header().channels().findChannel(name);
      require(channel && channel->type == Exr::FLOAT,
              "generated float EXR was stored as half float");
    }
  }

  // A fresh level has a fresh image-manager identity, so this cannot read the
  // generated drawing from its original in-memory cache.
  ToonzScene reopenedScene;
  TProjectManager::instance()->initializeScene(&reopenedScene);
  TXshSimpleLevel *reopened = addLevel(reopenedScene, path, L"Reopened", floating);
  reopened->set16BitChannelLevel(false);
  reopened->setFloatChannelLevel(false);
  reopened->getProperties()->setBpp(0);
  reopened->getProperties()->setColorSpaceGamma(gamma);
  reopened->load();
  reopened->setIsReadOnly(true);
  require(Cadmium::canPaintLevel(reopened),
          "read-only full-color input must allow a separate painted result");
  require(reopened->is16BitChannelLevel() == !floating &&
              reopened->isFloatChannelLevel() == floating &&
              reopened->getProperties()->getBpp() == (floating ? 128 : 64),
          "reopened level lost its resource precision");
  ImageManager *manager = ImageManager::instance();
  ImageBuilderP originalBuilder = manager->getBuilder(reopened->getImageId(fid));
  require(originalBuilder, "reopened drawing has no image builder");
  TRasterImageP proxy = reopened->getFrame(fid, ImageManager::none, 1);
  require(proxy && TRaster32P(proxy->getRaster()),
          "ordinary viewport request did not produce an 8-bit proxy");
  // Exercise the controller loader with a lower-precision image already cached.
  TRasterImageP loaded = Cadmium::loadDrawing(reopened, fid);
  verifySamples(expected, loaded, floating);
  require(manager->getBuilder(reopened->getImageId(fid)) ==
              originalBuilder.getPointer(),
          "native loading changed drawing identity");
  reopened->load();
  verifySamples(expected, TRasterImageP(Cadmium::loadDrawing(reopened, fid)),
                floating);
  require(manager->getBuilder(reopened->getImageId(fid)) ==
              originalBuilder.getPointer(),
          "resource reload changed drawing identity");

  // Holding the old builder alive prevents address reuse from hiding drawing
  // replacement even when the frame id and every pixel are identical.
  TImageP replacement(loaded->cloneImage());
  reopened->eraseFrame(fid);
  reopened->setFrame(fid, replacement);
  ImageBuilderP replacementBuilder =
      manager->getBuilder(reopened->getImageId(fid));
  require(replacementBuilder &&
              replacementBuilder.getPointer() != originalBuilder.getPointer(),
          "recreating an identical drawing retained its old identity");
  verifySamples(expected, TRasterImageP(Cadmium::loadDrawing(reopened, fid)),
                floating);

  const int renumberCount = reopened->getRenumberCount();
  const TFrameId renumbered(7);
  reopened->renumber(std::vector<TFrameId>{renumbered});
  require(reopened->getRenumberCount() == renumberCount + 1,
          "drawing renumber did not update the revision");
  reopened->renumber(std::vector<TFrameId>{fid});
  require(reopened->getRenumberCount() == renumberCount + 2,
          "restoring original frame ids lost the renumber revision");
  verifySamples(expected, TRasterImageP(Cadmium::loadDrawing(reopened, fid)),
                floating);
}

void retainedExrMetadata(const QDir &dir) {
  const TFilePath fixture = pathOf(dir.filePath("paint..exr"))
                                .withFrame(TFrameId(1));
  const TFilePath sequence = pathOf(dir.filePath("retained..exr"));
  const TFrameId first(1), second(2, QStringLiteral("A"));
  for (const TFrameId &fid : {first, second})
    require(QFile::copy(fixture.getQString(), sequence.withFrame(fid).getQString()),
            "cannot create retained EXR source fixture");

  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  TXshExrLevel *source = new TXshExrLevel(L"Retained Source");
  source->setScene(&scene);
  source->setPath(sequence, true);
  source->getProperties()->setColorSpaceGamma(1.0);
  require(scene.getLevelSet()->insertLevel(source),
          "cannot insert retained source level");
  source->load();
  require(source->isFid(first) && source->isFid(second),
          "retained EXR sequence lost a letter-suffixed drawing");
  require(source->isReadOnly() && Cadmium::canPaintLevel(source),
          "retained EXR is not available as a protected painting input");
  source->setCadmiumReference(first, true);
  source->setCadmiumReference(second, true);
  const std::set<TFrameId> references = {first, second};
  const QString sourceId = source->cadmiumLevelId();
  TXshSimpleLevel ancestor;
  const QString ancestorId = ancestor.cadmiumLevelId();
  source->setCadmiumSourceLevelId(ancestorId);
  const TRasterImageP original(Cadmium::loadDrawing(source, first));
  require(original && TRasterFP(original->getRaster()),
          "retained source was not captured as native float");

  auto verifySource = [&](TXshExrLevel *level) {
    require(level->getCadmiumReferences() == references,
            "retained EXR lost painted reference marks");
    require(level->cadmiumLevelId() == sourceId &&
                level->cadmiumSourceLevelId() == ancestorId,
            "retained EXR lost its stable identity or source association");
    require(level->isReadOnly() && level->isFloatChannelLevel(),
            "retained EXR lost its protected native format");
  };

  // Resource rebuilds, explicit subsets and temporary loading ranges must not
  // delete marks for drawings that are currently outside the visible subset.
  source->load();
  verifySource(source);
  setLoadingLevelRange(first, first);
  source->load();
  require(source->isFid(first) && !source->isFid(second),
          "retained source loading range was not applied");
  verifySource(source);
  source->load(std::vector<TFrameId>{second});
  require(!source->isFid(first) && source->isFid(second),
          "retained source explicit subset was not applied");
  verifySource(source);
  source->load();
  verifySource(source);
  verifySamples(original, TRasterImageP(Cadmium::loadDrawing(source, first)), true);

  TXshSimpleLevel *result = addLevel(
      scene, pathOf(dir.filePath("paint..exr")), L"Retained Paint", true);
  result->setCadmiumSourceLevelId(sourceId);
  result->setFrame(first, drawing(true));
  const TFilePath scenePath = pathOf(dir.filePath("retained-scene.tnz"));
  scene.saveCopy(scenePath);

  ToonzScene reopenedScene;
  reopenedScene.loadTnzFile(scenePath);
  auto *restored = dynamic_cast<TXshExrLevel *>(
      reopenedScene.getLevelSet()->getLevel(L"Retained Source"));
  require(restored, "scene reload discarded the dedicated EXR level class");
  verifySource(restored);
  restored->load();
  verifySource(restored);
  verifySamples(original, TRasterImageP(Cadmium::loadDrawing(restored, first)), true);
  TXshLevel *restoredResult =
      reopenedScene.getLevelSet()->getLevel(L"Retained Paint");
  require(restoredResult && restoredResult->getSimpleLevel() &&
              restoredResult->getSimpleLevel()->cadmiumSourceLevelId() ==
                  restored->cadmiumLevelId(),
          "scene reload detached generated paint from its retained EXR source");
}

void verifyOrdinaryWriterDefaults(const QDir &dir) {
  ToonzScene scene;
  TProjectManager::instance()->initializeScene(&scene);
  for (bool floating : {false, true}) {
    TXshSimpleLevel *level = addLevel(
        scene, pathOf(dir.filePath(floating ? "ordinary..exr" : "ordinary..tif")),
        floating ? L"Ordinary Float" : L"Ordinary 16", floating);
    LevelUpdater updater(level);
    const TEnumProperty *bpp = static_cast<const TEnumProperty *>(
        updater.getLevelWriter()->getProperties()->getProperty("Bits Per Pixel"));
    // Existing new-level enforcement chooses the smallest format divisible by
    // 32, which is RGB half float for an ordinary EXR level.
    require(bpp && bpp->getValue() ==
                       (floating ? L"96(RGB)_HF" : L"32(RGBM)"),
            "ordinary output writer defaults changed");
    updater.update(TFrameId(1), drawing(floating));
    updater.close();

    // Existing ordinary EXR resources use the writer's RGBA half-float default
    // because reader metadata does not retain the original writer properties.
    updater.open(level);
    bpp = static_cast<const TEnumProperty *>(
        updater.getLevelWriter()->getProperties()->getProperty("Bits Per Pixel"));
    require(bpp && bpp->getValue() ==
                       (floating ? L"128(RGBA)_HF" : L"32(RGBM)"),
            "existing resource writer precision changed");
    updater.close();
  }
}

template <typename Operation>
bool runCase(const char *label, Operation operation) {
  try {
    operation();
    std::cout << label << " passed\n";
    return true;
  } catch (const TException &) {
    std::cerr << label << ": resource operation failed\n";
  } catch (const std::string &error) {
    std::cerr << label << ": " << error << '\n';
  } catch (const std::exception &error) {
    std::cerr << label << ": " << error.what() << '\n';
  }
  return false;
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    QTemporaryDir temporary;
    require(temporary.isValid(), "cannot create test storage");
    const QDir dir(temporary.path());
    const QString stuff = dir.filePath("stuff");
    QDir().mkpath(QDir(stuff).filePath("profiles/layouts/rooms/Default"));
    QDir().mkpath(QDir(stuff).filePath("library"));
    TEnv::setStuffDir(pathOf(stuff));
    Preferences::instance();
    TEnv::setApplicationFullName("cadmium_save_test");
    QDir().mkpath(dir.filePath("cache"));
    TImageCache::instance()->setRootDir(pathOf(dir.filePath("cache")));
    TThread::init();
    initImageIo();
    bool passed = true;
    if (!runCase("TIFF16 roundtrip", [&] { roundTrip(dir, false); }))
      passed = false;
    if (!runCase("Float EXR linear roundtrip", [&] { roundTrip(dir, true); }))
      passed = false;
    if (!runCase("Float EXR default gamma roundtrip", [&] {
          roundTrip(dir, true, LevelOptions::DefaultColorSpaceGamma);
        }))
      passed = false;
    if (!runCase("Ordinary writer defaults", [&] {
          verifyOrdinaryWriterDefaults(dir);
        }))
      passed = false;
    if (!runCase("Retained EXR metadata", [&] { retainedExrMetadata(dir); }))
      passed = false;
    if (!passed) return 1;
    std::cout << "Cadmium resource precision checks passed\n";
    return 0;
  } catch (const TException &) {
    std::cerr << "Cadmium resource save/reopen failed\n";
  } catch (const std::string &error) {
    std::cerr << error << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
