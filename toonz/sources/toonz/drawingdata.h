#pragma once

#ifndef DRAWINGDATA_INCLUDED
#define DRAWINGDATA_INCLUDED

#include "toonz/txshsimplelevel.h"
#include "toonz/hook.h"
#include "toonzqt/dvmimedata.h"

#include <set>
#include <vector>
#include <memory>
#include "timage.h"
#include "tfilepath.h"

//=============================================================================
// DrawingData
// Sono i fotogrammi della filmstrip
//-----------------------------------------------------------------------------

class DrawingData final : public DvMimeData {
  struct CacheEntries;
  std::shared_ptr<CacheEntries> m_cacheEntries;

public:
  std::map<TFrameId, QString>
      m_imageSet;  // images are in the cache, the QString is the cache id
  HookSet m_levelHooks;
  TXshSimpleLevelP m_level;
  bool m_keepVectorFills = false;

  /*! Define possible level image setting.
  *  \li INSERT Insert images before first selected;
  *  \li OVER_FRAMEID Replace images with the same frameId images in data;
  *  \li OVER_SELECTION Replace selected images with images in data.
  */
  enum ImageSetType { INSERT, OVER_FRAMEID, OVER_SELECTION };

  struct PasteResult {
    bool pasted = false;
    // Original IDs ordered by the surviving drawings after an INSERT.
    std::vector<TFrameId> originalFrames;
  };

  DrawingData() {}

  DrawingData(const DrawingData* src)
      : m_cacheEntries(src->m_cacheEntries)
      , m_imageSet(src->m_imageSet)
      , m_levelHooks(src->m_levelHooks)
      , m_level(src->m_level) {}

  void releaseData() override;
  ~DrawingData();

  DrawingData *clone() const override;

  // data <- filmstrip
  void setLevelFrames(TXshSimpleLevel *sl, std::set<TFrameId> &frames);

  // data -> filmstrip
  // Se setType == INSERT inserisce i frames nel livello se necessario spostando
  // verso il basso i preesistenti. Altrimenti sostituisce i frames.
  PasteResult getLevelFrames(TXshSimpleLevel* sl, std::set<TFrameId>& frames,
                             ImageSetType setType, bool cloneImages,
                             bool& keepOriginalPalette,
                             bool isRedo = false) const;

  // Adopt temporary cached frames. Clones share the resulting snapshot lease;
  // callers may reuse their original cache IDs after this transfer.
  void setFrames(const std::map<TFrameId, QString> &imageSet,
                 TXshSimpleLevel *level, const HookSet &levelHooks);
  // Return frameId contained in m_imageSet.
  void getFrames(std::set<TFrameId> &frameId) const;
  TXshSimpleLevel *getLevel() const { return m_level.getPointer(); }
  void setKeepVectorFills(bool keep) { m_keepVectorFills = keep; }
  bool getKeepVectorFills() { return m_keepVectorFills; }

protected:
  TImageP getImage(QString imageId, TXshSimpleLevel *sl,
                   const std::map<int, int> &styleTable,
                   bool keepVectorFills = false) const;
};

#endif  // DRAWINGDATA_INCLUDED
