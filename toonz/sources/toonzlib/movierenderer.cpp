

// TnzCore includes
#include "tsystem.h"
#include "tstopwatch.h"
#include "tthreadmessage.h"
#include "timagecache.h"
#include "tlevel_io.h"
#include "trasterimage.h"
#include "timageinfo.h"
#include "trop.h"
#include "tsop.h"

#include "tiio.h"

// TnzLib includes
#include "toonz/toonzscene.h"
#include "toonz/sceneproperties.h"
#include "toonz/txsheet.h"
#include "toonz/tcamera.h"
#include "toonz/preferences.h"
#include "toonz/trasterimageutils.h"
#include "toonz/levelupdater.h"
#include "toutputproperties.h"
#include "toonz/boardsettings.h"

// tcg includes
#include "tcg/tcg_macros.h"

// Qt includes
#include <QCoreApplication>
#include <QTimer>
#include <atomic>

#include "toonz/movierenderer.h"
#include "movierenderqueue.h"
#include "movierenderoutput.h"

//**************************************************************************
//    Local Namespace  stuff
//**************************************************************************

namespace {

int RenderSessionId = 0;

//---------------------------------------------------------

void addMark(const TRasterP &mark, TRasterImageP img) {
  TRasterP raster = img->getRaster();

  if (raster->getLx() >= mark->getLx() && raster->getLy() >= mark->getLy()) {
    TRasterP ras = raster->clone();

    int borderx = troundp(0.035 * (ras->getLx() - mark->getLx()));
    int bordery = troundp(0.035 * (ras->getLy() - mark->getLy()));

    TRect rect = TRect(borderx, bordery, borderx + mark->getLx() - 1,
                       bordery + mark->getLy() - 1);
    TRop::over(ras->extract(rect), mark);

    img->setRaster(ras);
  }
}

//---------------------------------------------------------

void getRange(ToonzScene *scene, bool isPreview, int &from, int &to) {
  TSceneProperties *sprop = scene->getProperties();

  int step;
  if (isPreview)
    sprop->getPreviewProperties()->getRange(from, to, step);
  else
    sprop->getOutputProperties()->getRange(from, to, step);

  if (to < 0) {
    TXsheet *xs = scene->getXsheet();

    // NOTE: Use of numeric_limits::min is justified since the type is
    // *INTERGRAL*.
    from = (std::numeric_limits<int>::max)(),
    to   = (std::numeric_limits<int>::min)();

    for (int k = 0; k < xs->getColumnCount(); ++k) {
      int r0, r1;
      xs->getCellRange(k, r0, r1);

      TXshColumn *col         = xs->getColumn(k);
      TXshSoundColumn *sndCol = col ? col->getSoundColumn() : 0;

      if (sndCol) r0 = 0;
      from = std::min(from, r0), to = std::max(to, r1);
    }
  }
}

//---------------------------------------------------------

QString getPreviewName(unsigned long renderSessionId) {
  return "previewed" + QString::number(renderSessionId) + ".noext";
}

}  // namespace

//**************************************************************************
//    MovieRenderer::Imp  definition
//**************************************************************************

class MovieRenderer::Imp final : public TRenderPort, public TSmartObject {
public:
  ToonzScene *m_scene;
  TRenderer m_renderer;
  TFilePath m_fp;

  TRenderSettings m_renderSettings;
  TDimension m_frameSize;
  double m_xDpi, m_yDpi;

  std::set<MovieRenderer::Listener *> m_listeners;
  std::unique_ptr<LevelUpdater> m_levelUpdaterA, m_levelUpdaterB;
  TSoundTrackP m_st;

  struct FrameRasters {
    TRasterP first, second;
    bool applyGamma = false;
  };
  MovieRenderQueue<FrameRasters> m_toBeSaved;
  std::vector<std::pair<double, TFxPair>> m_framesToBeRendered;
  std::string m_renderCacheId;

  TThread::Mutex m_mutex;
  TThread::Mutex m_cancelMutex;
  std::vector<TLevelWriterP> m_cancelWriters;

  int m_renderSessionId;
  long m_whiteSample;

  int m_savingThreadsCount;
  bool m_firstCompletedRaster;
  bool m_failure;
  bool m_cacheResults;
  bool m_preview;
  bool m_movieType;
  bool m_seqRequired;
  bool m_started = false;
  bool m_finished = false;
  std::atomic<bool> m_canceled{false};
  std::wstring m_error;

public:
  Imp(ToonzScene *scene, const TFilePath &moviePath, int threadCount,
      bool cacheResults);
  ~Imp();

  // TRenderPort methods

  void onRenderRasterCompleted(const RenderData &renderData) override;
  void onRenderFailure(const RenderData &renderData, TException &e) override;

  /*-- Do not update m_overallRenderedRegion on cancel --*/
  void onRenderFinished(bool isCanceled = false) override;

  void doRenderRasterCompleted(const RenderData &renderData);
  void doPreviewRasterCompleted(const RenderData &renderData);

  // Helper methods

  void prepareForStart();
  void addSoundtrack(int r0, int r1, double fps, int boardDuration = 0);

  // writeInLinearColorSpace : Whether the format will save image in linear
  // color space. (true only in EXR format) writingGamma : Color space gamma to
  // be used for saving in the file ("Color Space Gamma" property in EXR format)
  // renderingGamma : Color space gamma used on rendering ( "Color Space Gamma"
  // value in the Render Settings )
  void postProcessImage(const TRasterImageP &img, bool has64bitOutputSupport,
                        bool writeInLinearColorSpace, bool isFirstTime,
                        double writingGamma, double renderingGamma,
                        const TRasterP &mark, int frame);

  //! Saves a frame or throws; returns its time-adjusted level frame number.
  int saveFrame(double frame, const FrameRasters &rasters);
  int outputFrame(double frame) const;
  void recordFailure(const std::wstring &error);
  void cancel();
  void requestCancel();
  void abortWriters() noexcept;
  void drainFrames(const std::vector<double> &frames);
  std::string getRenderCacheId();

  // returns board duration in frame
  int addBoard();
  // save clapperboard frame (for non-movie formats)
  void saveBoardFrame();
};

//---------------------------------------------------------

MovieRenderer::Imp::Imp(ToonzScene *scene, const TFilePath &moviePath,
                        int threadCount, bool cacheResults)
    : m_scene(scene)
    , m_renderer(threadCount)
    , m_fp(moviePath)
    , m_frameSize(scene->getCurrentCamera()->getRes())
    , m_xDpi(72)
    , m_yDpi(72)
    , m_renderSessionId(RenderSessionId++)
    , m_whiteSample(0)
    , m_savingThreadsCount(0)
    , m_firstCompletedRaster(
          true)         //< I know, sounds weird - it's just set to false
    , m_failure(false)  //  AFTER the first completed raster gets processed
    , m_cacheResults(cacheResults)
    , m_preview(moviePath.isEmpty())
    , m_movieType(isMovieType(moviePath))
    , m_seqRequired(isSequencialRequired(moviePath)) {
  m_renderCacheId =
      m_fp.withName(m_fp.getName() + "#RENDERID" +
                    QString::number(m_renderSessionId).toStdString())
          .getLevelName();

  m_renderer.addPort(this);
  // Bound admitted camera rasters and reordered clones, not FX/postprocessing
  // working memory, predictive caches, encoder buffers, or total process memory.
  m_renderer.setOutputRasterBudget(std::uint64_t(256) * 1024 * 1024);
}

//---------------------------------------------------------

MovieRenderer::Imp::~Imp() {
  m_renderer.removePort(this);  // Please, note: a TRenderer instance is
                                // currently a shared-pointer-like
}  // object to a private worker. *That* object may outlive the TRenderer
   // instance.

//---------------------------------------------------------

void MovieRenderer::Imp::prepareForStart() {
  struct locals {
    static void eraseUncompatibleExistingLevel(
        const TFilePath &fp, const TDimension &imageSize)  // nothrow
    {
      assert(!fp.isEmpty());
      // FFmpeg writers stage replacement output and publish only after a
      // successful encode. Keep the previous movie intact until then.
      if (MovieRenderOutput::usesFfmpeg(fp)) return;

      if (TSystem::doesExistFileOrLevel(fp)) {
        // In case the raster specifics are different from those of a currently
        // existing movie, erase it
        try {
          TLevelReaderP lr(fp);
          lr->loadInfo();

          const TImageInfo *info = lr->getImageInfo();
          if (!info || info->m_lx != imageSize.lx ||
              info->m_ly != imageSize.ly)
            TSystem::removeFileOrLevel(fp);  // nothrow
        } catch (...) {
          // Same if the level could not be read/opened
          TSystem::removeFileOrLevel(fp);  // nothrow
        }

        // NOTE: The level removal procedure could still fail.
        // In this case, no signaling takes place. The level readers will throw
        // when the time to write on the file comes, leading to a render
        // failure.
      }
    }
  };

  TOutputProperties *oprop = m_scene->getProperties()->getOutputProperties();
  double frameRate         = (double)oprop->getFrameRate();

  /*-- stretch of the Frame rate --*/
  double stretchFactor = oprop->getRenderSettings().m_timeStretchTo /
                         oprop->getRenderSettings().m_timeStretchFrom;
  frameRate *= stretchFactor;

  // Get the shrink
  int shrinkX = m_renderSettings.m_shrinkX,
      shrinkY = m_renderSettings.m_shrinkY;

  // Build the render area
  TPointD cameraPos(-0.5 * m_frameSize.lx, -0.5 * m_frameSize.ly);
  TDimensionD cameraRes(double(m_frameSize.lx) / shrinkX,
                        double(m_frameSize.ly) / shrinkY);
  TDimension cameraResI(cameraRes.lx, cameraRes.ly);

  TRectD renderArea(cameraPos.x, cameraPos.y, cameraPos.x + cameraRes.lx,
                    cameraPos.y + cameraRes.ly);
  setRenderArea(renderArea);

  if (!m_fp.isEmpty()) {
    try  // Construction of a LevelUpdater may throw (well, almost ANY operation
         // on a LevelUpdater
    {    // could throw). But due to backward compatibility this function is
         // assumed to be non-throwing.
      if (!m_renderSettings.m_stereoscopic) {
        locals::eraseUncompatibleExistingLevel(
            m_fp, oprop->getOutputSize(cameraResI));

        m_levelUpdaterA.reset(new LevelUpdater(
            m_fp, oprop->getFileFormatProperties(m_fp.getType()),
            oprop->formatTemplateFId(), MovieRenderOutput::usesFfmpeg(m_fp)));
        m_levelUpdaterA->getLevelWriter()->setFrameRate(frameRate);
        m_fp = m_levelUpdaterA->getLevelWriter()->getFilePath();
      } else {
        TFilePath leftFp  = m_fp.withName(m_fp.getName() + "_l");
        TFilePath rightFp = m_fp.withName(m_fp.getName() + "_r");

        locals::eraseUncompatibleExistingLevel(
            leftFp, oprop->getOutputSize(cameraResI));
        locals::eraseUncompatibleExistingLevel(
            rightFp, oprop->getOutputSize(cameraResI));

        m_levelUpdaterA.reset(new LevelUpdater(
            leftFp, oprop->getFileFormatProperties(leftFp.getType()),
            oprop->formatTemplateFId(), MovieRenderOutput::usesFfmpeg(leftFp)));
        m_levelUpdaterA->getLevelWriter()->setFrameRate(frameRate);
        leftFp = m_levelUpdaterA->getLevelWriter()->getFilePath();

        m_levelUpdaterB.reset(new LevelUpdater(
            rightFp, oprop->getFileFormatProperties(rightFp.getType()),
            oprop->formatTemplateFId(), MovieRenderOutput::usesFfmpeg(rightFp)));
        m_levelUpdaterB->getLevelWriter()->setFrameRate(frameRate);
        rightFp = m_levelUpdaterB->getLevelWriter()->getFilePath();
      }
    } catch (const TException &e) {
      recordFailure(e.getMessage());
      abortWriters();
      m_levelUpdaterA.reset();
      m_levelUpdaterB.reset();
    } catch (...) {
      // If we get here, it's because one of the LevelUpdaters could not be
      // created. So, let's say
      // that if one could not be created, then ALL OF THEM couldn't (ie saving
      // is not possible at all).
      abortWriters();
      m_levelUpdaterA.reset();
      m_levelUpdaterB.reset();
      recordFailure(L"Could not open the output level for writing.");
    }
  }
}

//---------------------------------------------------------

void MovieRenderer::Imp::addSoundtrack(int r0, int r1, double fps,
                                       int boardDuration) {
  TCG_ASSERT(r0 <= r1, return);

  TXsheet::SoundProperties *prop =
      new TXsheet::SoundProperties();  // Ownership will be surrendered ...
  prop->m_frameRate = fps;

  TSoundTrack *snd = m_scene->getXsheet()->makeSound(prop);  // ... here
  if (!snd) {
    // No soundtrack
    m_whiteSample = (r1 - r0 + 1) * 918;  // 918?? wtf... I don't think it has
    return;  // any arcane meaning - but i'm not touching it.
  }  // My impression would be that... no sound implies
     // no access to m_whiteSample, no?

  double samplePerFrame = snd->getSampleRate() / fps;

  // Extract the useful part of scene soundtrack
  TSoundTrackP snd1 = snd->extract((TINT32)(r0 * samplePerFrame),
                                   (TINT32)(r1 * samplePerFrame));
  assert(!m_st);
  if (!m_st) {
    // First, add white sound before the 'from' instant
    m_st          = TSoundTrack::create(snd1->getFormat(), m_whiteSample);
    m_whiteSample = 0;  // Why?  Probably being pedantic here... I guess
  }

  // Then, add the rest
  TINT32 fromSample = m_st->getSampleCount();
  TINT32 numSample =
      std::max(TINT32((r1 - r0 + 1) * samplePerFrame), snd1->getSampleCount());

  m_st = TSop::insertBlank(m_st, fromSample, numSample + m_whiteSample);
  m_st->copy(snd1, TINT32(fromSample + m_whiteSample));

  // insert blank sound for clapperboard
  if (boardDuration > 0)
    m_st = TSop::insertBlank(m_st, 0, TINT32(boardDuration * samplePerFrame));

  m_whiteSample = 0;
}

//---------------------------------------------------------

void MovieRenderer::Imp::onRenderRasterCompleted(const RenderData &renderData) {
  try {
    if (m_preview)
      doPreviewRasterCompleted(renderData);
    else
      doRenderRasterCompleted(renderData);
  } catch (TException &e) {
    onRenderFailure(renderData, e);
  } catch (...) {
    TException e("Could not process the rendered frame.");
    onRenderFailure(renderData, e);
  }
}

//---------------------------------------------------------

void MovieRenderer::Imp::postProcessImage(const TRasterImageP &img,
                                          bool has64bitOutputSupport,
                                          bool writeInLinearColorSpace,
                                          bool isFirstTime, double writingGamma,
                                          double renderingGamma,
                                          const TRasterP &mark, int frame) {
  img->setDpi(m_xDpi, m_yDpi);

  if (img->getRaster()->getPixelSize() == 8 && !has64bitOutputSupport) {
    TRaster32P aux(img->getRaster()->getLx(), img->getRaster()->getLy());
    TRop::convert(aux, img->getRaster());
    img->setRaster(aux);
  }

  // raster will be converted to linear before saving in exr format
  if (isFirstTime) {
    if (img->getRaster()->isLinear()) {
      if (!writeInLinearColorSpace)
        TRop::tosRGB(img->getRaster(), renderingGamma);
      // write in linear color space, but with different gamma
      else if (!areAlmostEqual(renderingGamma, writingGamma)) {
        double gammaAdjust = writingGamma / renderingGamma;
        // temporarily release the linear flag in order to use toLinearRGB
        img->getRaster()->setLinear(false);
        TRop::toLinearRGB(img->getRaster(), gammaAdjust);
      }
    } else if (!img->getRaster()->isLinear() && writeInLinearColorSpace) {
      TRop::toLinearRGB(img->getRaster(), writingGamma);
    }
  }

  // After the color space conversion, which is done in place once for the
  // frames that share a raster: the crop makes a copy.
  const TOutputProperties &out =
      *m_scene->getProperties()->getOutputProperties();
  if (out.getAspectMode() != TOutputProperties::NoAspectExport)
    img->setRaster(out.fitToAspectRatio(img->getRaster(), writingGamma));

  if (mark) addMark(mark, img);

  if (Preferences::instance()->isSceneNumberingEnabled())
    TRasterImageUtils::addGlobalNumbering(img, m_scene->getSceneName(), frame);
}

//---------------------------------------------------------------------

int MovieRenderer::Imp::saveFrame(double frame, const FrameRasters &rasters) {
  const int fr = outputFrame(frame);

  int boardDuration = 0;
  if (m_movieType) {
    BoardSettings *bs =
        m_scene->getProperties()->getOutputProperties()->getBoardSettings();
    boardDuration = (bs->isActive()) ? bs->getDuration() : 0;
  }

  TFrameId fid(fr + 1 + boardDuration);

  if (!m_levelUpdaterA || !rasters.first)
    throw TException("Output writer or rendered frame is unavailable.");

  {
    assert(m_levelUpdaterB.get() || !rasters.second);

    // Analyze writer
    bool has64bitOutputSupport   = false;
    bool writeInLinearColorSpace = false;
    double writingGamma          = 2.2;
    {
      if (TImageWriterP writerA =
              m_levelUpdaterA->getLevelWriter()->getFrameWriter(fid)) {
        has64bitOutputSupport = writerA->is64bitOutputSupported();

        const std::string &type = toLower(m_fp.getType());
        std::unique_ptr<Tiio::Writer> writer(Tiio::makeWriter(type));
        writeInLinearColorSpace = writer && writer->writeInLinearColorSpace();
        if (writeInLinearColorSpace) {
          TDoubleProperty *gammaProp =
              (TDoubleProperty *)(m_levelUpdaterA->getLevelWriter()
                                      ->getProperties()
                                      ->getProperty("Color Space Gamma"));
          if (gammaProp) writingGamma = gammaProp->getValue();
        }
      }

      // NOTE: If the writer could not be retrieved, the updater will throw.
      // Failure will be caught then.
    }

    // Prepare the images to be flushed
    TRasterP rasterA = rasters.first, rasterB = rasters.second;
    assert(rasterA);

    /*--- When caching the same raster, gamma only the first one and use the
result in subsequent frames
---*/
    if (m_renderSettings.m_gamma != 1.0 && rasters.applyGamma) {
      TRop::gammaCorrect(rasterA, m_renderSettings.m_gamma);
      if (rasterB) TRop::gammaCorrect(rasterB, m_renderSettings.m_gamma);
    }

    // Flush images
    {
      TRasterImageP imgA(rasterA);
      postProcessImage(imgA, has64bitOutputSupport, writeInLinearColorSpace,
                       rasters.applyGamma, writingGamma,
                       m_renderSettings.m_colorSpaceGamma,
                       m_renderSettings.m_mark, fid.getNumber());

      m_levelUpdaterA->update(fid, imgA);

      if (rasterB) {
        TRasterImageP imgB(rasterB);
        postProcessImage(imgB, has64bitOutputSupport, writeInLinearColorSpace,
                         rasters.applyGamma, writingGamma,
                         m_renderSettings.m_colorSpaceGamma,
                         m_renderSettings.m_mark, fid.getNumber());

        m_levelUpdaterB->update(fid, imgB);
      }

      if (m_cacheResults) {
        if (imgA->getRaster()->getPixelSize() == 8 ||
            imgA->getRaster()->getPixelSize() == 16) {
          // Convert 64-bit / float images to 32 - cached images are supposed to
          // be 32-bit
          TRaster32P aux(imgA->getRaster()->getLx(),
                         imgA->getRaster()->getLy());

          TRop::convert(aux, imgA->getRaster());
          imgA->setRaster(aux);
        }

        TImageCache::instance()->add(
            m_renderCacheId + std::to_string(fid.getNumber()), imgA);
      }
    }
  }

  return fr;
}

//---------------------------------------------------------------------

void MovieRenderer::Imp::doRenderRasterCompleted(const RenderData &renderData) {
  assert(!(m_cacheResults &&
           m_levelUpdaterB.get()));  // Cannot cache results on stereoscopy

  QMutexLocker locker(&m_mutex);

  if (m_canceled || m_finished || renderData.m_frames.empty()) return;
  if (!m_levelUpdaterA)
    throw TException("Output writer is unavailable.");

  // Build soundtrack at the first time a frame is completed - and the filetype
  // is that of a movie.
  if (m_firstCompletedRaster) {
    m_firstCompletedRaster = false;
    if (m_movieType && !m_st) {
      // The soundtrack is saved before the clapperboard frames: writers that
      // stream frames to ffmpeg start it, audio input included, on the first
      // frame
      BoardSettings *bs =
          m_scene->getProperties()->getOutputProperties()->getBoardSettings();
      int boardDuration = (bs->isActive()) ? bs->getDuration() : 0;

      int from, to;
      getRange(m_scene, false, from, to);

      TLevelP oldLevel(m_levelUpdaterA->getInputLevel());
      if (oldLevel && oldLevel->begin() != oldLevel->end()) {
        from = std::min(from, oldLevel->begin()->first.getNumber() - 1);
        to   = std::max(to, (--oldLevel->end())->first.getNumber() - 1);
      }

      addSoundtrack(
          from, to,
          m_scene->getProperties()->getOutputProperties()->getFrameRate(),
          boardDuration);

      if (m_st) {
        m_levelUpdaterA->getLevelWriter()->saveSoundTrack(m_st.getPointer());
        if (m_levelUpdaterB.get())
          m_levelUpdaterB->getLevelWriter()->saveSoundTrack(m_st.getPointer());
      }

      addBoard();
    } else {  // !m_movieType
      saveBoardFrame();
    }
  }

  // Each cluster owns its clones. Independent image clusters can write in
  // parallel, but frames sharing one raster must apply gamma only once.
  FrameRasters rasters;
  rasters.first = renderData.m_rasA->clone();
  rasters.second = renderData.m_rasB ? renderData.m_rasB->clone() : TRasterP();
  rasters.applyGamma = true;
  for (double frame : renderData.m_frames) {
    m_toBeSaved.push(frame, rasters);
    rasters.applyGamma = false;
  }
  locker.unlock();
  drainFrames(renderData.m_frames);
}

int MovieRenderer::Imp::outputFrame(double frame) const {
  if (m_preview) return int(frame);
  const double stretch = double(m_renderSettings.m_timeStretchTo) /
                         m_renderSettings.m_timeStretchFrom;
  return stretch != 1.0 ? tround(frame * stretch) : int(frame);
}

void MovieRenderer::Imp::recordFailure(const std::wstring &error) {
  m_failure = true;
  if (m_error.empty()) m_error = error;
}

void MovieRenderer::Imp::cancel() {
  m_toBeSaved.discardPending();
  // Keep the writers alive until all active save callbacks have returned.
  requestCancel();
}

void MovieRenderer::Imp::requestCancel() {
  m_canceled = true;
  // This lock never covers frame saving/finalization. requestAbort() only sets
  // the writer's cancellation token, so it can interrupt a full pipe queue or
  // an encoder's nested event loop without destroying an active writer.
  {
    QMutexLocker locker(&m_cancelMutex);
    for (const TLevelWriterP &writer : m_cancelWriters) writer->requestAbort();
  }
  m_renderer.stopRendering();
}

void MovieRenderer::Imp::abortWriters() noexcept {
  if (m_levelUpdaterA) m_levelUpdaterA->abort();
  if (m_levelUpdaterB) m_levelUpdaterB->abort();
}

void MovieRenderer::Imp::drainFrames(const std::vector<double> &frames) {
  QMutexLocker locker(&m_mutex);
  const bool requireSeq = !m_preview &&
      (Preferences::instance()->getFfmpegMultiThread() ? m_seqRequired
                                                       : m_movieType);
  size_t clusterIndex = 0;
  while (!m_canceled && !m_finished) {
    double frame;
    MovieRenderQueue<FrameRasters>::Result result;
    if (requireSeq) {
      if (!m_toBeSaved.takeNext(frame, result)) break;
    } else {
      // Do not let another worker consume half of a shared-raster cluster
      // while its first frame is still being color-converted.
      if (clusterIndex == frames.size()) break;
      frame = frames[clusterIndex++];
      if (!m_toBeSaved.take(frame, result)) continue;
    }

    if (!result.failed && !m_preview) {
      if (m_savingThreadsCount++ == 0) TStopWatch::global(0).start();
      if (!requireSeq) locker.unlock();
      try {
        saveFrame(frame, result.payload);
      } catch (const TException &e) {
        result.failed = true;
        result.error = e.getMessage();
      } catch (...) {
        result.failed = true;
        result.error = L"Could not save the rendered frame.";
      }
      if (!requireSeq) locker.relock();
      if (--m_savingThreadsCount == 0) TStopWatch::global(0).stop();
    }

    if (result.failed) recordFailure(result.error);
    if (m_canceled) break;
    bool keepRendering = true;
    TException error(result.error);
    for (MovieRenderer::Listener *listener : m_listeners) {
      keepRendering &= result.failed
          ? listener->onFrameFailed(outputFrame(frame), error)
          : listener->onFrameCompleted(outputFrame(frame));
    }
    if (!keepRendering) cancel();
  }
}

//---------------------------------------------------------

void MovieRenderer::Imp::doPreviewRasterCompleted(
    const RenderData &renderData) {
  QMutexLocker locker(&m_mutex);
  if (m_canceled || m_finished || renderData.m_frames.empty()) return;

  const QString name = getPreviewName(m_renderSessionId);
  TRasterP ras = renderData.m_rasA->clone();
  if (renderData.m_rasB) {
    assert(m_renderSettings.m_stereoscopic);
    TRop::makeStereoRaster(ras, renderData.m_rasB);
  }

  TRasterImageP img(ras);
  img->setDpi(m_xDpi, m_yDpi);
  if (renderData.m_info.m_mark) addMark(renderData.m_info.m_mark, img);
  if (img->getRaster()->getPixelSize() == 8 ||
      img->getRaster()->getPixelSize() == 16) {
    TRaster32P aux(img->getRaster()->getLx(), img->getRaster()->getLy());
    TRop::convert(aux, img->getRaster());
    img->setRaster(aux);
  }

  for (double frame : renderData.m_frames) {
    const QString frameName = name + QString::number(frame + 1);
    TImageCache::instance()->add(frameName.toStdString(), img);
    m_toBeSaved.push(frame, FrameRasters());
  }
  locker.unlock();
  drainFrames(renderData.m_frames);
}

//---------------------------------------------------------

void MovieRenderer::Imp::onRenderFailure(const RenderData &renderData,
                                        TException &e) {
  {
    QMutexLocker locker(&m_mutex);
    if (m_canceled || m_finished) return;
    recordFailure(e.getMessage());
    for (double frame : renderData.m_frames)
      m_toBeSaved.fail(frame, e.getMessage());
  }
  drainFrames(renderData.m_frames);
}

//---------------------------------------------------------

void MovieRenderer::Imp::onRenderFinished(bool isCanceled) {
  {
    QMutexLocker locker(&m_mutex);
    if (m_finished) return;
    m_finished = true;
    if (isCanceled) m_canceled = true;
    if (!m_canceled && !m_toBeSaved.complete())
      recordFailure(L"Rendering ended before all requested frames were saved.");
    m_toBeSaved.discardPending();
  }

  // TRenderer finishes only after every frame callback has returned. Explicit
  // close includes trailing original frames and fallible encoder finalization.
  // Abort incomplete renders before releasing writers so destructor fallback
  // cannot publish an otherwise successfully encoded partial movie.
  for (LevelUpdater *updater : {m_levelUpdaterA.get(), m_levelUpdaterB.get()}) {
    if (!updater) continue;
    if (m_canceled || m_failure) {
      updater->abort();
      continue;
    }
    try {
      updater->close();
    } catch (const TException &e) {
      recordFailure(e.getMessage());
      updater->abort();
    } catch (...) {
      recordFailure(L"Could not finalize the output level.");
      updater->abort();
    }
  }
  m_levelUpdaterA.reset();
  m_levelUpdaterB.reset();
  {
    QMutexLocker locker(&m_cancelMutex);
    m_cancelWriters.clear();
  }

  if ((m_canceled || m_failure) && (m_cacheResults || m_preview)) {
    const QString previewName = getPreviewName(m_renderSessionId);
    BoardSettings *board =
        m_scene->getProperties()->getOutputProperties()->getBoardSettings();
    const int boardDuration = m_movieType && board->isActive()
                                  ? board->getDuration() : 0;
    for (const auto &frame : m_framesToBeRendered) {
      const std::string key = m_preview
          ? (previewName + QString::number(frame.first + 1)).toStdString()
          : m_renderCacheId +
                std::to_string(outputFrame(frame.first) + 1 + boardDuration);
      TImageCache::instance()->remove(key);
    }
  }

  const TFilePath levelName = m_preview
      ? TFilePath(getPreviewName(m_renderSessionId).toStdWString()) : m_fp;
  const CompletionStatus status = m_canceled ? CompletionStatus::Canceled
      : m_failure ? CompletionStatus::Failed : CompletionStatus::Completed;
  for (MovieRenderer::Listener *listener : m_listeners)
    listener->onSequenceFinished(levelName, status, m_error);

  release();  // Release the reference owned by this render process.
}

//---------------------------------------------------------

int MovieRenderer::Imp::addBoard() {
  BoardSettings *boardSettings =
      m_scene->getProperties()->getOutputProperties()->getBoardSettings();
  if (!boardSettings->isActive()) return 0;
  int duration = boardSettings->getDuration();
  if (duration == 0) return 0;
  // Get the image size
  int shrinkX = m_renderSettings.m_shrinkX,
      shrinkY = m_renderSettings.m_shrinkY;
  TDimensionD cameraRes(double(m_frameSize.lx) / shrinkX,
                        double(m_frameSize.ly) / shrinkY);
  TDimension cameraResI(cameraRes.lx, cameraRes.ly);

  // the board at the size of the written frames
  TDimension outputSize =
      m_scene->getProperties()->getOutputProperties()->getOutputSize(
          cameraResI);
  TRaster32P boardRas =
      boardSettings->getBoardRaster(outputSize, shrinkX, m_scene);

  if (m_levelUpdaterA.get()) {
    // Flush images
    try {
      TRasterImageP img(boardRas);
      for (int f = 0; f < duration; f++) {
        m_levelUpdaterA->update(TFrameId(f + 1), img);
        if (m_levelUpdaterB.get())
          m_levelUpdaterB->update(TFrameId(f + 1), img);
      }
    } catch (const TException &e) {
      recordFailure(e.getMessage());
    } catch (...) {
      recordFailure(L"Could not save the clapperboard frames.");
    }
  }
  return duration;
}

//---------------------------------------------------------
// save clapperboard frame (for non-movie formats)

void MovieRenderer::Imp::saveBoardFrame() {
  BoardSettings *boardSettings =
      m_scene->getProperties()->getOutputProperties()->getBoardSettings();
  if (!boardSettings->isActive()) return;
  int duration = boardSettings->getDuration();
  if (duration == 0) return;

  // Get the image size
  int shrinkX = m_renderSettings.m_shrinkX,
      shrinkY = m_renderSettings.m_shrinkY;
  TDimensionD cameraRes(double(m_frameSize.lx) / shrinkX,
                        double(m_frameSize.ly) / shrinkY);
  TDimension cameraResI(cameraRes.lx, cameraRes.ly);

  // the board at the size of the written frames
  TDimension outputSize =
      m_scene->getProperties()->getOutputProperties()->getOutputSize(
          cameraResI);
  TRaster32P boardRas =
      boardSettings->getBoardRaster(outputSize, shrinkX, m_scene);

  if (m_levelUpdaterA.get()) {
    // Flush images
    try {
      TRasterImageP img(boardRas);

      TFilePath rendPath = m_levelUpdaterA->getLevelWriter()->getFilePath();
      QString baseName   = QString::fromStdString(rendPath.getName());
      QString levelName  = QString::fromStdString(rendPath.getLevelName());
      int dotPos         = levelName.lastIndexOf('.');
      QString suffix     = boardSettings->fileNameSuffix();
      // if the suffix starts with "." or "_", then replace the sepchar
      if (!suffix.startsWith('.') && !suffix.startsWith('_'))
        suffix.prepend(rendPath.getSepChar());
      QString fileName =
          baseName + suffix + levelName.right(levelName.size() - dotPos);
      TFilePath boardPath = rendPath.getParentDir() + TFilePath(fileName);

      TImageWriterP imgWriter =
          m_levelUpdaterA->getLevelWriter()->getFrameWriter(0);
      imgWriter->setFilePath(boardPath);
      imgWriter->save(img);

      // do nothing for stereoscopic render
      // if (m_levelUpdaterB.get()) {}
    } catch (const TException &e) {
      recordFailure(e.getMessage());
    } catch (...) {
      recordFailure(L"Could not save the clapperboard image.");
    }
  }
  return;
}

//======================================================================================

//======================
//    MovieRenderer
//----------------------

MovieRenderer::MovieRenderer(ToonzScene *scene, const TFilePath &moviePath,
                             int threadCount, bool cacheResults)
    : m_imp(new Imp(scene, moviePath, threadCount, cacheResults)) {
  m_imp->addRef();  // See MovieRenderer::start(). Can't just delete it in the
                    // dtor.
}

//---------------------------------------------------------

MovieRenderer::~MovieRenderer() { m_imp->release(); }

//---------------------------------------------------------

void MovieRenderer::setRenderSettings(const TRenderSettings &renderSettings) {
  m_imp->m_renderSettings = renderSettings;
}

//---------------------------------------------------------

void MovieRenderer::setDpi(double xDpi, double yDpi) {
  m_imp->m_xDpi = xDpi;
  m_imp->m_yDpi = yDpi;
}

//---------------------------------------------------------

void MovieRenderer::addListener(Listener *listener) {
  m_imp->m_listeners.insert(listener);
}

//---------------------------------------------------------

void MovieRenderer::addFrame(double frame, const TFxPair &fxPair) {
  m_imp->m_framesToBeRendered.push_back(std::make_pair(frame, fxPair));
  m_imp->m_toBeSaved.addFrame(frame);
}

//---------------------------------------------------------

void MovieRenderer::enablePrecomputing(bool on) {
  m_imp->m_renderer.enablePrecomputing(on);
}

//---------------------------------------------------------

bool MovieRenderer::isPrecomputingEnabled() const {
  return m_imp->m_renderer.isPrecomputingEnabled();
}

//---------------------------------------------------------

void MovieRenderer::start() {
  if (m_imp->m_started) return;
  m_imp->m_started = true;

  // The render process owns this reference until its one terminal callback,
  // which follows all frame callbacks. Startup failures use the same callback.
  m_imp->addRef();
  try {
    // Window admission and sequential writers must use the same ascending
    // order as the result queue, even when a caller adds frames out of order.
    std::stable_sort(m_imp->m_framesToBeRendered.begin(),
                     m_imp->m_framesToBeRendered.end(),
                     [](const std::pair<double, TFxPair> &a,
                        const std::pair<double, TFxPair> &b) {
                       return a.first < b.first;
                     });
    if (m_imp->m_framesToBeRendered.empty())
      m_imp->recordFailure(L"No frames were requested for rendering.");
    else if (!m_imp->m_canceled)
      m_imp->prepareForStart();

    if (!m_imp->m_failure && !m_imp->m_canceled) {
      {
        QMutexLocker locker(&m_imp->m_cancelMutex);
        for (LevelUpdater *updater : {m_imp->m_levelUpdaterA.get(),
                                      m_imp->m_levelUpdaterB.get()}) {
          if (!updater) continue;
          TLevelWriterP writer =
              MovieRenderOutput::cancelableWriter(updater->getLevelWriter());
          if (writer) m_imp->m_cancelWriters.push_back(writer);
        }
      }

      // Keep ownership while allocating/populating the request vector. The
      // queued renderer takes ownership only when submission succeeds.
      std::unique_ptr<RenderDataVector> data(new RenderDataVector);
      data->reserve(m_imp->m_framesToBeRendered.size());
      for (const auto &frame : m_imp->m_framesToBeRendered)
        data->push_back(TRenderer::RenderData(
            frame.first, m_imp->m_renderSettings, frame.second));
      m_imp->m_renderer.startRendering(data.get());
      data.release();
      return;
    }
  } catch (const TException &e) {
    m_imp->recordFailure(e.getMessage());
  } catch (...) {
    m_imp->recordFailure(L"Could not initialize rendering.");
  }

  // Match the asynchronous renderer contract even for startup failures.
  // Listeners can enter their event loop before the terminal notification.
  Imp *imp = m_imp;
  QTimer::singleShot(0, [imp]() { imp->onRenderFinished(); });
}

//---------------------------------------------------------

void MovieRenderer::onCanceled() { m_imp->requestCancel(); }

std::function<void()> MovieRenderer::getCancelCallback() {
  TSmartPointerT<Imp> imp(m_imp);
  return [imp]() { imp->requestCancel(); };
}

//---------------------------------------------------------

TRenderer *MovieRenderer::getTRenderer() {
  // Again, this is somewhat BAD. The pointed-to object dies together with the
  // MovieRenderer instance.
  // Since a TRenderer is already smart-pointer-like, we could just return a
  // copy - however, it really
  // shouldn't be that way. Maybe one day we'll revert that and actually use a
  // smart pointer class.

  // For now, no use of this function seems to access the returned pointer
  // beyond the lifespan of the
  // associated MovieRenderer instance - so I'm not gonna touch the class
  // interface.
  return &m_imp->m_renderer;
}
