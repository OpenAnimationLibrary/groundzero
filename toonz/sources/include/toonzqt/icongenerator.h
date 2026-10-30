#pragma once

#ifndef ICON_GENERATOR_H
#define ICON_GENERATOR_H

// TnzCore includes
#include "tthread.h"
#include "tgeometry.h"
#include "tfilepath.h"
#include "traster.h"
#include "timage.h"
#include "tpixel.h"

// Qt includes
#include <QPixmap>
#include <QThreadStorage>
#include <QEventLoop>

// STD includes
#include <map>
#include <vector>
#include <set>
#include <functional>
#include <memory>

#undef DVAPI
#undef DVVAR
#ifdef TOONZQT_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

//==============================================================

//    Forward declarations

class TFilePath;
class TXshLevel;
class TXshSimpleLevel;
class TXsheet;
class TStageObjectSpline;
class ToonzScene;
class TOfflineGL;
class QTimer;

//==============================================================

//**********************************************************************************
//    IconGenerator  definition
//**********************************************************************************

/*!
  \brief    The class responsible for icons management in Toonz.

  \details  It's a singleton class - in particular, rendered icons are stored in
            a shared map container for fast retrieval upon repeated icon
  requests.

            IconGenerator provides methods to submit icon requests, and to
  invalidate or
            remove icons from the internal database. In order to keep outer
  entities
            informed of the icon generation status, an iconGenerated() signal is
  emitted
            once an icon has been generated.
*/

class DVAPI IconGenerator final : public QObject {
  Q_OBJECT

public:
  class Settings {
  public:
    bool m_blackBgCheck;
    bool m_transparencyCheck;
    bool m_inksOnly;
    int m_inkIndex;
    int m_paintIndex;
    bool m_inkCheckEnabled;
    bool m_ink1CheckEnabled;
    bool m_paintCheckEnabled;

    Settings()
        : m_transparencyCheck(false)
        , m_blackBgCheck(false)
        , m_inksOnly(false)
        , m_inkIndex(-1)
        , m_paintIndex(-1)
        , m_inkCheckEnabled(false)
        , m_ink1CheckEnabled(false)
        , m_paintCheckEnabled(false) {}
  };

public:
  IconGenerator();
  ~IconGenerator();

  static IconGenerator *instance();

  void setSettings(const Settings &settings) { m_settings = settings; }
  const Settings getSettings() const { return m_settings; }
  static void setFilmstripIconSize(const TDimension &dim);

  TDimension getIconSize() const;

  TOfflineGL *getOfflineGLContext(const TDimension &minSize = TDimension());

  // icons from splines
  QPixmap getIcon(TStageObjectSpline *spline);
  void invalidate(TStageObjectSpline *spline);
  void remove(TStageObjectSpline *spline);

  // icons from toonz levels
  QPixmap getIcon(TXshLevel *sl, const TFrameId &fid, bool filmStrip = true,
                  bool onDemand = false);
  // GUI-thread requests share the existing renderer/cache. A nonzero token
  // identifies one actual generation, not a level/frame key. It completes once
  // on success, error or cancellation; running cancellations finish normally.
  quint64 requestIcon(TXshSimpleLevel *sl, const TFrameId &fid,
                      bool filmStrip = true);
  // Cache-only query: never queues or decodes a drawing.
  quint64 pendingIconRequest(TXshSimpleLevel *sl, const TFrameId &fid,
                             bool filmStrip = true) const;
  QPixmap getSizedIcon(TXshLevel *sl, const TFrameId &fid, std::string newId,
                       TDimension dim = TDimension(0, 0));
  QPixmap getResponsiveIcon(TXshLevel *sl, const TFrameId &fid,
                            const TDimension &dim);
  void invalidate(TXshLevel *sl, const TFrameId &fid,
                  bool onlyFilmStrip = false);
  void remove(TXshLevel *sl, const TFrameId &fid, bool onlyFilmStrip = false);

  // icons from files
  QPixmap getIcon(const TFilePath &path,
                  const TFrameId &fid = TFrameId::NO_FRAME);
  void invalidate(const TFilePath &path,
                  const TFrameId &fid = TFrameId::NO_FRAME);
  void remove(const TFilePath &path, const TFrameId &fid = TFrameId::NO_FRAME);

  // camera-stand render of a frame of an xsheet (e.g. storyboard thumbnails);
  // returns a null pixmap until the icon is generated
  QPixmap getXsheetFrameIcon(TXsheet *xsh, int row, const TDimension &dim,
                             bool onDemand = false);
  void removeXsheetFrameIcons(TXsheet *xsh);

  QPixmap getSceneIcon(ToonzScene *scene);  // Unused in Toonz
  void invalidateSceneIcon();

  void remap(const std::string &newIconId, const std::string &oldIconId);

  void clearRequests();
  void clearSceneIcons();

  // GUI-thread, terminal and idempotent. Keep the scene, cache and QApplication
  // alive until this returns: it services events while retiring all accepted
  // requests and their workers, including worker-local OpenGL resources.
  void shutdown();
  bool isShutdownComplete() const { return m_shutdownComplete; }
  // GUI-thread nonterminal scene transition. Prepare a replacement executor
  // before closing admission. Preparation failure preserves the existing
  // executor; an already-paused or terminal generator also refuses (false).
  bool pauseForSceneChange();
  void resumeAfterSceneChange() noexcept;

#ifdef BUILD_UI_PORT_TESTS
  using TaskTestHook = std::function<void(quint64)>;
  void setTaskTestHook(TaskTestHook hook);
  std::vector<TThread::RunnableP> lifecycleTestTasks() const;
  bool lifecycleTestStopping() const { return m_shuttingDown; }
  bool lifecycleTestPaused() const { return m_sceneChangePaused; }
#endif

  static TRaster32P generateVectorFileIcon(const TFilePath &path,
                                           const TDimension &iconSize,
                                           const TFrameId &fid);
  static TRaster32P generateRasterFileIcon(const TFilePath &path,
                                           const TDimension &iconSize,
                                           const TFrameId &fid);
  static TRaster32P generateSceneFileIcon(const TFilePath &path,
                                          const TDimension &iconSize, int row);
  static TRaster32P generateSplineFileIcon(const TFilePath &path,
                                           const TDimension &iconSize);
  static TRaster32P generateMeshFileIcon(const TFilePath &path,
                                         const TDimension &iconSize,
                                         const TFrameId &fid);

  // This function is called when only colors of styles are changed in toonz
  // raster levels. In such case it doesn't need to re-compute icons but needs
  // to let panels to update. See TApp::onLevelColorStyleChanged() for details.
  void notifyIconGenerated() {
    if (!m_shuttingDown && !m_sceneChangePaused) emit iconGenerated();
  }
signals:

  void iconGenerated();
  void iconRequestFinished(quint64 request, bool generated);

public slots:

  void onStarted(TThread::RunnableP iconRenderer);
  void onCanceled(TThread::RunnableP iconRenderer);
  void onFinished(TThread::RunnableP iconRenderer);
  void onException(TThread::RunnableP iconRenderer);
  void onTerminated(TThread::RunnableP iconRenderer);

private:
  std::unique_ptr<TThread::Executor> m_executor;
  std::unique_ptr<TThread::Executor> m_resumeExecutor;
  QTimer *m_resumeNotification;
  QThreadStorage<TOfflineGL *> m_contexts;
  TDimension m_iconSize;

  Settings m_settings;
  quint64 m_nextRequest = 0;
  std::map<std::string, quint64> m_pendingRequests;
  // Includes superseded generations; the latest-per-key map cannot prove drain.
  std::map<quint64, TThread::RunnableP> m_admittedRequests;
  bool m_shuttingDown = false;
  bool m_shutdownComplete = false;
  bool m_sceneChangePaused = false;

private:
  void addTask(const std::string &id, TThread::RunnableP iconRenderer);
  bool isCurrentRequest(TThread::RunnableP iconRenderer) const;
  void finishRequest(TThread::RunnableP iconRenderer, bool generated);
  void drainExecutor();
};

//**********************************************************************************
//    Related non-member functions
//**********************************************************************************

template <class It>
inline void invalidateIcons(TXshLevel *sl, It fBegin, It fEnd,
                            bool onlyFilmStrip = false) {
  for (It ft = fBegin; ft != fEnd; ++ft)
    IconGenerator::instance()->invalidate(sl, *ft, onlyFilmStrip);
}

//------------------------------------------------------------------------

template <class C>
inline void invalidateIcons(TXshLevel *sl, const C &fids,
                            bool onlyFilmStrip = false) {
  invalidateIcons(sl, fids.begin(), fids.end(), onlyFilmStrip);
}

//------------------------------------------------------------------------

template <typename It>
inline void removeIcons(TXshLevel *sl, It fBegin, It fEnd,
                        bool onlyFilmStrip = false) {
  for (It ft = fBegin; ft != fEnd; ++ft)
    IconGenerator::instance()->remove(sl, *ft, onlyFilmStrip);
}

//------------------------------------------------------------------------

template <typename C>
inline void removeIcons(TXshLevel *sl, const C &fids,
                        bool onlyFilmStrip = false) {
  removeIcons(sl, fids.begin(), fids.end(), onlyFilmStrip);
}

#endif  // ICON_GENERATOR_H
