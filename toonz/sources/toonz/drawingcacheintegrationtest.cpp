#include "drawingcacheintegrationtest.h"

#include "drawingdata.h"
#include "filmstripcommand.h"
#include "nativeuiintegrationtest.h"
#include "tapp.h"
#include "texception.h"
#include "timagecache.h"
#include "tpalette.h"
#include "trasterimage.h"
#include "tundo.h"
#include "tools/toolhandle.h"
#include "toonz/levelproperties.h"
#include "toonz/tcolumnhandle.h"
#include "toonz/tframehandle.h"
#include "toonz/toonzscene.h"
#include "toonz/tscenehandle.h"
#include "toonz/txshcell.h"
#include "toonz/txsheet.h"
#include "toonz/txsheethandle.h"
#include "toonz/txshlevelhandle.h"
#include "toonz/txshleveltypes.h"
#include "toonz/txshsimplelevel.h"
#include "toonzqt/dvmimedata.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QTimer>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using Images = std::map<TFrameId, QString>;
const TFrameId sourceFrame(1, "a");

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class SavedClipboard final {
  std::unique_ptr<QMimeData> m_data;
  QString m_tool;

public:
  SavedClipboard()
      : m_tool(TApp::instance()->getCurrentTool()->getRequestedToolName()) {
    const auto* source = QApplication::clipboard()->mimeData();
    const auto* native = dynamic_cast<const DvMimeData*>(source);
    m_data.reset(native ? native->clone() : new QMimeData);
    if (!source) return;
    for (const auto& format : source->formats())
      m_data->setData(format, source->data(format));
    if (source->hasImage()) m_data->setImageData(source->imageData());
  }

  ~SavedClipboard() {
    TApp::instance()->getCurrentTool()->setTool(m_tool);
    QApplication::clipboard()->setMimeData(m_data.release());
  }
};

TImageP drawing(const TPixel32& color, TPalette* palette) {
  TRaster32P raster(4, 4);
  raster->fill(color);
  TRasterImageP image(raster);
  image->setDpi(96.0, 96.0);
  image->setPalette(palette);
  return image;
}

TXshSimpleLevel* makeLevel(const std::wstring& name, TPalette* palette) {
  auto* level = TApp::instance()
                    ->getCurrentScene()
                    ->getScene()
                    ->createNewLevel(OVL_XSHLEVEL, name)
                    ->getSimpleLevel();
  level->setPalette(palette);
  level->getProperties()->setImageRes(TDimension(4, 4));
  level->getProperties()->setImageDpi(TPointD(96.0, 96.0));
  return level;
}

void checkColor(const TImageP& image, const TPixel32& color) {
  TRasterImageP rasterImage = image;
  require(bool(rasterImage), "drawing cache lost a fullcolor image");
  TRaster32P raster = rasterImage->getRaster();
  require(raster && raster->getSize() == TDimension(4, 4),
          "drawing cache changed the image type or dimensions");
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x)
      require(raster->pixels(y)[x] == color,
              "drawing cache changed snapshot pixels");
}

void checkCached(const Images& images, bool expected) {
  require(!images.empty(), "drawing cache fixture has no copied IDs");
  for (const auto& [frame, id] : images)
    require(TImageCache::instance()->isCached(id.toStdString()) == expected,
            "drawing cache lifetime differs from its remaining owners");
}

const DrawingData* copiedDrawings() {
  const auto* data =
      dynamic_cast<const DrawingData*>(QApplication::clipboard()->mimeData());
  require(data && !data->m_imageSet.empty(),
          "Filmstrip Copy did not publish drawings");
  return data;
}

void copyFrames(TXshSimpleLevel* source) {
  std::set<TFrameId> frames{sourceFrame, TFrameId(3)};
  FilmstripCmd::copy(source, frames);
}

void checkCopyPixels(const DrawingData& data) {
  auto* cache = TImageCache::instance();
  require(
      data.m_imageSet.size() == 2 && data.m_imageSet.count(sourceFrame) == 1,
      "drawing snapshot changed lettered frame identities");
  checkColor(cache->get(data.m_imageSet.at(sourceFrame), false),
             TPixel32::Blue);
  checkColor(cache->get(data.m_imageSet.at(TFrameId(3)), false),
             TPixel32::Green);
}

void checkClipboardClones(TXshSimpleLevel* source) {
  copyFrames(source);
  std::unique_ptr<DrawingData> clone(copiedDrawings()->clone());
  const Images old = clone->m_imageSet;
  QApplication::clipboard()->clear();
  checkCached(old, true);
  checkCopyPixels(*clone);
  clone->releaseData();
  checkCached(old, false);

  Images previous;
  for (int cycle = 0; cycle < 16; ++cycle) {
    copyFrames(source);
    if (!previous.empty()) checkCached(previous, false);
    checkCopyPixels(*copiedDrawings());
    previous = copiedDrawings()->m_imageSet;
    checkCached(previous, true);
  }
  QApplication::clipboard()->clear();
  checkCached(previous, false);
}

void checkUndoOwner(TXshSimpleLevel* source, TXshSimpleLevel* target) {
  auto* app  = TApp::instance();
  auto* undo = TUndoManager::manager();
  std::set<TFrameId> frames{sourceFrame};
  FilmstripCmd::copy(source, frames);
  const Images images = copiedDrawings()->m_imageSet;
  std::vector<TFrameId> before;
  target->getFids(before);
  app->getCurrentColumn()->setColumnIndex(1);
  app->getCurrentLevel()->setLevel(target);
  frames = {TFrameId(10)};
  FilmstripCmd::paste(target, frames);
  require(undo->getHistoryCount() == 1 && frames.size() == 1,
          "Filmstrip Paste did not create one undoable drawing");
  const TFrameId inserted = *frames.begin();
  checkColor(target->getFrame(inserted, false), TPixel32::Blue);
  std::vector<TFrameId> after;
  target->getFids(after);
  QApplication::clipboard()->clear();
  checkCached(images, true);
  require(undo->undo(), "drawing cache fixture could not undo paste");
  std::vector<TFrameId> restored;
  target->getFids(restored);
  require(restored == before, "paste undo changed the original drawing IDs");
  require(undo->redo(), "clipboard clearing broke drawing paste redo");
  target->getFids(restored);
  require(restored == after, "paste redo changed drawing IDs");
  checkColor(target->getFrame(inserted, false), TPixel32::Blue);
  undo->reset();
  checkCached(images, false);
  checkColor(target->getFrame(inserted, false), TPixel32::Blue);
}

void checkTransferredGenerations(TXshSimpleLevel* source, bool releaseFirst) {
  auto* cache            = TImageCache::instance();
  const QString producer = QString::fromStdString(cache->getUniqueId());
  HookSet hooks;
  cache->add(producer, drawing(TPixel32::Blue, source->getPalette()));
  DrawingData first;
  first.setFrames({{sourceFrame, producer}, {TFrameId(3), producer}}, source,
                  hooks);
  const Images old = first.m_imageSet;
  require(old.at(sourceFrame) == old.at(TFrameId(3)) &&
              old.at(sourceFrame) != producer,
          "cache transfer lost aliases or reused the producer key");
  std::unique_ptr<DrawingData> clone(first.clone());
  first.releaseData();
  cache->add(producer, drawing(TPixel32::Green, source->getPalette()));
  DrawingData second;
  second.setFrames({{sourceFrame, producer}}, source, hooks);
  const Images next = second.m_imageSet;
  checkColor(cache->get(old.at(sourceFrame), false), TPixel32::Blue);
  checkColor(cache->get(next.at(sourceFrame), false), TPixel32::Green);
  require(!cache->isCached(producer.toStdString()),
          "transfer left its temporary key behind");
  if (releaseFirst) {
    clone.reset();
    checkCached(old, false);
    checkCached(next, true);
    checkColor(cache->get(next.at(sourceFrame), false), TPixel32::Green);
    second.releaseData();
  } else {
    second.releaseData();
    checkCached(next, false);
    checkCached(old, true);
    checkColor(cache->get(old.at(sourceFrame), false), TPixel32::Blue);
    clone.reset();
  }
  checkCached(next, false);
  checkCached(old, false);
}

void checkReplacementAndFailure(TXshSimpleLevel* source) {
  auto* cache = TImageCache::instance();
  DrawingData data;
  std::set<TFrameId> frames{sourceFrame, TFrameId(3)};
  data.setLevelFrames(source, frames);
  const Images old = data.m_imageSet;
  std::unique_ptr<DrawingData> clone(data.clone());
  const QString producer = QString::fromStdString(cache->getUniqueId());
  const QString missing  = QString::fromStdString(cache->getUniqueId());
  cache->add(producer, drawing(TPixel32::Green, source->getPalette()));
  bool rejected = false;
  try {
    data.setFrames({{TFrameId(1), producer}, {TFrameId(2), missing}}, source,
                   *source->getHookSet());
  } catch (const TException&) {
    rejected = true;
  }
  require(rejected && data.m_imageSet == old,
          "failed cache transfer replaced the prior drawing snapshot");
  require(cache->isCached(producer.toStdString()),
          "failed cache transfer consumed a producer entry");
  checkCopyPixels(data);
  cache->remove(producer);
  data.setFrames(data.m_imageSet, source, *source->getHookSet());
  require(data.m_imageSet != old,
          "self-transfer did not create a distinct cache generation");
  checkCached(old, true);
  checkCopyPixels(*clone);
  clone.reset();
  checkCached(old, false);
  const Images replacement = data.m_imageSet;
  data.setLevelFrames(source, frames);
  checkCached(replacement, false);
  checkCopyPixels(data);
  const Images last = data.m_imageSet;
  data.releaseData();
  checkCached(last, false);
}

void checkConsumingPaste(TXshSimpleLevel* source, TXshSimpleLevel* target) {
  DrawingData data;
  std::set<TFrameId> frames{sourceFrame};
  data.setLevelFrames(source, frames);
  const Images images = data.m_imageSet;
  frames              = {TFrameId(10)};
  bool keepPalette    = true;
  const auto result   = data.getLevelFrames(
      target, frames, DrawingData::OVER_SELECTION, false, keepPalette, true);
  require(result.pasted, "consuming drawing paste failed");
  checkCached(images, false);
  checkColor(target->getFrame(TFrameId(10), false), TPixel32::Blue);
  data.releaseData();
  checkColor(target->getFrame(TFrameId(10), false), TPixel32::Blue);
}

void checkDrawingOwners() {
  QString error;
  if (!resetNativeUiIntegrationScene(&error))
    throw std::runtime_error(error.toStdString());
  auto* app = TApp::instance();
  app->getCurrentTool()->setTool("T_Hand");
  TPaletteP palette = new TPalette;
  auto* source      = makeLevel(L"DrawingCacheSource", palette.getPointer());
  auto* target      = makeLevel(L"DrawingCacheTarget", palette.getPointer());
  source->setFrame(sourceFrame, drawing(TPixel32::Blue, palette.getPointer()));
  source->setFrame(TFrameId(3), drawing(TPixel32::Green, palette.getPointer()));
  target->setFrame(TFrameId(10),
                   drawing(TPixel32::Transparent, palette.getPointer()));
  auto* sheet = app->getCurrentXsheet()->getXsheet();
  sheet->setCell(0, 0, TXshCell(source, sourceFrame));
  sheet->setCell(0, 1, TXshCell(target, TFrameId(10)));
  app->getCurrentXsheet()->notifyXsheetChanged();
  app->getCurrentFrame()->setFrame(0);
  app->getCurrentLevel()->setLevel(source);
  checkClipboardClones(source);
  for (int cycle = 0; cycle < 4; ++cycle) checkUndoOwner(source, target);
  for (int cycle = 0; cycle < 8; ++cycle) {
    for (bool releaseFirst : {false, true})
      checkTransferredGenerations(source, releaseFirst);
    checkReplacementAndFailure(source);
    checkConsumingPaste(source, target);
  }
}
}  // namespace

bool runDrawingCacheIntegrationTest(QString* error) {
  SavedClipboard saved;
  QString failure;
  bool unexpectedDialog = false;
  QTimer dialogs;
  QObject::connect(&dialogs, &QTimer::timeout, &dialogs, [&] {
    if (auto* dialog =
            qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
      unexpectedDialog = true;
      dialog->reject();
    }
  });
  dialogs.start(10);
  try {
    checkDrawingOwners();
    require(!unexpectedDialog,
            "drawing cache fixture opened an unexpected dialog");
  } catch (const TException& exception) {
    failure = QString::fromStdWString(exception.getMessage());
  } catch (const std::exception& exception) {
    failure = QString::fromUtf8(exception.what());
  } catch (...) {
    failure = "Unknown exception during drawing cache ownership checks";
  }
  dialogs.stop();
  QString cleanup;
  if (!resetNativeUiIntegrationScene(&cleanup))
    failure += " Fixture cleanup: " + cleanup;
  if (error) *error = failure;
  if (!failure.isEmpty()) return false;
  std::cout << "drawing-cache-native: PASS clipboard/undo leases, repeated "
               "generations, "
               "failure preservation and consuming paste"
            << std::endl;
  return true;
}
