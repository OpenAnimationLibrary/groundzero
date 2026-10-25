// Render fx trees through TRenderer with and without frame strips (see
// TRenderer::enableFrameStrips) and compare the results. The blur source is
// included so that the production fx is tested without linking tnzstdfx.
#include "../blurfx.cpp"

// blurfx.cpp calls this stdfx.cpp helper; tnzstdfx is not linked, so define
// it here with the same tolerance.
bool isAlmostIsotropic(const TAffine &aff) { return aff.isIsotropic(0.001); }

#include "trenderer.h"
#include "tthread.h"
#include "tparamcontainer.h"
#include "tconvert.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

//==================================================================

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

// Frame geometry: 4 threads split its 256 rows into 4 strips of 64 rows.
const int c_frameX0 = -160, c_frameY0 = -128;
const int c_frameLx = 320, c_frameLy = 256;
const int c_threads = 4, c_strips = 4;

// Requests received by StripsPatternFx, and the threads computing them.
QMutex requestsMutex;
std::vector<TRectD> requests;
std::set<QThread *> requestThreads;

void clearRequests() {
  QMutexLocker locker(&requestsMutex);
  requests.clear();
  requestThreads.clear();
}

}  // namespace

//==================================================================

//! Deterministic per-pixel pattern with a finite bounding box. Its result does
//! not depend on the tiles it is computed on.
class StripsPatternFx final : public TStandardRasterFx {
  FX_PLUGIN_DECLARATION(StripsPatternFx)

public:
  bool canHandle(const TRenderSettings &, double) override { return true; }

  bool doGetBBox(double, TRectD &bBox, const TRenderSettings &) override {
    bBox = TRectD(-150, -100, 140, 110);
    return true;
  }

  void doCompute(TTile &tile, double, const TRenderSettings &) override {
    TRaster32P ras = tile.getRaster();
    if (!ras) throw TException("StripsPatternFx: unsupported pixel type");

    int x0 = int(std::floor(tile.m_pos.x)), y0 = int(std::floor(tile.m_pos.y));
    {
      QMutexLocker locker(&requestsMutex);
      requests.push_back(TRectD(x0, y0, x0 + ras->getLx(), y0 + ras->getLy()));
      requestThreads.insert(QThread::currentThread());
    }

    ras->lock();
    for (int y = 0; y < ras->getLy(); ++y) {
      TPixel32 *pix = ras->pixels(y);
      for (int x = 0; x < ras->getLx(); ++x, ++pix) {
        unsigned int h =
            unsigned(x0 + x) * 73856093u ^ unsigned(y0 + y) * 19349663u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;

        int m  = 64 + int(h >> 24) % 192;
        pix->r = (int(h & 0xff) * m) / 255;
        pix->g = (int((h >> 8) & 0xff) * m) / 255;
        pix->b = (int((h >> 16) & 0xff) * m) / 255;
        pix->m = m;
      }
    }
    ras->unlock();
  }
};

FX_PLUGIN_IDENTIFIER(StripsPatternFx, "stripsPatternTestFx")

//==================================================================

namespace {

class CapturePort final : public TRenderPort {
  QMutex m_mutex;
  TRasterP m_raster;
  std::string m_error;
  QEventLoop *m_loop;

public:
  explicit CapturePort(QEventLoop *loop) : m_loop(loop) {
    setRenderArea(TRectD(c_frameX0, c_frameY0, c_frameX0 + c_frameLx,
                         c_frameY0 + c_frameLy));
  }

  void onRenderRasterCompleted(const RenderData &renderData) override {
    QMutexLocker locker(&m_mutex);
    m_raster = renderData.m_rasA->clone();
  }

  void onRenderFailure(const RenderData &, TException &e) override {
    QMutexLocker locker(&m_mutex);
    m_error = ::to_string(e.getMessage());
    if (m_error.empty()) m_error = "unknown render failure";
  }

  void onRenderFinished(bool) override {
    QMetaObject::invokeMethod(m_loop, "quit", Qt::QueuedConnection);
  }

  TRasterP raster() {
    QMutexLocker locker(&m_mutex);
    return m_raster;
  }
  std::string error() {
    QMutexLocker locker(&m_mutex);
    return m_error;
  }
};

//------------------------------------------------------------------

//! Renders frame 0 of the passed tree through TRenderer's asynchronous API.
TRaster32P render(const TRasterFxP &fx, int threads, bool frameStrips) {
  TRenderer renderer(threads);
  renderer.enableFrameStrips(frameStrips);

  QEventLoop loop;
  CapturePort port(&loop);
  renderer.addPort(&port);

  TRasterFxP noFx;
  renderer.startRendering(0, TRenderSettings(), TFxPair(fx, noFx));

  QTimer::singleShot(120000, &loop, &QEventLoop::quit);
  loop.exec();

  renderer.stopRendering(true);
  renderer.removePort(&port);

  require(port.error().empty(), "Render failed: " + port.error());
  TRaster32P ras = port.raster();
  require(ras && ras->getLx() == c_frameLx && ras->getLy() == c_frameLy,
          "No frame of the expected size was rendered");
  return ras;
}

//------------------------------------------------------------------

//! Computes the frame strip by strip on the calling thread, with the strip
//! layout documented in TRenderer::enableFrameStrips.
TRaster32P computeStrips(const TRasterFxP &fx, int stripsCount) {
  TRenderer renderer(1);
  unsigned long renderId = TRenderer::buildRenderId();

  renderer.install(renderId);
  renderer.declareRenderStart(renderId);
  renderer.declareFrameStart(0);

  TRaster32P frame(c_frameLx, c_frameLy);
  frame->clear();

  TRenderSettings info;
  for (int s = 0; s < stripsCount; ++s) {
    int y0 = c_frameLy * s / stripsCount,
        y1 = c_frameLy * (s + 1) / stripsCount;

    TRaster32P stripRas(c_frameLx, y1 - y0);
    stripRas->clear();

    TTile strip(stripRas, TPointD(c_frameX0, c_frameY0 + y0));
    fx->compute(strip, 0, info);
    frame->copy(strip.getRaster(), TPoint(0, y0));
  }

  renderer.declareFrameEnd(0);
  renderer.declareRenderEnd(renderId);
  renderer.uninstall();

  return frame;
}

//------------------------------------------------------------------

//! Returns the number of differing pixels; maxDiff receives the largest
//! channel difference.
int compare(const TRaster32P &a, const TRaster32P &b, int &maxDiff) {
  int count = 0;
  maxDiff   = 0;

  a->lock();
  b->lock();
  for (int y = 0; y < a->getLy(); ++y) {
    const TPixel32 *pa = a->pixels(y), *pb = b->pixels(y);
    for (int x = 0; x < a->getLx(); ++x, ++pa, ++pb) {
      int diff = std::max({std::abs(pa->r - pb->r), std::abs(pa->g - pb->g),
                           std::abs(pa->b - pb->b), std::abs(pa->m - pb->m)});
      if (diff) ++count;
      maxDiff = std::max(maxDiff, diff);
    }
  }
  b->unlock();
  a->unlock();

  return count;
}

//------------------------------------------------------------------

void testPatternStrips() {
  TRasterFxP pattern(new StripsPatternFx);

  clearRequests();
  TRaster32P reference = render(pattern, 1, false);
  {
    QMutexLocker locker(&requestsMutex);
    require(requests.size() == 1, "The unsplit frame must be one request");
  }

  clearRequests();
  TRaster32P strips = render(pattern, c_threads, true);
  {
    QMutexLocker locker(&requestsMutex);
    std::cout << "pattern: " << requests.size() << " requests on "
              << requestThreads.size() << " threads" << std::endl;
    require(requests.size() == size_t(c_strips),
            "The frame was not split into the expected strips");
  }

  int maxDiff = 0;
  require(compare(reference, strips, maxDiff) == 0,
          "Strips of a per-pixel fx differ from the unsplit frame");
}

//------------------------------------------------------------------

void testBlurStrips() {
  TFx *checkBoard = TFx::create("checkBoardFx");
  require(checkBoard, "checkBoardFx is not registered");

  TRasterFxP blur(new BlurFx);
  require(blur->connect("Source", checkBoard), "Could not connect the blur");

  TDoubleParamP value = TParamP(blur->getParams()->getParam("value"));
  require(bool(value), "Missing blur value parameter");
  value->setDefaultValue(17.5);

  TRaster32P reference = render(blur, 1, false);
  TRaster32P strips    = render(blur, c_threads, true);
  TRaster32P expected  = computeStrips(blur, c_strips);

  int maxDiff = 0;
  require(compare(expected, strips, maxDiff) == 0,
          "Parallel strips differ from the same strips computed in sequence");

  // TRop::blur accumulates its vertical pass from the bottom of each input
  // tile, so strips may round differently from the unsplit frame.
  int count = compare(reference, strips, maxDiff);
  std::cout << "blur: " << count << " pixels differ from the unsplit frame"
            << " (max channel difference " << maxDiff << ")" << std::endl;
  require(maxDiff <= 1, "Blurred strips differ from the unsplit frame");
}

}  // namespace

//==================================================================

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  TThread::init();
  TRenderer::initialize();

  int result = 0;
  try {
    testPatternStrips();
    testBlurStrips();
    std::cout << "framestrips: OK" << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "framestrips: FAILED: " << e.what() << std::endl;
    result = 1;
  }

  TThread::shutdown();
  return result;
}
